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

#include "core/lifo.h"
#include "core/packet_codec.h"

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

enum codec_kind { CODEC_NONE, CODEC_LZ4, CODEC_ZSTD };

struct codec {
	enum codec_kind kind;
	int level;
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
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	static char iobuf[1 << 20];
	setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

	uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
	if (fread(header, 1, sizeof(header), f) != sizeof(header)
	    || packet_codec_read_file_header(header) != 0) {
		fprintf(stderr, "%s : pas un .back compact de cette géométrie (magie %s attendue)\n",
		        path, PACKET_CODEC_FILE_MAGIC);
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

static size_t codec_bound(const struct codec *c, size_t raw)
{
	switch (c->kind) {
#ifdef BENCH_HAVE_LZ4
	case CODEC_LZ4: return (size_t)LZ4_compressBound((int)raw);
#endif
#ifdef BENCH_HAVE_ZSTD
	case CODEC_ZSTD: return ZSTD_compressBound(raw);
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
	if (c->kind == CODEC_ZSTD) {
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
	        "codecs compilés :%s%s none\n",
	        argv0,
#ifdef BENCH_HAVE_LZ4
	        " lz4",
#else
	        "",
#endif
#ifdef BENCH_HAVE_ZSTD
	        " zstd-N"
#else
	        ""
#endif
	);
}

int main(int argc, char **argv)
{
	size_t count = 2000000, skip = 0;
	int shuffle = 0;
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
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
			count = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--skip") == 0 && i + 1 < argc)
			skip = strtoull(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "--shuffle") == 0)
			shuffle = 1;
		else if (strcmp(argv[i], "--blocks") == 0 && i + 1 < argc)
			blocks_arg = argv[++i];
		else if (strcmp(argv[i], "--codecs") == 0 && i + 1 < argc)
			codecs_arg = argv[++i];
		else if (argv[i][0] != '-' && path == NULL)
			path = argv[i];
		else {
			usage(argv[0]);
			return 1;
		}
	}
	if (path == NULL || count == 0) {
		usage(argv[0]);
		return 1;
	}

	struct sample s;
	double t0 = now_s();
	if (load_sample(path, skip, count, &s) != 0)
		return 1;
	if (s.count == 0) {
		fprintf(stderr, "%s : aucune possibilité après en avoir sauté %zu\n", path, skip);
		return 1;
	}
	if (shuffle && shuffle_sample(&s) != 0)
		return 2;
	printf("échantillon : %zu possibilités (après %zu sautées%s), %.1f octets compacts en moyenne, lu en %.1f s\n",
	       s.count, skip, shuffle ? ", MÉLANGÉES" : "", (double)s.nbytes / (double)s.count,
	       now_s() - t0);
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
