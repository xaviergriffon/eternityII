/*
 * Banc de l'ÉTAGE RAM COMPRESSÉ : combien d'octets RÉSIDENTS coûte une
 * possibilité du stock selon la façon dont on la range, et à quel débit on
 * passe d'une forme à l'autre ?
 *
 * Question posée par docs/conception/etage_ram_compresse.md : avant de
 * déborder sur disque, emballer la partie FROIDE des files de stock en blocs
 * contigus (éventuellement compressés) plutôt qu'en maillons `Element` d'une
 * liste chaînée. Ce banc mesure les trois grandeurs dont dépend la décision :
 *
 *   1. octets par possibilité VUS PAR L'ALLOCATEUR (`mallinfo2`, bourrage et
 *      en-têtes de chunk compris) — la liste chaînée actuelle d'abord, puis
 *      chaque configuration de bloc ;
 *   2. débit d'ÉVICTION : dépiler la TÊTE de la file (`scroll_fifo_sized`,
 *      exactement ce que fait le débordement), emballer, compresser ;
 *   3. débit de RECHARGEMENT : décompresser, redécouper, réinsérer dans la
 *      file (`put_sized`), bloc le plus récent d'abord.
 *
 * Chaque configuration tourne dans un FILS (`fork`) : l'allocateur y part de
 * l'état du père, et les blocs d'une configuration ne restent pas dans le tas
 * de la suivante. La mesure principale est `mallinfo2` (octets en usage dans
 * le tas + blocs mmap), pas le RSS — le RSS du fils compte aussi les pages de
 * l'échantillon source héritées du père, et garde les pages que l'allocateur
 * ne rend pas. Le delta de RSS est affiché à titre indicatif.
 *
 * Aucune possibilité n'est DÉCODÉE : les enregistrements sont lus et déplacés
 * sous forme compacte (`core/packet_codec.h`), exactement comme dans les pools
 * de stock. La longueur d'un enregistrement se déduit de son bitmap
 * (`packet_codec_peek_placed`), un bloc n'a donc besoin que d'un compte.
 *
 * Vérification : après rechargement, la file doit contenir exactement le même
 * MULTI-ENSEMBLE d'enregistrements que la source (compte + somme de hachages
 * indépendante de l'ordre) — sans quoi la configuration est déclarée FAUSSE et
 * le banc sort en échec.
 *
 * Codecs : `none` toujours ; `lz4` et `zstd-N` seulement si leurs en-têtes
 * sont visibles à la compilation (`__has_include`). Ce banc est le SEUL code du
 * dépôt qui en dépend, et rien de la production ne le lie.
 *
 * Compilation/exécution : `make bench-ram-tier BENCH_RAM_TIER_ARGS="..."`.
 */
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(__GLIBC__)
#error "bench_ram_tier mesure l'allocateur glibc (mallinfo2) : Linux uniquement"
#endif

#include <dirent.h>

#include "core/lifo.h"
#include "core/packet_codec.h"
#include "core/stock_spill.h"
#include "core/backup_stream.h"

#if defined(__has_include)
#if __has_include(<zstd.h>)
#include <zstd.h>
#define BENCH_HAVE_ZSTD 1
#endif
#if __has_include(<lz4.h>)
#include <lz4.h>
#define BENCH_HAVE_LZ4 1
#endif
#endif

/* Octets lisibles avant de connaître la longueur d'un enregistrement. */
#define RECORD_PREFIX (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES)

/* En-tête d'un bloc de l'étage : ce qu'une implémentation devrait porter au
 * minimum. `records` suffit à redécouper le bloc (longueurs déduites du
 * bitmap) ; `raw_bytes` borne la décompression. */
struct tier_block {
	uint32_t records;
	uint32_t raw_bytes;
	uint32_t stored_bytes;
	uint8_t data[];
};

enum codec_kind { CODEC_NONE, CODEC_LZ4, CODEC_ZSTD, CODEC_DELTA_ZSTD };

struct codec {
	enum codec_kind kind;
	int level;
	int sorted;   /* `s…` : enregistrements du bloc triés avant compression */
	char name[16];
};

/* --- Échantillon source ------------------------------------------------- */

struct sample {
	uint8_t *bytes;       /* enregistrements concaténés */
	size_t nbytes;
	size_t count;
	uint64_t hash_sum;    /* somme des hachages : indépendante de l'ordre */
};

static size_t record_len(const uint8_t *rec)
{
	uint16_t placed = packet_codec_peek_placed(rec, RECORD_PREFIX);
	return RECORD_PREFIX + PACKET_CODEC_VALUE_BYTES(placed);
}

static uint64_t record_hash(const uint8_t *rec, size_t len)
{
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < len; i++) {
		h ^= rec[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static int load_sample(const char *path, size_t skip, size_t want, struct sample *s)
{
	// Corps en clair ou compressé (`.back` d'un serveur `make ZSTD=1`, banc
	// compilé avec ZSTD=1) : `backup_stream_fopen_read` rend un flux de
	// lecture des enregistrements dans les deux cas.
	int format = 0;
	FILE *f = backup_stream_fopen_read(path, &format, NULL);
	if (f == NULL || format != BACKUP_STREAM_PACKED) {
		fprintf(stderr, "%s : pas un .back compact de cette géométrie (magie %s attendue)\n",
		        path, PACKET_CODEC_FILE_MAGIC);
		if (f != NULL)
			fclose(f);
		return -1;
	}

	size_t cap = want * 80 + PACKET_CODEC_MAX_BYTES;
	memset(s, 0, sizeof(*s));
	s->bytes = malloc(cap);
	if (s->bytes == NULL) {
		fclose(f);
		return -1;
	}

	uint8_t rec[PACKET_CODEC_MAX_BYTES];
	size_t seen = 0;
	while (s->count < want) {
		size_t got = fread(rec, 1, RECORD_PREFIX, f);
		if (got == 0)
			break;
		if (got != RECORD_PREFIX)
			goto truncated;
		size_t len = record_len(rec);
		if (len > PACKET_CODEC_MAX_BYTES
		    || fread(rec + RECORD_PREFIX, 1, len - RECORD_PREFIX, f) != len - RECORD_PREFIX)
			goto truncated;
		if (seen++ < skip)
			continue;
		if (s->nbytes + len > cap) {
			cap *= 2;
			uint8_t *grown = realloc(s->bytes, cap);
			if (grown == NULL) {
				fclose(f);
				return -1;
			}
			s->bytes = grown;
		}
		memcpy(s->bytes + s->nbytes, rec, len);
		s->nbytes += len;
		s->count++;
		s->hash_sum += record_hash(rec, len);
	}
	fclose(f);
	return 0;

truncated:
	fprintf(stderr, "%s : enregistrement tronqué après %zu possibilités\n", path, seen);
	fclose(f);
	return -1;
}

/* Source --from-spill : les blocs RÉELS d'un débordement de production, lus par
 * le décodeur du débordement (`stock_spill_read_segment`), dans l'ordre où ils
 * ont été écrits — l'ordre de l'étage, frères contigus. Le début d'un `.back`
 * n'a pas cet ordre (listes et étage y passent avant le débordement). */
struct spill_load {
	struct sample *s;
	size_t cap;
	size_t want;
	unsigned long long prod_stored;   /* ce que les trames occupent vraiment */
	unsigned long long prod_records;
};

static int spill_load_frame(const uint8_t *raw, size_t raw_bytes, int records, size_t stored_bytes, void *ctx)
{
	struct spill_load *L = ctx;
	struct sample *s = L->s;
	if (s->count >= L->want)
		return 0;
	if (s->nbytes + raw_bytes > L->cap) {
		size_t cap = (L->cap + raw_bytes) * 2;
		uint8_t *grown = realloc(s->bytes, cap);
		if (grown == NULL)
			return -1;
		s->bytes = grown;
		L->cap = cap;
	}
	const uint8_t *p = raw;
	for (int r = 0; r < records; r++) {
		size_t len = record_len(p);
		s->hash_sum += record_hash(p, len);
		p += len;
	}
	memcpy(s->bytes + s->nbytes, raw, raw_bytes);
	s->nbytes += raw_bytes;
	s->count += (size_t)records;
	L->prod_stored += stored_bytes;
	L->prod_records += (unsigned long long)records;
	return 0;
}

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int load_spill(const char *dir, size_t every, size_t want, struct sample *s,
                      unsigned long long *prod_stored, unsigned long long *prod_records)
{
	DIR *d = opendir(dir);
	if (d == NULL) {
		fprintf(stderr, "%s : répertoire illisible\n", dir);
		return -1;
	}
	size_t n = 0, cap = 1024;
	char **names = malloc(cap * sizeof(*names));
	struct dirent *e;
	while (names != NULL && (e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "spill_", 6) != 0)
			continue;
		if (n == cap) {
			cap *= 2;
			char **grown = realloc(names, cap * sizeof(*names));
			if (grown == NULL)
				break;
			names = grown;
		}
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	if (names == NULL)
		return -1;
	qsort(names, n, sizeof(*names), cmp_name);

	memset(s, 0, sizeof(*s));
	struct spill_load L = { s, 0, want, 0, 0 };
	int rc = 0;
	size_t segments = 0;
	for (size_t i = 0; i < n && s->count < want; i += every) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (stock_spill_read_segment(path, spill_load_frame, &L) != 0) {
			fprintf(stderr, "%s : segment illisible\n", path);
			rc = -1;
			break;
		}
		segments++;
	}
	for (size_t i = 0; i < n; i++)
		free(names[i]);
	free(names);
	*prod_stored = L.prod_stored;
	*prod_records = L.prod_records;
	printf("débordement : %zu segment(s) lus sur %zu (un sur %zu)\n", segments, n, every);
	return rc;
}

/* Mélange l'échantillon (Fisher-Yates, graine fixe) : casse toute localité
 * entre voisins. Le `.back` range les possibilités dans l'ordre des files ;
 * en RAM, la tête d'une file peut entrelacer les dépôts de nombreux clients.
 * Le ratio mesuré sur l'échantillon mélangé est donc un PLANCHER. */
static int shuffle_sample(struct sample *s)
{
	size_t *off = malloc(s->count * sizeof(*off));
	uint8_t *out = malloc(s->nbytes);
	if (off == NULL || out == NULL)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < s->count; i++) {
		off[i] = pos;
		pos += record_len(s->bytes + pos);
	}
	uint64_t x = 0x9E3779B97F4A7C15ULL;
	for (size_t i = s->count - 1; i > 0; i--) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		size_t j = (size_t)(x % (i + 1));
		size_t tmp = off[i]; off[i] = off[j]; off[j] = tmp;
	}
	pos = 0;
	for (size_t i = 0; i < s->count; i++) {
		size_t len = record_len(s->bytes + off[i]);
		memcpy(out + pos, s->bytes + off[i], len);
		pos += len;
	}
	free(off);
	free(s->bytes);
	s->bytes = out;
	return 0;
}

/* Tri GLOBAL de l'échantillon (`--sort CLÉ`, par fenêtres de `--sort-window`
 * possibilités, 0 = tout) : ce que rendrait une remise en ordre d'un stock
 * mélangé, avant de la coder. Clés :
 *  - `raw`   : forme compacte entière (en-tête d'abord : x, y de la case
 *              suivante, checked…) ;
 *  - `board` : bitmap puis valeurs, l'en-tête en dernier ;
 *  - `grid`  : plateau case par case dans l'ordre des lignes, case vide
 *              APRÈS toute valeur. */
enum sort_key { SORT_NONE, SORT_RAW, SORT_BOARD, SORT_GRID };
static enum sort_key g_sort_key;

static unsigned rec_value(const uint8_t *vals, size_t k)
{
	size_t bit = k * PACKET_CODEC_VALUE_BITS;
	uint32_t w = 0;
	for (size_t b = 0; b < 3 && (bit / 8 + b) < PACKET_CODEC_VALUE_BYTES(ETERN_PARTS); b++)
		w |= (uint32_t)vals[bit / 8 + b] << (8 * b);
	return (w >> (bit % 8)) & ((1u << PACKET_CODEC_VALUE_BITS) - 1u);
}

static int cmp_grid(const uint8_t *ra, const uint8_t *rb)
{
	const uint8_t *ba = ra + PACKET_CODEC_HEADER_BYTES, *bb = rb + PACKET_CODEC_HEADER_BYTES;
	const uint8_t *va = ra + RECORD_PREFIX, *vb = rb + RECORD_PREFIX;
	size_t ka = 0, kb = 0;
	for (int c = 0; c < ETERN_PARTS; c++) {
		int fa = (ba[c / 8] >> (c % 8)) & 1, fb = (bb[c / 8] >> (c % 8)) & 1;
		unsigned xa = fa ? rec_value(va, ka++) : PACKET_CODEC_VALUE_MAX + 1u;
		unsigned xb = fb ? rec_value(vb, kb++) : PACKET_CODEC_VALUE_MAX + 1u;
		if (xa != xb)
			return xa < xb ? -1 : 1;
	}
	return memcmp(ra, rb, PACKET_CODEC_HEADER_BYTES);
}

static int cmp_sort_key(const void *a, const void *b)
{
	const uint8_t *ra = *(const uint8_t *const *)a, *rb = *(const uint8_t *const *)b;
	if (g_sort_key == SORT_GRID)
		return cmp_grid(ra, rb);
	size_t la = record_len(ra), lb = record_len(rb);
	if (g_sort_key == SORT_BOARD) {
		size_t h = PACKET_CODEC_HEADER_BYTES;
		int c = memcmp(ra + h, rb + h, (la < lb ? la : lb) - h);
		if (c != 0)
			return c;
		if (la != lb)
			return (la > lb) - (la < lb);
		return memcmp(ra, rb, h);
	}
	int c = memcmp(ra, rb, la < lb ? la : lb);
	return c != 0 ? c : (la > lb) - (la < lb);
}

static int sort_sample(struct sample *s, enum sort_key key, size_t window)
{
	const uint8_t **recs = malloc(s->count * sizeof(*recs));
	uint8_t *out = malloc(s->nbytes);
	if (recs == NULL || out == NULL)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < s->count; i++) {
		recs[i] = s->bytes + pos;
		pos += record_len(s->bytes + pos);
	}
	g_sort_key = key;
	if (window == 0 || window > s->count)
		window = s->count;
	for (size_t i = 0; i < s->count; i += window)
		qsort(recs + i, (s->count - i < window) ? s->count - i : window, sizeof(*recs), cmp_sort_key);
	pos = 0;
	for (size_t i = 0; i < s->count; i++) {
		size_t len = record_len(recs[i]);
		memcpy(out + pos, recs[i], len);
		pos += len;
	}
	free(recs);
	free(s->bytes);
	s->bytes = out;
	return 0;
}

/* --- Mesures ------------------------------------------------------------ */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Octets que l'allocateur tient pour l'appelant : chunks en usage du tas
 * (en-têtes compris) + blocs servis par mmap. */
static size_t heap_in_use(void)
{
	struct mallinfo2 mi = mallinfo2();
	return mi.uordblks + mi.hblkhd;
}

static long rss_bytes(void)
{
	long pages_total = 0, pages_res = 0;
	FILE *f = fopen("/proc/self/statm", "r");
	if (f == NULL)
		return 0;
	if (fscanf(f, "%ld %ld", &pages_total, &pages_res) != 2)
		pages_res = 0;
	fclose(f);
	return pages_res * sysconf(_SC_PAGESIZE);
}

static void fill_file(File *file, const struct sample *s)
{
	init_file_variable(file);
	const uint8_t *p = s->bytes;
	for (size_t i = 0; i < s->count; i++) {
		size_t len = record_len(p);
		if (!put_sized(file, p, len)) {
			fprintf(stderr, "put_sized a échoué à %zu\n", i);
			exit(2);
		}
		p += len;
	}
}

/* --- Codecs ------------------------------------------------------------- */

/*
 * Variante TRIÉE (`szstd-N`, `sdzstd-N`) : les enregistrements d'un bloc sont
 * rangés par ordre lexicographique de leur forme compacte avant compression —
 * même occupation et mêmes premières valeurs côte à côte, donc frères et
 * cousins voisins. Un bloc est une unité atomique ; l'ordre à l'intérieur ne
 * change que l'ordre de service de ses possibilités, et le rechargement le
 * vérifie en multi-ensemble (hachage indépendant de l'ordre).
 */
static int cmp_record(const void *a, const void *b)
{
	const uint8_t *ra = *(const uint8_t *const *)a, *rb = *(const uint8_t *const *)b;
	size_t la = record_len(ra), lb = record_len(rb);
	int c = memcmp(ra, rb, la < lb ? la : lb);
	return c != 0 ? c : (la > lb) - (la < lb);
}

static const uint8_t *sort_block(const uint8_t *raw, size_t n)
{
	static uint8_t *out;
	static const uint8_t **recs;
	static size_t cap_out, cap_recs;
	size_t count = n / RECORD_PREFIX + 1;
	if (n > cap_out) {
		free(out);
		out = malloc(n);
		cap_out = out ? n : 0;
	}
	if (count > cap_recs) {
		free(recs);
		recs = malloc(count * sizeof(*recs));
		cap_recs = recs ? count : 0;
	}
	if (out == NULL || recs == NULL)
		return NULL;
	size_t m = 0;
	for (size_t pos = 0; pos < n; pos += record_len(raw + pos))
		recs[m++] = raw + pos;
	qsort(recs, m, sizeof(*recs), cmp_record);
	size_t w = 0;
	for (size_t i = 0; i < m; i++) {
		size_t len = record_len(recs[i]);
		memcpy(out + w, recs[i], len);
		w += len;
	}
	return out;
}

/*
 * Forme DIFFÉRENTIELLE d'un bloc (`dzstd-N`), candidate de
 * docs/conception/etude_arbre_expansion_profonde.md (PR 3) : deux frères ne
 * diffèrent que d'une case, et un bloc est une unité atomique — l'objection
 * de docs/format_stock_compact.md au différentiel (couplage d'un enregistrement
 * au précédent dans un fichier restaurable partiellement) ne s'y applique pas.
 *
 * Chaque enregistrement est écrit comme son écart au PRÉCÉDENT du bloc (le
 * premier, à un plateau vide) :
 *   u8  drapeaux   bit 0 : l'en-tête (6 octets) change, et suit en clair
 *   u16 n          cases dont la valeur change
 *   n × (u8 case, i16 valeur)   case c = x * ETERN_SIZE + y, valeur -2 = vidée
 * puis le tout passe dans zstd. Le décodage rebâtit la grille et réencode la
 * forme compacte (`packet_codec_encode`) : un enregistrement canonique revient
 * octet pour octet, ce que vérifie le hachage du rechargement.
 */
#define DELTA_RECORD_MAX (1 + PACKET_CODEC_HEADER_BYTES + 2 + 3 * ETERN_PARTS)

static size_t delta_cap(size_t raw)
{
	return (raw / RECORD_PREFIX + 1) * DELTA_RECORD_MAX;
}

static uint8_t *delta_buf(size_t need)
{
	static uint8_t *buf;
	static size_t cap;
	if (need > cap) {
		free(buf);
		buf = malloc(need);
		cap = (buf != NULL) ? need : 0;
	}
	return buf;
}

/* Écart moyen mesuré : enregistrements, cases changées, en-têtes changés. */
static unsigned long long g_delta_records, g_delta_changes, g_delta_headers;

static size_t delta_encode(const uint8_t *raw, size_t n, uint8_t *out)
{
	int16_t prev[ETERN_PARTS];
	uint8_t prev_h[PACKET_CODEC_HEADER_BYTES] = { 0 };
	for (int c = 0; c < ETERN_PARTS; c++)
		prev[c] = -2;
	size_t pos = 0, w = 0;
	while (pos < n) {
		struct possibility_packet pk;
		size_t used = 0;
		if (packet_codec_decode(raw + pos, n - pos, &pk, &used) != 0)
			return 0;
		const uint8_t *h = raw + pos;
		int hdr = memcmp(h, prev_h, PACKET_CODEC_HEADER_BYTES) != 0;
		out[w++] = (uint8_t)hdr;
		if (hdr) {
			memcpy(out + w, h, PACKET_CODEC_HEADER_BYTES);
			memcpy(prev_h, h, PACKET_CODEC_HEADER_BYTES);
			w += PACKET_CODEC_HEADER_BYTES;
		}
		size_t count_at = w;
		w += 2;
		uint16_t changes = 0;
		for (int c = 0; c < ETERN_PARTS; c++) {
			int16_t v = pk.grid[c / ETERN_SIZE][c % ETERN_SIZE];
			if (v == prev[c])
				continue;
			out[w++] = (uint8_t)c;
			out[w++] = (uint8_t)((uint16_t)v & 0xFF);
			out[w++] = (uint8_t)((uint16_t)v >> 8);
			prev[c] = v;
			changes++;
		}
		g_delta_records++;
		g_delta_changes += changes;
		g_delta_headers += (unsigned long long)hdr;
		out[count_at] = (uint8_t)(changes & 0xFF);
		out[count_at + 1] = (uint8_t)(changes >> 8);
		pos += used;
	}
	return w;
}

/* Renvoie les octets bruts rebâtis, ou 0 si le flux est incohérent. */
static size_t delta_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	struct possibility_packet pk;
	memset(&pk, 0, sizeof(pk));
	for (int c = 0; c < ETERN_PARTS; c++)
		pk.grid[c / ETERN_SIZE][c % ETERN_SIZE] = -2;
	uint8_t h[PACKET_CODEC_HEADER_BYTES] = { 0 };
	size_t r = 0, w = 0;
	while (r < n) {
		uint8_t flags = in[r++];
		if (flags & 1) {
			if (r + PACKET_CODEC_HEADER_BYTES > n)
				return 0;
			memcpy(h, in + r, PACKET_CODEC_HEADER_BYTES);
			r += PACKET_CODEC_HEADER_BYTES;
		}
		if (r + 2 > n)
			return 0;
		uint16_t changes = (uint16_t)(in[r] | (in[r + 1] << 8));
		r += 2;
		if (r + 3 * (size_t)changes > n)
			return 0;
		for (uint16_t k = 0; k < changes; k++, r += 3) {
			int c = in[r];
			if (c >= ETERN_PARTS)
				return 0;
			pk.grid[c / ETERN_SIZE][c % ETERN_SIZE] = (int16_t)(uint16_t)(in[r + 1] | (in[r + 2] << 8));
		}
		pk.x = h[0];
		pk.y = h[1];
		pk.checked = h[2];
		pk.min_candidats = (int16_t)(uint16_t)(h[4] | (h[5] << 8));
		size_t written = 0;
		if (packet_codec_encode(&pk, out + w, cap - w, &written) != 0)
			return 0;
		w += written;
	}
	return w;
}

static size_t codec_bound(const struct codec *c, size_t raw)
{
	switch (c->kind) {
#ifdef BENCH_HAVE_LZ4
	case CODEC_LZ4: return (size_t)LZ4_compressBound((int)raw);
#endif
#ifdef BENCH_HAVE_ZSTD
	case CODEC_ZSTD: return ZSTD_compressBound(raw);
	case CODEC_DELTA_ZSTD: return ZSTD_compressBound(delta_cap(raw));
#endif
	default: return raw;
	}
}

/* Renvoie la taille compressée, 0 sur échec. */
static size_t codec_compress(const struct codec *c, void *cctx, const uint8_t *in, size_t n,
                             uint8_t *out, size_t cap)
{
	(void)cctx;
	(void)cap;
	if (c->sorted && (in = sort_block(in, n)) == NULL)
		return 0;
	switch (c->kind) {
	case CODEC_NONE:
		memcpy(out, in, n);
		return n;
#ifdef BENCH_HAVE_LZ4
	case CODEC_LZ4: {
		int r = LZ4_compress_default((const char *)in, (char *)out, (int)n, (int)cap);
		return r > 0 ? (size_t)r : 0;
	}
#endif
#ifdef BENCH_HAVE_ZSTD
	case CODEC_ZSTD: {
		size_t r = ZSTD_compressCCtx(cctx, out, cap, in, n, c->level);
		return ZSTD_isError(r) ? 0 : r;
	}
	case CODEC_DELTA_ZSTD: {
		uint8_t *d = delta_buf(delta_cap(n));
		size_t dn = (d != NULL) ? delta_encode(in, n, d) : 0;
		if (dn == 0)
			return 0;
		size_t r = ZSTD_compressCCtx(cctx, out, cap, d, dn, c->level);
		return ZSTD_isError(r) ? 0 : r;
	}
#endif
	default:
		return 0;
	}
}

static int codec_decompress(const struct codec *c, void *dctx, const uint8_t *in, size_t n,
                            uint8_t *out, size_t raw)
{
	(void)dctx;
	switch (c->kind) {
	case CODEC_NONE:
		memcpy(out, in, n);
		return n == raw ? 0 : -1;
#ifdef BENCH_HAVE_LZ4
	case CODEC_LZ4:
		return LZ4_decompress_safe((const char *)in, (char *)out, (int)n, (int)raw) == (int)raw ? 0 : -1;
#endif
#ifdef BENCH_HAVE_ZSTD
	case CODEC_ZSTD: {
		size_t r = ZSTD_decompressDCtx(dctx, out, raw, in, n);
		return (!ZSTD_isError(r) && r == raw) ? 0 : -1;
	}
	case CODEC_DELTA_ZSTD: {
		size_t cap = delta_cap(raw);
		uint8_t *d = delta_buf(cap);
		if (d == NULL)
			return -1;
		size_t dn = ZSTD_decompressDCtx(dctx, d, cap, in, n);
		if (ZSTD_isError(dn))
			return -1;
		return delta_decode(d, dn, out, raw) == raw ? 0 : -1;
	}
#endif
	default:
		return -1;
	}
}

static int parse_codec(const char *name, struct codec *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->name, sizeof(c->name), "%s", name);
	if (strcmp(name, "none") == 0) {
		c->kind = CODEC_NONE;
		return 0;
	}
	if (strcmp(name, "lz4") == 0) {
#ifdef BENCH_HAVE_LZ4
		c->kind = CODEC_LZ4;
		return 0;
#else
		return -1;
#endif
	}
	if (name[0] == 's' && (strncmp(name + 1, "zstd-", 5) == 0 || strncmp(name + 1, "dzstd-", 6) == 0)) {
		int rc = parse_codec(name + 1, c);
		snprintf(c->name, sizeof(c->name), "%s", name);
		c->sorted = 1;
		return rc;
	}
	if (strncmp(name, "dzstd-", 6) == 0) {
#ifdef BENCH_HAVE_ZSTD
		c->kind = CODEC_DELTA_ZSTD;
		c->level = atoi(name + 6);
		return 0;
#else
		return -1;
#endif
	}
	if (strncmp(name, "zstd-", 5) == 0) {
#ifdef BENCH_HAVE_ZSTD
		c->kind = CODEC_ZSTD;
		c->level = atoi(name + 5);
		return 0;
#else
		return -1;
#endif
	}
	return -1;
}

/* --- Une configuration, dans un fils ------------------------------------ */

/* Imprime la ligne de référence et renvoie ses octets/possibilité. */
static double run_list_baseline(const struct sample *s)
{
	size_t heap0 = heap_in_use();
	long rss0 = rss_bytes();
	File file;
	double t0 = now_s();
	fill_file(&file, s);
	double t1 = now_s();
	size_t heap = heap_in_use() - heap0;
	long rss = rss_bytes() - rss0;
	printf("%-10s %8s %9.1f %9.1f %8s %10s %10.2f %10s   (RSS : %.1f oct/poss, insertion)\n",
	       "liste", "-", (double)heap / (double)s->count, (double)s->nbytes / (double)s->count,
	       "x1.00", "-", (double)s->count / (t1 - t0) / 1e6, "-", (double)rss / (double)s->count);
	fflush(stdout);
	return (double)heap / (double)s->count;
}

static int run_config(const struct sample *s, const struct codec *c, size_t block_bytes,
                      double list_bpp)
{
	File file;
	fill_file(&file, s);
	malloc_trim(0);

	size_t nblocks_cap = s->nbytes / (block_bytes - PACKET_CODEC_MAX_BYTES) + 16;
	struct tier_block **blocks = calloc(nblocks_cap, sizeof(*blocks));
	uint8_t *raw = malloc(block_bytes);
	size_t bound = codec_bound(c, block_bytes);
	uint8_t *scratch = malloc(bound);
	if (blocks == NULL || raw == NULL || scratch == NULL)
		return 2;
	void *cctx = NULL, *dctx = NULL;
#ifdef BENCH_HAVE_ZSTD
	if (c->kind == CODEC_ZSTD || c->kind == CODEC_DELTA_ZSTD) {
		cctx = ZSTD_createCCtx();
		dctx = ZSTD_createDCtx();
	}
#endif

	/* Éviction : tête de file (la plus froide) -> blocs. */
	size_t nblocks = 0, stored_total = 0;
	double t0 = now_s();
	size_t pending_len = 0;
	uint8_t pending[PACKET_CODEC_MAX_BYTES];
	int have_pending = 0;
	while (have_pending || file.size > 0) {
		size_t used = 0;
		uint32_t recs = 0;
		for (;;) {
			if (!have_pending) {
				if (file.size == 0)
					break;
				if (!scroll_fifo_sized(&file, pending, sizeof(pending), &pending_len))
					return 2;
				have_pending = 1;
			}
			if (used + pending_len > block_bytes)
				break;
			memcpy(raw + used, pending, pending_len);
			used += pending_len;
			recs++;
			have_pending = 0;
		}
		size_t clen = codec_compress(c, cctx, raw, used, scratch, bound);
		if (clen == 0) {
			fprintf(stderr, "%s : compression en échec\n", c->name);
			return 2;
		}
		struct tier_block *b = malloc(sizeof(*b) + clen);
		if (b == NULL || nblocks == nblocks_cap)
			return 2;
		b->records = recs;
		b->raw_bytes = (uint32_t)used;
		b->stored_bytes = (uint32_t)clen;
		memcpy(b->data, scratch, clen);
		blocks[nblocks++] = b;
		stored_total += clen;
	}
	double t1 = now_s();

	/* Ce que les blocs coûtent à l'allocateur : taille utilisable de chaque
	 * chunk + son en-tête, plus le tableau qui les indexe. Même comptabilité
	 * que `mallinfo2` pour la liste (qui, elle, ne peut se mesurer que par
	 * différence : ses maillons sont trop nombreux pour être sommés). */
	size_t heap_tier = nblocks * sizeof(*blocks);
	for (size_t i = 0; i < nblocks; i++)
		heap_tier += malloc_usable_size(blocks[i]) + sizeof(size_t);

	/* Rechargement : bloc le plus récent d'abord, réinsertion en file. */
	init_file_variable(&file);
	uint64_t hash_sum = 0;
	double t2 = now_s();
	for (size_t i = nblocks; i-- > 0;) {
		struct tier_block *b = blocks[i];
		if (codec_decompress(c, dctx, b->data, b->stored_bytes, raw, b->raw_bytes) != 0) {
			fprintf(stderr, "%s : décompression en échec (bloc %zu)\n", c->name, i);
			return 2;
		}
		const uint8_t *p = raw;
		for (uint32_t r = 0; r < b->records; r++) {
			size_t len = record_len(p);
			if (p + len > raw + b->raw_bytes || !put_sized(&file, p, len)) {
				fprintf(stderr, "%s/%zu : FAUX — bloc %zu illisible à l'enregistrement %u\n",
				        c->name, block_bytes, i, r);
				return 3;
			}
			hash_sum += record_hash(p, len);
			p += len;
		}
		free(b);
	}
	double t3 = now_s();

	if (file.size != s->count || hash_sum != s->hash_sum) {
		fprintf(stderr, "%s/%zu : FAUX — %llu possibilités rechargées sur %zu, hachage %s\n",
		        c->name, block_bytes, file.size, s->count,
		        hash_sum == s->hash_sum ? "égal" : "DIFFÉRENT");
		return 3;
	}

	double bpp = (double)heap_tier / (double)s->count;
	char ratio[16];
	snprintf(ratio, sizeof(ratio), "x%.2f", list_bpp / bpp);
	printf("%-10s %7zuK %9.1f %9.1f %8s %10.2f %10.2f %10.2f\n", c->name, block_bytes >> 10,
	       bpp, (double)stored_total / (double)s->count, ratio,
	       (double)s->count / (t1 - t0) / 1e6,
	       (double)s->count / (t3 - t2) / 1e6,
	       (double)s->nbytes / (double)stored_total);
	if (c->kind == CODEC_DELTA_ZSTD && g_delta_records > 0)
		printf("%-10s écart au précédent : %.2f case(s) changée(s) par possibilité, en-tête changé %.1f %%\n", "",
		       (double)g_delta_changes / (double)g_delta_records,
		       100.0 * (double)g_delta_headers / (double)g_delta_records);
	fflush(stdout);
#ifdef BENCH_HAVE_ZSTD
	if (cctx) ZSTD_freeCCtx(cctx);
	if (dctx) ZSTD_freeDCtx(dctx);
#endif
	return 0;
}

static int in_child(const struct sample *s, const struct codec *c, size_t bs, double list_bpp)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 2;
	}
	if (pid == 0)
		exit(run_config(s, c, bs, list_bpp));
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
}

/* Mesure la liste dans un fils et remonte ses octets/possibilité par un
 * tube, pour que le ratio des blocs se lise contre la VRAIE liste. */
static double measure_list_bpp(const struct sample *s)
{
	int fds[2];
	if (pipe(fds) != 0)
		return 0;
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		double bpp = run_list_baseline(s);
		ssize_t w = write(fds[1], &bpp, sizeof(bpp));
		exit(w == (ssize_t)sizeof(bpp) ? 0 : 2);
	}
	close(fds[1]);
	double bpp = 0;
	if (read(fds[0], &bpp, sizeof(bpp)) != (ssize_t)sizeof(bpp))
		bpp = 0;
	close(fds[0]);
	waitpid(pid, NULL, 0);
	return bpp;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
	        "usage : %s [--count N] [--skip N] [--shuffle] [--blocks 16,64,256] [--codecs none,lz4,zstd-1,zstd-3] fichier.back\n"
	        "        %s --from-spill RÉPERTOIRE [--every K] [--count N] [...] : blocs réels d'un débordement\n"
	        "codecs compilés :%s%s none\n",
	        argv0, argv0,
#ifdef BENCH_HAVE_LZ4
	        " lz4",
#else
	        "",
#endif
#ifdef BENCH_HAVE_ZSTD
	        " zstd-N dzstd-N szstd-N sdzstd-N"
#else
	        ""
#endif
	);
}

int main(int argc, char **argv)
{
	size_t count = 2000000, skip = 0;
	int shuffle = 0;
	enum sort_key sort_key = SORT_NONE;
	size_t sort_window = 0;
	const char *blocks_arg = "16,64,256";
	const char *codecs_arg = "none"
#ifdef BENCH_HAVE_LZ4
	                         ",lz4"
#endif
#ifdef BENCH_HAVE_ZSTD
	                         ",zstd-1,zstd-3"
#endif
	                         ;
	const char *path = NULL;
	const char *spill_dir = NULL;
	size_t every = 1;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
			count = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--skip") == 0 && i + 1 < argc)
			skip = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--shuffle") == 0)
			shuffle = 1;
		else if (strcmp(argv[i], "--sort") == 0 && i + 1 < argc) {
			const char *k = argv[++i];
			sort_key = strcmp(k, "raw") == 0 ? SORT_RAW : strcmp(k, "board") == 0 ? SORT_BOARD
			         : strcmp(k, "grid") == 0 ? SORT_GRID : SORT_NONE;
			if (sort_key == SORT_NONE) {
				usage(argv[0]);
				return 1;
			}
		} else if (strcmp(argv[i], "--sort-window") == 0 && i + 1 < argc)
			sort_window = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc)
			blocks_arg = argv[++i];
		else if (strcmp(argv[i], "--codecs") == 0 && i + 1 < argc)
			codecs_arg = argv[++i];
		else if (strcmp(argv[i], "--from-spill") == 0 && i + 1 < argc)
			spill_dir = argv[++i];
		else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc)
			every = strtoull(argv[++i], NULL, 10);
		else if (argv[i][0] != '-' && path == NULL)
			path = argv[i];
		else {
			usage(argv[0]);
			return 1;
		}
	}
	if ((path == NULL) == (spill_dir == NULL) || count == 0 || every == 0) {
		usage(argv[0]);
		return 1;
	}

	struct sample s;
	double t0 = now_s();
	unsigned long long prod_stored = 0, prod_records = 0;
	if (spill_dir != NULL) {
		if (load_spill(spill_dir, every, count, &s, &prod_stored, &prod_records) != 0)
			return 1;
	} else if (load_sample(path, skip, count, &s) != 0)
		return 1;
	if (s.count == 0) {
		fprintf(stderr, "%s : aucune possibilité après en avoir sauté %zu\n", path, skip);
		return 1;
	}
	if (shuffle && shuffle_sample(&s) != 0)
		return 2;
	if (sort_key != SORT_NONE) {
		double ts = now_s();
		if (sort_sample(&s, sort_key, sort_window) != 0)
			return 2;
		printf("tri global (clé %s, fenêtre %zu) en %.1f s\n",
		       sort_key == SORT_RAW ? "raw" : sort_key == SORT_BOARD ? "board" : "grid", sort_window, now_s() - ts);
	}
	printf("échantillon : %zu possibilités (après %zu sautées%s), %.1f octets compacts en moyenne, lu en %.1f s\n",
	       s.count, skip, shuffle ? ", MÉLANGÉES" : "", (double)s.nbytes / (double)s.count,
	       now_s() - t0);
	if (prod_records > 0)
		printf("trames de production lues : %.2f octets stockés par possibilité (codec de l'étage, blocs réels)\n",
		       (double)prod_stored / (double)prod_records);
	printf("oct/poss = octets tenus par l'allocateur (chunks + en-têtes) ; stockés = charge utile seule\n");
	printf("éviction/rechargement en millions de possibilités/s ; ratio codec = octets compacts / octets stockés\n\n");
	printf("%-10s %8s %9s %9s %8s %10s %10s %10s\n", "forme", "bloc", "oct/poss", "stockés",
	       "vs liste", "éviction", "recharge", "ratio codec");

	fflush(stdout);
	int rc;
	double list_bpp = measure_list_bpp(&s);
	if (list_bpp <= 0)
		return 2;

	char *codecs = strdup(codecs_arg);
	for (char *cn = strtok(codecs, ","); cn != NULL; cn = strtok(NULL, ",")) {
		struct codec c;
		if (parse_codec(cn, &c) != 0) {
			fprintf(stderr, "codec « %s » inconnu ou non compilé\n", cn);
			usage(argv[0]);
			return 1;
		}
		char *blist = strdup(blocks_arg);
		char *save = NULL;
		for (char *bn = strtok_r(blist, ",", &save); bn != NULL; bn = strtok_r(NULL, ",", &save)) {
			size_t bs = strtoull(bn, NULL, 10) << 10;
			if (bs < 2 * PACKET_CODEC_MAX_BYTES) {
				fprintf(stderr, "bloc de %s Kio trop petit\n", bn);
				return 1;
			}
			rc = in_child(&s, &c, bs, list_bpp);
			if (rc != 0)
				return rc;
		}
		free(blist);
	}
	free(codecs);
	free(s.bytes);
	return 0;
}
