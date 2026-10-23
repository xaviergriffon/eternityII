#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#ifdef ETII_ZSTD
#include <zstd.h>
#endif

#include "core/backup_sort.h"
#include "core/backup_stream.h"
#include "core/packet_codec.h"
#include "core/possibility.h"
#include "ui/logger.h"

#define SORT_PREFIX (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES)
/// Octets décompressés demandés à l'entrée par lecture : borne aussi le reste
/// reporté d'un tampon au suivant.
#define SORT_READ_CHUNK (1 << 20)
/// Bloc d'un paquet : unité de compression ET de reprise de la fusion. Un fil
/// de fusion tient un bloc décompressé par paquet.
#define RUN_BLOCK_RAW (64 * 1024)
/// Bloc (trame zstd) d'une partie de la sortie.
#define OUT_BLOCK_RAW (1024 * 1024)
/// Préfixe de clé gardé dans l'index d'un paquet et dans les bornes de plage.
#define KEY_PFX 128
/// Entrée d'index : offset u64, stored u32, raw u32, records u32, codec u16,
/// longueur de clé u16, clé.
#define IDX_BYTES (8 + 4 + 4 + 4 + 2 + 2 + KEY_PFX)
/// Plages de fusion par fil (équilibrage).
#define RANGES_PER_THREAD 4

enum { CODEC_RAW = 0, CODEC_ZSTD = 1 };

static double sort_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static size_t rec_len(const uint8_t *rec)
{
	return SORT_PREFIX + PACKET_CODEC_VALUE_BYTES(packet_codec_peek_placed(rec, SORT_PREFIX));
}

int backup_sort_record_cmp(const uint8_t *a, size_t la, const uint8_t *b, size_t lb)
{
	size_t h = PACKET_CODEC_HEADER_BYTES;
	size_t n = (la < lb ? la : lb);
	int c = (n > h) ? memcmp(a + h, b + h, n - h) : 0;
	if (c != 0) {
		return c;
	}
	if (la != lb) {
		return (la < lb) ? -1 : 1;
	}
	return memcmp(a, b, h < la ? h : la);
}

/**
 * Clé TRONQUÉE d'un enregistrement : ses `KEY_PFX` premiers octets de clé
 * (bitmap puis valeurs, sans l'en-tête). L'ordre des clés tronquées (octets
 * communs, puis la plus courte d'abord) est MONOTONE avec l'ordre complet :
 * x <= y entraîne T(x) <= T(y). Des plages définies sur T gardent donc
 * ensemble tout ce qui est égal, et une fusion par plages reste exacte.
 */
static size_t tkey(const uint8_t *rec, size_t len, const uint8_t **out)
{
	size_t h = PACKET_CODEC_HEADER_BYTES;
	*out = rec + h;
	size_t k = (len > h) ? len - h : 0;
	return (k > KEY_PFX) ? KEY_PFX : k;
}

static int tkey_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
	size_t m = (an < bn) ? an : bn;
	int c = memcmp(a, b, m);
	if (c != 0) {
		return c;
	}
	return (an < bn) ? -1 : (an > bn) ? 1 : 0;
}

unsigned long long backup_sort_raise_open_files(void)
{
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
		return 0;
	}
	if (rl.rlim_cur < rl.rlim_max) {
		struct rlimit up = rl;
		up.rlim_cur = rl.rlim_max;
		if (setrlimit(RLIMIT_NOFILE, &up) == 0) {
			rl = up;
		}
	}
	return (rl.rlim_cur == RLIM_INFINITY) ? ~0ULL : (unsigned long long)rl.rlim_cur;
}

int backup_sort_auto_threads(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if (n < 1) {
		n = 1;
	}
	return (n > BACKUP_SORT_THREADS_AUTO_MAX) ? BACKUP_SORT_THREADS_AUTO_MAX : (int)n;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
	return v;
}

/* --- Compression de blocs indépendants ------------------------------------------- */

typedef struct {
#ifdef ETII_ZSTD
	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;
#endif
	int unused;
} codec_ctx_t;

static void codec_ctx_free(codec_ctx_t *c)
{
#ifdef ETII_ZSTD
	ZSTD_freeCCtx(c->cctx);
	ZSTD_freeDCtx(c->dctx);
	c->cctx = NULL;
	c->dctx = NULL;
#endif
	(void)c;
}

static size_t block_bound(size_t raw)
{
#ifdef ETII_ZSTD
	return ZSTD_compressBound(raw);
#else
	return raw;
#endif
}

/// Compresse `raw` dans `dst` (au moins `block_bound(n)`) ; garde le brut si
/// zstd ne le réduit pas — sauf `force_zstd`. Rend la taille stockée, et le
/// codec dans `*codec`.
static size_t block_pack_ex(codec_ctx_t *c, const uint8_t *raw, size_t n, uint8_t *dst, int *codec, int force_zstd)
{
#ifdef ETII_ZSTD
	if (c->cctx == NULL) {
		c->cctx = ZSTD_createCCtx();
		if (c->cctx != NULL) {
			ZSTD_CCtx_setParameter(c->cctx, ZSTD_c_compressionLevel, BACKUP_STREAM_ZSTD_LEVEL);
			ZSTD_CCtx_setParameter(c->cctx, ZSTD_c_checksumFlag, 1);
		}
	}
	if (c->cctx != NULL) {
		size_t r = ZSTD_compress2(c->cctx, dst, ZSTD_compressBound(n), raw, n);
		if (!ZSTD_isError(r) && (r < n || force_zstd)) {
			*codec = CODEC_ZSTD;
			return r;
		}
	}
#endif
	(void)c;
	(void)force_zstd;
	memcpy(dst, raw, n);
	*codec = CODEC_RAW;
	return n;
}

static size_t block_pack(codec_ctx_t *c, const uint8_t *raw, size_t n, uint8_t *dst, int *codec)
{
	return block_pack_ex(c, raw, n, dst, codec, 0);
}

static int block_unpack(codec_ctx_t *c, int codec, const uint8_t *src, size_t stored, uint8_t *raw, size_t raw_len)
{
	if (codec == CODEC_RAW) {
		if (stored != raw_len) {
			return -1;
		}
		memcpy(raw, src, raw_len);
		return 0;
	}
#ifdef ETII_ZSTD
	if (c->dctx == NULL && (c->dctx = ZSTD_createDCtx()) == NULL) {
		return -1;
	}
	size_t r = ZSTD_decompressDCtx(c->dctx, raw, raw_len, src, stored);
	return (!ZSTD_isError(r) && r == raw_len) ? 0 : -1;
#else
	(void)c;
	return -1;
#endif
}

/* --- Lecture de l'entrée ----------------------------------------------------------- */

#ifdef ETII_ZSTD
/// Décompression PARALLÈLE du corps zstd. Le corps d'un `.back` est une suite
/// de trames (blocs de l'étage et du débordement recopiés tels quels, mesuré :
/// ~25 Ko décompressés en moyenne sur le niveau 27) : un fil répartiteur les
/// découpe, des fils les décompressent, et le lecteur les reprend DANS L'ORDRE
/// — exactement le flux séquentiel, sans rien supposer de l'alignement des
/// trames sur les enregistrements. Une trame sans taille annoncée ou de plus
/// de `PIN_FRAME_MAX` est décompressée au fil de l'eau par le répartiteur.
#define PIN_WINDOW (64u << 20)
#define PIN_FRAME_MAX (16u << 20)
#define PIN_CHUNK (4u << 20)

typedef struct {
	uint8_t *src;    ///< Trame compressée à décompresser.
	size_t src_len, src_cap;
	uint8_t *out;    ///< Contenu décompressé. Tampons gardés d'un usage à l'autre.
	size_t out_len, out_cap;
	int state;       ///< 0 libre, 1 à décompresser, 2 en cours, 3 prêt, 4 fin, -1 erreur.
} pin_slot_t;

typedef struct {
	FILE *raw;
	int dispatcher_on;
	pthread_t dispatcher;
	pthread_t *workers;
	int nworkers;
	pthread_mutex_t lock;
	pthread_cond_t space;  ///< Répartiteur : un créneau s'est libéré.
	pthread_cond_t work;   ///< Fils : une trame attend.
	pthread_cond_t ready;  ///< Lecteur : le créneau suivant est prêt.
	pin_slot_t *ring;
	size_t ring_n;
	uint64_t produced;   ///< Prochain numéro de trame à produire.
	uint64_t consumed;   ///< Prochain numéro à lire.
	uint64_t pending;    ///< Prochain numéro à décompresser.
	int stop;
	size_t pos;          ///< Lecture dans le créneau courant.
} pin_t;

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
	if (*cap >= need) {
		return 0;
	}
	size_t c = (need < 65536) ? 65536 : need;
	uint8_t *p = realloc(*buf, c);
	if (p == NULL) {
		return -1;
	}
	*buf = p;
	*cap = c;
	return 0;
}

/// Prend le créneau du prochain numéro à produire (attend qu'il soit libre).
static pin_slot_t *pin_reserve(pin_t *p)
{
	pthread_mutex_lock(&p->lock);
	while (!p->stop && p->produced - p->consumed >= p->ring_n) {
		pthread_cond_wait(&p->space, &p->lock);
	}
	pin_slot_t *s = p->stop ? NULL : &p->ring[p->produced % p->ring_n];
	pthread_mutex_unlock(&p->lock);
	return s;
}

/// Publie le créneau réservé dans l'état `state` (1 : à décompresser).
static void pin_publish(pin_t *p, pin_slot_t *s, int state)
{
	pthread_mutex_lock(&p->lock);
	s->state = state;
	p->produced++;
	if (state == 1) {
		pthread_cond_signal(&p->work);
	} else {
		pthread_cond_signal(&p->ready);
	}
	pthread_mutex_unlock(&p->lock);
}

static void *pin_worker_main(void *arg)
{
	pin_t *p = arg;
	ZSTD_DCtx *d = ZSTD_createDCtx();
	pthread_mutex_lock(&p->lock);
	for (;;) {
		// Les créneaux prêts dès le répartiteur (trames décompressées au fil de
		// l'eau) ou déjà lus ne sont pas à prendre.
		while (!p->stop) {
			if (p->pending < p->consumed) {
				p->pending = p->consumed;
			}
			if (p->pending < p->produced && p->ring[p->pending % p->ring_n].state != 1) {
				p->pending++;
				continue;
			}
			if (p->pending < p->produced) {
				break;
			}
			pthread_cond_wait(&p->work, &p->lock);
		}
		if (p->stop) {
			break;
		}
		uint64_t seq = p->pending++;
		pin_slot_t *s = &p->ring[seq % p->ring_n];
		s->state = 2;
		if (p->pending < p->produced) {
			pthread_cond_signal(&p->work); // il en reste : un autre fil
		}
		pthread_mutex_unlock(&p->lock);
		size_t r = (d == NULL) ? (size_t)-1 : ZSTD_decompressDCtx(d, s->out, s->out_len, s->src, s->src_len);
		int ok = (d != NULL && !ZSTD_isError(r) && r == s->out_len);
		pthread_mutex_lock(&p->lock);
		s->state = ok ? 3 : -1;
		if (seq == p->consumed) {
			pthread_cond_signal(&p->ready);
		}
	}
	pthread_mutex_unlock(&p->lock);
	ZSTD_freeDCtx(d);
	return NULL;
}

static void *pin_dispatch_main(void *arg)
{
	pin_t *p = arg;
	uint8_t *win = malloc(PIN_WINDOW);
	ZSTD_DStream *ds = ZSTD_createDStream();
	size_t len = 0, pos = 0;
	int eof = 0, err = (win == NULL || ds == NULL);
	while (!err) {
		if (!eof && len - pos < PIN_WINDOW / 2) {
			memmove(win, win + pos, len - pos);
			len -= pos;
			pos = 0;
			size_t got = fread(win + len, 1, PIN_WINDOW - len, p->raw);
			len += got;
			if (got == 0) {
				if (ferror(p->raw)) {
					err = 1;
					break;
				}
				eof = 1;
			}
		}
		if (pos == len) {
			break; // fin propre
		}
		unsigned long long cs = ZSTD_getFrameContentSize(win + pos, len - pos);
		size_t fs = ZSTD_findFrameCompressedSize(win + pos, len - pos);
		if (cs != ZSTD_CONTENTSIZE_UNKNOWN && cs != ZSTD_CONTENTSIZE_ERROR && cs <= PIN_FRAME_MAX
		    && !ZSTD_isError(fs)) {
			pin_slot_t *s = pin_reserve(p);
			if (s == NULL || grow(&s->src, &s->src_cap, fs) != 0 || grow(&s->out, &s->out_cap, (size_t)cs) != 0) {
				err = 1;
				break;
			}
			memcpy(s->src, win + pos, fs);
			s->src_len = fs;
			s->out_len = (size_t)cs;
			pos += fs;
			pin_publish(p, s, 1);
			continue;
		}
		if (cs == ZSTD_CONTENTSIZE_ERROR) {
			err = 1;
			break;
		}
		// Trame sans taille annoncée, trop grande, ou pas entière dans la
		// fenêtre : décompressée ici, au fil de l'eau, par morceaux.
		ZSTD_initDStream(ds);
		size_t ret = 1;
		while (!err && ret != 0) {
			pin_slot_t *s = pin_reserve(p);
			if (s == NULL || grow(&s->out, &s->out_cap, PIN_CHUNK) != 0) {
				err = 1;
				break;
			}
			ZSTD_outBuffer o = { s->out, PIN_CHUNK, 0 };
			while (o.pos < o.size && ret != 0) {
				if (pos == len) {
					if (eof) {
						err = 1; // trame tronquée
						break;
					}
					len = fread(win, 1, PIN_WINDOW, p->raw);
					pos = 0;
					if (len == 0) {
						eof = 1;
						if (ferror(p->raw)) {
							err = 1;
							break;
						}
						continue;
					}
				}
				ZSTD_inBuffer in = { win, len, pos };
				ret = ZSTD_decompressStream(ds, &o, &in);
				pos = in.pos;
				if (ZSTD_isError(ret)) {
					err = 1;
				}
			}
			if (!err && o.pos > 0) {
				s->out_len = o.pos;
				pin_publish(p, s, 3);
			}
		}
	}
	free(win);
	ZSTD_freeDStream(ds);
	pin_slot_t *s = pin_reserve(p);
	if (s != NULL) {
		s->out_len = 0;
		pin_publish(p, s, err ? -1 : 4);
	}
	return NULL;
}

static int pin_start(pin_t *p, FILE *raw, int threads)
{
	memset(p, 0, sizeof *p);
	p->raw = raw;
	p->ring_n = (size_t)threads * 16 + 16;
	p->ring = calloc(p->ring_n, sizeof *p->ring);
	p->workers = calloc((size_t)threads, sizeof *p->workers);
	if (p->ring == NULL || p->workers == NULL) {
		return -1;
	}
	pthread_mutex_init(&p->lock, NULL);
	pthread_cond_init(&p->space, NULL);
	pthread_cond_init(&p->work, NULL);
	pthread_cond_init(&p->ready, NULL);
	for (int i = 0; i < threads; i++) {
		if (pthread_create(&p->workers[p->nworkers], NULL, pin_worker_main, p) == 0) {
			p->nworkers++;
		}
	}
	if (p->nworkers == 0 || pthread_create(&p->dispatcher, NULL, pin_dispatch_main, p) != 0) {
		return -1;
	}
	p->dispatcher_on = 1;
	return 0;
}

static void pin_stop(pin_t *p)
{
	if (p->ring == NULL) {
		free(p->workers);
		return;
	}
	pthread_mutex_lock(&p->lock);
	p->stop = 1;
	pthread_cond_broadcast(&p->space);
	pthread_cond_broadcast(&p->work);
	pthread_cond_broadcast(&p->ready);
	pthread_mutex_unlock(&p->lock);
	if (p->dispatcher_on) {
		pthread_join(p->dispatcher, NULL);
	}
	for (int i = 0; i < p->nworkers; i++) {
		pthread_join(p->workers[i], NULL);
	}
	for (size_t i = 0; i < p->ring_n; i++) {
		free(p->ring[i].src);
		free(p->ring[i].out);
	}
	free(p->ring);
	free(p->workers);
	pthread_mutex_destroy(&p->lock);
	pthread_cond_destroy(&p->space);
	pthread_cond_destroy(&p->work);
	pthread_cond_destroy(&p->ready);
	memset(p, 0, sizeof *p);
}

/// Jusqu'à `cap` octets décompressés, dans l'ordre. 0 en fin, -1 sur erreur.
static long pin_read(pin_t *p, uint8_t *dst, size_t cap)
{
	size_t w = 0;
	while (w < cap) {
		pthread_mutex_lock(&p->lock);
		pin_slot_t *s = &p->ring[p->consumed % p->ring_n];
		while (p->consumed == p->produced || (s->state != 3 && s->state != 4 && s->state != -1)) {
			pthread_cond_wait(&p->ready, &p->lock);
		}
		int state = s->state;
		pthread_mutex_unlock(&p->lock);
		if (state == -1) {
			return -1;
		}
		if (state == 4) {
			return (long)w; // fin : le créneau reste, les lectures suivantes rendent 0
		}
		size_t n = s->out_len - p->pos;
		if (n > cap - w) {
			n = cap - w;
		}
		memcpy(dst + w, s->out + p->pos, n);
		w += n;
		p->pos += n;
		if (p->pos == s->out_len) {
			pthread_mutex_lock(&p->lock);
			s->state = 0;
			p->consumed++;
			p->pos = 0;
			pthread_cond_signal(&p->space);
			// Le suivant est peut-être déjà prêt : pas d'attente à la prochaine
			// boucle, rien à signaler.
			pthread_mutex_unlock(&p->lock);
		}
	}
	return (long)w;
}
#endif

typedef struct {
	int legacy;   ///< Structures brutes d'avant le format compact.
	FILE *f;      ///< Flux `backup_stream` (corps en clair, ou hérité).
#ifdef ETII_ZSTD
	FILE *raw;    ///< Corps zstd lu DIRECTEMENT, sans le flux à cookie.
	pin_t pin;
	int pin_on;
#endif
	uint8_t carry[SORT_READ_CHUNK + PACKET_CODEC_MAX_BYTES];
	size_t carry_len;
	int done;
} in_reader_t;

static int in_open(in_reader_t *r, const char *path, uint8_t *header, int threads)
{
	memset(r, 0, sizeof *r);
	int format = 0;
	// Validation (magie, version, géométrie, zstd compilé) par le chemin
	// commun, qui journalise un refus.
	FILE *f = backup_stream_fopen_read(path, &format, header);
	if (f == NULL) {
		log_errno("sortback : %s ", path);
		return -1;
	}
	r->legacy = (format == BACKUP_STREAM_LEGACY);
#ifdef ETII_ZSTD
	if (!r->legacy && packet_codec_file_header_is_zstd(header)) {
		fclose(f);
		r->raw = fopen(path, "rb");
		if (r->raw == NULL || fseek(r->raw, PACKET_CODEC_FILE_HEADER_BYTES, SEEK_SET) != 0
		    || pin_start(&r->pin, r->raw, threads) != 0) {
			log_errno("sortback : %s ", path);
			return -1;
		}
		r->pin_on = 1;
		return 0;
	}
#endif
	(void)threads;
	r->f = f;
	return 0;
}

static void in_close(in_reader_t *r)
{
	if (r->f != NULL) {
		fclose(r->f);
	}
#ifdef ETII_ZSTD
	if (r->pin_on) {
		pin_stop(&r->pin);
	}
	if (r->raw != NULL) {
		fclose(r->raw);
	}
#endif
	memset(r, 0, sizeof *r);
}

/// Jusqu'à `cap` octets du corps décompressé. 0 en fin de corps, -1 sur erreur
/// (lecture, trame tronquée ou abîmée).
static long body_read(in_reader_t *r, uint8_t *dst, size_t cap)
{
#ifdef ETII_ZSTD
	if (r->pin_on) {
		return pin_read(&r->pin, dst, cap);
	}
#endif
	size_t got = fread(dst, 1, cap, r->f);
	if (got == 0 && ferror(r->f)) {
		return -1;
	}
	return (long)got;
}

/* --- Tampons de tri ------------------------------------------------------------------ */

typedef struct {
	const uint8_t *p;
	uint32_t len;
} rec_ref_t;

static int ref_cmp(const void *pa, const void *pb)
{
	const rec_ref_t *a = pa, *b = pb;
	return backup_sort_record_cmp(a->p, a->len, b->p, b->len);
}

typedef struct {
	uint8_t *arena;
	size_t arena_cap;
	rec_ref_t *refs;   ///< Index lu (trié par tranches)…
	rec_ref_t *merged; ///< … puis fusionné ici.
	size_t ref_cap;
	size_t n;
} batch_t;

static int batch_alloc(batch_t *b, size_t budget)
{
	memset(b, 0, sizeof *b);
	// Deux index de 16 octets par enregistrement contre ~76 octets de
	// données : un tiers du budget pour les index.
	b->ref_cap = (budget / 3) / (2 * sizeof(rec_ref_t));
	if (b->ref_cap < 64) {
		b->ref_cap = 64;
	}
	b->arena_cap = (budget > 2 * b->ref_cap * sizeof(rec_ref_t)) ? budget - 2 * b->ref_cap * sizeof(rec_ref_t) : 0;
	if (b->arena_cap < 4 * SORT_READ_CHUNK) {
		b->arena_cap = 4 * SORT_READ_CHUNK;
	}
	b->arena = malloc(b->arena_cap);
	b->refs = malloc(b->ref_cap * sizeof *b->refs);
	b->merged = malloc(b->ref_cap * sizeof *b->merged);
	return (b->arena != NULL && b->refs != NULL && b->merged != NULL) ? 0 : -1;
}

static void batch_free(batch_t *b)
{
	free(b->arena);
	free(b->refs);
	free(b->merged);
	memset(b, 0, sizeof *b);
}

/**
 * @brief Remplit `b` depuis l'entrée : le corps décompressé y est écrit
 *        directement et découpé en enregistrements (longueur par le bitmap ;
 *        validation et forme canonique plus tard, sur les fils de tri).
 * @return 1 plein (l'entrée continue), 0 entrée épuisée, -1 entrée tronquée,
 *         illisible ou incohérente.
 */
static int batch_fill(batch_t *b, in_reader_t *r, backup_sort_stats_t *st)
{
	b->n = 0;
	if (r->legacy) {
		size_t used = 0;
		while (b->n < b->ref_cap && b->arena_cap - used >= PACKET_CODEC_MAX_BYTES) {
			struct possibility_packet p;
			if (fread(&p, sizeof p, 1, r->f) != 1) {
				return ferror(r->f) ? -1 : 0;
			}
			// Mêmes normalisations qu'`import`.
			if (p.checked != 1) {
				p.checked = 0;
			}
			p.alloc = (uint16_t)possibility_placed_count(&p);
			p.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
			size_t written = 0;
			if (packet_codec_encode(&p, b->arena + used, PACKET_CODEC_MAX_BYTES, &written) != 0) {
				return -1;
			}
			b->refs[b->n].p = b->arena + used;
			b->refs[b->n].len = (uint32_t)written;
			b->n++;
			used += written;
			st->records++;
		}
		return 1;
	}
	memcpy(b->arena, r->carry, r->carry_len);
	size_t used = r->carry_len, parsed = 0;
	r->carry_len = 0;
	for (;;) {
		while (b->n < b->ref_cap && used - parsed >= SORT_PREFIX) {
			size_t len = rec_len(b->arena + parsed);
			if (used - parsed < len) {
				break;
			}
			b->refs[b->n].p = b->arena + parsed;
			b->refs[b->n].len = (uint32_t)len;
			b->n++;
			parsed += len;
			st->records++;
		}
		if (b->n == b->ref_cap || r->done || b->arena_cap - used < SORT_READ_CHUNK) {
			if (r->done && b->n < b->ref_cap) {
				return (used == parsed) ? 0 : -1;
			}
			// Le reste — moins d'une lecture — passe au tampon suivant.
			r->carry_len = used - parsed;
			memcpy(r->carry, b->arena + parsed, r->carry_len);
			return 1;
		}
		long got = body_read(r, b->arena + used, SORT_READ_CHUNK);
		if (got < 0) {
			return -1;
		}
		if (got == 0) {
			r->done = 1;
			continue;
		}
		used += (size_t)got;
	}
}

/* --- Tri parallèle d'un tampon -------------------------------------------------------- */

typedef struct {
	rec_ref_t *r;
	size_t n;
	int canon;
	int bad;
} slice_t;

static void *slice_sort_main(void *arg)
{
	slice_t *s = arg;
	// Forme canonique (et validation) ICI, sur les fils de tri : sur le seul
	// fil lecteur, elle coûtait autant que la décompression de l'entrée.
	for (size_t i = 0; s->canon && i < s->n; i++) {
		long n = packet_codec_canonicalize((uint8_t *)s->r[i].p, s->r[i].len, POSSIBILITY_MIN_CANDIDATS_UNKNOWN, NULL);
		if (n != (long)s->r[i].len) {
			s->bad = 1;
			return NULL;
		}
	}
	qsort(s->r, s->n, sizeof *s->r, ref_cmp);
	return NULL;
}

static void slices_down(slice_t **h, int n, int i)
{
	for (;;) {
		int l = 2 * i + 1, m = i;
		if (l < n && ref_cmp(h[l]->r, h[m]->r) < 0) {
			m = l;
		}
		if (l + 1 < n && ref_cmp(h[l + 1]->r, h[m]->r) < 0) {
			m = l + 1;
		}
		if (m == i) {
			return;
		}
		slice_t *t = h[i];
		h[i] = h[m];
		h[m] = t;
		i = m;
	}
}

#define BACKUP_SORT_BAD_RECORD (-2)

/// Plage `j` d'une fusion parallèle de tranches : pour chaque tranche, les
/// éléments [lo[t], hi[t]) vont à partir de `out + off`.
typedef struct {
	const slice_t *slices;
	int k;
	size_t lo[BACKUP_SORT_THREADS_MAX], hi[BACKUP_SORT_THREADS_MAX];
	rec_ref_t *out;
} range_merge_t;

static void *range_merge_main(void *arg)
{
	range_merge_t *m = arg;
	slice_t part[BACKUP_SORT_THREADS_MAX];
	slice_t *heap[BACKUP_SORT_THREADS_MAX];
	int n = 0;
	for (int t = 0; t < m->k; t++) {
		part[t] = (slice_t){ m->slices[t].r + m->lo[t], m->hi[t] - m->lo[t], 0, 0 };
		if (part[t].n > 0) {
			heap[n++] = &part[t];
		}
	}
	for (int i = n / 2; i-- > 0;) {
		slices_down(heap, n, i);
	}
	size_t w = 0;
	while (n > 0) {
		slice_t *s = heap[0];
		m->out[w++] = *s->r;
		s->r++;
		if (--s->n == 0) {
			heap[0] = heap[--n];
		}
		slices_down(heap, n, 0);
	}
	return NULL;
}

/// Premier indice de `s` dont l'élément n'est pas avant `key`.
static size_t lower_bound(const slice_t *s, const rec_ref_t *key)
{
	size_t a = 0, b = s->n;
	while (a < b) {
		size_t m = a + (b - a) / 2;
		if (ref_cmp(&s->r[m], key) < 0) {
			a = m + 1;
		} else {
			b = m;
		}
	}
	return a;
}

/**
 * @brief Fusionne `k` tranches triées dans `out`, sur `k` fils : des bornes
 *        tirées des tranches découpent l'ordre en `k` plages, chaque plage est
 *        fusionnée par son fil directement à sa place. Sur un fil, cette fusion
 *        coûtait ~0,5 s par tampon de 4,4 M possibilités, plus que le tri
 *        lui-même : la lecture l'attendait.
 */
static int merge_slices_parallel(const slice_t *slices, int k, rec_ref_t *out)
{
	size_t total = 0;
	for (int t = 0; t < k; t++) {
		total += slices[t].n;
	}
	if (total == 0) {
		return 0;
	}
	// Échantillon régulier de chaque tranche, trié : ses quantiles bornent les
	// plages.
	enum { PER = 64 };
	rec_ref_t sample[BACKUP_SORT_THREADS_MAX * PER];
	int ns = 0;
	for (int t = 0; t < k; t++) {
		for (int i = 0; i < PER && slices[t].n > 0; i++) {
			sample[ns++] = slices[t].r[(size_t)i * slices[t].n / PER];
		}
	}
	qsort(sample, (size_t)ns, sizeof *sample, ref_cmp);
	range_merge_t ranges[BACKUP_SORT_THREADS_MAX];
	size_t off = 0;
	for (int j = 0; j < k; j++) {
		range_merge_t *m = &ranges[j];
		m->slices = slices;
		m->k = k;
		for (int t = 0; t < k; t++) {
			m->lo[t] = (j == 0) ? 0 : ranges[j - 1].hi[t];
			m->hi[t] = (j == k - 1) ? slices[t].n : lower_bound(&slices[t], &sample[(size_t)(j + 1) * (size_t)ns / (size_t)k]);
			if (m->hi[t] < m->lo[t]) {
				m->hi[t] = m->lo[t];
			}
		}
		m->out = out + off;
		for (int t = 0; t < k; t++) {
			off += m->hi[t] - m->lo[t];
		}
	}
	pthread_t tid[BACKUP_SORT_THREADS_MAX];
	int started[BACKUP_SORT_THREADS_MAX] = { 0 };
	for (int j = 1; j < k; j++) {
		started[j] = (pthread_create(&tid[j], NULL, range_merge_main, &ranges[j]) == 0);
		if (!started[j]) {
			range_merge_main(&ranges[j]);
		}
	}
	range_merge_main(&ranges[0]);
	for (int j = 1; j < k; j++) {
		if (started[j]) {
			pthread_join(tid[j], NULL);
		}
	}
	return (off == total) ? 0 : -1;
}

/// Trie `b->refs` sur `threads` fils puis fusionne les tranches dans
/// `b->merged`. Indépendant du nombre de fils : deux enregistrements égaux
/// pour la clé sont identiques à l'octet.
static int batch_sort(batch_t *b, int threads, int canon)
{
	size_t n = b->n;
	if (threads < 1) {
		threads = 1;
	}
	if ((size_t)threads > n / 4096 + 1) {
		threads = (int)(n / 4096 + 1);
	}
	slice_t slices[BACKUP_SORT_THREADS_MAX];
	pthread_t tid[BACKUP_SORT_THREADS_MAX];
	int started[BACKUP_SORT_THREADS_MAX] = { 0 };
	size_t per = n / (size_t)threads, off = 0;
	for (int t = 0; t < threads; t++) {
		slices[t] = (slice_t){ b->refs + off, (t == threads - 1) ? n - off : per, canon, 0 };
		off += slices[t].n;
	}
	for (int t = 1; t < threads; t++) {
		started[t] = (pthread_create(&tid[t], NULL, slice_sort_main, &slices[t]) == 0);
		if (!started[t]) {
			slice_sort_main(&slices[t]);
		}
	}
	slice_sort_main(&slices[0]);
	for (int t = 1; t < threads; t++) {
		if (started[t]) {
			pthread_join(tid[t], NULL);
		}
	}
	for (int t = 0; t < threads; t++) {
		if (slices[t].bad) {
			return BACKUP_SORT_BAD_RECORD;
		}
	}
	return merge_slices_parallel(slices, threads, b->merged);
}

/* --- Paquets : blocs indépendants + index ---------------------------------------------- */

typedef struct {
	char path[PATH_MAX + 64];
	char idx_path[PATH_MAX + 64];
	int fd, idx_fd;
	uint64_t nblocks;
} run_t;

typedef struct {
	run_t *v;
	size_t n, cap;
} run_list_t;

static void runs_unlink(run_list_t *runs)
{
	for (size_t i = 0; i < runs->n; i++) {
		if (runs->v[i].fd >= 0) {
			close(runs->v[i].fd);
		}
		if (runs->v[i].idx_fd >= 0) {
			close(runs->v[i].idx_fd);
		}
		unlink(runs->v[i].path);
		unlink(runs->v[i].idx_path);
	}
	free(runs->v);
	memset(runs, 0, sizeof *runs);
}

/// Blocs d'un tampon fusionné : [first, first + count) de `merged`.
typedef struct {
	size_t first, count, raw;
} blk_span_t;

typedef struct {
	const batch_t *b;
	const blk_span_t *spans;
	size_t from, to;      ///< Blocs confiés à ce fil.
	uint8_t *out;         ///< Leurs octets stockés, bout à bout.
	size_t out_len, out_cap;
	uint32_t *stored;     ///< Taille stockée de chacun.
	uint16_t *codec;
	int rc;
} pack_job_t;

static void *pack_job_main(void *arg)
{
	pack_job_t *j = arg;
	codec_ctx_t c = { 0 };
	uint8_t *raw = malloc(RUN_BLOCK_RAW);
	size_t bound = block_bound(RUN_BLOCK_RAW);
	j->rc = (raw == NULL) ? -1 : 0;
	for (size_t i = j->from; j->rc == 0 && i < j->to; i++) {
		const blk_span_t *s = &j->spans[i];
		size_t w = 0;
		for (size_t k = 0; k < s->count; k++) {
			const rec_ref_t *r = &j->b->merged[s->first + k];
			memcpy(raw + w, r->p, r->len);
			w += r->len;
		}
		if (j->out_len + bound > j->out_cap) {
			size_t cap = (j->out_cap ? j->out_cap * 2 : 4 << 20) + bound;
			uint8_t *p = realloc(j->out, cap);
			if (p == NULL) {
				j->rc = -1;
				break;
			}
			j->out = p;
			j->out_cap = cap;
		}
		int codec = CODEC_RAW;
		size_t st = block_pack(&c, raw, w, j->out + j->out_len, &codec);
		j->stored[i - j->from] = (uint32_t)st;
		j->codec[i - j->from] = (uint16_t)codec;
		j->out_len += st;
	}
	free(raw);
	codec_ctx_free(&c);
	return NULL;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/// Écrit `b->merged` comme un paquet : blocs compressés en parallèle, puis
/// écrits dans l'ordre avec leur entrée d'index.
static int write_run(const batch_t *b, int threads, run_t *run)
{
	size_t nspans = 0, cap = b->n / 64 + 16;
	blk_span_t *spans = malloc(cap * sizeof *spans);
	if (spans == NULL) {
		return -1;
	}
	for (size_t i = 0; i < b->n;) {
		if (nspans == cap) {
			cap *= 2;
			blk_span_t *p = realloc(spans, cap * sizeof *spans);
			if (p == NULL) {
				free(spans);
				return -1;
			}
			spans = p;
		}
		blk_span_t *s = &spans[nspans++];
		s->first = i;
		s->count = 0;
		s->raw = 0;
		while (i < b->n && s->raw + b->merged[i].len <= RUN_BLOCK_RAW) {
			s->raw += b->merged[i].len;
			s->count++;
			i++;
		}
	}
	if ((size_t)threads > nspans) {
		threads = (int)nspans;
	}
	if (threads < 1) {
		threads = 1;
	}
	pack_job_t jobs[BACKUP_SORT_THREADS_MAX];
	pthread_t tid[BACKUP_SORT_THREADS_MAX];
	int started[BACKUP_SORT_THREADS_MAX] = { 0 };
	int rc = 0;
	size_t per = nspans / (size_t)threads, from = 0;
	for (int t = 0; t < threads; t++) {
		size_t to = (t == threads - 1) ? nspans : from + per;
		jobs[t] = (pack_job_t){ b, spans, from, to, NULL, 0, 0,
		                        malloc((to - from + 1) * sizeof(uint32_t)), malloc((to - from + 1) * sizeof(uint16_t)), 0 };
		if (jobs[t].stored == NULL || jobs[t].codec == NULL) {
			rc = -1;
		}
		from = to;
	}
	for (int t = 1; rc == 0 && t < threads; t++) {
		started[t] = (pthread_create(&tid[t], NULL, pack_job_main, &jobs[t]) == 0);
		if (!started[t]) {
			pack_job_main(&jobs[t]);
		}
	}
	if (rc == 0) {
		pack_job_main(&jobs[0]);
	}
	for (int t = 1; t < threads; t++) {
		if (started[t]) {
			pthread_join(tid[t], NULL);
		}
	}
	uint64_t off = 0;
	uint8_t entry[IDX_BYTES];
	for (int t = 0; rc == 0 && t < threads; t++) {
		pack_job_t *j = &jobs[t];
		if (j->rc != 0 || write_all(run->fd, j->out, j->out_len) != 0) {
			rc = -1;
			break;
		}
		for (size_t i = j->from; i < j->to; i++) {
			const blk_span_t *s = &spans[i];
			const rec_ref_t *first = &b->merged[s->first];
			const uint8_t *k;
			size_t kn = tkey(first->p, first->len, &k);
			memset(entry, 0, sizeof entry);
			put_u64(entry, off);
			put_u32(entry + 8, j->stored[i - j->from]);
			put_u32(entry + 12, (uint32_t)s->raw);
			put_u32(entry + 16, (uint32_t)s->count);
			put_u16(entry + 20, j->codec[i - j->from]);
			put_u16(entry + 22, (uint16_t)kn);
			memcpy(entry + 24, k, kn);
			if (write_all(run->idx_fd, entry, sizeof entry) != 0) {
				rc = -1;
				break;
			}
			off += j->stored[i - j->from];
		}
	}
	run->nblocks = nspans;
	for (int t = 0; t < threads; t++) {
		free(jobs[t].out);
		free(jobs[t].stored);
		free(jobs[t].codec);
	}
	free(spans);
	return rc;
}

/// Tri et écriture d'un tampon, sur un fil à part pendant que la lecture
/// remplit l'autre.
typedef struct {
	batch_t *b;
	int threads;
	int canon;
	run_t *run;
	int rc;
} run_job_t;

static void *run_job_main(void *arg)
{
	run_job_t *j = arg;
	j->rc = batch_sort(j->b, j->threads, j->canon);
	if (j->rc == 0) {
		j->rc = write_run(j->b, j->threads, j->run);
	}
	return NULL;
}

static run_t *run_new(run_list_t *runs, const char *tmp_dir)
{
	if (runs->n == runs->cap) {
		size_t cap = runs->cap ? runs->cap * 2 : 64;
		run_t *p = realloc(runs->v, cap * sizeof *p);
		if (p == NULL) {
			return NULL;
		}
		runs->v = p;
		runs->cap = cap;
	}
	run_t *r = &runs->v[runs->n];
	memset(r, 0, sizeof *r);
	snprintf(r->path, sizeof r->path, "%s/sortback-%ld-%zu.run", tmp_dir, (long)getpid(), runs->n);
	snprintf(r->idx_path, sizeof r->idx_path, "%s/sortback-%ld-%zu.idx", tmp_dir, (long)getpid(), runs->n);
	r->fd = open(r->path, O_RDWR | O_CREAT | O_EXCL, 0600);
	r->idx_fd = (r->fd >= 0) ? open(r->idx_path, O_RDWR | O_CREAT | O_EXCL, 0600) : -1;
	runs->n++; // compté même en échec : `runs_unlink` nettoie
	if (r->fd < 0 || r->idx_fd < 0) {
		if (errno == EMFILE) {
			struct rlimit rl;
			getrlimit(RLIMIT_NOFILE, &rl);
			log_error("sortback : limite de fichiers ouverts atteinte au paquet %zu (%llu, deux par paquet) — "
			          "relever la limite dure (ulimit -Hn) ou augmenter mémoire_Mo pour des paquets moins "
			          "nombreux\n", runs->n, (unsigned long long)rl.rlim_cur);
		} else {
			log_errno("sortback : paquet dans %s ", tmp_dir);
		}
		return NULL;
	}
	return r;
}

/* --- Fusion par plages -------------------------------------------------------------------- */

typedef struct {
	uint8_t k[KEY_PFX];
	uint16_t n;
} bound_t;

static int bound_cmp(const void *a, const void *b)
{
	const bound_t *x = a, *y = b;
	return tkey_cmp(x->k, x->n, y->k, y->n);
}

static int idx_read(const run_t *run, uint64_t i, uint8_t *entry)
{
	return (pread(run->idx_fd, entry, IDX_BYTES, (off_t)(i * IDX_BYTES)) == IDX_BYTES) ? 0 : -1;
}

/// Curseur d'un fil de fusion dans un paquet.
typedef struct {
	const run_t *run;
	uint64_t blk;
	uint8_t *raw;
	size_t raw_len, pos;
	const uint8_t *cur;
	size_t cur_len;
} cursor_t;

typedef struct {
	codec_ctx_t c;
	uint8_t *cbuf;
	double io_s;     ///< Temps passé à lire et décompresser des blocs de paquets.
} reader_ctx_t;

/// Charge le bloc `i` du curseur. 0, 1 si au-delà du dernier, -1 sur erreur.
static int cursor_load(cursor_t *cu, reader_ctx_t *rc, uint64_t i)
{
	if (i >= cu->run->nblocks) {
		return 1;
	}
	uint8_t e[IDX_BYTES];
	if (idx_read(cu->run, i, e) != 0) {
		return -1;
	}
	uint64_t off = get_u64(e);
	uint32_t stored = get_u32(e + 8), raw = get_u32(e + 12);
	int codec = get_u16(e + 20);
	double t = sort_now();
	if (raw == 0 || raw > RUN_BLOCK_RAW || stored > block_bound(RUN_BLOCK_RAW)
	    || pread(cu->run->fd, rc->cbuf, stored, (off_t)off) != (ssize_t)stored
	    || block_unpack(&rc->c, codec, rc->cbuf, stored, cu->raw, raw) != 0) {
		return -1;
	}
	rc->io_s += sort_now() - t;
	cu->blk = i;
	cu->raw_len = raw;
	cu->pos = 0;
	cu->cur = cu->raw;
	cu->cur_len = rec_len(cu->raw);
	return 0;
}

/// Avance d'un enregistrement. 0, 1 en fin de paquet, -1 sur erreur.
static int cursor_next(cursor_t *cu, reader_ctx_t *rc)
{
	cu->pos += cu->cur_len;
	if (cu->pos >= cu->raw_len) {
		return cursor_load(cu, rc, cu->blk + 1);
	}
	cu->cur = cu->raw + cu->pos;
	cu->cur_len = rec_len(cu->cur);
	return 0;
}

/// Dernier bloc dont la première clé tronquée est STRICTEMENT avant `lo` (0
/// s'il n'y en a pas) : tout ce qui le précède est avant `lo`.
static int run_seek(const run_t *run, const bound_t *lo, uint64_t *out)
{
	uint64_t a = 0, b = run->nblocks; // cherche le premier bloc >= lo
	uint8_t e[IDX_BYTES];
	while (a < b) {
		uint64_t m = a + (b - a) / 2;
		if (idx_read(run, m, e) != 0) {
			return -1;
		}
		if (tkey_cmp(e + 24, get_u16(e + 22), lo->k, lo->n) < 0) {
			a = m + 1;
		} else {
			b = m;
		}
	}
	*out = (a > 0) ? a - 1 : 0;
	return 0;
}

static int cur_less(const cursor_t *a, const cursor_t *b)
{
	return backup_sort_record_cmp(a->cur, a->cur_len, b->cur, b->cur_len) < 0;
}

static void cur_down(cursor_t **h, size_t n, size_t i)
{
	for (;;) {
		size_t l = 2 * i + 1, m = i;
		if (l < n && cur_less(h[l], h[m])) {
			m = l;
		}
		if (l + 1 < n && cur_less(h[l + 1], h[m])) {
			m = l + 1;
		}
		if (m == i) {
			return;
		}
		cursor_t *t = h[i];
		h[i] = h[m];
		h[m] = t;
		i = m;
	}
}

typedef struct {
	const run_list_t *runs;
	const bound_t *bounds;   ///< nranges - 1 bornes.
	size_t nranges;
	const char *tmp_dir;
	pthread_mutex_t lock;
	size_t next;             ///< Prochaine plage à prendre.
	int failed;
	unsigned long long dups, raw_bytes, records;
	double io_s;             ///< Lecture des paquets, cumulée sur les fils.
} merge_ctl_t;

static void part_path(char *buf, size_t n, const char *tmp_dir, size_t r)
{
	snprintf(buf, n, "%s/sortback-%ld-part-%zu", tmp_dir, (long)getpid(), r);
}

typedef struct {
	int fd;
	codec_ctx_t c;
	uint8_t *raw, *packed;
	size_t used;
	uint8_t prev[PACKET_CODEC_MAX_BYTES];
	size_t prev_len;
	unsigned long long dups, raw_bytes, records;
} part_t;

static int part_flush(part_t *p)
{
	if (p->used == 0) {
		return 0;
	}
	// Le corps d'un `.back` zstd n'est fait QUE de trames zstd : une partie
	// que zstd ne réduit pas (quelques petits enregistrements) reste une trame.
	int codec;
	size_t st = block_pack_ex(&p->c, p->raw, p->used, p->packed, &codec, 1);
	p->used = 0;
#ifdef ETII_ZSTD
	if (codec != CODEC_ZSTD) {
		return -1;
	}
#endif
	return write_all(p->fd, p->packed, st);
}

static int part_put(part_t *p, const uint8_t *rec, size_t len)
{
	if (p->prev_len == len && memcmp(p->prev, rec, len) == 0) {
		p->dups++;
	}
	memcpy(p->prev, rec, len);
	p->prev_len = len;
	p->raw_bytes += len;
	p->records++;
	if (p->used + len > OUT_BLOCK_RAW && part_flush(p) != 0) {
		return -1;
	}
	memcpy(p->raw + p->used, rec, len);
	p->used += len;
	return 0;
}

/// Fusionne la plage `r` de tous les paquets dans sa partie de sortie.
static int merge_range(merge_ctl_t *m, size_t r, cursor_t *cur, cursor_t **heap, reader_ctx_t *rc, part_t *p)
{
	const bound_t *lo = (r > 0) ? &m->bounds[r - 1] : NULL;
	const bound_t *hi = (r + 1 < m->nranges) ? &m->bounds[r] : NULL;
	char path[PATH_MAX + 64];
	part_path(path, sizeof path, m->tmp_dir, r);
	p->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (p->fd < 0) {
		return -1;
	}
	p->used = 0;
	p->prev_len = 0;
	size_t n = 0;
	int rc_ = 0;
	for (size_t i = 0; i < m->runs->n && rc_ == 0; i++) {
		cursor_t *cu = &cur[i];
		cu->run = &m->runs->v[i];
		uint64_t start = 0;
		if (lo != NULL && run_seek(cu->run, lo, &start) != 0) {
			rc_ = -1;
			break;
		}
		int s = cursor_load(cu, rc, start);
		// Saute ce qui précède la plage.
		while (s == 0 && lo != NULL) {
			const uint8_t *k;
			size_t kn = tkey(cu->cur, cu->cur_len, &k);
			if (tkey_cmp(k, kn, lo->k, lo->n) >= 0) {
				break;
			}
			s = cursor_next(cu, rc);
		}
		if (s < 0) {
			rc_ = -1;
		} else if (s == 0) {
			heap[n++] = cu;
		}
	}
	for (size_t i = n / 2; rc_ == 0 && i-- > 0;) {
		cur_down(heap, n, i);
	}
	while (rc_ == 0 && n > 0) {
		cursor_t *cu = heap[0];
		if (hi != NULL) {
			const uint8_t *k;
			size_t kn = tkey(cu->cur, cu->cur_len, &k);
			if (tkey_cmp(k, kn, hi->k, hi->n) >= 0) {
				// La tête du tas est au-delà de la plage : tout le reste aussi.
				break;
			}
		}
		if (part_put(p, cu->cur, cu->cur_len) != 0) {
			rc_ = -1;
			break;
		}
		int s = cursor_next(cu, rc);
		if (s < 0) {
			rc_ = -1;
			break;
		}
		if (s == 1) {
			heap[0] = heap[--n];
		}
		cur_down(heap, n, 0);
	}
	if (rc_ == 0 && part_flush(p) != 0) {
		rc_ = -1;
	}
	if (close(p->fd) != 0) {
		rc_ = -1;
	}
	return rc_;
}

static void *merge_worker_main(void *arg)
{
	merge_ctl_t *m = arg;
	size_t nr = m->runs->n;
	cursor_t *cur = calloc(nr, sizeof *cur);
	cursor_t **heap = calloc(nr, sizeof *heap);
	uint8_t *raws = malloc(nr * (size_t)RUN_BLOCK_RAW);
	reader_ctx_t rc = { { 0 }, malloc(block_bound(RUN_BLOCK_RAW)), 0 };
	part_t p = { 0 };
	p.raw = malloc(OUT_BLOCK_RAW);
	p.packed = malloc(block_bound(OUT_BLOCK_RAW));
	int ok = (cur != NULL && heap != NULL && raws != NULL && rc.cbuf != NULL && p.raw != NULL && p.packed != NULL);
	for (size_t i = 0; ok && i < nr; i++) {
		cur[i].raw = raws + i * (size_t)RUN_BLOCK_RAW;
	}
	for (;;) {
		pthread_mutex_lock(&m->lock);
		if (!ok) {
			m->failed = 1;
		}
		size_t r = m->next++;
		int stop = m->failed || r >= m->nranges;
		pthread_mutex_unlock(&m->lock);
		if (stop) {
			break;
		}
		if (merge_range(m, r, cur, heap, &rc, &p) != 0) {
			ok = 0;
		}
	}
	pthread_mutex_lock(&m->lock);
	m->io_s += rc.io_s;
	m->dups += p.dups;
	m->raw_bytes += p.raw_bytes;
	m->records += p.records;
	pthread_mutex_unlock(&m->lock);
	codec_ctx_free(&p.c);
	codec_ctx_free(&rc.c);
	free(p.raw);
	free(p.packed);
	free(rc.cbuf);
	free(raws);
	free(cur);
	free(heap);
	return NULL;
}

/// Bornes de plage : quantiles des premières clés de blocs de tous les paquets
/// (chaque bloc pèse à peu près le même nombre d'octets).
static size_t choose_bounds(const run_list_t *runs, size_t want, bound_t **out)
{
	uint64_t total = 0;
	for (size_t i = 0; i < runs->n; i++) {
		total += runs->v[i].nblocks;
	}
	uint64_t target = (uint64_t)want * 64;
	uint64_t step = (total > target) ? total / target : 1;
	size_t cap = (size_t)(total / step) + runs->n + 1, ns = 0;
	bound_t *s = malloc(cap * sizeof *s);
	*out = NULL;
	if (s == NULL) {
		return 0;
	}
	uint8_t e[IDX_BYTES];
	for (size_t i = 0; i < runs->n; i++) {
		for (uint64_t b = 0; b < runs->v[i].nblocks && ns < cap; b += step) {
			if (idx_read(&runs->v[i], b, e) != 0) {
				free(s);
				return 0;
			}
			s[ns].n = get_u16(e + 22);
			memcpy(s[ns].k, e + 24, KEY_PFX);
			ns++;
		}
	}
	qsort(s, ns, sizeof *s, bound_cmp);
	bound_t *b = malloc((want > 0 ? want : 1) * sizeof *b);
	size_t nb = 0;
	for (size_t j = 1; b != NULL && j < want && ns > 0; j++) {
		const bound_t *c = &s[j * ns / want];
		if (nb == 0 || bound_cmp(&b[nb - 1], c) < 0) {
			b[nb++] = *c;
		}
	}
	free(s);
	*out = b;
	return nb;
}

static int append_file(int out_fd, const char *path, uint8_t *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -1;
	}
	int rc = 0;
	for (;;) {
		ssize_t n = read(fd, buf, cap);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			rc = -1;
			break;
		}
		if (n == 0) {
			break;
		}
		if (write_all(out_fd, buf, (size_t)n) != 0) {
			rc = -1;
			break;
		}
	}
	close(fd);
	return rc;
}

/// Fusion parallèle des paquets dans `out_tmp` : en-tête, puis les parties
/// des plages bout à bout (une suite de trames, comme le corps d'un `.back`).
static int merge_runs(run_list_t *runs, int threads, const char *tmp_dir, const char *out_tmp, uint8_t flags,
                      backup_sort_stats_t *st)
{
	bound_t *bounds = NULL;
	size_t nb = choose_bounds(runs, (size_t)threads * RANGES_PER_THREAD, &bounds);
	merge_ctl_t m = { runs, bounds, nb + 1, tmp_dir, PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0, 0, 0 };
	double t_merge = sort_now();
	pthread_t tid[BACKUP_SORT_THREADS_MAX];
	int started[BACKUP_SORT_THREADS_MAX] = { 0 };
	for (int t = 1; t < threads; t++) {
		started[t] = (pthread_create(&tid[t], NULL, merge_worker_main, &m) == 0);
	}
	merge_worker_main(&m);
	for (int t = 1; t < threads; t++) {
		if (started[t]) {
			pthread_join(tid[t], NULL);
		}
	}
	int rc = m.failed ? -1 : 0;
	// Durées de la phase 2, séparées : au niveau 27 elle a pris 86 min pour
	// ~18 attendues d'après le banc (cache froid, 1 044 paquets compris), sans
	// qu'on puisse dire si c'était la fusion, la lecture des paquets ou la
	// concaténation finale (~127 Go réécrits).
	log_info("sortback : fusion de %zu plage(s) en %.0f s — lecture et décompression des paquets %.0f s cumulées "
	         "sur %d fil(s)\n", m.nranges, sort_now() - t_merge, m.io_s, threads);
	double t_concat = sort_now();
	// Les paquets ne servent plus : effacés AVANT la concaténation, le pic
	// disque est parties + sortie, pas paquets + parties + sortie.
	for (size_t i = 0; i < runs->n; i++) {
		run_t *r = &runs->v[i];
		if (r->fd >= 0) {
			close(r->fd);
			r->fd = -1;
		}
		if (r->idx_fd >= 0) {
			close(r->idx_fd);
			r->idx_fd = -1;
		}
		unlink(r->path);
		unlink(r->idx_path);
	}
	st->duplicates = m.dups;
	st->raw_bytes = m.raw_bytes;
	if (rc == 0 && m.records != st->records) {
		log_error("sortback : la fusion a rendu %llu possibilité(s) pour %llu lues\n", m.records, st->records);
		rc = -1;
	}
	int fd = (rc == 0) ? open(out_tmp, O_WRONLY | O_CREAT | O_EXCL, 0644) : -1;
	if (rc == 0 && fd < 0) {
		log_errno("sortback : %s ", out_tmp);
		rc = -1;
	}
	if (fd >= 0) {
		uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
#ifdef ETII_ZSTD
		packet_codec_write_file_header_codec(header, flags, 1);
#else
		packet_codec_write_file_header_codec(header, flags, 0);
#endif
		if (write_all(fd, header, sizeof header) != 0) {
			rc = -1;
		}
	}
	uint8_t *buf = malloc(8 << 20);
	if (buf == NULL) {
		rc = -1;
	}
	char path[PATH_MAX + 64];
	for (size_t r = 0; r < m.nranges; r++) {
		part_path(path, sizeof path, tmp_dir, r);
		if (rc == 0 && append_file(fd, path, buf, 8 << 20) != 0) {
			log_errno("sortback : %s ", out_tmp);
			rc = -1;
		}
		unlink(path);
	}
	free(buf);
	if (fd >= 0) {
		if (rc == 0 && fsync(fd) != 0) {
			log_errno("sortback : fsync %s ", out_tmp);
			rc = -1;
		}
		close(fd);
	}
	if (rc == 0) {
		log_info("sortback : concaténation des parties et mise sur disque en %.0f s\n", sort_now() - t_concat);
	}
	free(bounds);
	return rc;
}

/* --- Sortie directe (tout tient dans un tampon) ------------------------------------------------ */

static FILE *writer_open(const char *path, uint8_t flags)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		log_errno("sortback : %s ", path);
		return NULL;
	}
	FILE *raw = fdopen(fd, "wb");
	if (raw == NULL) {
		close(fd);
		unlink(path);
		return NULL;
	}
	setvbuf(raw, NULL, _IOFBF, 1 << 20);
	FILE *f = backup_stream_open_write(raw, flags);
	if (f == NULL) {
		log_errno("sortback : %s ", path);
		unlink(path);
	}
	return f;
}

static int write_direct(const batch_t *b, const char *out_tmp, uint8_t flags, backup_sort_stats_t *st)
{
	FILE *f = writer_open(out_tmp, flags);
	if (f == NULL) {
		return -1;
	}
	int rc = 0;
	const rec_ref_t *prev = NULL;
	for (size_t i = 0; i < b->n && rc == 0; i++) {
		const rec_ref_t *r = &b->merged[i];
		if (prev != NULL && prev->len == r->len && memcmp(prev->p, r->p, r->len) == 0) {
			st->duplicates++;
		}
		st->raw_bytes += r->len;
		prev = r;
		if (fwrite(r->p, 1, r->len, f) != r->len) {
			rc = -1;
		}
	}
	if (fclose(f) != 0) {
		log_errno("sortback : %s ", out_tmp);
		rc = -1;
	}
	int fd = (rc == 0) ? open(out_tmp, O_RDONLY) : -1;
	if (rc == 0 && (fd < 0 || fsync(fd) != 0)) {
		rc = -1;
	}
	if (fd >= 0) {
		close(fd);
	}
	return rc;
}

/* --- Point d'entrée ------------------------------------------------------------------------------ */

int backup_sort(const char *in, const char *out, const backup_sort_options_t *opts, backup_sort_stats_t *stats)
{
	backup_sort_stats_t local;
	backup_sort_stats_t *st = (stats != NULL) ? stats : &local;
	memset(st, 0, sizeof *st);
	double t0 = sort_now();
	// Deux fichiers ouverts par paquet jusqu'à la fin de la fusion : ~1 800
	// paquets au niveau 27. La limite SOUPLE d'un terminal (souvent 1 024)
	// arrêtait le tri au 510e ; la limite dure, elle, est en général large.
	unsigned long long nofile = backup_sort_raise_open_files();
	log_info("sortback : jusqu'à %llu fichiers ouverts, soit ~%llu paquets\n", nofile,
	         nofile > 64 ? (nofile - 64) / 2 : 0ULL);

	if (access(out, F_OK) == 0) {
		log_error("sortback : %s existe déjà — jamais écrasé\n", out);
		return BACKUP_SORT_ERR_OUTPUT_EXISTS;
	}
	uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
	in_reader_t *in_r = malloc(sizeof *in_r);
	if (in_r == NULL) {
		return BACKUP_SORT_ERR_IO;
	}
	if (in_open(in_r, in, header, (opts != NULL && opts->threads > 0) ? opts->threads : backup_sort_auto_threads()) != 0) {
		in_close(in_r);
		free(in_r);
		return BACKUP_SORT_ERR_INPUT;
	}
	uint8_t flags = in_r->legacy ? 0 : packet_codec_file_header_flags(header);
	if ((flags & PACKET_CODEC_FILE_FLAG_COMPLETE) == 0) {
		char sidecar[PATH_MAX + 16];
		snprintf(sidecar, sizeof sidecar, "%s.spillcount", in);
		if (access(sidecar, F_OK) == 0) {
			log_error("sortback : %s n'est pas autonome (son débordement est dans le cliché que nomme %s) — "
			          "faire d'abord une sauvegarde « backup », qui l'embarque\n", in, sidecar);
			in_close(in_r);
			free(in_r);
			return BACKUP_SORT_ERR_INCOMPLETE;
		}
	}
	int canon = !in_r->legacy;

	char tmp_dir[PATH_MAX];
	if (opts != NULL && opts->tmp_dir != NULL) {
		snprintf(tmp_dir, sizeof tmp_dir, "%s", opts->tmp_dir);
	} else {
		snprintf(tmp_dir, sizeof tmp_dir, "%s", out);
		char *slash = strrchr(tmp_dir, '/');
		if (slash == NULL) {
			snprintf(tmp_dir, sizeof tmp_dir, ".");
		} else if (slash == tmp_dir) {
			tmp_dir[1] = '\0';
		} else {
			*slash = '\0';
		}
	}
	char out_tmp[PATH_MAX + 16];
	snprintf(out_tmp, sizeof out_tmp, "%s.sorting", out);

	size_t mem = (opts != NULL && opts->mem_bytes > 0) ? opts->mem_bytes : (size_t)BACKUP_SORT_MEM_MB_DEFAULT << 20;
	int threads = (opts != NULL && opts->threads > 0) ? opts->threads : backup_sort_auto_threads();
	if (threads > BACKUP_SORT_THREADS_MAX) {
		threads = BACKUP_SORT_THREADS_MAX;
	}
	// Deux tampons de la moitié du budget : la lecture remplit l'un pendant
	// que l'autre est trié et écrit.
	batch_t batch[2];
	memset(batch, 0, sizeof batch);
	run_list_t runs = { 0 };
	int rc = BACKUP_SORT_OK;
	int direct = 0;
	if (batch_alloc(&batch[0], mem / 2) != 0 || batch_alloc(&batch[1], mem / 2) != 0) {
		rc = BACKUP_SORT_ERR_IO;
	}

	pthread_t job_tid;
	run_job_t job = { 0 };
	int job_active = 0;
	int cur = 0;
	int more = (rc == BACKUP_SORT_OK);
	double t_read = 0, t_wait = 0;
	while (more) {
		double ta = sort_now();
		int fill = batch_fill(&batch[cur], in_r, st);
		t_read += sort_now() - ta;
		if (fill < 0) {
			log_error("sortback : %s — entrée tronquée ou illisible après %llu possibilité(s)\n", in, st->records);
			rc = BACKUP_SORT_ERR_INPUT;
			break;
		}
		more = fill;
		if (!more && runs.n == 0 && !job_active) {
			// Tout a tenu dans un tampon : la sortie directement, sans paquet.
			direct = 1;
			int r = batch_sort(&batch[cur], threads, canon);
			if (r == BACKUP_SORT_BAD_RECORD) {
				log_error("sortback : %s — enregistrement incohérent\n", in);
				rc = BACKUP_SORT_ERR_INPUT;
			} else if (r != 0 || write_direct(&batch[cur], out_tmp, flags, st) != 0) {
				rc = BACKUP_SORT_ERR_IO;
			}
			st->runs = 1;
			break;
		}
		if (job_active) {
			double tw = sort_now();
			pthread_join(job_tid, NULL);
			t_wait += sort_now() - tw;
			job_active = 0;
			if (job.rc != 0) {
				rc = (job.rc == BACKUP_SORT_BAD_RECORD) ? BACKUP_SORT_ERR_INPUT : BACKUP_SORT_ERR_IO;
				log_error("sortback : paquet %zu — %s\n", runs.n,
				          rc == BACKUP_SORT_ERR_INPUT ? "enregistrement incohérent dans l'entrée" : "écriture impossible");
				break;
			}
			log_info("sortback : paquet %zu trié (%llu possibilité(s) lues, %.0f s)\n", runs.n, st->records,
			         sort_now() - t0);
		}
		if (batch[cur].n > 0) {
			run_t *run = run_new(&runs, tmp_dir);
			if (run == NULL) {
				rc = BACKUP_SORT_ERR_IO;
				break;
			}
			job = (run_job_t){ &batch[cur], threads, canon, run, -1 };
			if (pthread_create(&job_tid, NULL, run_job_main, &job) == 0) {
				job_active = 1;
			} else {
				run_job_main(&job);
				if (job.rc != 0) {
					rc = (job.rc == BACKUP_SORT_BAD_RECORD) ? BACKUP_SORT_ERR_INPUT : BACKUP_SORT_ERR_IO;
					break;
				}
			}
			cur ^= 1;
		}
	}
	if (job_active) {
		pthread_join(job_tid, NULL);
		if (job.rc != 0 && rc == BACKUP_SORT_OK) {
			rc = (job.rc == BACKUP_SORT_BAD_RECORD) ? BACKUP_SORT_ERR_INPUT : BACKUP_SORT_ERR_IO;
			log_error("sortback : dernier paquet — %s\n",
			          rc == BACKUP_SORT_ERR_INPUT ? "enregistrement incohérent dans l'entrée" : "écriture impossible");
		}
	}
	in_close(in_r);
	free(in_r);
	batch_free(&batch[0]);
	batch_free(&batch[1]);

	if (rc == BACKUP_SORT_OK && !direct) {
		st->runs = runs.n;
		log_info("sortback : fusion de %zu paquet(s) sur %d fil(s) (%.0f s ; lecture %.0f s, attente du tri %.0f s)\n",
		         runs.n, threads, sort_now() - t0, t_read, t_wait);
		if (merge_runs(&runs, threads, tmp_dir, out_tmp, flags, st) != 0) {
			rc = BACKUP_SORT_ERR_IO;
		}
	}
	runs_unlink(&runs);
	if (rc == BACKUP_SORT_OK) {
		log_info("sortback : sortie écrite (%.0f s)\n", sort_now() - t0);
		if (rename(out_tmp, out) != 0) {
			log_errno("sortback : renommage en %s ", out);
			rc = BACKUP_SORT_ERR_IO;
		}
	}
	if (rc != BACKUP_SORT_OK) {
		unlink(out_tmp);
	}
	return rc;
}
