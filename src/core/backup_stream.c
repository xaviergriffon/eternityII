// fopencookie (glibc) n'est déclaré que sous _GNU_SOURCE, que -std=gnu99 ne
// définit pas.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "core/backup_stream.h"
#include "core/core_static_variables.h"
#include "core/packet_codec.h"
#include "ui/logger.h"

#ifdef ETII_ZSTD
#include <zstd.h>
#endif

/// Tampon stdio des flux (fichier sous-jacent et flux compressé).
#define BACKUP_STREAM_IO_BYTES (1 << 20)

static int g_compress_override = -1;

/// Fils de compression zstd d'un flux d'écriture (cf. `backup_stream_set_workers`) :
/// la compression quitte le fil qui écrit sous le gel des pools, ce fil ne fait
/// plus que recopier dans les tampons des tâches.
static int g_workers = 0;

void backup_stream_set_workers(int workers)
{
	g_workers = (workers > 0) ? workers : 0;
}

int backup_stream_compresses(void)
{
#ifdef ETII_ZSTD
	return g_compress_override != 0;
#else
	return 0;
#endif
}

void backup_stream_set_compress_for_tests(int on)
{
	g_compress_override = (on < 0) ? -1 : (on != 0);
}

#ifdef ETII_ZSTD

/// Compressé lu entre deux restitutions du cache de pages au noyau : comme
/// l'import en clair (`import_drop_read_cache`, `core/datamanager.c`), un `.back`
/// ne se relit pas et ses Go en cache poussaient le serveur en swap.
#define BACKUP_STREAM_DROP_CACHE_BYTES (64ULL << 20)


typedef struct {
	FILE *raw;
	ZSTD_CCtx *cctx;           // écriture
	ZSTD_DCtx *dctx;           // lecture
	uint8_t *buf;              // compressé : à écrire, ou lu en attente
	size_t cap;
	ZSTD_inBuffer in;          // lecture : octets compressés pas encore décodés
	int raw_eof;
	int frame_open;            // lecture : une trame est entamée et pas finie
	int error;
	unsigned long long since_drop;
} zstream_t;

static void zs_free(zstream_t *z)
{
	ZSTD_freeCCtx(z->cctx);
	ZSTD_freeDCtx(z->dctx);
	free(z->buf);
	free(z);
}

static long zs_write(zstream_t *z, const char *data, size_t len)
{
	if (z->error) {
		errno = EIO;
		return -1;
	}
	ZSTD_inBuffer in = { data, len, 0 };
	while (in.pos < in.size) {
		ZSTD_outBuffer out = { z->buf, z->cap, 0 };
		size_t r = ZSTD_compressStream2(z->cctx, &out, &in, ZSTD_e_continue);
		if (ZSTD_isError(r) || (out.pos > 0 && fwrite(z->buf, 1, out.pos, z->raw) != out.pos)) {
			z->error = 1;
			errno = EIO;
			return -1;
		}
	}
	return (long)len;
}

static void zs_drop_cache(zstream_t *z, size_t got)
{
#if defined(POSIX_FADV_DONTNEED)
	z->since_drop += got;
	if (z->since_drop >= BACKUP_STREAM_DROP_CACHE_BYTES || got == 0) {
		off_t pos = ftello(z->raw);
		if (pos > 0) {
			(void)posix_fadvise(fileno(z->raw), 0, pos, POSIX_FADV_DONTNEED);
		}
		z->since_drop = 0;
	}
#else
	(void)z;
	(void)got;
#endif
}

static long zs_read(zstream_t *z, char *dst, size_t len)
{
	if (z->error) {
		errno = EIO;
		return -1;
	}
	ZSTD_outBuffer out = { dst, len, 0 };
	while (out.pos == 0) {
		if (z->in.pos == z->in.size && !z->raw_eof) {
			size_t got = fread(z->buf, 1, z->cap, z->raw);
			if (got == 0) {
				if (ferror(z->raw)) {
					z->error = 1;
					errno = EIO;
					return -1;
				}
				z->raw_eof = 1;
			}
			z->in.src = z->buf;
			z->in.size = got;
			z->in.pos = 0;
			zs_drop_cache(z, got);
		}
		size_t before = z->in.pos;
		size_t r = ZSTD_decompressStream(z->dctx, &out, &z->in);
		if (ZSTD_isError(r)) {
			z->error = 1;
			errno = EIO;
			return -1;
		}
		int progress = (out.pos > 0 || z->in.pos > before);
		if (progress) {
			z->frame_open = (r != 0);
		} else if (z->in.pos == z->in.size && z->raw_eof) {
			if (z->frame_open) {
				// Trame entamée et fichier fini : corps tronqué.
				z->error = 1;
				errno = EIO;
				return -1;
			}
			return 0;
		}
	}
	return (long)out.pos;
}

static int zs_close(zstream_t *z)
{
	int failed = z->error;
	if (z->cctx != NULL && !failed) {
		ZSTD_inBuffer in = { NULL, 0, 0 };
		size_t r;
		do {
			ZSTD_outBuffer out = { z->buf, z->cap, 0 };
			r = ZSTD_compressStream2(z->cctx, &out, &in, ZSTD_e_end);
			if (ZSTD_isError(r) || (out.pos > 0 && fwrite(z->buf, 1, out.pos, z->raw) != out.pos)) {
				failed = 1;
				break;
			}
		} while (r != 0);
	}
	if (fclose(z->raw) != 0) {
		failed = 1;
	}
	zs_free(z);
	if (failed) {
		errno = EIO;
		return -1;
	}
	return 0;
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
static int zs_read_fn(void *c, char *buf, int n)
{
	return (int)zs_read(c, buf, (size_t)n);
}

static int zs_write_fn(void *c, const char *buf, int n)
{
	return (int)zs_write(c, buf, (size_t)n);
}

static int zs_close_fn(void *c)
{
	return zs_close(c);
}
#else
static ssize_t zs_read_fn(void *c, char *buf, size_t n)
{
	return (ssize_t)zs_read(c, buf, n);
}

/// fopencookie : 0, jamais un négatif, signale l'erreur d'écriture.
static ssize_t zs_write_fn(void *c, const char *buf, size_t n)
{
	long w = zs_write(c, buf, n);
	return (w < 0) ? 0 : (ssize_t)w;
}

static int zs_close_fn(void *c)
{
	return zs_close(c);
}
#endif

/// Enveloppe `raw` dans un flux zstd. @return NULL si rien n'a pu être alloué
/// (`raw` reste alors ouvert, à l'appelant de le fermer).
static FILE *zs_open(FILE *raw, int reading)
{
	zstream_t *z = calloc(1, sizeof *z);
	if (z == NULL) {
		return NULL;
	}
	z->raw = raw;
	z->cap = reading ? ZSTD_DStreamInSize() : ZSTD_CStreamOutSize();
	if (z->cap < BACKUP_STREAM_IO_BYTES) {
		z->cap = BACKUP_STREAM_IO_BYTES;
	}
	z->buf = malloc(z->cap);
	if (reading) {
		z->dctx = ZSTD_createDCtx();
	} else {
		z->cctx = ZSTD_createCCtx();
		if (z->cctx != NULL
		    && (ZSTD_isError(ZSTD_CCtx_setParameter(z->cctx, ZSTD_c_compressionLevel, BACKUP_STREAM_ZSTD_LEVEL))
		        || ZSTD_isError(ZSTD_CCtx_setParameter(z->cctx, ZSTD_c_checksumFlag, 1)))) {
			ZSTD_freeCCtx(z->cctx);
			z->cctx = NULL;
		}
		if (z->cctx != NULL) {
			// Sans support multi-fil dans la libzstd, l'appel échoue : la
			// compression reste dans le fil appelant, rien d'autre ne change.
			if (g_workers > 0) {
				(void)ZSTD_CCtx_setParameter(z->cctx, ZSTD_c_nbWorkers, g_workers);
			}
		}
	}
	if (z->buf == NULL || (reading ? z->dctx == NULL : z->cctx == NULL)) {
		zs_free(z);
		return NULL;
	}
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
	FILE *f = funopen(z, reading ? zs_read_fn : NULL, reading ? NULL : zs_write_fn, NULL, zs_close_fn);
#else
	cookie_io_functions_t io = {
		.read = reading ? zs_read_fn : NULL,
		.write = reading ? NULL : zs_write_fn,
		.seek = NULL,
		.close = zs_close_fn,
	};
	FILE *f = fopencookie(z, reading ? "r" : "w", io);
#endif
	if (f == NULL) {
		zs_free(z);
		return NULL;
	}
	setvbuf(f, NULL, _IOFBF, BACKUP_STREAM_IO_BYTES);
	return f;
}

#endif /* ETII_ZSTD */

FILE *backup_stream_open_write(FILE *raw, uint8_t flags)
{
	int zstd = backup_stream_compresses();
	uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
	packet_codec_write_file_header_codec(header, flags, zstd);
	if (fwrite(header, 1, sizeof header, raw) != sizeof header) {
		int e = errno;
		fclose(raw);
		errno = (e != 0) ? e : EIO;
		return NULL;
	}
	if (!zstd) {
		return raw;
	}
#ifdef ETII_ZSTD
	FILE *f = zs_open(raw, 0);
	if (f == NULL) {
		fclose(raw);
		errno = ENOMEM;
	}
	return f;
#else
	return raw; // inatteignable : backup_stream_compresses() vaut 0
#endif
}

FILE *backup_stream_open_read(FILE *raw, const char *name, int *out_format, uint8_t *out_header)
{
	uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
	size_t got = fread(header, 1, sizeof header, raw);
	if (got != sizeof header || memcmp(header, PACKET_CODEC_FILE_MAGIC, 8) != 0) {
		// Pas de magie (ou fichier plus court que l'en-tête) : format hérité.
		*out_format = BACKUP_STREAM_LEGACY;
		if (out_header != NULL) {
			memset(out_header, 0, PACKET_CODEC_FILE_HEADER_BYTES);
		}
		rewind(raw);
		return raw;
	}
	if (packet_codec_read_file_header(header) != 0) {
		log_error("%s : fichier de stock compacté d'une version ou d'une géométrie "
		          "incompatible avec ce binaire (ETERN_SIZE=%d, ETERN_PARTS=%d) — "
		          "refusé, aucune possibilité importée\n",
		          name, ETERN_SIZE, ETERN_PARTS);
		fclose(raw);
		return NULL;
	}
	*out_format = BACKUP_STREAM_PACKED;
	if (out_header != NULL) {
		memcpy(out_header, header, sizeof header);
	}
	if (!packet_codec_file_header_is_zstd(header)) {
		return raw;
	}
#ifdef ETII_ZSTD
	FILE *f = zs_open(raw, 1);
	if (f == NULL) {
		log_error("%s : contexte de décompression zstd non alloué — refusé, aucune possibilité importée\n", name);
		fclose(raw);
	}
	return f;
#else
	log_error("%s : fichier de stock compressé par zstd, illisible par ce binaire compilé sans zstd "
	          "(recompiler avec make ZSTD=1) — refusé, aucune possibilité importée\n", name);
	fclose(raw);
	return NULL;
#endif
}

FILE *backup_stream_fopen_read(const char *path, int *out_format, uint8_t *out_header)
{
	FILE *raw = fopen(path, "rb");
	if (raw == NULL) {
		return NULL;
	}
	return backup_stream_open_read(raw, path, out_format, out_header);
}
