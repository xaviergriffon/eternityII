/*
 * check_rings — vérifie un fichier `.back` d'anneaux de bordure produit par
 * `border_mass --save-rings`, en UNE passe et à mémoire bornée.
 *
 * Répond à deux questions distinctes :
 *
 *  1. COHÉRENCE — chaque enregistrement est-il un anneau valide ? Délégué à
 *     `border_ring_validate` (tests/tools/border_walk.c), testé unitairement.
 *
 *  2. DOUBLONS — deux enregistrements décrivent-ils le même anneau ?
 *     ATTENTION : `possibility_packet` a du PADDING caché (cf. AGENTS.md), le
 *     hacher brut est INTERDIT — deux anneaux identiques pourraient différer
 *     sur des octets de bourrage et le doublon passerait inaperçu. On hache
 *     donc les BORDER_RING_LEN valeurs de grille dans l'ordre de l'anneau,
 *     rien d'autre : c'est l'identité de l'anneau, pas sa représentation.
 *
 * Pourquoi ça tient en RAM : on ne peut pas garder 10^9 empreintes en
 * mémoire, mais on n'en a pas besoin. Chaque empreinte 128 bits part dans
 * l'un de NB_BUCKETS seaux selon ses bits de poids fort — deux empreintes
 * égales tombent forcément dans le MÊME seau. Il suffit donc de traiter les
 * seaux un par un : tri en mémoire, balayage des adjacents. Mesuré sur un
 * fichier de 537 Gio (10^9 anneaux, 12 workers) : pic RAM 239 Mo, 14 Go de
 * fichiers temporaires, ~18 min au total, 959 Mo/s en lecture.
 *
 * Coquille d'E/S uniquement (fork/seaux/tri), non testée unitairement —
 * même statut que border_mass.c et gen_root.c ; toute la logique vérifiable
 * vit dans border_ring_validate.
 *
 * Usage :
 *   make check-rings
 *   tests/tools/check_rings <rings.back> <pieces.csv> <work_dir> [nb_workers]
 *
 * Code de sortie 0 si et seulement si le fichier est intègre.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <inttypes.h>

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "tools/border_walk.h"
#include "tools/ring_codec.h"

#define NB_BUCKETS 256
#define READ_BATCH 2048
#define BUCKET_BUF 4096          /* empreintes tamponnées par seau avant écriture */

struct u128 { uint64_t hi, lo; };

static int u128_cmp(const void *a, const void *b)
{
    const struct u128 *x = a, *y = b;
    if (x->hi != y->hi) return x->hi < y->hi ? -1 : 1;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return 0;
}

static int8_t ring_order[BORDER_RING_LEN][2];

/* Empreinte des 60 cases de l'anneau — JAMAIS les octets bruts du struct. */
/* Empreinte d'un anneau. En `.back`, on hache les valeurs de GRILLE, jamais
   les octets bruts du paquet : `possibility_packet` a du padding caché (cf.
   AGENTS.md) et deux anneaux identiques pourraient différer dessus, laissant
   passer un doublon. En packed6 le problème disparaît — l'enregistrement est
   déjà une forme canonique, sans bourrage ni champ annexe : on hache les
   octets tels quels. */
static struct u128 ring_fingerprint(const struct possibility_packet *p)
{
    uint64_t h1 = 1469598103934665603ULL, h2 = 1099511628211ULL;
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        uint64_t v = (uint64_t)(uint16_t)p->grid[ring_order[i][0]][ring_order[i][1]];
        h1 = (h1 ^ v) * 1099511628211ULL;
        h2 = (h2 ^ (v + (uint64_t)i)) * 0x9E3779B97F4A7C15ULL;
        h2 ^= h2 >> 29;
    }
    struct u128 r = { h1, h2 };
    return r;
}

static struct u128 bytes_fingerprint(const uint8_t *b, size_t n)
{
    uint64_t h1 = 1469598103934665603ULL, h2 = 1099511628211ULL;
    for (size_t i = 0; i < n; i++) {
        h1 = (h1 ^ b[i]) * 1099511628211ULL;
        h2 = (h2 ^ (b[i] + (uint64_t)i)) * 0x9E3779B97F4A7C15ULL;
        h2 ^= h2 >> 29;
    }
    struct u128 r = { h1, h2 };
    return r;
}

struct worker_out {
    FILE *f[NB_BUCKETS];
    struct u128 buf[NB_BUCKETS][BUCKET_BUF];
    int n[NB_BUCKETS];
};

static void bucket_flush(struct worker_out *o, int b)
{
    if (o->n[b] > 0) {
        if (fwrite(o->buf[b], sizeof(struct u128), (size_t)o->n[b], o->f[b]) != (size_t)o->n[b]) {
            fprintf(stderr, "check_rings : ecriture d'un seau impossible\n");
            exit(1);
        }
        o->n[b] = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <rings.back> <pieces.csv> <work_dir> [nb_workers]\n", argv[0]);
        return 2;
    }
    const char *rings_path = argv[1];
    const char *work_dir = argv[3];
    int nb_workers = (argc > 4) ? atoi(argv[4]) : 8;
    if (nb_workers < 1) nb_workers = 1;

    struct array_part *apart = read_parts(argv[2]);
    struct array_part *all = rotate_all_parts(apart);
    border_ring_order(ring_order);

    FILE *f = fopen(rings_path, "rb");
    if (f == NULL) { perror("fopen"); return 1; }
    if (fseeko(f, 0, SEEK_END) != 0) { perror("fseeko"); return 1; }
    off_t size = ftello(f);
    fclose(f);

    /* Format reconnu à la magie, jamais supposé : un `.back` relu comme du
       packed6 (ou l'inverse) donnerait des anneaux absurdes en silence. */
    struct ring_codec_table table;
    int packed = 0;
    off_t hdr = 0;
    off_t rec = (off_t)sizeof(struct possibility_packet);

    f = fopen(rings_path, "rb");
    if (f == NULL) { perror("fopen"); return 1; }
    uint8_t probe[RING_CODEC_HEADER_BYTES];
    if (size >= (off_t)sizeof probe && fread(probe, sizeof probe, 1, f) == 1 &&
        memcmp(probe, RING_CODEC_MAGIC, 8) == 0) {
        if (ring_codec_read_header(probe, &table) != 0) {
            fprintf(stderr, "check_rings : ECHEC — en-tete packed6 illisible (version, geometrie "
                            "ou table incompatibles avec ce binaire)\n");
            fclose(f);
            return 1;
        }
        packed = 1;
        hdr = (off_t)RING_CODEC_HEADER_BYTES;
        rec = (off_t)RING_CODEC_PACKED_BYTES;
    }
    fclose(f);

    if ((size - hdr) % rec != 0) {
        fprintf(stderr, "check_rings : ECHEC — taille %lld non multiple de %lld (enregistrement tronque)\n",
                (long long)(size - hdr), (long long)rec);
        return 1;
    }
    long long total = (long long)((size - hdr) / rec);
    fprintf(stderr, "check_rings : format %s, %lld anneaux (%lld o/anneau), %d workers, seaux dans %s\n",
            packed ? "packed6" : "back", total, (long long)rec, nb_workers, work_dir);

    /* --- Passe 1 : validation + repartition des empreintes en seaux --- */
    pid_t *pids = malloc((size_t)nb_workers * sizeof *pids);
    for (int w = 0; w < nb_workers; w++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            long long lo = total * w / nb_workers;
            long long hi = total * (w + 1) / nb_workers;

            struct worker_out *o = calloc(1, sizeof *o);
            for (int b = 0; b < NB_BUCKETS; b++) {
                char path[512];
                snprintf(path, sizeof path, "%s/b_%03d_%03d.bin", work_dir, b, w);
                o->f[b] = fopen(path, "wb");
                if (o->f[b] == NULL) { perror("fopen bucket"); exit(1); }
            }

            FILE *in = fopen(rings_path, "rb");
            if (in == NULL || fseeko(in, hdr + lo * rec, SEEK_SET) != 0) { perror("open/seek"); exit(1); }

            uint8_t *batch = malloc(READ_BATCH * (size_t)rec);
            long long done = 0, bad = 0;
            while (done < hi - lo) {
                size_t want = (size_t)((hi - lo - done) < READ_BATCH ? (hi - lo - done) : READ_BATCH);
                size_t got = fread(batch, (size_t)rec, want, in);
                if (got == 0) break;
                for (size_t i = 0; i < got; i++) {
                    const uint8_t *raw = batch + i * (size_t)rec;
                    struct possibility_packet ring;
                    struct u128 h;
                    int rc;

                    if (packed) {
                        /* -30 : le décodage lui-même a échoué (index hors
                           table, ou aucune rotation ne met les faces nulles
                           vers l'extérieur) — distinct des BORDER_RING_BAD_*,
                           qui portent sur un plateau déjà reconstruit. */
                        rc = (ring_codec_unpack(raw, &table, all, &ring) != 0) ? -30 : 0;
                        if (rc == 0) {
                            rc = border_ring_validate(&ring, all);
                        }
                        /* L'enregistrement packed6 est déjà canonique : on le
                           hache tel quel, sans repasser par la grille. */
                        h = bytes_fingerprint(raw, (size_t)rec);
                    } else {
                        memcpy(&ring, raw, sizeof ring);
                        rc = border_ring_validate(&ring, all);
                        h = ring_fingerprint(&ring);
                    }

                    if (rc != 0) {
                        if (bad < 5) {
                            fprintf(stderr, "check_rings[w%d] : anneau #%lld INVALIDE (code %d)\n",
                                    w, lo + done + (long long)i, rc);
                        }
                        bad++;
                    }
                    int b = (int)(h.hi >> 56);
                    o->buf[b][o->n[b]++] = h;
                    if (o->n[b] == BUCKET_BUF) bucket_flush(o, b);
                }
                done += (long long)got;
            }
            for (int b = 0; b < NB_BUCKETS; b++) { bucket_flush(o, b); fclose(o->f[b]); }
            fprintf(stderr, "check_rings[w%d] : %lld anneaux verifies, %lld invalide(s)\n", w, done, bad);
            exit(bad == 0 ? 0 : 3);
        }
        pids[w] = pid;
    }

    int invalid = 0;
    for (int w = 0; w < nb_workers; w++) {
        int st; waitpid(pids[w], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) invalid = 1;
    }

    /* --- Passe 2 : un seau a la fois en RAM, tri, doublons adjacents --- */
    long long dups = 0, fingerprints = 0;
    size_t cap = 1 << 20;
    struct u128 *arr = malloc(cap * sizeof *arr);
    for (int b = 0; b < NB_BUCKETS; b++) {
        size_t n = 0;
        for (int w = 0; w < nb_workers; w++) {
            char path[512];
            snprintf(path, sizeof path, "%s/b_%03d_%03d.bin", work_dir, b, w);
            FILE *in = fopen(path, "rb");
            if (in == NULL) continue;
            struct u128 tmp[BUCKET_BUF];
            size_t got;
            while ((got = fread(tmp, sizeof *tmp, BUCKET_BUF, in)) > 0) {
                if (n + got > cap) {
                    while (n + got > cap) cap *= 2;
                    arr = realloc(arr, cap * sizeof *arr);
                    if (arr == NULL) { fprintf(stderr, "check_rings : RAM insuffisante\n"); return 1; }
                }
                memcpy(arr + n, tmp, got * sizeof *tmp);
                n += got;
            }
            fclose(in);
            remove(path);
        }
        qsort(arr, n, sizeof *arr, u128_cmp);
        for (size_t i = 1; i < n; i++) {
            if (u128_cmp(&arr[i - 1], &arr[i]) == 0) dups++;
        }
        fingerprints += (long long)n;
        if ((b % 64) == 0) fprintf(stderr, "check_rings : seau %d/%d...\n", b, NB_BUCKETS);
    }

    printf("anneaux            : %lld\n", total);
    printf("empreintes reparties: %lld\n", fingerprints);
    printf("invalides          : %s\n", invalid ? "OUI (voir stderr)" : "aucun");
    printf("doublons           : %lld\n", dups);
    return (invalid || dups != 0 || fingerprints != total) ? 1 : 0;
}
