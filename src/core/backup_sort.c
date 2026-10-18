#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/backup_sort.h"
#include "core/backup_stream.h"
#include "core/packet_codec.h"
#include "core/possibility.h"
#include "ui/logger.h"

#define SORT_PREFIX (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES)
#define SORT_READ_CHUNK (4 << 20)

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

static int cmp_ptr(const void *pa, const void *pb)
{
	const uint8_t *a = *(const uint8_t *const *)pa, *b = *(const uint8_t *const *)pb;
	return backup_sort_record_cmp(a, rec_len(a), b, rec_len(b));
}

/* --- Lecture d'un `.back` (entrée ou paquet), enregistrement par enregistrement --- */

typedef struct {
	FILE *f;
	int packed;
	uint8_t *buf;
	size_t len, off;
	int eof;
} rec_reader_t;

static int reader_open(rec_reader_t *r, const char *path, uint8_t *header)
{
	memset(r, 0, sizeof *r);
	FILE *raw = fopen(path, "rb");
	if (raw == NULL) {
		log_errno("sortback : %s ", path);
		return -1;
	}
	r->f = backup_stream_open_read(raw, path, &r->packed, header);
	if (r->f == NULL) {
		return -1;
	}
	r->buf = malloc(SORT_READ_CHUNK);
	if (r->buf == NULL) {
		fclose(r->f);
		r->f = NULL;
		return -1;
	}
	return 0;
}

static void reader_close(rec_reader_t *r)
{
	if (r->f != NULL) {
		fclose(r->f);
	}
	free(r->buf);
	memset(r, 0, sizeof *r);
}

/**
 * @brief Copie dans `dst` (`PACKET_CODEC_MAX_BYTES` octets) l'enregistrement
 *        suivant, sous forme canonique — celle que `restore` lui donnerait.
 * @return Sa taille, 0 en fin de fichier propre, -1 s'il est tronqué ou
 *         incohérent.
 */
static long reader_next(rec_reader_t *r, uint8_t *dst)
{
	if (!r->packed) {
		// `.back` hérité : mêmes normalisations qu'`import`.
		struct possibility_packet p;
		if (fread(&p, sizeof p, 1, r->f) != 1) {
			return ferror(r->f) ? -1 : 0;
		}
		if (p.checked != 1) {
			p.checked = 0;
		}
		p.alloc = (uint16_t)possibility_placed_count(&p);
		p.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
		size_t written = 0;
		return (packet_codec_encode(&p, dst, PACKET_CODEC_MAX_BYTES, &written) == 0) ? (long)written : -1;
	}
	for (;;) {
		if (!r->eof && r->len - r->off < PACKET_CODEC_MAX_BYTES) {
			memmove(r->buf, r->buf + r->off, r->len - r->off);
			r->len -= r->off;
			r->off = 0;
			size_t got = fread(r->buf + r->len, 1, SORT_READ_CHUNK - r->len, r->f);
			r->len += got;
			if (got == 0) {
				if (ferror(r->f)) {
					return -1;
				}
				r->eof = 1;
			}
		}
		if (r->off == r->len) {
			return 0;
		}
		long n = packet_codec_canonicalize(r->buf + r->off, r->len - r->off, POSSIBILITY_MIN_CANDIDATS_UNKNOWN, NULL);
		if (n == 0 && !r->eof) {
			continue;
		}
		if (n <= 0) {
			return -1;
		}
		memcpy(dst, r->buf + r->off, (size_t)n);
		r->off += (size_t)n;
		return n;
	}
}

/* --- Écriture --------------------------------------------------------------- */

/// Ouvre `path` (qui ne doit pas exister) en `.back` aux drapeaux `flags`.
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

/// Ferme et met sur disque `path`.
static int writer_close(FILE *f, const char *path)
{
	if (fclose(f) != 0) {
		log_errno("sortback : %s ", path);
		return -1;
	}
	int fd = open(path, O_RDONLY);
	if (fd < 0 || fsync(fd) != 0) {
		log_errno("sortback : fsync %s ", path);
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	close(fd);
	return 0;
}

typedef struct {
	FILE *f;
	uint8_t prev[PACKET_CODEC_MAX_BYTES];
	size_t prev_len;
	backup_sort_stats_t *st;
} out_t;

static int out_put(out_t *o, const uint8_t *rec, size_t len)
{
	if (o->prev_len == len && memcmp(o->prev, rec, len) == 0) {
		o->st->duplicates++;
	}
	memcpy(o->prev, rec, len);
	o->prev_len = len;
	o->st->raw_bytes += len;
	return (fwrite(rec, 1, len, o->f) == len) ? 0 : -1;
}

/* --- Paquets ---------------------------------------------------------------- */

typedef struct {
	char **paths;
	size_t n, cap;
} run_list_t;

static void runs_unlink(run_list_t *runs)
{
	for (size_t i = 0; i < runs->n; i++) {
		unlink(runs->paths[i]);
		free(runs->paths[i]);
	}
	free(runs->paths);
	memset(runs, 0, sizeof *runs);
}

static int write_run(run_list_t *runs, const char *tmp_dir, uint8_t **recs, size_t n)
{
	if (runs->n == runs->cap) {
		size_t cap = runs->cap ? runs->cap * 2 : 64;
		char **p = realloc(runs->paths, cap * sizeof *p);
		if (p == NULL) {
			return -1;
		}
		runs->paths = p;
		runs->cap = cap;
	}
	char path[PATH_MAX + 64];
	snprintf(path, sizeof path, "%s/sortback-%ld-%zu.run", tmp_dir, (long)getpid(), runs->n);
	FILE *f = writer_open(path, 0);
	if (f == NULL) {
		return -1;
	}
	runs->paths[runs->n] = strdup(path);
	if (runs->paths[runs->n] == NULL) {
		fclose(f);
		unlink(path);
		return -1;
	}
	runs->n++;
	for (size_t i = 0; i < n; i++) {
		size_t len = rec_len(recs[i]);
		if (fwrite(recs[i], 1, len, f) != len) {
			fclose(f);
			return -1;
		}
	}
	return (fclose(f) == 0) ? 0 : -1;
}

/* --- Fusion ------------------------------------------------------------------ */

typedef struct {
	rec_reader_t r;
	uint8_t cur[PACKET_CODEC_MAX_BYTES];
	size_t len;
} merge_src_t;

static int src_less(const merge_src_t *a, const merge_src_t *b)
{
	return backup_sort_record_cmp(a->cur, a->len, b->cur, b->len) < 0;
}

static void heap_down(merge_src_t **h, size_t n, size_t i)
{
	for (;;) {
		size_t l = 2 * i + 1, m = i;
		if (l < n && src_less(h[l], h[m])) {
			m = l;
		}
		if (l + 1 < n && src_less(h[l + 1], h[m])) {
			m = l + 1;
		}
		if (m == i) {
			return;
		}
		merge_src_t *t = h[i];
		h[i] = h[m];
		h[m] = t;
		i = m;
	}
}

static int merge_runs(const run_list_t *runs, out_t *o)
{
	merge_src_t *src = calloc(runs->n, sizeof *src);
	merge_src_t **heap = calloc(runs->n, sizeof *heap);
	int rc = (src != NULL && heap != NULL) ? 0 : -1;
	size_t n = 0;
	for (size_t i = 0; rc == 0 && i < runs->n; i++) {
		if (reader_open(&src[i].r, runs->paths[i], NULL) != 0) {
			rc = -1;
			break;
		}
		long len = reader_next(&src[i].r, src[i].cur);
		if (len < 0) {
			rc = -1;
		} else if (len > 0) {
			src[i].len = (size_t)len;
			heap[n++] = &src[i];
		}
	}
	for (size_t i = n / 2; rc == 0 && i-- > 0;) {
		heap_down(heap, n, i);
	}
	while (rc == 0 && n > 0) {
		merge_src_t *s = heap[0];
		if (out_put(o, s->cur, s->len) != 0) {
			rc = -1;
			break;
		}
		long len = reader_next(&s->r, s->cur);
		if (len < 0) {
			rc = -1;
			break;
		}
		if (len == 0) {
			heap[0] = heap[--n];
		} else {
			s->len = (size_t)len;
		}
		heap_down(heap, n, 0);
	}
	for (size_t i = 0; src != NULL && i < runs->n; i++) {
		if (src[i].r.f != NULL) {
			reader_close(&src[i].r);
		}
	}
	free(src);
	free(heap);
	return rc;
}

/* --- Point d'entrée ---------------------------------------------------------- */

int backup_sort(const char *in, const char *out, const backup_sort_options_t *opts, backup_sort_stats_t *stats)
{
	backup_sort_stats_t local;
	backup_sort_stats_t *st = (stats != NULL) ? stats : &local;
	memset(st, 0, sizeof *st);

	if (access(out, F_OK) == 0) {
		log_error("sortback : %s existe déjà — jamais écrasé\n", out);
		return BACKUP_SORT_ERR_OUTPUT_EXISTS;
	}
	uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
	rec_reader_t in_r;
	if (reader_open(&in_r, in, header) != 0) {
		return BACKUP_SORT_ERR_INPUT;
	}
	uint8_t flags = in_r.packed ? packet_codec_file_header_flags(header) : 0;
	if ((flags & PACKET_CODEC_FILE_FLAG_COMPLETE) == 0) {
		char sidecar[PATH_MAX + 16];
		snprintf(sidecar, sizeof sidecar, "%s.spillcount", in);
		if (access(sidecar, F_OK) == 0) {
			log_error("sortback : %s n'est pas autonome (son débordement est dans le cliché que nomme %s) — "
			          "faire d'abord une sauvegarde « backup », qui l'embarque\n", in, sidecar);
			reader_close(&in_r);
			return BACKUP_SORT_ERR_INCOMPLETE;
		}
	}

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
	if (mem < 2 * PACKET_CODEC_MAX_BYTES + 64 * sizeof(uint8_t *)) {
		mem = 2 * PACKET_CODEC_MAX_BYTES + 64 * sizeof(uint8_t *);
	}
	// Un huitième du budget pour l'index : ~74 octets par enregistrement
	// contre 8 de pointeur.
	size_t ptr_cap = (mem / 8) / sizeof(uint8_t *);
	size_t arena_cap = mem - ptr_cap * sizeof(uint8_t *);
	uint8_t *arena = malloc(arena_cap);
	uint8_t **recs = malloc(ptr_cap * sizeof *recs);
	run_list_t runs = { 0 };
	out_t o = { .f = NULL, .prev_len = 0, .st = st };
	int rc = BACKUP_SORT_OK;
	if (arena == NULL || recs == NULL) {
		rc = BACKUP_SORT_ERR_IO;
	}

	int more = (rc == BACKUP_SORT_OK);
	while (more) {
		size_t used = 0, n = 0;
		while (n < ptr_cap && arena_cap - used >= PACKET_CODEC_MAX_BYTES) {
			long len = reader_next(&in_r, arena + used);
			if (len < 0) {
				log_error("sortback : %s — enregistrement tronqué ou incohérent après %llu possibilité(s)\n", in,
				          st->records);
				rc = BACKUP_SORT_ERR_INPUT;
				break;
			}
			if (len == 0) {
				more = 0;
				break;
			}
			recs[n++] = arena + used;
			used += (size_t)len;
			st->records++;
		}
		if (rc != BACKUP_SORT_OK) {
			break;
		}
		qsort(recs, n, sizeof *recs, cmp_ptr);
		if (!more && runs.n == 0) {
			// Tout a tenu en RAM : la sortie directement, sans paquet.
			o.f = writer_open(out_tmp, flags);
			if (o.f == NULL) {
				rc = BACKUP_SORT_ERR_IO;
				break;
			}
			for (size_t i = 0; i < n && rc == BACKUP_SORT_OK; i++) {
				if (out_put(&o, recs[i], rec_len(recs[i])) != 0) {
					rc = BACKUP_SORT_ERR_IO;
				}
			}
			st->runs = 1;
			break;
		}
		if (n > 0 && write_run(&runs, tmp_dir, recs, n) != 0) {
			log_errno("sortback : paquet dans %s ", tmp_dir);
			rc = BACKUP_SORT_ERR_IO;
			break;
		}
		if (n > 0) {
			log_info("sortback : paquet %zu trié (%llu possibilité(s) lues)\n", runs.n, st->records);
		}
	}
	reader_close(&in_r);
	free(arena);
	free(recs);

	if (rc == BACKUP_SORT_OK && o.f == NULL) {
		st->runs = runs.n;
		o.f = writer_open(out_tmp, flags);
		if (o.f == NULL || merge_runs(&runs, &o) != 0) {
			rc = BACKUP_SORT_ERR_IO;
		}
	}
	runs_unlink(&runs);
	if (o.f != NULL) {
		if (writer_close(o.f, out_tmp) != 0 && rc == BACKUP_SORT_OK) {
			rc = BACKUP_SORT_ERR_IO;
		}
	}
	if (rc == BACKUP_SORT_OK && rename(out_tmp, out) != 0) {
		log_errno("sortback : renommage en %s ", out);
		rc = BACKUP_SORT_ERR_IO;
	}
	if (rc != BACKUP_SORT_OK) {
		unlink(out_tmp);
	}
	return rc;
}
