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

static void spill_segment_path(char *buf, size_t bufsize, int is_checked, int file_index, int seq)
{
	snprintf(buf, bufsize, "%s/spill_%c_%d_%d.dat", g_spill_dir, is_checked ? 'c' : 'u', file_index, seq);
}

static stock_spill_descriptor_t *spill_descriptor(int is_checked, int file_index)
{
	return is_checked ? &g_spill_checked[file_index] : &g_spill_unchecked[file_index];
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

	// Purge des segments résiduels d'un précédent démarrage : ce module n'a
	// aucune conscience de sauvegarde/restauration (PR3, non livré) — tout
	// segment trouvé ici est un débordement dont le processus PRÉCÉDENT n'a
	// jamais eu l'occasion de le réintégrer en RAM avant de s'arrêter.
	// Ne supprime QUE les fichiers correspondant EXACTEMENT au motif
	// attendu, jamais un effacement générique du répertoire (fourni par
	// l'opérateur, potentiellement partagé avec autre chose).
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
	if (discarded_packets > 0) {
		log_error("stock_spill_configure : %llu possibilité(s) dans %llu segment(s) résiduel(s) "
		          "de « %s » supprimées au démarrage — le débordement ne survit pas encore à un "
		          "redémarrage (nécessite la cohérence sauvegarde/restauration, PR3, non livrée) ; "
		          "sauvegarder (backup) avant tout redémarrage pour ne rien perdre\n",
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
