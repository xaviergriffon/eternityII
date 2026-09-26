#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "ui/logger.h"
#include "core/possibility.h"
#include "core/datamanager.h"
#include "core/packet_codec.h"
#include "core/stock_spill.h"
#include "core/stock_tier.h"

static void tier_configure(int nb_files);

/**
 * @brief État de débordement d'UN (pool, file de stock) — une pile de
 *        segments numérotés first_seq..last_seq, chacun une suite de TRAMES
 *        (un bloc de l'étage RAM par trame, cf. `spill_frame_t`).
 *
 * Un segment reçoit des trames tant qu'il tient moins de
 * `STOCK_SPILL_SEGMENT_RECORDS` possibilités ; une trame n'est jamais coupée
 * entre deux segments. Seul le sommet (`last_seq`) est mutable : tout segment
 * dessous a été ramené à son sommet logique au moment où la pile a roulé
 * (`spill_prepare_top_locked`), sa taille physique EST donc sa taille logique
 * — c'est ce qui permet de le lier dans un cliché et de reprendre ses
 * compteurs sur le disque quand il redevient sommet (`spill_load_top_locked`).
 *
 * `first_seq` vaut 1 sauf quand une expansion a consommé la pile PAR LE BAS
 * (`stock_spill_expansion_take`) : les numéros restent alors ceux d'origine,
 * sans renommage, et le cliché les renumérote de son côté à partir de 1.
 */
typedef struct {
	int first_seq;               ///< 0 si aucun segment ; sinon le bas de la pile (1 hors expansion).
	int last_seq;                ///< 0 si aucun segment ; sinon le sommet de la pile.
	unsigned long long packets;  ///< Total de possibilités déportées, ce (pool, file).
	long tail_bytes;             ///< Sommet logique du segment `last_seq`, en octets (frontière de trame).
	long tail_records;           ///< Possibilités du segment `last_seq` sous `tail_bytes`.
} stock_spill_descriptor_t;

static char *g_spill_dir = NULL;
static int g_spill_nb_files = 0;
static int g_spill_enabled = 0;
static stock_spill_descriptor_t *g_spill_unchecked = NULL; // [g_spill_nb_files]
static stock_spill_descriptor_t *g_spill_checked = NULL;   // [g_spill_nb_files]
static pthread_mutex_t g_spill_mutex = PTHREAD_MUTEX_INITIALIZER;

/// Nombre de segments d'une pile (0 si vide).
static int spill_segment_count(const stock_spill_descriptor_t *desc)
{
	return (desc->last_seq == 0) ? 0 : desc->last_seq - desc->first_seq + 1;
}

/// État d'hystérésis courant (cf. la doc de `stock_spill_step`) : IDLE (rien
/// à faire), EVICTING (occupation RAM >= 90 % du plafond, en train d'évacuer
/// vers le disque) ou RELOADING (occupation <= 25 %, en train de recharger).
typedef enum { SPILL_MODE_IDLE = 0, SPILL_MODE_EVICTING = 1, SPILL_MODE_RELOADING = 2 } spill_mode_t;
static spill_mode_t g_spill_mode = SPILL_MODE_IDLE;

/// Surcharge réservée aux tests (0 = désactivée, utiliser
/// `STOCK_SPILL_SEGMENT_RECORDS`) — cf. `stock_spill_set_segment_records_for_tests`.
/// 131 072 possibilités par segment rendent le franchissement d'une frontière
/// de segment impraticable à exercer dans un test unitaire rapide sans elle.
static long g_segment_records_override = 0;

/// Possibilités au-delà desquelles un segment n'accepte plus de trame.
static long spill_segment_records(void)
{
	return (g_segment_records_override > 0) ? g_segment_records_override : STOCK_SPILL_SEGMENT_RECORDS;
}

/**
 * @brief Format des segments d'un cliché, d'après la magie de son manifeste.
 *
 * Seul `SPILL_FORMAT_FRAMED` est encore écrit. Les deux autres sont des
 * segments à pas FIXE, relus seulement depuis un cliché antérieur (les
 * segments vivants sont purgés au démarrage et réécrits par ce binaire) :
 * restaurés par réempaquetage, jamais par lien direct.
 */
typedef enum {
	SPILL_FORMAT_FRAMED = 0,  ///< v3 : trames de blocs (`spill_frame_t`).
	SPILL_FORMAT_COMPACT = 1, ///< v2 : forme compacte à pas fixe `PACKET_CODEC_MAX_BYTES`.
	SPILL_FORMAT_RAW = 2      ///< v1 : `struct possibility_packet` bruts.
} spill_format_t;

/// Pas d'un enregistrement d'un segment à pas fixe (formats hérités).
static long spill_stride_bytes(spill_format_t format)
{
	return (format == SPILL_FORMAT_RAW) ? (long)sizeof(struct possibility_packet) : (long)PACKET_CODEC_MAX_BYTES;
}

/**
 * @brief Décode `n` enregistrements à pas fixe de `raw` vers `buf`.
 * @return Nombre de paquets décodés — < `n` au premier enregistrement illisible.
 */
static int spill_decode_stride(const uint8_t *raw, int n, spill_format_t format, struct possibility_packet *buf)
{
	if (format == SPILL_FORMAT_RAW) {
		memcpy(buf, raw, (size_t)n * sizeof(struct possibility_packet));
		return n;
	}
	long stride = spill_stride_bytes(format);
	for (int i = 0; i < n; i++) {
		if (packet_codec_decode(raw + (size_t)i * (size_t)stride, (size_t)stride, &buf[i], NULL) != 0) {
			return i;
		}
	}
	return n;
}

/// Décode les enregistrements compacts concaténés de `raw` vers `out` (au plus `max`).
/// @return Le nombre décodé, ou -1 sur enregistrement illisible.
static int tier_decode_block(const uint8_t *raw, size_t raw_bytes, struct possibility_packet *out, int max)
{
	size_t off = 0;
	int n = 0;
	while (off < raw_bytes) {
		size_t len = stock_tier_record_len(raw + off, raw_bytes - off);
		if (len == 0 || n >= max || packet_codec_decode(raw + off, len, &out[n], NULL) != 0) {
			return -1;
		}
		off += len;
		n++;
	}
	return n;
}

/// Nombre maximal d'enregistrements dans un bloc (tous vides : en-tête +
/// bitmap seulement).
#define STOCK_TIER_MAX_RECORDS (STOCK_TIER_BLOCK_BYTES / (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES))

/*
 * Trame d'un segment : un bloc, sous la forme STOCKÉE que l'étage RAM lui donne
 * (`stock_tier_pack` — compressé par zstd sous `make ZSTD=1`), encadré d'un
 * en-tête et d'un pied, petit-boutistes :
 *
 *   en-tête (20 o) : "ETSB", codec u8, 3 o nuls, records u32, raw_bytes u32, stored_bytes u32
 *   charge         : stored_bytes octets
 *   pied    (12 o) : records u32, stored_bytes u32, "ETSE"
 *
 * Le pied permet de dépiler par le HAUT (rechargement) sans index : il donne
 * la longueur de la trame qu'il termine. L'en-tête permet de lire par le BAS
 * (expansion, cliché). Une trame s'écrit et se relit en entier, jamais
 * entamée — l'unité atomique de l'étage RAM le reste sur disque, et les
 * enregistrements de taille variable n'ont besoin d'aucune arithmétique à pas
 * variable : la sûreté de la pile (« peek puis commit », recalage sur le sommet
 * logique avant un ajout) porte sur des frontières de trame.
 */
#define SPILL_FRAME_HEADER_BYTES 20
#define SPILL_FRAME_TRAILER_BYTES 12
#define SPILL_FRAME_OVERHEAD (SPILL_FRAME_HEADER_BYTES + SPILL_FRAME_TRAILER_BYTES)
static const uint8_t k_frame_head_magic[4] = { 'E', 'T', 'S', 'B' };
static const uint8_t k_frame_tail_magic[4] = { 'E', 'T', 'S', 'E' };

typedef struct {
	int codec;
	uint32_t records;
	uint32_t raw_bytes;
	uint32_t stored_bytes;
} spill_frame_t;

static void spill_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t spill_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void spill_frame_header(uint8_t *h, const spill_frame_t *fr)
{
	memcpy(h, k_frame_head_magic, 4);
	h[4] = (uint8_t)fr->codec;
	h[5] = h[6] = h[7] = 0;
	spill_put_u32(h + 8, fr->records);
	spill_put_u32(h + 12, fr->raw_bytes);
	spill_put_u32(h + 16, fr->stored_bytes);
}

static void spill_frame_trailer(uint8_t *t, const spill_frame_t *fr)
{
	spill_put_u32(t, fr->records);
	spill_put_u32(t + 4, fr->stored_bytes);
	memcpy(t + 8, k_frame_tail_magic, 4);
}

/// @return 0 si `h` est un en-tête plausible : magie, codec connu, tailles
/// dans leurs bornes (`stock_tier_pack` ne garde une forme compressée que plus
/// petite que les octets bruts).
static int spill_frame_parse_header(const uint8_t *h, spill_frame_t *fr)
{
	if (memcmp(h, k_frame_head_magic, 4) != 0 || h[5] != 0 || h[6] != 0 || h[7] != 0) {
		return -1;
	}
	fr->codec = h[4];
	fr->records = spill_get_u32(h + 8);
	fr->raw_bytes = spill_get_u32(h + 12);
	fr->stored_bytes = spill_get_u32(h + 16);
	if ((fr->codec != STOCK_TIER_CODEC_RAW && fr->codec != STOCK_TIER_CODEC_ZSTD) || fr->records == 0
	    || fr->raw_bytes == 0 || fr->raw_bytes > STOCK_TIER_BLOCK_BYTES || fr->stored_bytes == 0
	    || fr->stored_bytes > fr->raw_bytes) {
		return -1;
	}
	return 0;
}

/// @return 0 si le pied `t` termine bien la trame décrite par `fr`.
static int spill_frame_check_trailer(const uint8_t *t, const spill_frame_t *fr)
{
	return (spill_get_u32(t) == fr->records && spill_get_u32(t + 4) == fr->stored_bytes
	        && memcmp(t + 8, k_frame_tail_magic, 4) == 0) ? 0 : -1;
}

/// Taille sur disque de la trame `fr`.
static long spill_frame_bytes(const spill_frame_t *fr)
{
	return (long)SPILL_FRAME_OVERHEAD + (long)fr->stored_bytes;
}

/// Taille physique de `path`, -1 s'il n'existe pas.
static long spill_file_size(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0) ? (long)st.st_size : -1;
}

/**
 * @brief Compte les possibilités des trames de `path`, de l'octet 0 à `limit`
 *        (< 0 : tout le fichier), en ne lisant que les en-têtes et les pieds.
 * @return 0 si `limit` tombe exactement sur une fin de trame et que chaque
 *         trame est cohérente (en-tête plausible, pied assorti), -1 sinon.
 */
static int spill_scan_segment(const char *path, long limit, unsigned long long *out_records)
{
	*out_records = 0;
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return -1;
	}
	if (limit < 0) {
		struct stat st;
		limit = (fstat(fileno(f), &st) == 0) ? (long)st.st_size : -1;
	}
	int ok = (limit >= 0);
	long off = 0;
	while (ok && off < limit) {
		uint8_t h[SPILL_FRAME_HEADER_BYTES], t[SPILL_FRAME_TRAILER_BYTES];
		spill_frame_t fr;
		ok = limit - off >= SPILL_FRAME_OVERHEAD && fseek(f, off, SEEK_SET) == 0
		     && fread(h, 1, sizeof h, f) == sizeof h && spill_frame_parse_header(h, &fr) == 0
		     && off + spill_frame_bytes(&fr) <= limit
		     && fseek(f, off + SPILL_FRAME_HEADER_BYTES + (long)fr.stored_bytes, SEEK_SET) == 0
		     && fread(t, 1, sizeof t, f) == sizeof t && spill_frame_check_trailer(t, &fr) == 0;
		if (ok) {
			*out_records += fr.records;
			off += spill_frame_bytes(&fr);
		}
	}
	fclose(f);
	return ok ? 0 : -1;
}

/**
 * @brief Parcourt les trames `[0, bytes[` d'un tampon, dans l'ordre d'écriture,
 *        et remet à `fn` les octets bruts de chacune (décompressés et
 *        revalidés), avec son décalage dans le tampon.
 * @return 0 si tout le tampon a été remis, -1 à la première trame incohérente
 *         ou refusée par `fn` (ce qui précède a été remis).
 */
typedef int (*spill_frame_fn)(const uint8_t *raw, size_t raw_bytes, int records, long offset, void *ctx);

static int spill_walk_frames(const uint8_t *buf, long bytes, long base_offset, uint8_t *raw, spill_frame_fn fn,
                             void *ctx)
{
	long off = 0;
	while (off < bytes) {
		spill_frame_t fr;
		if (bytes - off < SPILL_FRAME_OVERHEAD || spill_frame_parse_header(buf + off, &fr) != 0
		    || off + spill_frame_bytes(&fr) > bytes
		    || spill_frame_check_trailer(buf + off + SPILL_FRAME_HEADER_BYTES + fr.stored_bytes, &fr) != 0) {
			return -1;
		}
		int n = stock_tier_unpack(fr.codec, buf + off + SPILL_FRAME_HEADER_BYTES, fr.stored_bytes, fr.raw_bytes,
		                          fr.records, raw, STOCK_TIER_BLOCK_BYTES);
		if (n < 0 || fn(raw, fr.raw_bytes, n, base_offset + off, ctx) != 0) {
			return -1;
		}
		off += spill_frame_bytes(&fr);
	}
	return 0;
}

/**
 * @brief Même parcours que `spill_walk_frames`, sur les `limit` premiers octets
 *        du fichier `path` (< 0 : tout le fichier), lus trame par trame — un
 *        segment entier n'est jamais chargé d'un coup.
 */
static int spill_for_each_frame(const char *path, long limit, spill_frame_fn fn, void *ctx)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return -1;
	}
	if (limit < 0) {
		struct stat st;
		limit = (fstat(fileno(f), &st) == 0) ? (long)st.st_size : -1;
	}
	uint8_t *frame = malloc((size_t)SPILL_FRAME_OVERHEAD + STOCK_TIER_BLOCK_BYTES);
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	int ok = (limit >= 0 && frame != NULL && raw != NULL);
	long off = 0;
	while (ok && off < limit) {
		spill_frame_t fr;
		ok = limit - off >= SPILL_FRAME_OVERHEAD
		     && fread(frame, 1, SPILL_FRAME_HEADER_BYTES, f) == SPILL_FRAME_HEADER_BYTES
		     && spill_frame_parse_header(frame, &fr) == 0 && off + spill_frame_bytes(&fr) <= limit;
		if (ok) {
			size_t rest = (size_t)spill_frame_bytes(&fr) - SPILL_FRAME_HEADER_BYTES;
			ok = fread(frame + SPILL_FRAME_HEADER_BYTES, 1, rest, f) == rest
			     && spill_walk_frames(frame, spill_frame_bytes(&fr), off, raw, fn, ctx) == 0;
			off += spill_frame_bytes(&fr);
		}
	}
	free(frame);
	free(raw);
	fclose(f);
	return ok ? 0 : -1;
}

// Réservée aux tests (jamais appelée en production, non déclarée dans
// stock_spill.h — même convention que
// datamanager_set_ram_limit_packets_for_tests) : force un nombre minuscule de
// possibilités par segment pour exercer le franchissement d'une frontière de
// segment (rollover) sans écrire 131 072 possibilités par test.
void stock_spill_set_segment_records_for_tests(long records)
{
	g_segment_records_override = records;
}

/// Manifeste texte du cliché: en-tête magique + une ligne par
/// (pool, file) débordé au moment du cliché.
/// Manifeste v3 : les segments du cliché sont faits de TRAMES de blocs
/// (`spill_frame_t`). La v2 (forme compacte à pas fixe) et la v1
/// (`possibility_packet` bruts) restent RECONNUES en lecture — restaurées par
/// réempaquetage (cf. `stock_spill_restore_snapshot`), jamais par lien direct :
/// les formats n'ont pas les mêmes octets.
#define STOCK_SPILL_MANIFEST_MAGIC "eternityii-spill-manifest-v3"
#define STOCK_SPILL_MANIFEST_MAGIC_V2 "eternityii-spill-manifest-v2"
#define STOCK_SPILL_MANIFEST_MAGIC_V1 "eternityii-spill-manifest-v1"
#define STOCK_SPILL_MANIFEST_NAME "manifest.txt"

/// Taille des tampons destination construits à partir de `snap_dir` (lui-
/// même déjà `PATH_MAX`, borné par sa propre construction). Nécessaire —
/// pas seulement défensif — pour que gcc/glibc (`_FORTIFY_SOURCE`,
/// `-Wformat-truncation`) puisse prouver statiquement l'absence de
/// troncature possible : gcc propage par inlining la borne connue de
/// `snap_dir` (≤ PATH_MAX-1 après son propre `snprintf`) jusqu'au site
/// d'appel, et le pire cas (chemin + `/spill_%c_%d_%d.dat`, deux `%d` sur
/// des `int` non bornés — jusqu'à 11 chiffres chacun) dépasse alors
/// `PATH_MAX` de quelques dizaines d'octets — un faux positif sur les
/// valeurs réelles (jamais aussi grandes en pratique), mais un vrai calcul
/// de gcc, pas un bug de son analyseur (même piège que documenté dans
/// (même piège documenté pour `http_server.c`).
/// Un tampon destination `g_spill_dir` (chaîne `strdup`, taille inconnue du
/// compilateur) n'a PAS besoin de cette marge : gcc ne peut alors établir
/// aucune borne supérieure finie et ne déclenche pas l'avertissement.
#define SPILL_LOCAL_PATH_MAX (PATH_MAX + 64)

static void spill_segment_path(char *buf, size_t bufsize, int is_checked, int file_index, int seq)
{
	snprintf(buf, bufsize, "%s/spill_%c_%d_%d.dat", g_spill_dir, is_checked ? 'c' : 'u', file_index, seq);
}

/// Variante de `spill_segment_path` pour un répertoire EXPLICITE (snapshot/
/// restauration jonglent avec `g_spill_dir` ET `snap_dir`, deux répertoires
/// distincts). Le motif "buf/bufsize en paramètres de fonction"
/// est délibéré, pas seulement pour la réutilisation : passer par une
/// fonction plutôt qu'un `snprintf` direct dans un tableau local
/// `char[PATH_MAX]` évite les faux positifs `-Wformat-truncation` de gcc
/// (glibc/`_FORTIFY_SOURCE`) — gcc calcule un pire cas précis uniquement
/// quand la taille du tampon est visible statiquement au point d'appel, pas
/// quand elle transite par un paramètre `size_t bufsize`.
static void spill_segment_path_in(char *buf, size_t bufsize, const char *dir, int is_checked, int file_index, int seq)
{
	snprintf(buf, bufsize, "%s/spill_%c_%d_%d.dat", dir, is_checked ? 'c' : 'u', file_index, seq);
}

/// Concatène `dir`/`name` — même motif « buf/bufsize en paramètres » que
/// `spill_segment_path_in`, pour la même raison (éviter les faux positifs
/// `-Wformat-truncation`).
static void spill_join_path(char *buf, size_t bufsize, const char *dir, const char *name)
{
	snprintf(buf, bufsize, "%s/%s", dir, name);
}

/// Chemin `.tmp` associé à un chemin final — même motif que ci-dessus.
static void spill_tmp_path(char *buf, size_t bufsize, const char *final_path)
{
	snprintf(buf, bufsize, "%s.tmp", final_path);
}

static stock_spill_descriptor_t *spill_descriptor(int is_checked, int file_index)
{
	return is_checked ? &g_spill_checked[file_index] : &g_spill_unchecked[file_index];
}

/**
 * @brief Supprime TOUS les fichiers de segment vivants correspondant
 *        exactement au motif `spill_[uc]_<n>_<n>.dat` dans `g_spill_dir` —
 *        jamais un effacement générique du répertoire. Partagée par
 *        `stock_spill_configure` (purge de démarrage) et
 *        `stock_spill_restore_snapshot` (remplacement intégral avant
 *        remise en place du cliché).
 */
static void spill_purge_live_segments(unsigned long long *out_packets, unsigned long long *out_files)
{
	unsigned long long discarded_packets = 0;
	unsigned long long discarded_files = 0;
	DIR *d = opendir(g_spill_dir);
	if (d != NULL) {
		struct dirent *entry;
		while ((entry = readdir(d)) != NULL) {
			char pool_char;
			int file_idx = -1;
			int seq = -1;
			int consumed = 0;
			if (sscanf(entry->d_name, "spill_%c_%d_%d.dat%n", &pool_char, &file_idx, &seq, &consumed) == 3
			    && consumed == (int)strlen(entry->d_name)
			    && (pool_char == 'u' || pool_char == 'c')
			    && file_idx >= 0 && seq >= 1) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path), "%s/%s", g_spill_dir, entry->d_name);
				long size = spill_file_size(path);
				if (size > 0) {
					// Un segment laissé par un binaire antérieur est à pas fixe :
					// son compte n'est alors qu'une estimation.
					unsigned long long records = 0;
					if (spill_scan_segment(path, -1, &records) != 0) {
						records = (unsigned long long)(size / (long)PACKET_CODEC_MAX_BYTES);
					}
					discarded_packets += records;
					discarded_files++;
				}
				unlink(path);
			}
		}
		closedir(d);
	}
	if (out_packets != NULL) { *out_packets = discarded_packets; }
	if (out_files != NULL) { *out_files = discarded_files; }
}

/**
 * @brief Supprime un répertoire de cliché : ses segments, son manifeste (et
 *        un `.tmp` de manifeste), puis le répertoire lui-même. Jamais un
 *        effacement générique : un fichier étranger au cliché y reste, et le
 *        `rmdir` échoue alors sans conséquence.
 */
static void spill_remove_snapshot_dir(const char *snap_dir)
{
	DIR *d = opendir(snap_dir);
	if (d == NULL) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		char pool_char;
		int fidx = -1;
		int seq = -1;
		int consumed = 0;
		int is_segment = sscanf(entry->d_name, "spill_%c_%d_%d.dat%n", &pool_char, &fidx, &seq, &consumed) == 3
		                 && consumed == (int)strlen(entry->d_name);
		if (is_segment || strcmp(entry->d_name, STOCK_SPILL_MANIFEST_NAME) == 0
		    || strcmp(entry->d_name, STOCK_SPILL_MANIFEST_NAME ".tmp") == 0) {
			char path[SPILL_LOCAL_PATH_MAX];
			spill_join_path(path, sizeof(path), snap_dir, entry->d_name);
			unlink(path);
		}
	}
	closedir(d);
	rmdir(snap_dir);
}

/// Purge les clichés TEMPORAIRES de sauvegarde autonome
/// (`CONSISTENT_BACKUP_EMBED_PREFIX`) qu'un arrêt brutal a laissés : leur
/// sauvegarde n'a jamais été publiée, ils ne servent plus à rien.
static void spill_purge_embed_snapshots(void)
{
	DIR *d = opendir(g_spill_dir);
	if (d == NULL) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		if (strncmp(entry->d_name, CONSISTENT_BACKUP_EMBED_PREFIX, strlen(CONSISTENT_BACKUP_EMBED_PREFIX)) == 0) {
			char path[SPILL_LOCAL_PATH_MAX];
			spill_join_path(path, sizeof(path), g_spill_dir, entry->d_name);
			spill_remove_snapshot_dir(path);
		}
	}
	closedir(d);
}

void stock_spill_configure(const char *dir, int nb_files)
{
	free(g_spill_dir);
	g_spill_dir = strdup((dir != NULL) ? dir : STOCK_SPILL_DIR_DEFAULT);
	g_spill_nb_files = nb_files;
	g_spill_enabled = 0;
	g_spill_mode = SPILL_MODE_IDLE;
	g_segment_records_override = 0; // repart de STOCK_SPILL_SEGMENT_RECORDS (production)
	free(g_spill_unchecked);
	g_spill_unchecked = NULL;
	free(g_spill_checked);
	g_spill_checked = NULL;

	// L'étage RAM ne dépend pas du répertoire : il sert sous `--stock-max-ram`
	// même quand le disque est indisponible.
	tier_configure(nb_files);

	if (nb_files <= 0) {
		return;
	}

	if (mkdir(g_spill_dir, 0755) != 0 && errno != EEXIST) {
		log_error("stock_spill_configure : impossible de créer/utiliser le répertoire de "
		          "débordement « %s » (%s) — débordement disque désactivé, seul l'étage RAM "
		          "en blocs reste comme recours sous --stock-max-ram\n",
		          g_spill_dir, strerror(errno));
		return;
	}

	g_spill_unchecked = calloc((size_t)nb_files, sizeof(stock_spill_descriptor_t));
	g_spill_checked = calloc((size_t)nb_files, sizeof(stock_spill_descriptor_t));
	if (g_spill_unchecked == NULL || g_spill_checked == NULL) {
		log_error("stock_spill_configure : allocation échouée pour %d files — débordement désactivé\n", nb_files);
		free(g_spill_unchecked);
		g_spill_unchecked = NULL;
		free(g_spill_checked);
		g_spill_checked = NULL;
		return;
	}

	// Purge des segments résiduels d'un précédent démarrage : ce module lui
	// même n'a aucune conscience de sauvegarde/restauration — c'est
	// `stock_spill_restore_snapshot` (appelée par `restore_apply`,
	// `ui/command_lines.c`) qui remet en place un cliché APRÈS ce point.
	// Tout segment trouvé ici et non suivi d'un `restore` est un débordement
	// que le processus PRÉCÉDENT n'a jamais eu l'occasion de sauvegarder
	// (`backup`) avant de s'arrêter — perte réelle si aucun `restore` ne suit
	// ce démarrage.
	unsigned long long discarded_packets = 0;
	unsigned long long discarded_files = 0;
	spill_purge_live_segments(&discarded_packets, &discarded_files);
	spill_purge_embed_snapshots();
	if (discarded_files > 0) {
		log_error("stock_spill_configure : %llu possibilité(s) dans %llu segment(s) résiduel(s) "
		          "de « %s » supprimées au démarrage — lancer « restore » immédiatement si un "
		          "cliché de débordement existe (commande backup), sinon ces possibilités "
		          "sont perdues\n",
		          discarded_packets, discarded_files, g_spill_dir);
	}

	g_spill_enabled = 1;
}

static int spill_copy_file(const char *src, const char *dst, long max_bytes);

/**
 * @brief Ramène le segment `path` à `tail_bytes` octets physiques avant qu'on
 *        y ajoute, s'il en contient davantage.
 *
 * Le rechargement consomme le sommet en reculant `tail_bytes` SANS toucher au
 * fichier (lecture seule, « peek puis commit »). Sans ce recalage, l'ajout
 * suivant (`fopen("ab")`) écrivait à la fin PHYSIQUE, au-delà du sommet
 * logique : le rechargement d'après relisait des possibilités déjà servies
 * (doublons) et ne voyait jamais les nouvelles (perte).
 *
 * Le fichier peut être lié physiquement à un cliché (`stock_spill_snapshot`
 * lie les segments pleins, et un segment plein redevient sommet quand le
 * rechargement dépile jusqu'à lui) : le tronquer en place amputerait ce
 * cliché. Lié, on en recopie le préfixe dans un NOUVEL inode ; seul, on le
 * tronque — le cas courant, gratuit.
 *
 * @return 0 si le segment est prêt à recevoir l'ajout, -1 sinon (rien écrit).
 */
static int spill_trim_segment_to_tail(const char *path, long tail_bytes)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		return (errno == ENOENT) ? 0 : -1;
	}
	if (st.st_size <= (off_t)tail_bytes) {
		return 0;
	}
	if (st.st_nlink <= 1) {
		return truncate(path, (off_t)tail_bytes);
	}
	char tmp[PATH_MAX + 8];
	snprintf(tmp, sizeof(tmp), "%s.cow", path);
	if (spill_copy_file(path, tmp, tail_bytes) != 0) {
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/**
 * @brief Sous `g_spill_mutex` : reprend sur le disque les compteurs du segment
 *        `last_seq` devenu sommet (le précédent sommet vient d'être vidé).
 *
 * Un segment sous le sommet a été ramené à son sommet logique quand la pile a
 * roulé : sa taille physique fait foi, et ses trames donnent son compte.
 */
static void spill_load_top_locked(int is_checked, int file_index, stock_spill_descriptor_t *desc)
{
	char path[PATH_MAX];
	spill_segment_path(path, sizeof(path), is_checked, file_index, desc->last_seq);
	unsigned long long records = 0;
	long size = spill_file_size(path);
	desc->tail_bytes = (size > 0) ? size : 0;
	if (size < 0 || spill_scan_segment(path, desc->tail_bytes, &records) != 0) {
		log_error("stock_spill : segment « %s » absent ou incohérent en redevenant sommet — son rechargement "
		          "échouera\n", path);
	}
	desc->tail_records = (long)records;
}

/// Sous `g_spill_mutex` : supprime le segment sommet (vidé) et descend d'un cran.
static void spill_drop_top_locked(int is_checked, int file_index, stock_spill_descriptor_t *desc)
{
	char path[PATH_MAX];
	spill_segment_path(path, sizeof(path), is_checked, file_index, desc->last_seq);
	unlink(path);
	desc->last_seq--;
	if (desc->last_seq < desc->first_seq) {
		desc->first_seq = 0;
		desc->last_seq = 0;
		desc->tail_bytes = 0;
		desc->tail_records = 0;
	} else {
		spill_load_top_locked(is_checked, file_index, desc);
	}
}

/**
 * @brief Sous `g_spill_mutex` : prépare le sommet à recevoir une trame de
 *        `records` possibilités — crée la pile, ou roule vers un nouveau
 *        segment si le sommet, non vide, n'en a plus la place.
 *
 * Avant de rouler, le sommet quitté est ramené à son sommet logique : il ne
 * sera plus jamais écrit, et tout ce qui le relira ensuite (cliché, passe
 * d'expansion, retour au sommet) se fie à sa taille physique.
 */
static int spill_prepare_top_locked(int is_checked, int file_index, stock_spill_descriptor_t *desc, long records)
{
	if (desc->last_seq == 0) {
		desc->first_seq = 1;
		desc->last_seq = 1;
		desc->tail_bytes = 0;
		desc->tail_records = 0;
		return 0;
	}
	if (desc->tail_records > 0 && desc->tail_records + records > spill_segment_records()) {
		char path[PATH_MAX];
		spill_segment_path(path, sizeof(path), is_checked, file_index, desc->last_seq);
		if (spill_trim_segment_to_tail(path, desc->tail_bytes) != 0) {
			return -1;
		}
		desc->last_seq++;
		desc->tail_bytes = 0;
		desc->tail_records = 0;
	}
	return 0;
}

/**
 * @brief Sous `g_spill_mutex` : ajoute une trame au sommet de la pile — tout
 *        ou rien.
 *
 * Sur échec, le descripteur est rendu tel qu'il était ; des octets écrits en
 * partie au-delà du sommet logique seront recalés par l'ajout suivant.
 */
static int spill_append_frame_locked(int is_checked, int file_index, const spill_frame_t *fr, const uint8_t *stored)
{
	stock_spill_descriptor_t *desc = spill_descriptor(is_checked, file_index);
	stock_spill_descriptor_t saved = *desc;
	char path[PATH_MAX];
	int ok = (spill_prepare_top_locked(is_checked, file_index, desc, (long)fr->records) == 0);
	if (ok) {
		spill_segment_path(path, sizeof(path), is_checked, file_index, desc->last_seq);
		ok = (spill_trim_segment_to_tail(path, desc->tail_bytes) == 0);
	}
	FILE *f = ok ? fopen(path, "ab") : NULL;
	if (f != NULL) {
		uint8_t h[SPILL_FRAME_HEADER_BYTES], t[SPILL_FRAME_TRAILER_BYTES];
		spill_frame_header(h, fr);
		spill_frame_trailer(t, fr);
		ok = fwrite(h, 1, sizeof h, f) == sizeof h && fwrite(stored, 1, fr->stored_bytes, f) == fr->stored_bytes
		     && fwrite(t, 1, sizeof t, f) == sizeof t && fflush(f) == 0;
		if (ok) {
			fsync(fileno(f));
		}
		if (fclose(f) != 0) {
			ok = 0;
		}
	} else {
		ok = 0;
	}
	if (!ok) {
		if (desc->last_seq != saved.last_seq && desc->last_seq != 0) {
			// Segment ouvert pour cette trame : rien n'y est acquis.
			spill_segment_path(path, sizeof(path), is_checked, file_index, desc->last_seq);
			unlink(path);
		}
		*desc = saved;
		return -1;
	}
	desc->tail_bytes += spill_frame_bytes(fr);
	desc->tail_records += (long)fr->records;
	desc->packets += fr->records;
	return 0;
}

/**
 * @brief Écrit `n` possibilités déjà drainées de la RAM (`buf`) dans la pile
 *        de segments du (pool, file) désigné, en trames d'au plus un bloc
 *        (`STOCK_TIER_BLOCK_BYTES` d'enregistrements compacts, compressés comme
 *        l'étage RAM les compresse), roulant vers un nouveau segment dès que le
 *        courant est plein.
 *
 * @return Nombre effectivement écrit sur disque (< n seulement sur échec
 *         d'E/S ou possibilité non encodable — le reste de `buf` n'a alors pas
 *         été touché par l'appelant).
 */
static int stock_spill_write_block(int is_checked, int file_index, const struct possibility_packet *buf, int n)
{
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	size_t cap = stock_tier_pack_bound(STOCK_TIER_BLOCK_BYTES);
	uint8_t *stored = malloc(cap);
	if (raw == NULL || stored == NULL) {
		free(raw);
		free(stored);
		return 0;
	}

	pthread_mutex_lock(&g_spill_mutex);
	stock_spill_descriptor_t *desc = spill_descriptor(is_checked, file_index);
	int idx = 0;
	int encode_failed = 0;
	while (idx < n) {
		// Au plus ce que le sommet peut encore prendre, pour que la trame ne
		// fasse pas rouler la pile ; un sommet plein roule, et offre alors un
		// segment entier.
		long room = spill_segment_records() - ((desc->last_seq != 0) ? desc->tail_records : 0);
		if (room <= 0) {
			room = spill_segment_records();
		}
		size_t used = 0;
		int count = 0;
		while (idx + count < n && count < room) {
			size_t len = packet_codec_encoded_size(&buf[idx + count]);
			if (used + len > STOCK_TIER_BLOCK_BYTES) {
				break;
			}
			if (packet_codec_encode(&buf[idx + count], raw + used, STOCK_TIER_BLOCK_BYTES - used, NULL) != 0) {
				encode_failed = 1;
				break;
			}
			used += len;
			count++;
		}
		spill_frame_t fr = { STOCK_TIER_CODEC_RAW, 0, (uint32_t)used, 0 };
		int records = 0;
		if (count > 0) {
			fr.stored_bytes = (uint32_t)stock_tier_pack(raw, used, stored, cap, &fr.codec, &records);
		}
		fr.records = (uint32_t)records;
		if (count == 0 || records != count || spill_append_frame_locked(is_checked, file_index, &fr, stored) != 0) {
			break;
		}
		idx += count;
		if (encode_failed) {
			break;
		}
	}
	pthread_mutex_unlock(&g_spill_mutex);
	if (encode_failed) {
		log_error("stock_spill : possibilité non encodable (grille hors domaine) — "
		          "%d possibilité(s) de ce bloc non déportée(s)\n", n - idx);
	}
	free(raw);
	free(stored);
	return idx;
}

/**
 * @brief Évince jusqu'à `max_packets` possibilités depuis la tête (froide)
 *        de la file `file_index` du pool désigné vers son fichier de
 *        segment.
 *
 * En cas d'échec d'écriture, les possibilités déjà drainées de la RAM mais
 * PAS écrites sur disque sont remises en RAM (`datamanager_pool_refill`) —
 * jamais de perte sur erreur, même contrat que le reste de ce module.
 *
 * @return Nombre effectivement évincé (écrit sur disque avec succès).
 */
/// Cumuls depuis le démarrage, pour le point d'avancement de l'expansion : les
/// journaux du débordement ne notent que ses CHANGEMENTS de mode, rien ne
/// disait combien il déplaçait pendant qu'il était actif.
static unsigned long long g_spill_evicted_total = 0;
static unsigned long long g_spill_reloaded_total = 0;

static int stock_spill_evict(int is_checked, int file_index, int max_packets)
{
	if (!g_spill_enabled) {
		return 0;
	}
	struct possibility_packet *buf = malloc((size_t)max_packets * sizeof(struct possibility_packet));
	if (buf == NULL) {
		return 0;
	}
	int n = datamanager_pool_drain_head(is_checked, file_index, buf, max_packets);
	if (n <= 0) {
		free(buf);
		return 0;
	}

	int written = stock_spill_write_block(is_checked, file_index, buf, n);
	if (written < n) {
		// Échec d'écriture à partir de l'indice `written` : ces possibilités
		// n'ont jamais quitté `buf`, jamais été comptées dans le descripteur
		// -- les remettre en RAM immédiatement, aucune perte.
		log_error("stock_spill : échec d'écriture du segment (%s, file %d) — "
		          "%d possibilité(s) remise(s) en RAM sans perte\n",
		          is_checked ? "vérifié" : "non vérifié", file_index, n - written);
		datamanager_pool_refill(is_checked, file_index, &buf[written], n - written);
	}
	free(buf);
	if (written > 0) {
		__atomic_add_fetch(&g_spill_evicted_total, (unsigned long long)written, __ATOMIC_RELAXED);
	}
	return written;
}

/// Récepteur de `spill_walk_frames` : décode les enregistrements d'une trame
/// à la suite de ceux déjà reçus.
typedef struct {
	struct possibility_packet *buf;
	int count;
	int cap;
} spill_decode_ctx_t;

static int spill_decode_frame_into(const uint8_t *raw, size_t raw_bytes, int records, long offset, void *ctx)
{
	(void)offset;
	spill_decode_ctx_t *d = ctx;
	if (d->count + records > d->cap) {
		return -1;
	}
	int n = tier_decode_block(raw, raw_bytes, d->buf + d->count, records);
	if (n != records) {
		return -1;
	}
	d->count += n;
	return 0;
}

/**
 * @brief Recharge des TRAMES entières depuis le sommet de la pile de segments
 *        du (pool, file) désigné vers sa file RAM, tant que `max_packets`
 *        n'est pas atteint — au moins une, même plus grosse que le budget : une
 *        trame n'est jamais entamée.
 *
 * Lecture AVANT retrait (« peek puis commit ») : le segment n'est modifié
 * (sommet logique reculé, ou segment supprimé si vidé) qu'APRÈS confirmation
 * que `datamanager_pool_refill` a bien réinséré les possibilités lues —
 * jamais un octet du disque n'est perdu si le rechargement RAM échouait
 * (théoriquement possible seulement sur OOM de `put()`). Les trames sont
 * remises dans leur ordre d'écriture.
 *
 * @return Nombre effectivement rechargé.
 */
static int stock_spill_reload(int is_checked, int file_index, int max_packets)
{
	if (!g_spill_enabled) {
		return 0;
	}

	pthread_mutex_lock(&g_spill_mutex);
	stock_spill_descriptor_t *desc = spill_descriptor(is_checked, file_index);
	while (desc->last_seq != 0 && desc->tail_bytes == 0) {
		// Sommet vide (écriture ratée après une bascule, ou vidé par une
		// lecture antérieure) : on redescend avant de lire.
		spill_drop_top_locked(is_checked, file_index, desc);
	}
	if (desc->packets == 0 || desc->last_seq == 0) {
		pthread_mutex_unlock(&g_spill_mutex);
		return 0;
	}
	int seq = desc->last_seq;
	long tail = desc->tail_bytes;
	char path[PATH_MAX];
	spill_segment_path(path, sizeof(path), is_checked, file_index, seq);

	// Recul de pied en pied : où commencent les trames à reprendre.
	long from = tail;
	long take = 0;
	int read_ok = 0;
	FILE *f = fopen(path, "rb");
	if (f != NULL) {
		read_ok = 1;
		while (from > 0 && (take == 0 || take < max_packets)) {
			uint8_t t[SPILL_FRAME_TRAILER_BYTES];
			if (from < SPILL_FRAME_OVERHEAD || fseek(f, from - SPILL_FRAME_TRAILER_BYTES, SEEK_SET) != 0
			    || fread(t, 1, sizeof t, f) != sizeof t || memcmp(t + 8, k_frame_tail_magic, 4) != 0) {
				read_ok = 0;
				break;
			}
			long records = (long)spill_get_u32(t);
			long start = from - SPILL_FRAME_OVERHEAD - (long)spill_get_u32(t + 4);
			if (start < 0 || records <= 0) {
				read_ok = 0;
				break;
			}
			if (take > 0 && take + records > max_packets) {
				break;
			}
			take += records;
			from = start;
		}
	}
	uint8_t *bytes = read_ok ? malloc((size_t)(tail - from)) : NULL;
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	spill_decode_ctx_t dec = { read_ok ? malloc((size_t)take * sizeof(struct possibility_packet)) : NULL, 0, (int)take };
	read_ok = read_ok && bytes != NULL && raw != NULL && dec.buf != NULL && fseek(f, from, SEEK_SET) == 0
	          && fread(bytes, 1, (size_t)(tail - from), f) == (size_t)(tail - from)
	          && spill_walk_frames(bytes, tail - from, from, raw, spill_decode_frame_into, &dec) == 0
	          && dec.count == take;
	if (f != NULL) {
		fclose(f);
	}
	free(bytes);
	free(raw);
	// Rien n'a encore été modifié sur disque (lecture seule ci-dessus) : sûr
	// de déverrouiller avant le rechargement RAM, qui n'a pas besoin de ce
	// verrou (protège seulement les descripteurs/fichiers de débordement).
	pthread_mutex_unlock(&g_spill_mutex);

	struct possibility_packet *buf = dec.buf;
	if (!read_ok) {
		log_error("stock_spill : échec de lecture du segment « %s » — rechargement reporté au tick suivant\n", path);
		free(buf);
		return 0;
	}

	// Migration transparente (cf. docs/autosearch_step.md) : un segment de
	// débordement écrit avant VERSION 13 portait `alloc` au sens curseur, pas
	// au sens nombre de pièces posées. Le décodage compact le reconstruit déjà
	// depuis la grille ; `min_candidats` (score MRV), lui, n'est pas dérivable
	// de la grille : écrasé par la sentinelle « inconnu » plutôt que recompté,
	// même logique qu'à l'import.
	for (int i = 0; i < (int)take; i++) {
		buf[i].alloc = (uint16_t)possibility_placed_count(&buf[i]);
		buf[i].min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
	}

	datamanager_pool_refill(is_checked, file_index, buf, (int)take);
	free(buf);

	// Commit : maintenant que les possibilités sont confirmées en RAM, on
	// peut reculer le sommet logique (ou supprimer le segment) sans risque.
	pthread_mutex_lock(&g_spill_mutex);
	desc->tail_bytes = from;
	desc->tail_records = (desc->tail_records > take) ? desc->tail_records - take : 0;
	desc->packets -= (unsigned long long)take;
	if (from == 0) {
		spill_drop_top_locked(is_checked, file_index, desc);
	}
	pthread_mutex_unlock(&g_spill_mutex);
	__atomic_add_fetch(&g_spill_reloaded_total, (unsigned long long)take, __ATOMIC_RELAXED);
	return (int)take;
}

// ---------------------------------------------------------------------
// Étage RAM en blocs (core/stock_tier.h) : entre la liste chaînée des pools
// et le disque. docs/conception/etage_ram_compresse.md.
//
// Chaîne stricte : liste -> étage (tête froide de la liste, empilée au
// sommet de l'étage), étage -> disque (bas de l'étage, le plus ancien),
// étage -> liste (sommet de l'étage, le plus récent). Le disque ne recharge
// directement dans la liste que si l'étage est vide : tout ce qui est sur
// disque est plus ancien que tout ce qui est dans l'étage, l'ordre de pile du
// stock est donc conservé.
//
// Verrou : `g_tier_mutex`, pris AVANT `g_spill_mutex` et avant tout essai de
// verrou de pool (jamais d'attente sur un verrou de pool sous lui : une
// sauvegarde gèle les pools puis prend ce verrou). Chaque mouvement d'un bloc
// se fait entièrement sous lui : une sauvegarde qui fige l'étage voit donc un
// bloc soit d'un côté, soit de l'autre, jamais entre deux.
// ---------------------------------------------------------------------

static pthread_mutex_t g_tier_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_tier_nb_files = 0;
static stock_tier_stack_t *g_tier_unchecked = NULL; // [g_tier_nb_files]
static stock_tier_stack_t *g_tier_checked = NULL;   // [g_tier_nb_files]
/// Désactivable pour les seuls tests qui vérifient le débordement disque
/// historique (liste -> disque sans étage).
static int g_tier_enabled = 1;
static int g_hot_floor_pct = STOCK_TIER_HOT_FLOOR_DEFAULT;
static int g_hot_reload_pct = STOCK_TIER_HOT_RELOAD_DEFAULT;

/// Frontière d'une passe d'expansion dans chaque pile NON vérifiée : le
/// numéro du bloc au sommet au début de la passe (0 si vide). Un bloc de
/// numéro <= frontière est antérieur à la passe, donc à développer.
static int g_tier_expand_active = 0;
static unsigned long long *g_tier_expand_boundary = NULL;

/// Possibilités dans l'étage, tenu à chaque bloc empilé/retiré : lu SANS le
/// verrou de l'étage, qu'une sauvegarde garde pendant toute sa recopie — la
/// boucle `check`, `stockMemory` et `GET /api/v1/stats` ne doivent pas
/// l'attendre.
static unsigned long long g_tier_records = 0;

static unsigned long long g_tier_evicted_total = 0;
static unsigned long long g_tier_reloaded_total = 0;

int stock_spill_configure_tier(int hot_floor_pct, int hot_reload_pct)
{
	if (hot_reload_pct < 1 || hot_floor_pct > 100 || hot_reload_pct >= hot_floor_pct) {
		g_hot_floor_pct = STOCK_TIER_HOT_FLOOR_DEFAULT;
		g_hot_reload_pct = STOCK_TIER_HOT_RELOAD_DEFAULT;
		return -1;
	}
	g_hot_floor_pct = hot_floor_pct;
	g_hot_reload_pct = hot_reload_pct;
	return 0;
}

void stock_spill_set_tier_enabled_for_tests(int enabled)
{
	g_tier_enabled = enabled;
}

static int tier_active(void)
{
	return g_tier_enabled && g_tier_unchecked != NULL;
}

static stock_tier_stack_t *tier_stack(int is_checked, int file_index)
{
	return is_checked ? &g_tier_checked[file_index] : &g_tier_unchecked[file_index];
}

/// Sous `g_tier_mutex` : empile, en tenant le compte d'octets du datamanager.
static int tier_push_locked(stock_tier_stack_t *stack, const uint8_t *raw, size_t raw_bytes)
{
	unsigned long long before = stack->bytes;
	int n = stock_tier_push(stack, raw, raw_bytes);
	if (n > 0) {
		datamanager_ram_tier_bytes_add((long long)(stack->bytes - before));
		__atomic_add_fetch(&g_tier_records, (unsigned long long)n, __ATOMIC_RELAXED);
	}
	return n;
}

/// Sous `g_tier_mutex` : retire `block`, en tenant le compte d'octets.
static void tier_remove_locked(stock_tier_stack_t *stack, const stock_tier_block_t *block)
{
	unsigned long long before = stack->bytes;
	__atomic_sub_fetch(&g_tier_records, (unsigned long long)stock_tier_block_records(block), __ATOMIC_RELAXED);
	stock_tier_remove(stack, block);
	datamanager_ram_tier_bytes_add(-(long long)(before - stack->bytes));
}

/// Sous `g_tier_mutex` : vide toutes les piles.
static void tier_clear_all_locked(void)
{
	for (int f = 0; f < g_tier_nb_files; f++) {
		stock_tier_stack_t *stacks[2] = { &g_tier_unchecked[f], &g_tier_checked[f] };
		for (int k = 0; k < 2; k++) {
			datamanager_ram_tier_bytes_add(-(long long)stacks[k]->bytes);
			__atomic_sub_fetch(&g_tier_records, stacks[k]->records, __ATOMIC_RELAXED);
			stock_tier_stack_clear(stacks[k]);
		}
	}
}

/// (Ré)alloue les piles pour `nb_files` files ; ce qu'elles tenaient est perdu
/// (reconfiguration : démarrage ou tests).
static void tier_configure(int nb_files)
{
	pthread_mutex_lock(&g_tier_mutex);
	if (g_tier_unchecked != NULL) {
		tier_clear_all_locked();
	}
	free(g_tier_unchecked);
	free(g_tier_checked);
	free(g_tier_expand_boundary);
	g_tier_unchecked = NULL;
	g_tier_checked = NULL;
	g_tier_expand_boundary = NULL;
	g_tier_expand_active = 0;
	g_tier_nb_files = 0;
	if (nb_files > 0) {
		g_tier_unchecked = calloc((size_t)nb_files, sizeof *g_tier_unchecked);
		g_tier_checked = calloc((size_t)nb_files, sizeof *g_tier_checked);
		if (g_tier_unchecked == NULL || g_tier_checked == NULL) {
			log_error("stock_spill_configure : allocation échouée pour l'étage RAM de %d files — "
			          "étage désactivé\n", nb_files);
			free(g_tier_unchecked);
			free(g_tier_checked);
			g_tier_unchecked = NULL;
			g_tier_checked = NULL;
		} else {
			g_tier_nb_files = nb_files;
			for (int f = 0; f < nb_files; f++) {
				stock_tier_stack_init(&g_tier_unchecked[f]);
				stock_tier_stack_init(&g_tier_checked[f]);
			}
		}
	}
	pthread_mutex_unlock(&g_tier_mutex);
}

/**
 * @brief Évince la tête (froide) de la file `file_index` vers le sommet de sa
 *        pile d'étage, par blocs, jusqu'à `max_packets` possibilités.
 *
 * Sur échec d'empilement (allocation), le bloc drainé est remis au bout chaud
 * de la file : jamais de perte.
 */
static int tier_evict(int is_checked, int file_index, int max_packets)
{
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	if (raw == NULL) {
		return 0;
	}
	int moved = 0;
	pthread_mutex_lock(&g_tier_mutex);
	stock_tier_stack_t *stack = tier_stack(is_checked, file_index);
	while (moved < max_packets) {
		size_t used = 0;
		int n = datamanager_pool_drain_head_compact(is_checked, file_index, raw, STOCK_TIER_BLOCK_BYTES,
		                                            max_packets - moved, &used);
		if (n <= 0) {
			break;
		}
		if (tier_push_locked(stack, raw, used) != n) {
			pthread_mutex_unlock(&g_tier_mutex);
			struct possibility_packet *buf = malloc((size_t)n * sizeof *buf);
			int decoded = (buf != NULL) ? tier_decode_block(raw, used, buf, n) : -1;
			if (decoded == n) {
				datamanager_pool_refill(is_checked, file_index, buf, n);
				log_error("stock_spill : bloc de l'étage RAM non alloué (%s, file %d) — "
				          "%d possibilité(s) remise(s) en file sans perte\n",
				          is_checked ? "vérifié" : "non vérifié", file_index, n);
			} else {
				log_error("stock_spill : bloc de l'étage RAM non alloué ET non relisible (%s, file %d) — "
				          "%d possibilité(s) PERDUE(S)\n",
				          is_checked ? "vérifié" : "non vérifié", file_index, n);
			}
			free(buf);
			free(raw);
			__atomic_add_fetch(&g_tier_evicted_total, (unsigned long long)moved, __ATOMIC_RELAXED);
			return moved;
		}
		moved += n;
	}
	pthread_mutex_unlock(&g_tier_mutex);
	free(raw);
	__atomic_add_fetch(&g_tier_evicted_total, (unsigned long long)moved, __ATOMIC_RELAXED);
	return moved;
}

/**
 * @brief Remonte le bloc du SOMMET de la pile d'étage dans sa file, tant que
 *        `max_packets` n'est pas atteint. S'arrête sans rien perdre si le
 *        verrou de la file est pris (retenté au tick suivant).
 */
static int tier_reload(int is_checked, int file_index, int max_packets, unsigned long long stop_bytes)
{
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	if (raw == NULL) {
		return 0;
	}
	int moved = 0;
	pthread_mutex_lock(&g_tier_mutex);
	stock_tier_stack_t *stack = tier_stack(is_checked, file_index);
	while (moved < max_packets && datamanager_pools_resident_bytes() < stop_bytes) {
		const stock_tier_block_t *top = stock_tier_top(stack);
		if (top == NULL) {
			break;
		}
		int n = stock_tier_block_unpack(top, raw, STOCK_TIER_BLOCK_BYTES);
		if (n < 0) {
			log_error("stock_spill : bloc illisible au sommet de l'étage RAM (%s, file %d) — laissé en place\n",
			          is_checked ? "vérifié" : "non vérifié", file_index);
			break;
		}
		int r = datamanager_pool_refill_compact(is_checked, file_index, raw, stock_tier_block_raw_bytes(top));
		if (r != n) {
			break; // verrou pris ou allocation refusée : rien n'a bougé
		}
		tier_remove_locked(stack, top);
		moved += n;
	}
	pthread_mutex_unlock(&g_tier_mutex);
	free(raw);
	__atomic_add_fetch(&g_tier_reloaded_total, (unsigned long long)moved, __ATOMIC_RELAXED);
	return moved;
}

/// Sous `g_tier_mutex` : le bloc d'une pile qui peut partir sur disque — le
/// plus ancien, sauf pendant une passe d'expansion pour une pile non
/// vérifiée : le plus ancien écrit DEPUIS le début de la passe. Ceux d'avant
/// doivent rester jusqu'à ce que la passe les lise ; sur disque, ils
/// atterriraient au-dessus de la frontière du disque et ne seraient jamais
/// développés par cette passe.
static const stock_tier_block_t *tier_disk_candidate(int is_checked, int file_index)
{
	const stock_tier_stack_t *stack = tier_stack(is_checked, file_index);
	const stock_tier_block_t *b = stock_tier_bottom(stack);
	if (is_checked || !g_tier_expand_active) {
		return b;
	}
	while (b != NULL && stock_tier_block_seq(b) <= g_tier_expand_boundary[file_index]) {
		b = stock_tier_block_above(b);
	}
	return b;
}

/**
 * @brief Transfère vers le disque des blocs de la pile d'étage désignée
 *        (cf. `tier_disk_candidate`), jusqu'à `max_packets` possibilités.
 *
 * Le bloc part TEL QUEL : ses octets stockés (compressés sous `make ZSTD=1`)
 * deviennent la charge d'une trame, sans décodage ni recompression. Il n'est
 * retiré de l'étage qu'une fois sa trame écrite en entier ; une trame ratée
 * n'est pas acquise (`spill_append_frame_locked`) et le bloc reste en place.
 */
static int tier_to_disk(int is_checked, int file_index, int max_packets)
{
	int moved = 0;
	pthread_mutex_lock(&g_tier_mutex);
	stock_tier_stack_t *stack = tier_stack(is_checked, file_index);
	while (moved < max_packets) {
		const stock_tier_block_t *b = tier_disk_candidate(is_checked, file_index);
		if (b == NULL) {
			break;
		}
		spill_frame_t fr = { stock_tier_block_codec(b), stock_tier_block_records(b),
		                     (uint32_t)stock_tier_block_raw_bytes(b), (uint32_t)stock_tier_block_stored_bytes(b) };
		pthread_mutex_lock(&g_spill_mutex);
		int rc = spill_append_frame_locked(is_checked, file_index, &fr, stock_tier_block_data(b));
		pthread_mutex_unlock(&g_spill_mutex);
		if (rc != 0) {
			log_error("stock_spill : échec d'écriture du segment (%s, file %d) depuis l'étage RAM — "
			          "%u possibilité(s) gardée(s) en RAM\n",
			          is_checked ? "vérifié" : "non vérifié", file_index, fr.records);
			break;
		}
		tier_remove_locked(stack, b);
		moved += (int)fr.records;
	}
	pthread_mutex_unlock(&g_tier_mutex);
	if (moved > 0) {
		__atomic_add_fetch(&g_spill_evicted_total, (unsigned long long)moved, __ATOMIC_RELAXED);
	}
	return moved;
}

unsigned long long stock_spill_tier_packets(void)
{
	return __atomic_load_n(&g_tier_records, __ATOMIC_RELAXED);
}

unsigned long long stock_spill_tier_bytes(void)
{
	return datamanager_ram_tier_bytes();
}

/// Évince vers l'étage depuis la file RÉSIDENTE la plus pleine.
static int tier_evict_fullest(int max_packets)
{
	int best_pool = -1, best_file = -1;
	unsigned long long best_size = 0;
	for (int f = 0; f < g_tier_nb_files; f++) {
		unsigned long long u = file_size(f);
		if (u > best_size) {
			best_size = u;
			best_pool = STOCK_SPILL_POOL_UNCHECKED;
			best_file = f;
		}
		unsigned long long c = file_checked_size(f);
		if (c > best_size) {
			best_size = c;
			best_pool = STOCK_SPILL_POOL_CHECKED;
			best_file = f;
		}
	}
	return (best_file < 0) ? 0 : tier_evict(best_pool, best_file, max_packets);
}

/// Pile d'étage la plus chargée (en possibilités si `by_records`, sinon en
/// octets) ; pour le disque, seulement une pile qui a un bloc transférable.
static int tier_pick_stack(int for_disk, int *out_pool, int *out_file)
{
	unsigned long long best = 0;
	*out_file = -1;
	pthread_mutex_lock(&g_tier_mutex);
	for (int f = 0; f < g_tier_nb_files; f++) {
		for (int pool = 0; pool < 2; pool++) {
			const stock_tier_stack_t *s = tier_stack(pool, f);
			unsigned long long v = for_disk ? s->bytes : s->records;
			if (v > best && (!for_disk || tier_disk_candidate(pool, f) != NULL)) {
				best = v;
				*out_pool = pool;
				*out_file = f;
			}
		}
	}
	pthread_mutex_unlock(&g_tier_mutex);
	return *out_file >= 0;
}

static int tier_reload_fullest(int max_packets, unsigned long long stop_bytes)
{
	int pool = 0, file = -1;
	return tier_pick_stack(0, &pool, &file) ? tier_reload(pool, file, max_packets, stop_bytes) : 0;
}

static int tier_to_disk_fullest(int max_packets)
{
	int pool = 0, file = -1;
	return tier_pick_stack(1, &pool, &file) ? tier_to_disk(pool, file, max_packets) : 0;
}

/**
 * @brief Éviction avec étage : la liste descend vers l'étage tant qu'elle
 *        dépasse son plancher (`--stock-hot-floor`), puis c'est le bas de
 *        l'étage qui part sur disque. Sans disque (ou sans bloc transférable),
 *        la liste continue de descendre vers l'étage sous son plancher : les
 *        blocs restent plus denses que les maillons.
 */
/// Possibilités à évincer pour ramener la liste (`hot` octets) à `floor_bytes`,
/// au tarif moyen observé de ses possibilités, bornées par `budget`.
static int tier_need_to_floor(unsigned long long hot, unsigned long long floor_bytes, int budget)
{
	if (hot <= floor_bytes) {
		return 0;
	}
	unsigned long long packets = datamanager_resident_packets();
	unsigned long long per = (packets > 0) ? hot / packets : 1;
	if (per == 0) {
		per = 1;
	}
	unsigned long long need = (hot - floor_bytes + per - 1) / per;
	return (need < (unsigned long long)budget) ? (int)need : budget;
}

/**
 * @brief Compression PROACTIVE : ramène la liste chaude à son plancher
 *        (`--stock-hot-floor`) en rangeant sa tête froide dans l'étage, SANS
 *        attendre le seuil haut du plafond.
 *
 * Avant, l'étage n'agissait qu'au-dessus de 90 % du plafond, et s'arrêtait à
 * 75 % : sous un plafond de 42 Go, l'occupation se stabilisait vers 32 Go,
 * presque tout en liste chaînée (≈ 147 octets par possibilité), l'étage
 * n'ayant reçu que de quoi redescendre sous 75 %. La liste est désormais
 * tenue entre ses deux seuils (`--stock-hot-reload`, `--stock-hot-floor`) en
 * permanence ; le seuil haut ne sert plus qu'au disque. Jamais le disque ici :
 * le plafond n'est pas en jeu.
 */
static int tier_evict_to_floor(int max_packets, unsigned long long floor_bytes)
{
	int moved = 0;
	while (moved < max_packets) {
		int chunk = tier_need_to_floor(datamanager_pools_resident_bytes(), floor_bytes, max_packets - moved);
		int m = (chunk > 0) ? tier_evict_fullest(chunk) : 0;
		if (m <= 0) {
			break;
		}
		moved += m;
	}
	return moved;
}

static int tier_evict_step(int max_packets, unsigned long long cap)
{
	unsigned long long floor_bytes = cap * (unsigned long long)g_hot_floor_pct / 100;
	unsigned long long low = cap * STOCK_SPILL_LOW_PERCENT / 100;
	int moved = 0;
	while (moved < max_packets && datamanager_resident_bytes() > low) {
		int m = 0;
		unsigned long long hot = datamanager_pools_resident_bytes();
		if (hot > floor_bytes) {
			// Juste de quoi ramener la liste à son plancher — pas tout le
			// budget d'un coup.
			m = tier_evict_fullest(tier_need_to_floor(hot, floor_bytes, max_packets - moved));
		} else if (!g_spill_enabled) {
			m = tier_evict_fullest(max_packets - moved);
		} else {
			m = tier_to_disk_fullest(max_packets - moved);
			if (m == 0) {
				m = tier_evict_fullest(max_packets - moved);
			}
		}
		if (m <= 0) {
			break;
		}
		moved += m;
	}
	return moved;
}

// Rendre au système la mémoire que l'éviction libère (glibc).
//
// Les maillons évincés retournent dans le tas du processus, pas au système : le
// RSS restait à son plus haut pendant que `stockMemory` baissait. `malloc_trim`
// rend les pages entièrement libres — et l'éviction libère la TÊTE froide des
// files, allouée d'un seul tenant, donc des pages entières : mesuré sur 30 M
// maillons libérés par la tête, 4,5 → 1,4 Go de RSS en 0,7 s (contre 5,2 s pour
// une libération dispersée). Mais il tient la seule arène malloc du serveur
// (`server_cap_malloc_arenas`) pendant tout son parcours : appelé par petits
// lots (`STOCK_TIER_TRIM_BYTES`), jamais plus d'une fois par
// `STOCK_TIER_TRIM_MIN_INTERVAL_SEC`, hors de tout verrou, et journalisé avec
// sa durée pour que ce coût reste visible.

static unsigned long long g_trim_pending = 0;
static time_t g_trim_last = 0;
static void (*g_trim_fn)(void) = NULL; // NULL : malloc_trim(0) (glibc)
static unsigned long long g_trim_bytes = STOCK_TIER_TRIM_BYTES;

int stock_spill_should_trim(unsigned long long pending_bytes, time_t now, time_t last,
                            unsigned long long threshold_bytes)
{
	return pending_bytes >= threshold_bytes && (last == 0 || now - last >= STOCK_TIER_TRIM_MIN_INTERVAL_SEC);
}

// Réservée aux tests : remplace l'appel à malloc_trim (NULL rétablit) et le
// seuil d'octets libérés (0 rétablit STOCK_TIER_TRIM_BYTES).
void stock_spill_set_trim_for_tests(void (*fn)(void), unsigned long long threshold_bytes)
{
	g_trim_fn = fn;
	g_trim_bytes = threshold_bytes ? threshold_bytes : STOCK_TIER_TRIM_BYTES;
	g_trim_pending = 0;
	g_trim_last = 0;
}

static long rss_mb(void)
{
	long pages = 0, resident = 0;
	FILE *f = fopen("/proc/self/statm", "r");
	if (f == NULL) {
		return -1;
	}
	if (fscanf(f, "%ld %ld", &pages, &resident) != 2) {
		resident = -1;
	}
	fclose(f);
	return (resident < 0) ? -1 : resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}

/// Compte des octets de liste libérés par une éviction ; rend la mémoire au
/// système quand il y en a assez. Appelée par le pas du débordement, hors verrou.
/// Une éviction qui ne déplace plus rien (`freed == 0`, la liste a rejoint son
/// plancher) rend aussi le reliquat, dès `STOCK_TIER_TRIM_BYTES / 8` : sinon
/// les dernières centaines de Mo libérées restaient au processus jusqu'à la
/// prochaine vague d'éviction — mesuré, ~400 Mo sur un stock de 30 M.
static void tier_note_freed(unsigned long long freed)
{
	g_trim_pending += freed;
	time_t now = time(NULL);
	unsigned long long threshold = (freed == 0) ? g_trim_bytes / 8 : g_trim_bytes;
	if (!stock_spill_should_trim(g_trim_pending, now, g_trim_last, threshold)) {
		return;
	}
	unsigned long long pending = g_trim_pending;
	g_trim_pending = 0;
	g_trim_last = now;
	if (g_trim_fn != NULL) {
		g_trim_fn();
		return;
	}
#if defined(__GLIBC__)
	long before = rss_mb();
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	malloc_trim(0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long after = rss_mb();
	log_event("stock_spill : malloc_trim après %llu Mo évincés vers l'étage RAM — RSS %ld -> %ld Mo en %ld ms\n",
	          pending / (1024ULL * 1024ULL), before, after,
	          (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
#else
	(void)pending;
	(void)rss_mb;
#endif
}

// Crochets `datamanager_ram_tier_hooks_t`.

static void tier_hook_discard(void)
{
	pthread_mutex_lock(&g_tier_mutex);
	unsigned long long dropped = 0;
	for (int f = 0; f < g_tier_nb_files; f++) {
		dropped += g_tier_unchecked[f].records + g_tier_checked[f].records;
	}
	tier_clear_all_locked();
	pthread_mutex_unlock(&g_tier_mutex);
	if (dropped > 0) {
		log_event("stock_spill : %llu possibilité(s) de l'étage RAM remplacées par la sauvegarde restaurée\n",
		          dropped);
	}
}

static void tier_hook_freeze(void)
{
	pthread_mutex_lock(&g_tier_mutex);
}

static void tier_hook_thaw(void)
{
	pthread_mutex_unlock(&g_tier_mutex);
}

/// Sous `tier_hook_freeze` : les octets bruts d'un bloc SONT des
/// enregistrements de `.back`, recopiés tels quels.
static int tier_hook_write(FILE *out, unsigned long long *out_written)
{
	*out_written = 0;
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	if (raw == NULL) {
		return -1;
	}
	int rc = 0;
	for (int f = 0; rc == 0 && f < g_tier_nb_files; f++) {
		for (int pool = 0; rc == 0 && pool < 2; pool++) {
			const stock_tier_stack_t *s = tier_stack(pool, f);
			for (const stock_tier_block_t *b = stock_tier_bottom(s); rc == 0 && b != NULL;
			     b = stock_tier_block_above(b)) {
				int n = stock_tier_block_unpack(b, raw, STOCK_TIER_BLOCK_BYTES);
				size_t bytes = stock_tier_block_raw_bytes(b);
				if (n < 0 || fwrite(raw, 1, bytes, out) != bytes) {
					rc = -1;
				} else {
					*out_written += (unsigned long long)n;
				}
			}
		}
	}
	free(raw);
	return rc;
}

static const datamanager_ram_tier_hooks_t g_tier_hooks = {
	tier_hook_discard, tier_hook_freeze, tier_hook_write, tier_hook_thaw
};

const datamanager_ram_tier_hooks_t *stock_spill_ram_tier_hooks(void)
{
	return &g_tier_hooks;
}

// ---------------------------------------------------------------------
// Consommation du débordement par une passe d'expansion
// (`datamanager_set_expansion_disk_source`, core/datamanager.h).
// ---------------------------------------------------------------------

/// Sommet de chaque pile NON vérifiée au début de la passe en cours, et ce
/// qu'il contenait alors : tout segment strictement dessous est antérieur à la
/// passe, et ne sera plus écrit (l'éviction n'empile qu'au sommet).
static int g_expand_active = 0;
static int *g_expand_boundary_seq = NULL;
static long *g_expand_boundary_tail = NULL;

void stock_spill_expansion_begin(void)
{
	pthread_mutex_lock(&g_tier_mutex);
	free(g_tier_expand_boundary);
	g_tier_expand_boundary = NULL;
	g_tier_expand_active = 0;
	if (g_tier_nb_files > 0) {
		g_tier_expand_boundary = calloc((size_t)g_tier_nb_files, sizeof *g_tier_expand_boundary);
		g_tier_expand_active = (g_tier_expand_boundary != NULL);
		for (int f = 0; g_tier_expand_active && f < g_tier_nb_files; f++) {
			const stock_tier_block_t *top = stock_tier_top(&g_tier_unchecked[f]);
			g_tier_expand_boundary[f] = (top != NULL) ? stock_tier_block_seq(top) : 0;
		}
	}
	pthread_mutex_unlock(&g_tier_mutex);

	if (!g_spill_enabled) {
		return;
	}
	pthread_mutex_lock(&g_spill_mutex);
	free(g_expand_boundary_seq);
	free(g_expand_boundary_tail);
	g_expand_boundary_seq = calloc((size_t)g_spill_nb_files, sizeof *g_expand_boundary_seq);
	g_expand_boundary_tail = calloc((size_t)g_spill_nb_files, sizeof *g_expand_boundary_tail);
	g_expand_active = (g_expand_boundary_seq != NULL && g_expand_boundary_tail != NULL);
	for (int f = 0; g_expand_active && f < g_spill_nb_files; f++) {
		g_expand_boundary_seq[f] = g_spill_unchecked[f].last_seq;
		g_expand_boundary_tail[f] = g_spill_unchecked[f].tail_bytes;
	}
	pthread_mutex_unlock(&g_spill_mutex);
}

void stock_spill_expansion_stats(datamanager_spill_stats_t *out)
{
	out->spilled = stock_spill_total_packets();
	out->tier = stock_spill_tier_packets();
	out->evicted_total = __atomic_load_n(&g_spill_evicted_total, __ATOMIC_RELAXED);
	out->reloaded_total = __atomic_load_n(&g_spill_reloaded_total, __ATOMIC_RELAXED);
}

void stock_spill_expansion_end(void)
{
	pthread_mutex_lock(&g_tier_mutex);
	g_tier_expand_active = 0;
	free(g_tier_expand_boundary);
	g_tier_expand_boundary = NULL;
	pthread_mutex_unlock(&g_tier_mutex);

	pthread_mutex_lock(&g_spill_mutex);
	g_expand_active = 0;
	free(g_expand_boundary_seq);
	free(g_expand_boundary_tail);
	g_expand_boundary_seq = NULL;
	g_expand_boundary_tail = NULL;
	pthread_mutex_unlock(&g_spill_mutex);
}

/**
 * @brief Sous `g_spill_mutex` : le prochain segment qu'une passe peut
 *        consommer, toujours le BAS d'une pile non vérifiée.
 *
 * Sous la frontière, un segment est antérieur à la passe et immuable : tout y
 * est à développer. Le segment de frontière lui-même (le sommet au début de la
 * passe) contient l'ancien stock jusqu'à `g_expand_boundary_tail` — une fin de
 * trame —, puis les enfants que la passe y a ajoutés : il est lu en entier,
 * l'avant développé, l'après réinjecté tel quel. Sans lui, le dernier segment
 * de chaque pile ne serait jamais développé tant que la passe continue d'y
 * évincer.
 *
 * @param top       1 si le segment choisi est encore le sommet (mutable :
 *                  l'appelant garde le verrou jusqu'au commit).
 * @param old_bytes Octets à développer (les premières trames du segment).
 * @return 1 si trouvé, 0 sinon.
 */
static int spill_expansion_pick(int *out_file, int *out_seq, long *out_bytes, int *top, long *old_bytes)
{
	for (int f = 0; f < g_spill_nb_files; f++) {
		const stock_spill_descriptor_t *desc = &g_spill_unchecked[f];
		int boundary = g_expand_boundary_seq[f];
		if (desc->last_seq == 0 || boundary == 0 || desc->first_seq > boundary) {
			continue;
		}
		*out_file = f;
		*out_seq = desc->first_seq;
		*top = (desc->first_seq == desc->last_seq);
		if (*top) {
			*out_bytes = desc->tail_bytes;
		} else {
			char path[PATH_MAX];
			spill_segment_path(path, sizeof(path), 0, f, desc->first_seq);
			*out_bytes = spill_file_size(path);
		}
		*old_bytes = (desc->first_seq < boundary) ? *out_bytes : g_expand_boundary_tail[f];
		return 1;
	}
	return 0;
}

static int stock_spill_disk_expansion_take(datamanager_expansion_sink_fn sink, void *ctx, unsigned long long max_records);

/**
 * @brief Pendant de `stock_spill_expansion_take` pour l'étage RAM : le bloc du
 *        BAS d'une pile non vérifiée, s'il est antérieur à la passe.
 *
 * Les blocs d'avant la passe forment toujours le bas des piles : l'éviction
 * empile au sommet, le rechargement est suspendu pendant une expansion, et le
 * transfert vers le disque saute ces blocs (`tier_disk_candidate`). Le bloc est
 * relu sous le verrou, livré HORS verrou (le récepteur peut réveiller le
 * dégagement, qui prend ce même verrou), puis retiré s'il est toujours le bas
 * de sa pile — ce que rien d'autre ne peut changer pendant la passe.
 */
static int tier_expansion_take(datamanager_expansion_sink_fn sink, void *ctx, unsigned long long max_records)
{
	uint8_t *raw = malloc(STOCK_TIER_BLOCK_BYTES);
	struct possibility_packet *buf = malloc((size_t)STOCK_TIER_MAX_RECORDS * sizeof *buf);
	if (raw == NULL || buf == NULL) {
		free(raw);
		free(buf);
		return -1;
	}
	pthread_mutex_lock(&g_tier_mutex);
	int file_index = -1;
	const stock_tier_block_t *b = NULL;
	for (int f = 0; g_tier_expand_active && f < g_tier_nb_files; f++) {
		const stock_tier_block_t *bottom = stock_tier_bottom(&g_tier_unchecked[f]);
		if (bottom != NULL && stock_tier_block_seq(bottom) <= g_tier_expand_boundary[f]) {
			file_index = f;
			b = bottom;
			break;
		}
	}
	if (b == NULL) {
		pthread_mutex_unlock(&g_tier_mutex);
		free(raw);
		free(buf);
		return 0;
	}
	if ((unsigned long long)stock_tier_block_records(b) > max_records) {
		pthread_mutex_unlock(&g_tier_mutex);
		free(raw);
		free(buf);
		return DATAMANAGER_DISK_TAKE_NO_ROOM;
	}
	unsigned long long seq = stock_tier_block_seq(b);
	size_t raw_bytes = stock_tier_block_raw_bytes(b);
	int n = stock_tier_block_unpack(b, raw, STOCK_TIER_BLOCK_BYTES);
	pthread_mutex_unlock(&g_tier_mutex);

	int ok = (n > 0 && tier_decode_block(raw, raw_bytes, buf, STOCK_TIER_MAX_RECORDS) == n);
	for (int i = 0; ok && i < n; i++) {
		ok = sink(&buf[i], 1, ctx);
	}
	free(raw);
	free(buf);
	if (!ok) {
		log_error("stock_spill : bloc de l'étage RAM (file %d) illisible ou refusé par l'expansion — "
		          "laissé en place, développé à une passe suivante\n", file_index);
		return -1;
	}

	pthread_mutex_lock(&g_tier_mutex);
	b = stock_tier_bottom(&g_tier_unchecked[file_index]);
	if (b == NULL || stock_tier_block_seq(b) != seq) {
		pthread_mutex_unlock(&g_tier_mutex);
		log_error("stock_spill : bloc de l'étage RAM (file %d) déplacé pendant sa lecture par "
		          "l'expansion — lecture annulée\n", file_index);
		return -1;
	}
	tier_remove_locked(&g_tier_unchecked[file_index], b);
	pthread_mutex_unlock(&g_tier_mutex);
	return n;
}

int stock_spill_expansion_take(datamanager_expansion_sink_fn sink, void *ctx, unsigned long long max_records)
{
	// Le disque d'abord (le plus ancien), l'étage ensuite.
	int r = stock_spill_disk_expansion_take(sink, ctx, max_records);
	if (r != 0) {
		return r;
	}
	return tier_expansion_take(sink, ctx, max_records);
}

/// Récepteur de `spill_for_each_frame` pour une passe d'expansion : chaque
/// possibilité part vers `sink`, à développer si sa trame précède `old_bytes`.
typedef struct {
	datamanager_expansion_sink_fn sink;
	void *sink_ctx;
	long old_bytes;
	struct possibility_packet *buf;
} spill_expansion_ctx_t;

static int spill_expansion_frame(const uint8_t *raw, size_t raw_bytes, int records, long offset, void *ctx)
{
	spill_expansion_ctx_t *e = ctx;
	if (tier_decode_block(raw, raw_bytes, e->buf, records) != records) {
		return -1;
	}
	for (int i = 0; i < records; i++) {
		// Même normalisation que `stock_spill_reload`.
		e->buf[i].alloc = (uint16_t)possibility_placed_count(&e->buf[i]);
		e->buf[i].min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
		if (!e->sink(&e->buf[i], offset < e->old_bytes, e->sink_ctx)) {
			return -1;
		}
	}
	return 0;
}

static int stock_spill_disk_expansion_take(datamanager_expansion_sink_fn sink, void *ctx, unsigned long long max_records)
{
	if (!g_spill_enabled) {
		return 0;
	}

	pthread_mutex_lock(&g_spill_mutex);
	int file_index = -1, seq = 0, top = 0;
	long bytes = 0, old_bytes = 0;
	if (!g_expand_active || !spill_expansion_pick(&file_index, &seq, &bytes, &top, &old_bytes)) {
		pthread_mutex_unlock(&g_spill_mutex);
		return 0;
	}
	char path[PATH_MAX];
	spill_segment_path(path, sizeof(path), 0, file_index, seq);
	unsigned long long records = 0;
	if (bytes < 0 || spill_scan_segment(path, bytes, &records) != 0) {
		pthread_mutex_unlock(&g_spill_mutex);
		log_error("stock_spill : segment « %s » absent ou incohérent — laissé sur disque, non développé\n", path);
		return -1;
	}
	if (records > max_records) {
		pthread_mutex_unlock(&g_spill_mutex);
		return DATAMANAGER_DISK_TAKE_NO_ROOM;
	}
	// Un segment qui n'est plus le sommet est immuable (l'éviction n'empile
	// qu'au sommet) : lecture hors verrou. Le sommet, lui, garde le verrou
	// jusqu'au commit, sans quoi une éviction pourrait y ajouter entre la
	// lecture et la suppression.
	if (!top) {
		pthread_mutex_unlock(&g_spill_mutex);
	}

	spill_expansion_ctx_t e = { sink, ctx, old_bytes, malloc((size_t)STOCK_TIER_MAX_RECORDS * sizeof(struct possibility_packet)) };
	int ok = (e.buf != NULL) && spill_for_each_frame(path, bytes, spill_expansion_frame, &e) == 0;
	free(e.buf);

	if (!top) {
		pthread_mutex_lock(&g_spill_mutex);
	}
	if (!ok) {
		pthread_mutex_unlock(&g_spill_mutex);
		log_error("stock_spill : lecture du segment « %s » pour l'expansion impossible — "
		          "laissé sur disque, développé à une passe suivante\n", path);
		return -1;
	}
	// Commit : tout est chez l'appelant.
	stock_spill_descriptor_t *desc = &g_spill_unchecked[file_index];
	unlink(path);
	// Segment de frontière consommé : plus rien d'antérieur à la passe dans
	// cette pile. Sans cette marque, une pile vidée repartirait à 1 et les
	// enfants suivants passeraient pour « sous la frontière ».
	if (seq == g_expand_boundary_seq[file_index]) {
		g_expand_boundary_seq[file_index] = 0;
	}
	desc->packets -= records;
	if (seq == desc->last_seq) {
		desc->first_seq = 0;
		desc->last_seq = 0;
		desc->tail_bytes = 0;
		desc->tail_records = 0;
	} else {
		desc->first_seq++;
	}
	pthread_mutex_unlock(&g_spill_mutex);
	return (int)records;
}

unsigned long long stock_spill_total_packets(void)
{
	if (!g_spill_enabled) {
		return 0;
	}
	pthread_mutex_lock(&g_spill_mutex);
	unsigned long long total = 0;
	for (int f = 0; f < g_spill_nb_files; f++) {
		total += g_spill_unchecked[f].packets;
		total += g_spill_checked[f].packets;
	}
	pthread_mutex_unlock(&g_spill_mutex);
	return total;
}

unsigned long long stock_spill_total_segments(void)
{
	if (!g_spill_enabled) {
		return 0;
	}
	pthread_mutex_lock(&g_spill_mutex);
	unsigned long long total = 0;
	for (int f = 0; f < g_spill_nb_files; f++) {
		total += (unsigned long long)spill_segment_count(&g_spill_unchecked[f]);
		total += (unsigned long long)spill_segment_count(&g_spill_checked[f]);
	}
	pthread_mutex_unlock(&g_spill_mutex);
	return total;
}

/**
 * @brief Choisit le (pool, file) RÉSIDENT le plus plein (RAM) et y évince
 *        jusqu'à `max_packets` possibilités.
 */
static int stock_spill_evict_fullest(int max_packets)
{
	int best_pool = -1;
	int best_file = -1;
	unsigned long long best_size = 0;
	for (int f = 0; f < g_spill_nb_files; f++) {
		unsigned long long u = file_size(f);
		if (u > best_size) {
			best_size = u;
			best_pool = STOCK_SPILL_POOL_UNCHECKED;
			best_file = f;
		}
		unsigned long long c = file_checked_size(f);
		if (c > best_size) {
			best_size = c;
			best_pool = STOCK_SPILL_POOL_CHECKED;
			best_file = f;
		}
	}
	if (best_file < 0) {
		return 0;
	}
	return stock_spill_evict(best_pool, best_file, max_packets);
}

/**
 * @brief Choisit le (pool, file) le plus chargé SUR DISQUE et y recharge
 *        jusqu'à `max_packets` possibilités.
 */
static int stock_spill_reload_fullest(int max_packets)
{
	pthread_mutex_lock(&g_spill_mutex);
	int best_pool = -1;
	int best_file = -1;
	unsigned long long best_packets = 0;
	for (int f = 0; f < g_spill_nb_files; f++) {
		if (g_spill_unchecked[f].packets > best_packets) {
			best_packets = g_spill_unchecked[f].packets;
			best_pool = STOCK_SPILL_POOL_UNCHECKED;
			best_file = f;
		}
		if (g_spill_checked[f].packets > best_packets) {
			best_packets = g_spill_checked[f].packets;
			best_pool = STOCK_SPILL_POOL_CHECKED;
			best_file = f;
		}
	}
	pthread_mutex_unlock(&g_spill_mutex);
	if (best_file < 0) {
		return 0;
	}
	return stock_spill_reload(best_pool, best_file, max_packets);
}

static int stock_spill_step_impl(int max_packets, int caller_owns_maintenance)
{
	int tier = tier_active();
	if ((!g_spill_enabled && !tier) || max_packets <= 0) {
		return 0;
	}
	if (!caller_owns_maintenance && datamanager_is_maintenance_active()) {
		// Sauvegarde/restauration en cours : aucune E/S de débordement tant
		// qu'un cliché est en train d'être pris, sinon une possibilité
		// pourrait migrer entre RAM et disque pendant la capture (cf. doc
		// d'en-tête du module).
		return 0;
	}

	// Seuils calculés sur les OCTETS résidents, pas sur un nombre de
	// possibilités : c'est la grandeur que `--stock-max-ram` borne réellement
	// (cf. datamanager_resident_bytes). Les pourcentages 90/75/25 et toute la
	// mécanique d'hystérésis sont inchangés — seule l'unité l'est.
	unsigned long long cap = datamanager_ram_limit_bytes();
	if (cap == 0) {
		return 0; // illimité : le débordement n'a pas de sens sans plafond
	}

	unsigned long long resident = datamanager_resident_bytes();
	unsigned long long high = cap * STOCK_SPILL_HIGH_PERCENT / 100;
	unsigned long long low = cap * STOCK_SPILL_LOW_PERCENT / 100;
	unsigned long long reload_threshold = cap * STOCK_SPILL_RELOAD_PERCENT / 100;

	// Un seul log_event par TRANSITION de mode (pas par appel -- stock_spill_step
	// tourne toutes les 100 ms) : la fréquence de la bascule 90 %/75 %/25 % est
	// elle-même le signal utile pour un post-mortem (pression RAM soutenue vs.
	// pic isolé), un log par tick noierait ce signal dans du bruit.
	if (g_spill_mode != SPILL_MODE_EVICTING && resident >= high) {
		g_spill_mode = SPILL_MODE_EVICTING;
		log_event("stock_spill : eviction %s demarree (resident=%llu o plafond=%llu o)\n",
		          tier ? "etage RAM/disque" : "disque", resident, cap);
	} else if (g_spill_mode == SPILL_MODE_EVICTING && resident <= low) {
		g_spill_mode = SPILL_MODE_IDLE;
		log_event("stock_spill : eviction %s terminee (resident=%llu o plafond=%llu o)\n",
		          tier ? "etage RAM/disque" : "disque", resident, cap);
	}

	int expanding = datamanager_is_expansion_active();
	if (tier) {
		unsigned long long floor_bytes = cap * (unsigned long long)g_hot_floor_pct / 100;
		unsigned long long hot_before = datamanager_pools_resident_bytes();
		int moved = -1;
		if (g_spill_mode == SPILL_MODE_EVICTING) {
			moved = tier_evict_step(max_packets, cap);
		} else if (hot_before > floor_bytes) {
			moved = tier_evict_to_floor(max_packets * STOCK_TIER_PROACTIVE_FACTOR, floor_bytes);
		}
		if (moved > 0) {
			unsigned long long hot_after = datamanager_pools_resident_bytes();
			tier_note_freed(hot_before > hot_after ? hot_before - hot_after : 0);
			return moved;
		}
		// Rien d'évincé à ce pas : de quoi rendre le reliquat au système.
		tier_note_freed(0);
		if (moved == 0) {
			return 0;
		}
		// Rechargement depuis l'étage : piloté par la LISTE, pas par le total
		// — l'étage à lui seul peut dépasser 25 % du plafond, et la liste
		// resterait alors vide sans que rien ne remonte. Pas pendant une
		// expansion (même règle que le disque), ni au-dessus du seuil haut.
		if (stock_spill_tier_packets() > 0) {
			if (g_spill_mode == SPILL_MODE_RELOADING) {
				g_spill_mode = SPILL_MODE_IDLE;
			}
			unsigned long long reload_bytes = cap * (unsigned long long)g_hot_reload_pct / 100;
			if (!expanding && resident < high && hot_before < reload_bytes) {
				// Jusqu'au milieu des deux seuils, pas au-delà : remonter
				// jusqu'au plancher ferait repartir aussitôt la compression.
				return tier_reload_fullest(max_packets, (reload_bytes + floor_bytes) / 2);
			}
			// Le disque ne recharge jamais par-dessus l'étage : il est plus ancien.
			return 0;
		}
		if (!g_spill_enabled) {
			return 0;
		}
	}

	// Jamais de rechargement pendant une expansion : ce qui remonterait n'est
	// pas développé par la passe en cours et dispute la place à ses enfants —
	// l'éviction le renverrait sur disque. Et pendant le drainage (sous
	// `lock_all_file`), la file de travail n'est pas encore comptée dans
	// `resident`, qui paraît alors vide (cf. `datamanager_is_expansion_active`).
	// Le rechargement reprend au premier tick après l'expansion.
	if (g_spill_mode == SPILL_MODE_RELOADING && expanding) {
		g_spill_mode = SPILL_MODE_IDLE;
		log_event("stock_spill : rechargement disque suspendu pendant l'expansion (resident=%llu o plafond=%llu o)\n",
		          resident, cap);
	}

	unsigned long long total_spilled = stock_spill_total_packets();
	if (g_spill_mode != SPILL_MODE_RELOADING && !expanding && resident <= reload_threshold && total_spilled > 0) {
		g_spill_mode = SPILL_MODE_RELOADING;
		log_event("stock_spill : rechargement disque demarre (resident=%llu o plafond=%llu o debordees=%llu)\n",
		          resident, cap, total_spilled);
	} else if (g_spill_mode == SPILL_MODE_RELOADING && (resident >= low || total_spilled == 0)) {
		// Sort au seuil BAS (75 %), pas au seuil d'ENTRÉE (25 %) : avec le
		// même seuil pour entrer et sortir, un seul bloc rechargé (souvent
		// > 25 % du plafond à lui seul, cf. STOCK_SPILL_BLOCK_PACKETS)
		// dépasse immédiatement le seuil et arrête le rechargement après un
		// bloc, même quand la RAM a largement la place d'en accueillir plus
		// -- symétrique de l'éviction, qui vise elle aussi 75 % en sortie.
		g_spill_mode = SPILL_MODE_IDLE;
		log_event("stock_spill : rechargement disque termine (resident=%llu o plafond=%llu o debordees=%llu)\n",
		          resident, cap, total_spilled);
	}

	if (g_spill_mode == SPILL_MODE_EVICTING) {
		return stock_spill_evict_fullest(max_packets);
	}
	if (g_spill_mode == SPILL_MODE_RELOADING) {
		return stock_spill_reload_fullest(max_packets);
	}
	return 0;
}

int stock_spill_step(int max_packets)
{
	return stock_spill_step_impl(max_packets, 0);
}

int stock_spill_relieve(int max_packets)
{
	return stock_spill_step_impl(max_packets, 1);
}

// ---------------------------------------------------------------------
// Cohérence sauvegarde/restauration (stock_spill_snapshot /
// stock_spill_restore_snapshot) — cf. la doc de ces fonctions dans
// stock_spill.h.
// ---------------------------------------------------------------------

/// Copie `max_bytes` octets de `src` vers `dst` (`max_bytes < 0` ⇒ tout le
/// fichier). `dst` est toujours réécrit intégralement (jamais d'ajout).
static int spill_copy_file(const char *src, const char *dst, long max_bytes)
{
	FILE *in = fopen(src, "rb");
	if (in == NULL) {
		return -1;
	}
	FILE *out = fopen(dst, "wb");
	if (out == NULL) {
		fclose(in);
		return -1;
	}
	setvbuf(out, NULL, _IOFBF, 1 << 20);

	char buf[65536];
	long remaining = max_bytes;
	int ok = 1;
	for (;;) {
		size_t want = sizeof(buf);
		if (remaining >= 0 && (long)want > remaining) {
			want = (size_t)remaining;
		}
		if (want == 0) {
			break;
		}
		size_t got = fread(buf, 1, want, in);
		if (got == 0) {
			break;
		}
		if (fwrite(buf, 1, got, out) != got) {
			ok = 0;
			break;
		}
		if (remaining >= 0) {
			remaining -= (long)got;
		}
	}
	if (ok) {
		ok = (fflush(out) == 0);
	}
	fclose(in);
	if (fclose(out) != 0) {
		ok = 0;
	}
	if (!ok) {
		unlink(dst);
	}
	return ok ? 0 : -1;
}

static int spill_same_inode(const char *a, const char *b)
{
	struct stat sa, sb;
	if (stat(a, &sa) != 0 || stat(b, &sb) != 0) {
		return 0;
	}
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/// Averti une seule fois par processus si `link()` n'est pas disponible sur
/// le système de fichiers du répertoire de débordement (EXDEV, FS sans
/// lien physique) — le repli copie reste correct, juste plus lent sur un
/// stock volumineux.
static int g_spill_link_fallback_warned = 0;

/// Duplique `src` vers `dst` en préférant `link()` (O(1)), repli copie sur
/// échec. `dst` est d'abord supprimé (tolère son absence) pour garantir un
/// état propre avant `link()`/la copie.
static int spill_link_or_copy(const char *src, const char *dst)
{
	unlink(dst);
	if (link(src, dst) == 0) {
		return 0;
	}
	if (!g_spill_link_fallback_warned) {
		log_error("stock_spill : lien physique impossible sur « %s » (%s) — repli sur la copie "
		          "(peut ralentir les clichés/restaurations sur un stock volumineux)\n",
		          g_spill_dir, strerror(errno));
		g_spill_link_fallback_warned = 1;
	}
	return spill_copy_file(src, dst, -1);
}

static int spill_write_manifest(const char *snap_dir)
{
	char final_path[SPILL_LOCAL_PATH_MAX];
	char tmp_path[SPILL_LOCAL_PATH_MAX + 8]; // final_path + ".tmp" (4 car.) + nul
	spill_join_path(final_path, sizeof(final_path), snap_dir, STOCK_SPILL_MANIFEST_NAME);
	spill_tmp_path(tmp_path, sizeof(tmp_path), final_path);

	FILE *f = fopen(tmp_path, "w");
	if (f == NULL) {
		log_error("stock_spill_snapshot : impossible d'écrire le manifeste « %s » (%s)\n", tmp_path, strerror(errno));
		return -1;
	}
	fprintf(f, "%s\n", STOCK_SPILL_MANIFEST_MAGIC);
	fprintf(f, "# nb_files=%d (informatif — la restauration re-séquence via %%%% nb_files courant)\n", g_spill_nb_files);

	pthread_mutex_lock(&g_spill_mutex);
	int write_error = 0;
	for (int fidx = 0; fidx < g_spill_nb_files; fidx++) {
		if (g_spill_unchecked[fidx].packets > 0) {
			if (fprintf(f, "u %d %d %llu %ld\n", fidx, spill_segment_count(&g_spill_unchecked[fidx]),
			            g_spill_unchecked[fidx].packets, g_spill_unchecked[fidx].tail_bytes) < 0) {
				write_error = 1;
			}
		}
		if (g_spill_checked[fidx].packets > 0) {
			if (fprintf(f, "c %d %d %llu %ld\n", fidx, spill_segment_count(&g_spill_checked[fidx]),
			            g_spill_checked[fidx].packets, g_spill_checked[fidx].tail_bytes) < 0) {
				write_error = 1;
			}
		}
	}
	pthread_mutex_unlock(&g_spill_mutex);

	if (fflush(f) != 0) {
		write_error = 1;
	}
	if (fclose(f) != 0) {
		write_error = 1;
	}
	if (write_error) {
		log_error("stock_spill_snapshot : écriture incomplète du manifeste « %s »\n", tmp_path);
		unlink(tmp_path);
		return -1;
	}
	if (rename(tmp_path, final_path) != 0) {
		log_error("stock_spill_snapshot : impossible de publier le manifeste « %s » -> « %s » (%s)\n",
		          tmp_path, final_path, strerror(errno));
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

unsigned long long stock_spill_snapshot(const char *snapshot_subdir)
{
	if (!g_spill_enabled || snapshot_subdir == NULL) {
		return 0;
	}
	char snap_dir[PATH_MAX];
	snprintf(snap_dir, sizeof(snap_dir), "%s/%s", g_spill_dir, snapshot_subdir);
	if (mkdir(snap_dir, 0755) != 0 && errno != EEXIST) {
		log_error("stock_spill_snapshot : impossible de créer « %s » (%s) — cliché de débordement "
		          "sauté (la sauvegarde RAM appelante reste valide)\n", snap_dir, strerror(errno));
		return 0;
	}

	// Duplication des segments : PLEINS par lien (comparé par inode, pour ne
	// relier que le nouveau/renuméroté depuis le cliché précédent — un
	// segment rechargé puis réévincé peut réutiliser le même numéro de
	// séquence avec un contenu différent), segment de QUEUE toujours copié
	// (encore mutable côté vivant — un lien romprait l'immutabilité du
	// cliché déjà publié dès la prochaine éviction).
	pthread_mutex_lock(&g_spill_mutex);
	for (int pool = 0; pool < 2; pool++) {
		int is_checked = (pool == STOCK_SPILL_POOL_CHECKED);
		stock_spill_descriptor_t *arr = is_checked ? g_spill_checked : g_spill_unchecked;
		for (int fidx = 0; fidx < g_spill_nb_files; fidx++) {
			stock_spill_descriptor_t *desc = &arr[fidx];
			if (desc->last_seq == 0) {
				continue;
			}
			char live_path[PATH_MAX];
			char snap_path[SPILL_LOCAL_PATH_MAX];
			// Le cliché numérote toujours à partir de 1 (ce que relit
			// `stock_spill_restore_snapshot`), même quand la pile vivante
			// commence plus haut (`first_seq` > 1, cf. le descripteur).
			int shift = desc->first_seq - 1;
			for (int seq = desc->first_seq; seq < desc->last_seq; seq++) {
				spill_segment_path(live_path, sizeof(live_path), is_checked, fidx, seq);
				spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, fidx, seq - shift);
				if (!spill_same_inode(live_path, snap_path)) {
					spill_link_or_copy(live_path, snap_path);
				}
			}
			spill_segment_path(live_path, sizeof(live_path), is_checked, fidx, desc->last_seq);
			spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, fidx, desc->last_seq - shift);
			spill_copy_file(live_path, snap_path, desc->tail_bytes);
		}
	}
	pthread_mutex_unlock(&g_spill_mutex);

	// Purge des entrées du cliché devenues obsolètes (segment rechargé ou
	// renuméroté depuis le cliché précédent).
	DIR *d = opendir(snap_dir);
	if (d != NULL) {
		struct dirent *entry;
		while ((entry = readdir(d)) != NULL) {
			char pool_char;
			int fidx = -1;
			int seq = -1;
			int consumed = 0;
			if (sscanf(entry->d_name, "spill_%c_%d_%d.dat%n", &pool_char, &fidx, &seq, &consumed) != 3
			    || consumed != (int)strlen(entry->d_name)
			    || (pool_char != 'u' && pool_char != 'c')
			    || fidx < 0 || seq < 1) {
				continue;
			}
			pthread_mutex_lock(&g_spill_mutex);
			int stale = (fidx >= g_spill_nb_files) || (seq > spill_segment_count(spill_descriptor(pool_char == 'c', fidx)));
			pthread_mutex_unlock(&g_spill_mutex);
			if (stale) {
				char snap_path[SPILL_LOCAL_PATH_MAX];
				spill_join_path(snap_path, sizeof(snap_path), snap_dir, entry->d_name);
				unlink(snap_path);
			}
		}
		closedir(d);
	}

	spill_write_manifest(snap_dir);
	return stock_spill_total_packets();
}

typedef struct {
	char pool_char;
	int old_file_index;
	int last_seq;
	unsigned long long packets;
	long tail_bytes;
} spill_manifest_entry_t;

/// Lit et parse le manifeste de `snap_dir`. Tolérant ligne à ligne (une
/// ligne mal formée est journalisée et sautée, jamais fatale à tout le
/// reste) ; absence de fichier ou en-tête magique non reconnu ⇒ échec de
/// LA FONCTION entière (rien de fiable à en tirer).
static int spill_read_manifest(const char *snap_dir, spill_manifest_entry_t **out_entries, int *out_count,
                               spill_format_t *out_format)
{
	*out_format = SPILL_FORMAT_FRAMED;
	char path[SPILL_LOCAL_PATH_MAX];
	spill_join_path(path, sizeof(path), snap_dir, STOCK_SPILL_MANIFEST_NAME);
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return -1;
	}

	char line[256];
	if (fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return -1;
	}
	line[strcspn(line, "\r\n")] = '\0';
	if (strcmp(line, STOCK_SPILL_MANIFEST_MAGIC_V2) == 0) {
		*out_format = SPILL_FORMAT_COMPACT;
	} else if (strcmp(line, STOCK_SPILL_MANIFEST_MAGIC_V1) == 0) {
		*out_format = SPILL_FORMAT_RAW;
	} else if (strcmp(line, STOCK_SPILL_MANIFEST_MAGIC) != 0) {
		log_error("stock_spill_restore_snapshot : manifeste « %s » non reconnu (en-tête invalide) — "
		          "cliché de débordement ignoré\n", path);
		fclose(f);
		return -1;
	}

	int cap = 16;
	int n = 0;
	spill_manifest_entry_t *entries = malloc((size_t)cap * sizeof(spill_manifest_entry_t));
	if (entries == NULL) {
		fclose(f);
		return -1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
			continue;
		}
		char pool_char;
		int fidx = -1;
		int last_seq = -1;
		unsigned long long packets = 0;
		long tail_bytes = 0;
		if (sscanf(line, "%c %d %d %llu %ld", &pool_char, &fidx, &last_seq, &packets, &tail_bytes) != 5
		    || (pool_char != 'u' && pool_char != 'c') || fidx < 0 || last_seq < 1) {
			line[strcspn(line, "\r\n")] = '\0';
			log_error("stock_spill_restore_snapshot : ligne de manifeste ignorée (mal formée) : « %s »\n", line);
			continue;
		}
		if (n == cap) {
			cap *= 2;
			spill_manifest_entry_t *grown = realloc(entries, (size_t)cap * sizeof(spill_manifest_entry_t));
			if (grown == NULL) {
				break;
			}
			entries = grown;
		}
		entries[n].pool_char = pool_char;
		entries[n].old_file_index = fidx;
		entries[n].last_seq = last_seq;
		entries[n].packets = packets;
		entries[n].tail_bytes = tail_bytes;
		n++;
	}
	fclose(f);
	*out_entries = entries;
	*out_count = n;
	return 0;
}

/// Possibilités décodées par lecture d'un segment à pas fixe : un segment plein
/// en compte ~170 000, qu'on ne décode pas d'un bloc (~100 Mo de
/// `possibility_packet`).
#define SPILL_EMBED_CHUNK 4096

/// Récepteur de `spill_read_segment` : un lot de possibilités décodées, que le
/// récepteur peut modifier. @return 0 pour continuer.
typedef int (*spill_packets_fn)(struct possibility_packet *buf, int n, void *ctx);

typedef struct {
	spill_packets_fn fn;
	void *ctx;
	struct possibility_packet *buf;
	unsigned long long delivered;
} spill_segment_reader_t;

static int spill_reader_frame(const uint8_t *raw, size_t raw_bytes, int records, long offset, void *ctx)
{
	(void)offset;
	spill_segment_reader_t *r = ctx;
	if (tier_decode_block(raw, raw_bytes, r->buf, records) != records || r->fn(r->buf, records, r->ctx) != 0) {
		return -1;
	}
	r->delivered += (unsigned long long)records;
	return 0;
}

/**
 * @brief Remet à `fn`, par lots, les possibilités du segment de cliché `path`,
 *        quel que soit son format — jusqu'à `limit` octets (le sommet logique
 *        d'un segment de queue), ou tout le fichier si `limit` < 0.
 *
 * @param out_delivered Possibilités effectivement remises, même sur échec.
 * @return 0 si tout le segment a été remis, -1 sinon (absent, tronqué,
 *         incohérent, ou lot refusé par `fn`).
 */
static int spill_read_segment(const char *path, long limit, spill_format_t format, spill_packets_fn fn, void *ctx,
                              unsigned long long *out_delivered)
{
	int cap = (format == SPILL_FORMAT_FRAMED) ? (int)STOCK_TIER_MAX_RECORDS : SPILL_EMBED_CHUNK;
	spill_segment_reader_t r = { fn, ctx, malloc((size_t)cap * sizeof(struct possibility_packet)), 0 };
	int ok = (r.buf != NULL);
	if (ok && format == SPILL_FORMAT_FRAMED) {
		ok = spill_for_each_frame(path, limit, spill_reader_frame, &r) == 0;
	} else if (ok) {
		long stride = spill_stride_bytes(format);
		long bytes = (limit >= 0) ? limit : spill_file_size(path);
		FILE *sf = (bytes >= 0) ? fopen(path, "rb") : NULL;
		uint8_t *raw = malloc((size_t)SPILL_EMBED_CHUNK * (size_t)stride);
		ok = (sf != NULL && raw != NULL && bytes % stride == 0);
		for (long remaining = ok ? bytes / stride : 0; ok && remaining > 0; ) {
			int want = (remaining < SPILL_EMBED_CHUNK) ? (int)remaining : SPILL_EMBED_CHUNK;
			size_t got = fread(raw, (size_t)stride, (size_t)want, sf);
			int decoded = spill_decode_stride(raw, (int)got, format, r.buf);
			if (decoded > 0 && fn(r.buf, decoded, ctx) != 0) {
				decoded = 0;
			}
			r.delivered += (unsigned long long)(decoded > 0 ? decoded : 0);
			ok = (decoded == want);
			remaining -= want;
		}
		free(raw);
		if (sf != NULL) {
			fclose(sf);
		}
	}
	free(r.buf);
	*out_delivered = r.delivered;
	return ok ? 0 : -1;
}

/// Récepteur de réempaquetage : réécrit le lot dans la pile vivante `newf`.
typedef struct {
	int is_checked;
	int newf;
} spill_repack_ctx_t;

static int spill_repack_packets(struct possibility_packet *buf, int n, void *ctx)
{
	const spill_repack_ctx_t *rp = ctx;
	return (stock_spill_write_block(rp->is_checked, rp->newf, buf, n) == n) ? 0 : -1;
}

// NOTE VERSION 13 (cf. docs/autosearch_step.md) : cette fonction ne recompte
// JAMAIS `alloc`, y compris dans la branche de
// réempaquetage par collision ci-dessous qui relit pourtant des paquets en
// mémoire (`--stock-files` réduit depuis le cliché). Choix délibéré : le
// recomptage n'a besoin d'un seul point de passage, celui où un paquet
// quitte réellement le disque pour la RAM et devient utilisable par le
// moteur — `stock_spill_reload` (ci-dessus). Ici, un paquet reste un blob
// opaque sur disque (lien/copie d'octets, ou réécriture via
// `stock_spill_write_block` qui ne touche à aucun champ) : le recompter ici
// serait un second point de vérité à maintenir en plus de `stock_spill_reload`
// pour un gain nul, puisque `stock_spill_reload` recomptera de toute façon
// au premier rechargement en RAM qui suivra cette restauration.
unsigned long long stock_spill_restore_snapshot(const char *snapshot_subdir)
{
	if (!g_spill_enabled || snapshot_subdir == NULL) {
		return 0;
	}
	char snap_dir[PATH_MAX];
	snprintf(snap_dir, sizeof(snap_dir), "%s/%s", g_spill_dir, snapshot_subdir);

	spill_manifest_entry_t *entries = NULL;
	int n = 0;
	spill_format_t format = SPILL_FORMAT_FRAMED;
	if (spill_read_manifest(snap_dir, &entries, &n, &format) != 0) {
		log_event("stock_spill_restore_snapshot : aucun cliché de débordement valide dans « %s » — "
		         "rien à restaurer côté disque\n", snap_dir);
		return 0;
	}

	// Remplacement intégral, comme le drainage RAM que `restore()`
	// (`core/datamanager.c`) effectue déjà pour les deux pools résidents —
	// tout débordement courant non sauvegardé est une perte attendue ici,
	// symétrique de celle du drainage RAM (pas une régression introduite par
	// ce module).
	unsigned long long discarded_packets = 0;
	unsigned long long discarded_files = 0;
	spill_purge_live_segments(&discarded_packets, &discarded_files);
	if (discarded_packets > 0) {
		log_event("stock_spill_restore_snapshot : %llu possibilité(s) déportée(s) courante(s) "
		         "(%llu segment(s)) remplacées par le cliché restauré\n", discarded_packets, discarded_files);
	}

	pthread_mutex_lock(&g_spill_mutex);
	for (int f = 0; f < g_spill_nb_files; f++) {
		memset(&g_spill_unchecked[f], 0, sizeof(stock_spill_descriptor_t));
		memset(&g_spill_checked[f], 0, sizeof(stock_spill_descriptor_t));
	}
	pthread_mutex_unlock(&g_spill_mutex);

	unsigned long long total_linked = 0;
	unsigned long long total_repacked = 0;
	int linked_groups = 0;
	int repacked_groups = 0;
	if (format != SPILL_FORMAT_FRAMED) {
		log_event("stock_spill_restore_snapshot : cliché au format hérité (pas fixe) — "
		          "segments réécrits en trames pendant la restauration\n");
	}

	for (int is_checked = 0; is_checked <= 1; is_checked++) {
		char pc = is_checked ? 'c' : 'u';
		for (int newf = 0; newf < g_spill_nb_files; newf++) {
			int first_match = -1;
			int match_count = 0;
			for (int i = 0; i < n; i++) {
				if (entries[i].pool_char != pc || entries[i].old_file_index % g_spill_nb_files != newf) {
					continue;
				}
				if (first_match < 0) {
					first_match = i;
				}
				match_count++;
			}
			if (match_count == 0) {
				continue;
			}

			if (match_count == 1 && format == SPILL_FORMAT_FRAMED) {
				// Pas de collision de re-séquencement (le cas courant) :
				// aucun déplacement de données, seuls les liens et les
				// descripteurs changent — cf. la doc de cette fonction.
				//
				// Un manifeste peut lister un segment que le disque n'a plus,
				// ou qui ne se relit plus (fichier .dat supprimé, tronqué ou
				// abîmé alors que manifest.txt, lui, reste intact) : chaque
				// segment placé est relu trame par trame (en-têtes et pieds),
				// et le compte trouvé doit être celui du manifeste. Sinon le
				// groupe entier est invalide plutôt que prétendu restauré : ni
				// le descripteur ni total_linked ne sont mis à jour, et
				// `restore_apply` (ui/command_lines.c) le détecte ensuite via
				// le compte de sauvegarde (<fichier>.spillcount), puisque le
				// total renvoyé par cette fonction reflète alors fidèlement ce
				// qui a RÉELLEMENT été placé.
				spill_manifest_entry_t *e = &entries[first_match];
				char snap_path[SPILL_LOCAL_PATH_MAX];
				char live_path[PATH_MAX];
				int failed_at = -1;
				unsigned long long found = 0;
				unsigned long long top_records = 0;
				for (int seq = 1; seq <= e->last_seq && failed_at < 0; seq++) {
					int is_top = (seq == e->last_seq);
					spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, e->old_file_index, seq);
					spill_segment_path(live_path, sizeof(live_path), is_checked, newf, seq);
					int placed = is_top ? spill_copy_file(snap_path, live_path, e->tail_bytes)
					                    : spill_link_or_copy(snap_path, live_path);
					unsigned long long records = 0;
					if (placed != 0 || spill_scan_segment(live_path, is_top ? e->tail_bytes : -1, &records) != 0) {
						failed_at = seq;
					}
					found += records;
					top_records = records;
				}
				if (failed_at < 0 && found != e->packets) {
					failed_at = e->last_seq;
				}

				if (failed_at < 0) {
					pthread_mutex_lock(&g_spill_mutex);
					stock_spill_descriptor_t *desc = spill_descriptor(is_checked, newf);
					desc->first_seq = 1;
					desc->last_seq = e->last_seq;
					desc->packets = e->packets;
					desc->tail_bytes = e->tail_bytes;
					desc->tail_records = (long)top_records;
					pthread_mutex_unlock(&g_spill_mutex);

					total_linked += e->packets;
					linked_groups++;
				} else {
					log_error("stock_spill_restore_snapshot : segment de rang %d manquant, illisible ou "
					          "incomplet dans le cliché pour (pool %c, ancienne file %d) — %llu possibilité(s) "
					          "NON restaurée(s) pour cette file (cliché incomplet ou corrompu)\n",
					          failed_at, pc, e->old_file_index, e->packets);
					// Nettoyage best-effort de ce qui a été placé (ni
					// spill_link_or_copy ni spill_copy_file ne laissent de
					// fichier partiel derrière eux sur erreur).
					for (int seq = 1; seq <= failed_at; seq++) {
						spill_segment_path(live_path, sizeof(live_path), is_checked, newf, seq);
						unlink(live_path);
					}
				}
			} else {
				// Collision (--stock-files réduit depuis la sauvegarde), ou
				// cliché au format hérité : chaque source est relue et
				// réécrite via la même fonction que l'éviction normale, pour
				// ne jamais enterrer un sommet partiel venu d'une autre source
				// au milieu de la pile fusionnée.
				//
				// `entry_actual` somme ce qui a RÉELLEMENT été relu et réécrit,
				// jamais la promesse du manifeste : le total renvoyé par cette
				// fonction reste fidèle à l'état réel du disque, condition
				// nécessaire pour que la vérification de `restore_apply`
				// (comparaison au compte de sauvegarde, <fichier>.spillcount)
				// détecte l'anomalie.
				for (int i = 0; i < n; i++) {
					if (entries[i].pool_char != pc || entries[i].old_file_index % g_spill_nb_files != newf) {
						continue;
					}
					spill_manifest_entry_t *e = &entries[i];
					unsigned long long entry_actual = 0;
					int entry_incomplete = 0;
					spill_repack_ctx_t rp = { is_checked, newf };
					for (int seq = 1; seq <= e->last_seq; seq++) {
						char snap_path[SPILL_LOCAL_PATH_MAX];
						spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, e->old_file_index, seq);
						unsigned long long got = 0;
						if (spill_read_segment(snap_path, (seq < e->last_seq) ? -1 : e->tail_bytes, format,
						                       spill_repack_packets, &rp, &got) != 0) {
							entry_incomplete = 1;
						}
						entry_actual += got;
					}
					if (entry_incomplete || entry_actual != e->packets) {
						log_error("stock_spill_restore_snapshot : réempaquetage incomplet pour "
						          "(pool %c, ancienne file %d) — %llu/%llu possibilité(s) "
						          "effectivement récupérée(s) (segment manquant/tronqué dans le "
						          "cliché)\n", pc, e->old_file_index, entry_actual, e->packets);
					}
					total_repacked += entry_actual;
				}
				repacked_groups++;
			}
		}
	}

	free(entries);
	// log_file, pas log_event : le détail (chemin, 4 compteurs) dépasse
	// facilement les 200 octets d'EVENT_MSG_MAX et serait tronqué -- une
	// restauration est rare/à fort enjeu, la trace complète prime sur la
	// visibilité console immédiate (déjà couverte par le "backup restore"
	// court de restore_apply, cf. command_lines.c).
	log_file("stock_spill_restore_snapshot : cliché « %s » restauré (%llu possibilité(s) sur %d file(s) "
	         "sans collision, %llu possibilité(s) réempaquetée(s) sur %d file(s) — collision due à un "
	         "--stock-files réduit depuis la sauvegarde, ou format hérité)\n",
	         snap_dir, total_linked, linked_groups, total_repacked, repacked_groups);
	return total_linked + total_repacked;
}

/// Récepteur de recopie dans le `.back`.
typedef struct {
	FILE *out;
	int is_checked;
} spill_embed_ctx_t;

static int spill_embed_packets(struct possibility_packet *buf, int n, void *ctx)
{
	const spill_embed_ctx_t *em = ctx;
	for (int i = 0; i < n; i++) {
		// Le pool d'origine fait foi : c'est lui que la restauration
		// d'un cliché respectait, et `import` route par ce drapeau.
		buf[i].checked = (uint8_t)(em->is_checked ? 1 : 0);
		if (packet_codec_fwrite(em->out, &buf[i]) != 0) {
			return -1;
		}
	}
	return 0;
}

int stock_spill_embed_snapshot(const char *snapshot_subdir, FILE *out, unsigned long long *out_written)
{
	*out_written = 0;
	if (!g_spill_enabled || snapshot_subdir == NULL) {
		return 0;
	}
	char snap_dir[PATH_MAX];
	snprintf(snap_dir, sizeof(snap_dir), "%s/%s", g_spill_dir, snapshot_subdir);

	spill_manifest_entry_t *entries = NULL;
	int n = 0;
	spill_format_t format = SPILL_FORMAT_FRAMED;
	if (spill_read_manifest(snap_dir, &entries, &n, &format) != 0) {
		// Pas de manifeste : `stock_spill_snapshot` n'en écrit pas quand il
		// n'a pas pu créer le répertoire. L'appelant compare le compte écrit
		// (0) à celui du cliché : un débordement non vide ne passe donc pas.
		spill_remove_snapshot_dir(snap_dir);
		return 0;
	}

	int failed = 0;
	for (int i = 0; i < n && !failed; i++) {
		spill_manifest_entry_t *e = &entries[i];
		spill_embed_ctx_t em = { out, e->pool_char == 'c' };
		unsigned long long entry_written = 0;
		for (int seq = 1; seq <= e->last_seq && !failed; seq++) {
			char snap_path[SPILL_LOCAL_PATH_MAX];
			spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, em.is_checked, e->old_file_index, seq);
			unsigned long long got = 0;
			if (spill_read_segment(snap_path, (seq < e->last_seq) ? -1 : e->tail_bytes, format,
			                       spill_embed_packets, &em, &got) != 0) {
				log_error("stock_spill_embed_snapshot : segment « %s » manquant, tronqué ou illisible "
				          "(%llu possibilité(s) recopiée(s))\n", snap_path, got);
				failed = 1;
			}
			entry_written += got;
		}
		*out_written += entry_written;
		if (!failed && entry_written != e->packets) {
			log_error("stock_spill_embed_snapshot : (pool %c, file %d) — %llu possibilité(s) recopiée(s), "
			          "le manifeste en annonce %llu\n", e->pool_char, e->old_file_index, entry_written, e->packets);
			failed = 1;
		}
	}
	free(entries);
	spill_remove_snapshot_dir(snap_dir);
	return failed ? -1 : 0;
}

void stock_spill_drop_snapshot(const char *snapshot_subdir)
{
	if (!g_spill_enabled || snapshot_subdir == NULL) {
		return;
	}
	char snap_dir[PATH_MAX];
	snprintf(snap_dir, sizeof(snap_dir), "%s/%s", g_spill_dir, snapshot_subdir);
	spill_remove_snapshot_dir(snap_dir);
}

void stock_spill_discard_live(void)
{
	if (!g_spill_enabled) {
		return;
	}
	unsigned long long discarded_packets = 0;
	unsigned long long discarded_files = 0;
	spill_purge_live_segments(&discarded_packets, &discarded_files);
	pthread_mutex_lock(&g_spill_mutex);
	for (int f = 0; f < g_spill_nb_files; f++) {
		memset(&g_spill_unchecked[f], 0, sizeof(stock_spill_descriptor_t));
		memset(&g_spill_checked[f], 0, sizeof(stock_spill_descriptor_t));
	}
	pthread_mutex_unlock(&g_spill_mutex);
	if (discarded_packets > 0) {
		log_event("stock_spill : %llu possibilité(s) déportée(s) courante(s) (%llu segment(s)) "
		          "remplacées par la sauvegarde restaurée\n", discarded_packets, discarded_files);
	}
}

int stock_spill_prepare_restore(const char *stock_filename)
{
	if (datamanager_backup_is_complete(stock_filename)) {
		// Le fichier porte tout le stock : aucun cliché à remettre en place,
		// seulement le débordement courant à vider — `restore` vide de même
		// les deux pools résidents. Ce que l'import ne pourra pas garder en
		// RAM repartira sur disque par le crochet de dégagement.
		stock_spill_discard_live();
		return 0;
	}

	char subdir[64];
	unsigned long long expected = 0;
	int has_sidecar = datamanager_read_spillcount_sidecar(stock_filename, &expected, subdir, sizeof subdir);
	if (!has_sidecar) {
		snprintf(subdir, sizeof subdir, "%s", CONSISTENT_BACKUP_DEFAULT_SNAPSHOT);
	}
	unsigned long long restored = stock_spill_restore_snapshot(subdir);

	// Le `.spillcount`, écrit au moment de CETTE sauvegarde et indépendant du
	// répertoire de débordement, dit combien de possibilités auraient dû
	// revenir : un `--stock-spill-dir` oublié ou différent, un cliché
	// supprimé ou corrompu ne passent pas pour un succès. Son absence
	// (sauvegarde antérieure, ou sans débordement ce jour-là) n'est pas une
	// anomalie.
	if (has_sidecar && expected != restored) {
		log_error("restore : débordement disque INCOMPLET — %llu possibilité(s) attendue(s) "
		          "(déportées au moment de la sauvegarde de %s), %llu récupérée(s) depuis le "
		          "cliché « %s » (--stock-spill-dir absent/différent de celui utilisé à "
		          "la sauvegarde, ou cliché supprimé/corrompu ?) — %llu possibilité(s) "
		          "potentiellement perdue(s). La restauration continue (le stock résident "
		          "reste utilisable) mais est INCOMPLÈTE.\n",
		          expected, stock_filename, restored, subdir,
		          expected > restored ? expected - restored : 0);
		return -1;
	}
	return 0;
}
