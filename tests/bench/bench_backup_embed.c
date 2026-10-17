/*
 * Banc de RECOPIE DU DÉBORDEMENT dans un `.back` (`make bench-backup-embed`).
 *
 * Mesure `stock_spill_embed_snapshot` — la phase qui fait l'essentiel d'une
 * sauvegarde autonome (commande console `backup`) dès que le stock vit surtout
 * sur disque : au niveau 26 (26,65 G possibilités), 10 h 16 dans un seul fil.
 *
 * Usage :
 *   bench_backup_embed <dir_débordement> <dir_travail> <piles> <seq_début> <segments> [tours [garder.back]]
 *
 * Recopie, pour chacune des `piles` premières piles non vérifiées
 * (`spill_u_<f>_*`), `segments` segments consécutifs à partir de `seq_début`
 * dans `<dir_travail>/base` (une fois, lecture seule de la source), puis à
 * chaque tour : `stock_spill_configure(<dir_travail>/spill)`, un cliché
 * `snap` fait de liens physiques vers la base et de son manifeste, et la
 * recopie de ce cliché dans un `.back` compressé (fils zstd du serveur), dont
 * on mesure le temps réel et le CPU (tous fils). Le cliché est supprimé par la
 * recopie elle-même, comme en production ; la base reste. `garder.back` : le
 * `.back` du dernier tour y est conservé (comparer deux binaires octet à octet,
 * corps décompressé : `tail -c +33 x.back | zstd -dc | sha256sum`).
 *
 * Mettre `<dir_travail>` sur tmpfs pour mesurer le CPU et non le disque.
 * Jamais rattaché à `make test`.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/backup_stream.h"
#include "core/datamanager.h"
#include "core/stock_spill.h"

/// Absente des binaires antérieurs à la recopie des trames telles quelles :
/// le même banc se compile contre l'ancien code pour comparer.
unsigned long long backup_stream_frames_passed(void) __attribute__((weak));

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double cpu_s(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6 + (double)ru.ru_stime.tv_sec
	       + (double)ru.ru_stime.tv_usec / 1e6;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/// Possibilités d'un segment en trames (en-têtes « ETSB » seulement), -1 si
/// le fichier ne se découpe pas en trames.
static long long segment_records(const char *path, long *out_bytes)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return -1;
	}
	long long total = 0;
	uint8_t h[20];
	size_t got;
	while ((got = fread(h, 1, sizeof h, f)) == sizeof h) {
		if (memcmp(h, "ETSB", 4) != 0 || fseek(f, (long)get_le32(h + 16) + 12, SEEK_CUR) != 0) {
			fclose(f);
			return -1;
		}
		total += get_le32(h + 8);
	}
	*out_bytes = ftell(f);
	fclose(f);
	return (got == 0) ? total : -1;
}

static int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	if (in == NULL) {
		return -1;
	}
	FILE *out = fopen(to, "wb");
	if (out == NULL) {
		fclose(in);
		return -1;
	}
	static char buf[1 << 20];
	size_t n;
	int rc = 0;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			rc = -1;
			break;
		}
	}
	fclose(in);
	if (fclose(out) != 0) {
		rc = -1;
	}
	return rc;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		fprintf(stderr, "usage : %s <dir_débordement> <dir_travail> <piles> <seq_début> <segments> "
		        "[tours [garder.back]]\n",
		        argv[0]);
		return 2;
	}
	const char *src = argv[1];
	const char *work = argv[2];
	int stacks = atoi(argv[3]);
	int first = atoi(argv[4]);
	int segments = atoi(argv[5]);
	int runs = (argc > 6) ? atoi(argv[6]) : 3;
	const char *keep = (argc > 7) ? argv[7] : NULL;
	if (stacks <= 0 || first <= 0 || segments <= 0 || runs <= 0) {
		fprintf(stderr, "arguments invalides\n");
		return 2;
	}

	char base[PATH_MAX], spill[PATH_MAX], snap[PATH_MAX + 8], path[PATH_MAX + 64], from[PATH_MAX + 64];
	snprintf(base, sizeof base, "%s/base", work);
	snprintf(spill, sizeof spill, "%s/spill", work);
	mkdir(work, 0755);
	mkdir(base, 0755);

	unsigned long long *packets = calloc((size_t)stacks, sizeof *packets);
	long *tail = calloc((size_t)stacks, sizeof *tail);
	unsigned long long expected = 0;
	unsigned long long bytes_in = 0;
	for (int f = 0; f < stacks; f++) {
		for (int k = 0; k < segments; k++) {
			snprintf(path, sizeof path, "%s/spill_u_%d_%d.dat", base, f, k + 1);
			struct stat st;
			if (stat(path, &st) != 0) {
				snprintf(from, sizeof from, "%s/spill_u_%d_%d.dat", src, f, first + k);
				if (copy_file(from, path) != 0) {
					fprintf(stderr, "copie de %s impossible : %s\n", from, strerror(errno));
					return 1;
				}
			}
			long bytes = 0;
			long long n = segment_records(path, &bytes);
			if (n < 0) {
				fprintf(stderr, "%s : pas un segment en trames\n", path);
				return 1;
			}
			packets[f] += (unsigned long long)n;
			tail[f] = bytes;
			bytes_in += (unsigned long long)bytes;
		}
		expected += packets[f];
	}
	printf("échantillon : %d pile(s) x %d segment(s), %llu possibilités, %.1f Mo de segments (%.1f o/possibilité)\n",
	       stacks, segments, expected, (double)bytes_in / 1e6, (double)bytes_in / (double)expected);

	backup_stream_set_workers(BACKUP_STREAM_SERVER_WORKERS);
	backup_stream_set_compress_for_tests(1);
	for (int r = 0; r < runs; r++) {
		mkdir(spill, 0755);
		stock_spill_configure(spill, stacks);
		snprintf(snap, sizeof snap, "%s/snap", spill);
		mkdir(snap, 0755);
		for (int f = 0; f < stacks; f++) {
			for (int k = 0; k < segments; k++) {
				snprintf(from, sizeof from, "%s/spill_u_%d_%d.dat", base, f, k + 1);
				snprintf(path, sizeof path, "%s/spill_u_%d_%d.dat", snap, f, k + 1);
				if (link(from, path) != 0) {
					fprintf(stderr, "lien %s : %s\n", path, strerror(errno));
					return 1;
				}
			}
		}
		snprintf(path, sizeof path, "%s/manifest.txt", snap);
		FILE *mf = fopen(path, "w");
		fprintf(mf, "eternityii-spill-manifest-v3\n");
		for (int f = 0; f < stacks; f++) {
			fprintf(mf, "u %d %d %llu %ld\n", f, segments, packets[f], tail[f]);
		}
		fclose(mf);

		snprintf(path, sizeof path, "%s/out.back", work);
		FILE *raw = fopen(path, "wb");
		setvbuf(raw, NULL, _IOFBF, 1 << 20);
		FILE *out = backup_stream_open_write(raw, 0);
		unsigned long long passed0 = backup_stream_frames_passed ? backup_stream_frames_passed() : 0;
		double t0 = now_s(), c0 = cpu_s();
		unsigned long long written = 0;
		int rc = stock_spill_embed_snapshot("snap", out, &written);
		int closed = fclose(out);
		double dt = now_s() - t0, dc = cpu_s() - c0;
		unsigned long long passed = backup_stream_frames_passed ? backup_stream_frames_passed() - passed0 : 0;
		struct stat st;
		stat(path, &st);
		printf("tour %d : rc=%d fclose=%d %llu/%llu recopiées en %.2f s — %.0f ns/possibilité, "
		       "%.2f M/s, CPU %.2f s (%.0f %%), .back %.1f Mo, %llu trame(s) recopiée(s) telles quelles\n",
		       r + 1, rc, closed, written, expected, dt, dt * 1e9 / (double)expected, (double)expected / dt / 1e6,
		       dc, 100.0 * dc / dt, (double)st.st_size / 1e6, passed);
		if (keep != NULL && r == runs - 1) {
			if (rename(path, keep) != 0) {
				fprintf(stderr, "%s -> %s : %s\n", path, keep, strerror(errno));
				return 1;
			}
		} else {
			unlink(path);
		}
		if (rc != 0 || closed != 0 || written != expected) {
			return 1;
		}
	}
	free(packets);
	free(tail);
	return 0;
}
