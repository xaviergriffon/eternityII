#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

#include "ui/logger.h"
#include "core/possibility.h"
#include "core/datamanager.h"
#include "core/stock_spill.h"

/**
 * @brief État de débordement d'UN (pool, file de stock) — une pile de
 *        segments numérotés 1..last_seq, tous pleins (`STOCK_SPILL_SEGMENT_BYTES`,
 *        arrondi au paquet près, cf. `stock_spill_full_segment_bytes`) sauf
 *        le dernier (`last_seq`), seul partiel (`tail_bytes` < plein).
 */
typedef struct {
	int first_seq;               ///< 0 si jamais rien débordé pour ce (pool, file) ; sinon 1.
	int last_seq;                ///< 0 si aucun segment ; sinon le sommet de la pile.
	unsigned long long packets;  ///< Total de possibilités déportées, ce (pool, file).
	long tail_bytes;             ///< Octets déjà écrits dans le segment `last_seq` (partiel).
} stock_spill_descriptor_t;

static char *g_spill_dir = NULL;
static int g_spill_nb_files = 0;
static int g_spill_enabled = 0;
static stock_spill_descriptor_t *g_spill_unchecked = NULL; // [g_spill_nb_files]
static stock_spill_descriptor_t *g_spill_checked = NULL;   // [g_spill_nb_files]
static pthread_mutex_t g_spill_mutex = PTHREAD_MUTEX_INITIALIZER;

/// État d'hystérésis courant (cf. la doc de `stock_spill_step`) : IDLE (rien
/// à faire), EVICTING (occupation RAM >= 90 % du plafond, en train d'évacuer
/// vers le disque) ou RELOADING (occupation <= 25 %, en train de recharger).
typedef enum { SPILL_MODE_IDLE = 0, SPILL_MODE_EVICTING = 1, SPILL_MODE_RELOADING = 2 } spill_mode_t;
static spill_mode_t g_spill_mode = SPILL_MODE_IDLE;

/// Surcharge réservée aux tests (0 = désactivée, utiliser
/// `STOCK_SPILL_SEGMENT_BYTES`) — cf. `stock_spill_set_segment_bytes_for_tests`.
/// `STOCK_SPILL_SEGMENT_BYTES` (64 Mio, ~106 000 possibilités sur le puzzle
/// 256) rend le franchissement d'une frontière de segment impraticable à
/// exercer dans un test unitaire rapide sans cette surcharge.
static long g_segment_bytes_override = 0;

/**
 * @brief Taille RÉELLEMENT pleine d'un segment, en octets — arrondie au
 *        paquet inférieur (`STOCK_SPILL_SEGMENT_BYTES` n'est pas
 *        nécessairement un multiple exact de `sizeof(struct
 *        possibility_packet)`). Un segment plein a TOUJOURS exactement
 *        cette taille, jamais `STOCK_SPILL_SEGMENT_BYTES` brut : les deux
 *        divergent de jusqu'à `sizeof(struct possibility_packet) - 1`
 *        octets, une confusion entre les deux casserait le calcul de
 *        capacité restante (`stock_spill_evict`) et la restauration de
 *        `tail_bytes` en dépilant un segment plein (`stock_spill_reload`).
 */
static long stock_spill_full_segment_bytes(void)
{
	long packet_size = (long)sizeof(struct possibility_packet);
	long segment_bytes = (g_segment_bytes_override > 0) ? g_segment_bytes_override : STOCK_SPILL_SEGMENT_BYTES;
	return (segment_bytes / packet_size) * packet_size;
}

// Réservée aux tests (jamais appelée en production, non déclarée dans
// stock_spill.h — même convention que
// datamanager_set_ram_limit_packets_for_tests) : force une taille de
// segment minuscule pour pouvoir exercer le franchissement d'une frontière
// de segment (rollover) sans écrire ~106 000 possibilités par test.
void stock_spill_set_segment_bytes_for_tests(long bytes)
{
	g_segment_bytes_override = bytes;
}

/// Manifeste texte du cliché (PR3) : en-tête magique + une ligne par
/// (pool, file) débordé au moment du cliché.
#define STOCK_SPILL_MANIFEST_MAGIC "eternityii-spill-manifest-v1"
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
/// AGENTS.md, section « Build croisé ARM 64-bit », pour `http_server.c`).
/// Un tampon destination `g_spill_dir` (chaîne `strdup`, taille inconnue du
/// compilateur) n'a PAS besoin de cette marge : gcc ne peut alors établir
/// aucune borne supérieure finie et ne déclenche pas l'avertissement.
#define SPILL_LOCAL_PATH_MAX (PATH_MAX + 64)

static void spill_segment_path(char *buf, size_t bufsize, int is_checked, int file_index, int seq)
{
	snprintf(buf, bufsize, "%s/spill_%c_%d_%d.dat", g_spill_dir, is_checked ? 'c' : 'u', file_index, seq);
}

/// Variante de `spill_segment_path` pour un répertoire EXPLICITE (PR3 :
/// snapshot/restauration jonglent avec `g_spill_dir` ET `snap_dir`, deux
/// répertoires distincts). Le motif "buf/bufsize en paramètres de fonction"
/// est délibéré, pas seulement pour la réutilisation : passer par une
/// fonction plutôt qu'un `snprintf` direct dans un tableau local
/// `char[PATH_MAX]` évite les faux positifs `-Wformat-truncation` de gcc
/// (glibc/`_FORTIFY_SOURCE`) — gcc calcule un pire cas précis uniquement
/// quand la taille du tampon est visible statiquement au point d'appel, pas
/// quand elle transite par un paramètre `size_t bufsize` (cf. `AGENTS.md`,
/// section « Build croisé ARM 64-bit », pour un autre cas de ce même piège).
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
 *        remise en place du cliché, PR3).
 */
static void spill_purge_live_segments(unsigned long long *out_packets, unsigned long long *out_files)
{
	unsigned long long discarded_packets = 0;
	unsigned long long discarded_files = 0;
	long packet_size = (long)sizeof(struct possibility_packet);
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
				struct stat st;
				if (stat(path, &st) == 0 && st.st_size > 0) {
					discarded_packets += (unsigned long long)(st.st_size / packet_size);
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

void stock_spill_configure(const char *dir, int nb_files)
{
	free(g_spill_dir);
	g_spill_dir = strdup((dir != NULL) ? dir : STOCK_SPILL_DIR_DEFAULT);
	g_spill_nb_files = nb_files;
	g_spill_enabled = 0;
	g_spill_mode = SPILL_MODE_IDLE;
	g_segment_bytes_override = 0; // repart de STOCK_SPILL_SEGMENT_BYTES (production)
	free(g_spill_unchecked);
	g_spill_unchecked = NULL;
	free(g_spill_checked);
	g_spill_checked = NULL;

	if (nb_files <= 0) {
		return;
	}

	if (mkdir(g_spill_dir, 0755) != 0 && errno != EEXIST) {
		log_error("stock_spill_configure : impossible de créer/utiliser le répertoire de "
		          "débordement « %s » (%s) — débordement désactivé, le plafond RAM "
		          "(--stock-max-ram) restera un mur dur sans recours\n",
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
	// `ui/command_lines.c`, PR3) qui remet en place un cliché APRÈS ce point.
	// Tout segment trouvé ici et non suivi d'un `restore` est un débordement
	// que le processus PRÉCÉDENT n'a jamais eu l'occasion de sauvegarder
	// (`backup`) avant de s'arrêter — perte réelle si aucun `restore` ne suit
	// ce démarrage.
	unsigned long long discarded_packets = 0;
	unsigned long long discarded_files = 0;
	spill_purge_live_segments(&discarded_packets, &discarded_files);
	if (discarded_packets > 0) {
		log_error("stock_spill_configure : %llu possibilité(s) dans %llu segment(s) résiduel(s) "
		          "de « %s » supprimées au démarrage — lancer « restore » immédiatement si un "
		          "cliché de débordement existe (commande backup/PR3), sinon ces possibilités "
		          "sont perdues\n",
		          discarded_packets, discarded_files, g_spill_dir);
	}

	g_spill_enabled = 1;
}

/**
 * @brief Écrit `n` possibilités déjà drainées de la RAM (`buf`) dans la pile
 *        de segments du (pool, file) désigné, en empilant sur le sommet
 *        courant (`last_seq`), roulant vers un nouveau segment dès que le
 *        courant est plein.
 *
 * @return Nombre effectivement écrit sur disque (< n seulement sur échec
 *         d'E/S — le reste de `buf` n'a alors pas été touché par l'appelant).
 */
static int stock_spill_write_block(int is_checked, int file_index, const struct possibility_packet *buf, int n)
{
	long packet_size = (long)sizeof(struct possibility_packet);
	long full_bytes = stock_spill_full_segment_bytes();

	pthread_mutex_lock(&g_spill_mutex);
	stock_spill_descriptor_t *desc = spill_descriptor(is_checked, file_index);

	int idx = 0;
	int ok = 1;
	while (idx < n && ok) {
		if (desc->last_seq == 0) {
			desc->last_seq = 1;
			desc->first_seq = 1;
			desc->tail_bytes = 0;
		}
		long remaining = full_bytes - desc->tail_bytes;
		int fits = (int)(remaining / packet_size);
		if (fits <= 0) {
			desc->last_seq++;
			desc->tail_bytes = 0;
			continue; // recalcule fits sur le nouveau segment, sans consommer idx
		}
		int chunk = n - idx;
		if (chunk > fits) {
			chunk = fits;
		}

		char path[PATH_MAX];
		spill_segment_path(path, sizeof(path), is_checked, file_index, desc->last_seq);
		FILE *f = fopen(path, "ab");
		if (f == NULL) {
			ok = 0;
			break;
		}
		setvbuf(f, NULL, _IOFBF, 1 << 20);
		size_t written = fwrite(&buf[idx], (size_t)packet_size, (size_t)chunk, f);
		if (written == (size_t)chunk) {
			fflush(f);
			fsync(fileno(f));
		} else {
			ok = 0;
		}
		fclose(f);
		if (!ok) {
			break;
		}

		desc->tail_bytes += (long)chunk * packet_size;
		desc->packets += (unsigned long long)chunk;
		idx += chunk;
	}
	pthread_mutex_unlock(&g_spill_mutex);
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
	return written;
}

/**
 * @brief Recharge jusqu'à `max_packets` possibilités depuis le sommet de la
 *        pile de segments du (pool, file) désigné vers sa file RAM.
 *
 * Lecture AVANT retrait (« peek puis commit ») : le segment n'est modifié
 * (tronqué, ou supprimé si vidé) qu'APRÈS confirmation que
 * `datamanager_pool_refill` a bien réinséré les possibilités lues — jamais
 * un octet du disque n'est perdu si le rechargement RAM échouait
 * (théoriquement possible seulement sur OOM de `put()`).
 *
 * @return Nombre effectivement rechargé.
 */
static int stock_spill_reload(int is_checked, int file_index, int max_packets)
{
	if (!g_spill_enabled) {
		return 0;
	}
	long packet_size = (long)sizeof(struct possibility_packet);

	pthread_mutex_lock(&g_spill_mutex);
	stock_spill_descriptor_t *desc = spill_descriptor(is_checked, file_index);
	if (desc->packets == 0 || desc->last_seq == 0) {
		pthread_mutex_unlock(&g_spill_mutex);
		return 0;
	}
	int available = (int)(desc->tail_bytes / packet_size);
	int to_read = (available < max_packets) ? available : max_packets;
	if (to_read <= 0) {
		pthread_mutex_unlock(&g_spill_mutex);
		return 0;
	}
	int seq = desc->last_seq;
	long new_tail = desc->tail_bytes - (long)to_read * packet_size;

	char path[PATH_MAX];
	spill_segment_path(path, sizeof(path), is_checked, file_index, seq);
	struct possibility_packet *buf = malloc((size_t)to_read * sizeof(struct possibility_packet));
	int read_ok = 0;
	if (buf != NULL) {
		FILE *f = fopen(path, "rb");
		if (f != NULL) {
			if (fseek(f, new_tail, SEEK_SET) == 0) {
				size_t got = fread(buf, (size_t)packet_size, (size_t)to_read, f);
				read_ok = (got == (size_t)to_read);
			}
			fclose(f);
		}
	}
	// Rien n'a encore été modifié sur disque (lecture seule ci-dessus) : sûr
	// de déverrouiller avant le rechargement RAM, qui n'a pas besoin de ce
	// verrou (protège seulement les descripteurs/fichiers de débordement).
	pthread_mutex_unlock(&g_spill_mutex);

	if (!read_ok) {
		log_error("stock_spill : échec de lecture du segment « %s » — rechargement reporté au tick suivant\n", path);
		free(buf);
		return 0;
	}

	// Migration transparente (docs/conception/mrv_moteur_unique.md, PR2 §8) :
	// un segment de débordement écrit avant VERSION 13 porte `alloc` au sens
	// curseur, pas au sens nombre de pièces posées. Recomptage systématique
	// et inconditionnel — comme `import()` (core/datamanager.c) — avant que
	// le paquet ne rejoigne la RAM : idempotent sur un segment déjà v13,
	// donc aucun besoin de distinguer les deux cas.
	for (int i = 0; i < to_read; i++) {
		buf[i].alloc = (uint16_t)possibility_placed_count(&buf[i]);
	}

	datamanager_pool_refill(is_checked, file_index, buf, to_read);
	free(buf);

	// Commit : maintenant que les possibilités sont confirmées en RAM, on
	// peut réduire (ou supprimer) le segment sans risque.
	pthread_mutex_lock(&g_spill_mutex);
	desc->tail_bytes = new_tail;
	desc->packets -= (unsigned long long)to_read;
	if (new_tail == 0) {
		unlink(path);
		desc->last_seq--;
		if (desc->last_seq == 0) {
			desc->first_seq = 0;
		} else {
			// Tout segment SOUS le sommet est nécessairement plein (seul le
			// sommet peut être partiel) — cf. stock_spill_write_block.
			desc->tail_bytes = stock_spill_full_segment_bytes();
		}
	}
	pthread_mutex_unlock(&g_spill_mutex);
	return to_read;
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
		total += (unsigned long long)g_spill_unchecked[f].last_seq;
		total += (unsigned long long)g_spill_checked[f].last_seq;
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

int stock_spill_step(int max_packets)
{
	if (!g_spill_enabled || max_packets <= 0) {
		return 0;
	}
	if (datamanager_is_maintenance_active()) {
		// Sauvegarde/restauration en cours : aucune E/S de débordement tant
		// qu'un cliché est en train d'être pris, sinon une possibilité
		// pourrait migrer entre RAM et disque pendant la capture (cf. doc
		// d'en-tête du module).
		return 0;
	}

	unsigned long long cap = datamanager_ram_limit_packets();
	if (cap == 0) {
		return 0; // illimité : le débordement n'a pas de sens sans plafond
	}

	unsigned long long resident = datamanager_resident_packets();
	unsigned long long high = cap * STOCK_SPILL_HIGH_PERCENT / 100;
	unsigned long long low = cap * STOCK_SPILL_LOW_PERCENT / 100;
	unsigned long long reload_threshold = cap * STOCK_SPILL_RELOAD_PERCENT / 100;

	if (g_spill_mode != SPILL_MODE_EVICTING && resident >= high) {
		g_spill_mode = SPILL_MODE_EVICTING;
	} else if (g_spill_mode == SPILL_MODE_EVICTING && resident <= low) {
		g_spill_mode = SPILL_MODE_IDLE;
	}

	unsigned long long total_spilled = stock_spill_total_packets();
	if (g_spill_mode != SPILL_MODE_RELOADING && resident <= reload_threshold && total_spilled > 0) {
		g_spill_mode = SPILL_MODE_RELOADING;
	} else if (g_spill_mode == SPILL_MODE_RELOADING && (resident >= low || total_spilled == 0)) {
		// Sort au seuil BAS (75 %), pas au seuil d'ENTRÉE (25 %) : avec le
		// même seuil pour entrer et sortir, un seul bloc rechargé (souvent
		// > 25 % du plafond à lui seul, cf. STOCK_SPILL_BLOCK_PACKETS)
		// dépasse immédiatement le seuil et arrête le rechargement après un
		// bloc, même quand la RAM a largement la place d'en accueillir plus
		// -- symétrique de l'éviction, qui vise elle aussi 75 % en sortie.
		g_spill_mode = SPILL_MODE_IDLE;
	}

	if (g_spill_mode == SPILL_MODE_EVICTING) {
		return stock_spill_evict_fullest(max_packets);
	}
	if (g_spill_mode == SPILL_MODE_RELOADING) {
		return stock_spill_reload_fullest(max_packets);
	}
	return 0;
}

// ---------------------------------------------------------------------
// PR3 : cohérence sauvegarde/restauration (stock_spill_snapshot /
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
			if (fprintf(f, "u %d %d %llu %ld\n", fidx, g_spill_unchecked[fidx].last_seq,
			            g_spill_unchecked[fidx].packets, g_spill_unchecked[fidx].tail_bytes) < 0) {
				write_error = 1;
			}
		}
		if (g_spill_checked[fidx].packets > 0) {
			if (fprintf(f, "c %d %d %llu %ld\n", fidx, g_spill_checked[fidx].last_seq,
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
			for (int seq = 1; seq < desc->last_seq; seq++) {
				spill_segment_path(live_path, sizeof(live_path), is_checked, fidx, seq);
				spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, fidx, seq);
				if (!spill_same_inode(live_path, snap_path)) {
					spill_link_or_copy(live_path, snap_path);
				}
			}
			spill_segment_path(live_path, sizeof(live_path), is_checked, fidx, desc->last_seq);
			spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, fidx, desc->last_seq);
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
			int stale = (fidx >= g_spill_nb_files) || (seq > spill_descriptor(pool_char == 'c', fidx)->last_seq);
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
static int spill_read_manifest(const char *snap_dir, spill_manifest_entry_t **out_entries, int *out_count)
{
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
	if (strcmp(line, STOCK_SPILL_MANIFEST_MAGIC) != 0) {
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

// NOTE VERSION 13 (docs/conception/mrv_moteur_unique.md, PR2 §8) : cette
// fonction ne recompte JAMAIS `alloc`, y compris dans la branche de
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
	if (spill_read_manifest(snap_dir, &entries, &n) != 0) {
		log_info("stock_spill_restore_snapshot : aucun cliché de débordement valide dans « %s » — "
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
		log_info("stock_spill_restore_snapshot : %llu possibilité(s) déportée(s) courante(s) "
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
	long packet_size = (long)sizeof(struct possibility_packet);

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

			if (match_count == 1) {
				// Pas de collision de re-séquencement (le cas courant) :
				// aucun déplacement de données, seuls les liens et les
				// descripteurs changent — cf. la doc de cette fonction.
				//
				// Correctif : un manifeste peut lister un segment que le
				// disque n'a plus (fichier .dat supprimé/corrompu alors que
				// manifest.txt, lui, reste intact) — sans vérifier le
				// résultat de chaque lien/copie, le descripteur était posé
				// tel quel (desc->packets = e->packets) et ce groupe comptait
				// intégralement dans total_linked, alors que tout ou partie
				// des données restaurées n'existait tout simplement pas sur
				// disque : un import ultérieur lisait alors un flux tronqué
				// ou vide sans le signaler. `failed_at` (rang du premier
				// segment manquant/illisible, -1 si aucun) rend ce groupe
				// entièrement invalide plutôt que de prétendre l'avoir
				// restauré : ni le descripteur ni total_linked ne sont mis à
				// jour, et `restore_apply` (ui/command_lines.c) le détecte
				// ensuite via le compte de sauvegarde (<fichier>.spillcount),
				// puisque le total renvoyé par cette fonction reflète alors
				// fidèlement ce qui a RÉELLEMENT été placé.
				spill_manifest_entry_t *e = &entries[first_match];
				char snap_path[SPILL_LOCAL_PATH_MAX];
				char live_path[PATH_MAX];
				int failed_at = -1;
				for (int seq = 1; seq < e->last_seq && failed_at < 0; seq++) {
					spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, e->old_file_index, seq);
					spill_segment_path(live_path, sizeof(live_path), is_checked, newf, seq);
					if (spill_link_or_copy(snap_path, live_path) != 0) {
						failed_at = seq;
					}
				}
				if (failed_at < 0) {
					spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, e->old_file_index, e->last_seq);
					spill_segment_path(live_path, sizeof(live_path), is_checked, newf, e->last_seq);
					if (spill_copy_file(snap_path, live_path, e->tail_bytes) != 0) {
						failed_at = e->last_seq;
					}
				}

				if (failed_at < 0) {
					pthread_mutex_lock(&g_spill_mutex);
					stock_spill_descriptor_t *desc = spill_descriptor(is_checked, newf);
					desc->first_seq = 1;
					desc->last_seq = e->last_seq;
					desc->packets = e->packets;
					desc->tail_bytes = e->tail_bytes;
					pthread_mutex_unlock(&g_spill_mutex);

					total_linked += e->packets;
					linked_groups++;
				} else {
					log_error("stock_spill_restore_snapshot : segment de rang %d manquant/illisible "
					          "dans le cliché pour (pool %c, ancienne file %d) — %llu possibilité(s) "
					          "NON restaurée(s) pour cette file (cliché incomplet ou corrompu)\n",
					          failed_at, pc, e->old_file_index, e->packets);
					// Nettoyage best-effort des segments déjà placés avant
					// l'échec (jamais le segment en échec lui-même : ni
					// spill_link_or_copy ni spill_copy_file ne laissent de
					// fichier partiel derrière eux sur erreur).
					for (int seq = 1; seq < failed_at; seq++) {
						spill_segment_path(live_path, sizeof(live_path), is_checked, newf, seq);
						unlink(live_path);
					}
				}
			} else {
				// Collision (--stock-files réduit depuis la sauvegarde) :
				// chaque source est relue et réempaquetée via la même
				// fonction que l'éviction normale, pour ne jamais violer
				// l'invariant « tout segment sous le sommet est plein »
				// avec un sommet partiel venu d'une autre source placé au
				// milieu de la pile fusionnée.
				//
				// Correctif (même raison que le chemin sans collision
				// ci-dessus) : `total_repacked` comptait `e->packets` (la
				// promesse du manifeste) même quand un segment source
				// manquait ou n'était que partiellement lisible — `entry_actual`
				// somme au contraire ce que `fread` a RÉELLEMENT pu relire
				// (et donc ce que `stock_spill_write_block` a RÉELLEMENT
				// réécrit), rendant le total renvoyé par cette fonction fidèle
				// à l'état réel du disque, condition nécessaire pour que la
				// vérification de `restore_apply` (comparaison au compte de
				// sauvegarde, <fichier>.spillcount) détecte l'anomalie.
				for (int i = 0; i < n; i++) {
					if (entries[i].pool_char != pc || entries[i].old_file_index % g_spill_nb_files != newf) {
						continue;
					}
					spill_manifest_entry_t *e = &entries[i];
					unsigned long long entry_actual = 0;
					int entry_incomplete = 0;
					for (int seq = 1; seq <= e->last_seq; seq++) {
						long seg_bytes = (seq < e->last_seq) ? stock_spill_full_segment_bytes() : e->tail_bytes;
						int count = (int)(seg_bytes / packet_size);
						if (count <= 0) {
							continue;
						}
						char snap_path[SPILL_LOCAL_PATH_MAX];
						spill_segment_path_in(snap_path, sizeof(snap_path), snap_dir, is_checked, e->old_file_index, seq);
						struct possibility_packet *buf = malloc((size_t)count * sizeof(struct possibility_packet));
						if (buf == NULL) {
							entry_incomplete = 1;
							continue;
						}
						FILE *sf = fopen(snap_path, "rb");
						if (sf == NULL) {
							entry_incomplete = 1;
							free(buf);
							continue;
						}
						size_t got = fread(buf, (size_t)packet_size, (size_t)count, sf);
						fclose(sf);
						if (got > 0) {
							stock_spill_write_block(is_checked, newf, buf, (int)got);
							entry_actual += got;
						}
						if (got != (size_t)count) {
							entry_incomplete = 1;
						}
						free(buf);
					}
					if (entry_incomplete) {
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
	log_info("stock_spill_restore_snapshot : cliché « %s » restauré (%llu possibilité(s) sur %d file(s) "
	         "sans collision, %llu possibilité(s) réempaquetée(s) sur %d file(s) — collision due à un "
	         "--stock-files réduit depuis la sauvegarde)\n",
	         snap_dir, total_linked, linked_groups, total_repacked, repacked_groups);
	return total_linked + total_repacked;
}
