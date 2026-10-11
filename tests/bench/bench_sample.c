/*
 * Banc d'ÉCHANTILLONNAGE : combien coûterait d'épuiser TOUT un stock ?
 *
 * Un stock développé par l'expansion est une COUPE complète de l'arbre de
 * recherche : MRV branche sur tous les candidats d'une case, donc les
 * sous-arbres des possibilités du stock sont disjoints et leur réunion est
 * l'espace entier (indices posés). Mesurer le coût de fermeture de n racines
 * tirées UNIFORMÉMENT permet donc d'extrapoler à la population entière :
 *
 *   - part des racines que le plafond de nœuds suffit à réfuter (Wilson 95 %) ;
 *   - coût moyen d'épuisement d'une racine, donc du stock — une BORNE BASSE dès
 *     qu'une seule racine est censurée par le plafond ;
 *   - fonction de répartition des coûts de fermeture (une fermeture étant
 *     déterministe, un run au plafond B donne toute la courbe sous B).
 *
 * Pourquoi un outil distinct de `bench_refutation --kpi` : celui-ci prend une
 * racine sur k dans l'ordre du fichier. Or cet ordre n'a rien d'aléatoire
 * (l'expansion range les enfants d'un même parent côte à côte), et il lit le
 * stock deux fois — deux passes de plusieurs centaines de Go sur un stock de
 * 6,7·10⁹ racines. Ici : un réservoir (algorithme R), UNE passe, les
 * enregistrements gardés sous forme compacte et décodés seulement s'ils sont
 * tirés.
 *
 * Deux temps, séparables (le tirage sur un gros stock est long, la mesure se
 * rejoue à d'autres plafonds) :
 *
 *   tirage  : --from-back gros.back --sample N --out echantillon.back
 *             écrit aussi echantillon.back.meta (taille de la population, graine,
 *             filtres) que la mesure relit pour extrapoler ;
 *   mesure  : --from-back echantillon.back --measure [--budget B] [--workers W]
 *   les deux d'un coup : --from-back gros.back --sample N --measure
 *
 * La mesure appelle la primitive de la preuve bornée du pruner
 * (`search_packet_backtracking_mrv`, délégation coupée), dans W processus
 * fils : le moteur tient son état dans des globales de processus (compteurs,
 * `max_result`, `request`), un fork par fil de mesure est donc la seule façon
 * de paralléliser sans le modifier.
 *
 * Compilation/exécution : `make bench-sample` (voir le makefile).
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Les moteurs sont `static` : même technique que tests/bench/bench_refutation.c
 * et tests/core/test_etii_search.c — d'où l'absence d'etii_search.c dans les
 * modules liés. */
#include "core/etii_search.c"

#include "bench/sample_stats.h"
#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "app/etii_client.h"
#include "core/datamanager.h"
#include "core/packet_codec.h"
#include "core/backup_stream.h"

/* Octet `checked` d'un enregistrement compact (cf. le format en tête de
 * core/packet_codec.h : x, y, checked, réservé, …). Lu sans décoder, pour
 * compter les possibilités non encore vérifiées sur tout le stock. */
#define REC_CHECKED_OFFSET 2

/* Sans --sample, toutes les racines du fichier sont chargées (décodées, 576
 * octets chacune) : au-delà, c'est un stock entier qu'on mesurerait par erreur. */
#define KEEP_ALL_MAX 10000000

/* Tranches de min_candidats du bilan : 0..9, ≥ 10, inconnu. */
#define MINC_BUCKETS 12

static client_possibility_t g_client;
static int16_t g_idParts[ETERN_PARTS + 1][PART_SIZES];

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------- *
 * Lecture brute d'un .back
 * ------------------------------------------------------------------------- */

/**
 * @brief Lit l'enregistrement suivant SOUS FORME COMPACTE, sans le décoder.
 *
 * Même découpage que `packet_codec_fread` (en-tête + bitmap, dont le popcount
 * donne la longueur du plan des valeurs), moins le décodage — qui coûterait
 * ~300 ns × 6,7·10⁹ racines pour n'en garder que quelques milliers. Un `.back`
 * hérité (structs brutes) est réencodé : le réservoir ne connaît qu'une forme.
 *
 * @return 1 si lu, 0 en fin de fichier propre, -1 sur fichier tronqué/incohérent.
 */
static int read_record(FILE *f, int packed, uint8_t *rec, size_t *len)
{
    if (!packed) {
        struct possibility_packet pkt;
        size_t got = fread(&pkt, sizeof(pkt), 1, f);
        if (got != 1) {
            return feof(f) ? 0 : -1;
        }
        return packet_codec_encode(&pkt, rec, PACKET_CODEC_MAX_BYTES, len) == 0 ? 1 : -1;
    }
    const size_t prefix = (size_t)PACKET_CODEC_HEADER_BYTES + (size_t)PACKET_CODEC_BITMAP_BYTES;
    size_t got = fread(rec, 1, prefix, f);
    if (got == 0) {
        return feof(f) ? 0 : -1;
    }
    if (got < prefix) {
        return -1;
    }
    uint16_t placed = packet_codec_peek_placed(rec, prefix);
    if (placed > ETERN_PARTS) {
        return -1;
    }
    size_t rest = PACKET_CODEC_VALUE_BYTES((size_t)placed);
    if (rest > 0 && fread(rec + prefix, 1, rest, f) != rest) {
        return -1;
    }
    *len = prefix + rest;
    return 1;
}

/** @brief Un stock non autonome a une partie de ses racines AILLEURS. */
static void warn_if_not_self_contained(const char *path, int packed, const uint8_t *header)
{
    if (packed && (packet_codec_file_header_flags(header) & PACKET_CODEC_FILE_FLAG_COMPLETE)) {
        return;
    }
    char sidecar[4096];
    snprintf(sidecar, sizeof(sidecar), "%s.spillcount", path);
    struct stat st;
    if (stat(sidecar, &st) == 0) {
        fprintf(stderr,
                "ATTENTION : %s n'est pas une sauvegarde autonome et %s existe : la part du"
                " stock restée sur disque (débordement) n'est PAS dans ce fichier. Le tirage"
                " ne serait pas uniforme sur le stock réel — faire un `backup` manuel"
                " (autonome) et échantillonner celui-là.\n", path, sidecar);
    }
}

/* ------------------------------------------------------------------------- *
 * Échantillon
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *recs;        /* capacity × PACKET_CODEC_MAX_BYTES */
    uint16_t *lens;
    uint64_t capacity;
    uint64_t size;
    /* Population dont l'échantillon est tiré (racines retenues par le filtre). */
    uint64_t population;
    uint64_t seed;
    int min_pieces, max_pieces;
    /* 1 : tiré ici par réservoir ; 0 : fichier pris en entier (échantillon
       déjà tiré), dont la graine et la population sont dans son .meta. */
    int drawn;
} sample_t;

typedef struct {
    uint64_t total;
    uint64_t unchecked;
    uint64_t by_alloc[ETERN_PARTS + 1];
} stock_profile_t;

static int sample_alloc(sample_t *s, uint64_t capacity)
{
    s->capacity = capacity;
    s->size = 0;
    s->recs = malloc((size_t)capacity * PACKET_CODEC_MAX_BYTES);
    s->lens = calloc((size_t)capacity, sizeof(*s->lens));
    return (s->recs != NULL && s->lens != NULL) ? 0 : -1;
}

/**
 * @brief Une passe sur `path` : profil du stock entier, et tirage de
 *        `capacity` racines parmi celles dont `alloc` est dans [min, max].
 *        `capacity == 0` : garde TOUTES les racines retenues (mode mesure d'un
 *        échantillon déjà tiré), sans réservoir.
 */
static int draw_sample(const char *path, sample_t *s, stock_profile_t *prof)
{
    int format = 0;
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
    FILE *f = backup_stream_fopen_read(path, &format, header);
    if (f == NULL) {
        fprintf(stderr, "%s : stock illisible ou refusé (%s)\n", path, strerror(errno));
        return -1;
    }
    const int packed = (format == BACKUP_STREAM_PACKED);
    warn_if_not_self_contained(path, packed, header);

    const int keep_all = (s->capacity == 0);
    s->drawn = !keep_all;
    if (keep_all && sample_alloc(s, 1024) != 0) {
        fclose(f);
        return -1;
    }
    sample_reservoir_t res;
    sample_reservoir_init(&res, s->capacity, s->seed);

    memset(prof, 0, sizeof(*prof));
    uint8_t rec[PACKET_CODEC_MAX_BYTES];
    size_t len = 0;
    int rc;
    double t0 = now_seconds(), last = t0;
    while ((rc = read_record(f, packed, rec, &len)) == 1) {
        uint16_t placed = packet_codec_peek_placed(rec, len);
        prof->total++;
        prof->by_alloc[placed]++;
        if (rec[REC_CHECKED_OFFSET] != 1) {
            prof->unchecked++;
        }
        if (placed < s->min_pieces || placed > s->max_pieces) {
            continue;
        }
        int64_t slot;
        if (keep_all) {
            if (s->size == KEEP_ALL_MAX) {
                fprintf(stderr, "%s : plus de %d racines — c'est un stock, pas un échantillon :"
                        " tirer d'abord avec --sample\n", path, KEEP_ALL_MAX);
                fclose(f);
                return -1;
            }
            if (s->size == s->capacity) {
                uint64_t cap = s->capacity * 2;
                uint8_t *r2 = realloc(s->recs, (size_t)cap * PACKET_CODEC_MAX_BYTES);
                uint16_t *l2 = realloc(s->lens, (size_t)cap * sizeof(*l2));
                if (r2 == NULL || l2 == NULL) {
                    fprintf(stderr, "mémoire insuffisante pour %" PRIu64 " racines\n", cap);
                    free(r2 != NULL ? r2 : s->recs);
                    s->recs = NULL;
                    fclose(f);
                    return -1;
                }
                s->recs = r2;
                s->lens = l2;
                s->capacity = cap;
            }
            slot = (int64_t)s->size++;
            res.seen++;
        } else {
            slot = sample_reservoir_offer(&res);
        }
        if (slot >= 0) {
            memcpy(s->recs + (size_t)slot * PACKET_CODEC_MAX_BYTES, rec, len);
            s->lens[slot] = (uint16_t)len;
        }
        if ((prof->total & 0xFFFFF) == 0) {
            double t = now_seconds();
            if (t - last >= 10.0) {
                fprintf(stderr, "  lecture : %" PRIu64 " possibilités, %.0f /s\n",
                        prof->total, (double)prof->total / (t - t0));
                last = t;
            }
        }
    }
    fclose(f);
    if (rc < 0) {
        fprintf(stderr, "%s : enregistrement tronqué ou incohérent après %" PRIu64
                " possibilités — échantillon refusé (il ne porterait pas sur tout le stock)\n",
                path, prof->total);
        return -1;
    }
    s->population = res.seen;
    if (!keep_all) {
        s->size = sample_reservoir_size(&res);
    }
    return 0;
}

static void print_profile(const char *path, const stock_profile_t *p, const sample_t *s)
{
    printf("stock %s : %" PRIu64 " possibilités, dont %" PRIu64 " non vérifiées (%.2f %%)\n",
           path, p->total, p->unchecked,
           p->total ? 100.0 * (double)p->unchecked / (double)p->total : 0.0);
    printf("répartition par nombre de pièces posées :\n");
    for (int a = 0; a <= ETERN_PARTS; a++) {
        if (p->by_alloc[a] > 0) {
            printf("  %3d pièces : %15" PRIu64 "  (%.4f %%)\n", a, p->by_alloc[a],
                   100.0 * (double)p->by_alloc[a] / (double)p->total);
        }
    }
    if (s->drawn) {
        printf("population échantillonnée (pièces dans [%d, %d]) : %" PRIu64 " ; tirées : %" PRIu64
               " ; graine %" PRIu64 "\n",
               s->min_pieces, s->max_pieces, s->population, s->size, s->seed);
    } else {
        printf("racines prises en entier (pièces dans [%d, %d]) : %" PRIu64 "\n",
               s->min_pieces, s->max_pieces, s->size);
    }
    if (p->unchecked > 0) {
        printf("NB : le prunage n'est pas terminé — des racines encore non vérifiées sont dans"
               " l'échantillon ; certaines tomberaient au simple contrôle superficiel.\n");
    }
}

static int write_sample(const char *out, const sample_t *s, const char *source)
{
    FILE *raw = fopen(out, "wb");
    if (raw == NULL) {
        fprintf(stderr, "%s : %s\n", out, strerror(errno));
        return -1;
    }
    FILE *f = backup_stream_open_write(raw, 0);
    if (f == NULL) {
        fprintf(stderr, "%s : écriture de l'en-tête impossible\n", out);
        return -1;
    }
    int ok = 1;
    for (uint64_t i = 0; i < s->size && ok; i++) {
        ok = fwrite(s->recs + (size_t)i * PACKET_CODEC_MAX_BYTES, 1, s->lens[i], f) == s->lens[i];
    }
    /* L'erreur de fin de trame zstd ne remonte qu'ici (cf. backup_stream.h). */
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "%s : écriture incomplète\n", out);
        return -1;
    }

    char meta[4096];
    snprintf(meta, sizeof(meta), "%s.meta", out);
    FILE *m = fopen(meta, "w");
    if (m == NULL) {
        fprintf(stderr, "%s : %s\n", meta, strerror(errno));
        return -1;
    }
    fprintf(m, "population=%" PRIu64 "\nsample=%" PRIu64 "\nseed=%" PRIu64
            "\nmin_pieces=%d\nmax_pieces=%d\nsource=%s\n",
            s->population, s->size, s->seed, s->min_pieces, s->max_pieces, source);
    if (fclose(m) != 0) {
        fprintf(stderr, "%s : écriture incomplète\n", meta);
        return -1;
    }
    printf("échantillon écrit : %s (%" PRIu64 " racines) + %s\n", out, s->size, meta);
    return 0;
}

/** @brief Relit `population=` dans `<path>.meta` ; 0 si absent. */
static uint64_t read_meta_population(const char *path)
{
    char meta[4096];
    snprintf(meta, sizeof(meta), "%s.meta", path);
    FILE *m = fopen(meta, "r");
    if (m == NULL) {
        return 0;
    }
    char line[4352];
    uint64_t pop = 0;
    while (fgets(line, sizeof(line), m) != NULL) {
        if (strncmp(line, "population=", 11) == 0) {
            pop = strtoull(line + 11, NULL, 10);
        }
    }
    fclose(m);
    return pop;
}

/* ------------------------------------------------------------------------- *
 * Mesure
 * ------------------------------------------------------------------------- */

/* Message d'un fils au parent : taille fixe, bien sous PIPE_BUF, donc écrit
 * atomiquement même par plusieurs fils sur le même tube. */
typedef struct {
    uint64_t index;
    int32_t outcome;
    int32_t max_depth;
    uint64_t nodes;
    double seconds;
} wire_result_t;

static sample_outcome_t close_root(const struct possibility_packet *root, long budget,
                                   unsigned long long *nodes, double *seconds, int *depth)
{
    struct possibility_packet work;
    memcpy(&work, root, sizeof(work));
    counters[0] = 0;
    max_result = 0;
    request = REQUEST_CONTINUE;
    double t0 = now_seconds();
    bt_core_result_t r = search_packet_backtracking_mrv(&g_client, &work, g_idParts, budget, 0, nodes);
    *seconds = now_seconds() - t0;
    *depth = max_result;
    if (max_result >= ETERN_PARTS) {
        return SAMPLE_SOLUTION;
    }
    switch (r) {
        case BT_CORE_EXHAUSTED: return SAMPLE_CLOSED;
        case BT_CORE_BUDGET:    return SAMPLE_BUDGET;
        case BT_CORE_STOPPED:   break;
    }
    return SAMPLE_ERROR;
}

static void worker(int w, int workers, int fd, const struct possibility_packet *roots,
                   uint64_t n, long budget)
{
    for (uint64_t i = (uint64_t)w; i < n; i += (uint64_t)workers) {
        wire_result_t msg;
        memset(&msg, 0, sizeof(msg));
        unsigned long long nodes = 0;
        int depth = 0;
        msg.index = i;
        msg.outcome = (int32_t)close_root(&roots[i], budget, &nodes, &msg.seconds, &depth);
        msg.nodes = nodes;
        msg.max_depth = depth;
        if (write(fd, &msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
            exit(EXIT_FAILURE);
        }
    }
    /* exit() et non _exit() : rien à vider ici, mais c'est la convention du
       dépôt pour un fils (flush gcov/llvm-cov). */
    exit(EXIT_SUCCESS);
}

static void print_human(double v, char *buf, size_t sz)
{
    static const char *units[] = { "", " k", " M", " G", " T", " P", " E" };
    int u = 0;
    while (v >= 1000.0 && u < 6) {
        v /= 1000.0;
        u++;
    }
    snprintf(buf, sz, "%.3g%s", v, units[u]);
}

static int measure(const sample_t *s, uint64_t population, long budget, int workers,
                   const char *csv_path)
{
    const uint64_t n = s->size;
    if (n == 0) {
        fprintf(stderr, "échantillon vide : rien à mesurer\n");
        return -1;
    }
    struct possibility_packet *roots = malloc((size_t)n * sizeof(*roots));
    sample_result_t *res = calloc((size_t)n, sizeof(*res));
    int *depth = calloc((size_t)n, sizeof(*depth));
    unsigned long long *scratch = malloc((size_t)n * sizeof(*scratch));
    if (roots == NULL || res == NULL || depth == NULL || scratch == NULL) {
        fprintf(stderr, "mémoire insuffisante pour %" PRIu64 " racines\n", n);
        return -1;
    }
    for (uint64_t i = 0; i < n; i++) {
        if (packet_codec_decode(s->recs + (size_t)i * PACKET_CODEC_MAX_BYTES, s->lens[i],
                                &roots[i], NULL) != 0) {
            fprintf(stderr, "racine %" PRIu64 " indécodable\n", i);
            return -1;
        }
        res[i].outcome = SAMPLE_ERROR;
    }
    if ((uint64_t)workers > n) {
        workers = (int)n;
    }

    printf("\nmesure : %" PRIu64 " racines, plafond %ld nœuds par racine, %d processus\n",
           n, budget, workers);
    fflush(stdout);
    fflush(stderr);

    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        return -1;
    }
    pid_t *pids = calloc((size_t)workers, sizeof(*pids));
    for (int w = 0; w < workers; w++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            close(fds[0]);
            worker(w, workers, fds[1], roots, n, budget);
        }
        pids[w] = pid;
    }
    close(fds[1]);

    double t0 = now_seconds(), last = t0;
    uint64_t done = 0, closed = 0;
    wire_result_t msg;
    for (;;) {
        ssize_t got = read(fds[0], &msg, sizeof(msg));
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }
        if (got != (ssize_t)sizeof(msg) || msg.index >= n) {
            fprintf(stderr, "message de fils incohérent, mesure interrompue\n");
            break;
        }
        res[msg.index].outcome = (sample_outcome_t)msg.outcome;
        res[msg.index].nodes = msg.nodes;
        res[msg.index].seconds = msg.seconds;
        depth[msg.index] = msg.max_depth;
        done++;
        closed += (msg.outcome == SAMPLE_CLOSED);
        if (msg.outcome == SAMPLE_SOLUTION) {
            printf("!!! SOLUTION rencontrée sous la racine %" PRIu64 " de l'échantillon !!!\n",
                   msg.index);
            fflush(stdout);
        }
        double t = now_seconds();
        if (t - last >= 10.0) {
            double eta = (double)(n - done) * (t - t0) / (double)done;
            fprintf(stderr, "  %" PRIu64 "/%" PRIu64 " racines, %" PRIu64 " fermées, reste ~%.0f s\n",
                    done, n, closed, eta);
            last = t;
        }
    }
    close(fds[0]);
    int failed = 0;
    for (int w = 0; w < workers; w++) {
        int st = 0;
        waitpid(pids[w], &st, 0);
        failed |= !(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    free(pids);
    if (failed) {
        fprintf(stderr, "au moins un processus de mesure a échoué : ses racines comptent en erreur\n");
    }
    const double wall = now_seconds() - t0;

    if (csv_path != NULL) {
        FILE *c = fopen(csv_path, "w");
        if (c == NULL) {
            fprintf(stderr, "%s : %s\n", csv_path, strerror(errno));
        } else {
            fprintf(c, "index,pieces,min_candidats,outcome,nodes,seconds,max_depth\n");
            static const char *names[] = { "closed", "budget", "solution", "error" };
            for (uint64_t i = 0; i < n; i++) {
                fprintf(c, "%" PRIu64 ",%d,%d,%s,%llu,%.6f,%d\n", i,
                        possibility_placed_count(&roots[i]), roots[i].min_candidats,
                        names[res[i].outcome], res[i].nodes, res[i].seconds, depth[i]);
            }
            if (fclose(c) != 0) {
                fprintf(stderr, "%s : écriture incomplète\n", csv_path);
            } else {
                printf("résultats par racine : %s\n", csv_path);
            }
        }
    }

    sample_summary_t sum;
    sample_summarize(res, (size_t)n, scratch, &sum);
    char h1[32], h2[32], h3[32], h4[32];

    printf("\n%-34s %" PRIu64 " (dont %" PRIu64 " en erreur, exclues)\n", "racines mesurées :",
           sum.n, sum.errors);
    printf("%-34s %" PRIu64 "  (%.2f %%, IC 95 %% [%.2f ; %.2f] %%)\n", "fermées sous le plafond :",
           sum.closed, sum.n ? 100.0 * (double)sum.closed / (double)sum.n : 0.0,
           100.0 * sum.closed_lo, 100.0 * sum.closed_hi);
    printf("%-34s %" PRIu64 "\n", "plafond atteint (indéterminées) :", sum.budget);
    printf("%-34s %" PRIu64 "\n", "solutions rencontrées :", sum.solutions);
    if (sum.closed > 0) {
        printf("coût de fermeture (nœuds, racines fermées) : médiane %llu, p90 %llu, p99 %llu, max %llu\n",
               sum.q50, sum.q90, sum.q99, sum.qmax);
    }
    print_human(sum.nodes_per_second, h1, sizeof(h1));
    printf("débit par processus : %s nœuds/s ; durée murale %.1f s\n", h1, wall);

    printf("\nrépartition des fermetures (part des racines fermées en ≤ N nœuds) :\n");
    for (int i = 0; i < SAMPLE_ECDF_POINTS; i++) {
        if (sum.ecdf_at[i] > (unsigned long long)budget * 10ULL && i > 0) {
            break;
        }
        print_human((double)sum.ecdf_at[i], h1, sizeof(h1));
        printf("  ≤ %-8s %6.2f %%%s\n", h1, 100.0 * sum.ecdf[i],
               sum.ecdf_at[i] > (unsigned long long)budget ? "   (au-delà du plafond : non observé)" : "");
    }

    /* Ventilation par min_candidats : le score MRV porté par chaque racine
       prédit-il qu'elle se fermera ? */
    uint64_t mc_n[MINC_BUCKETS] = {0}, mc_closed[MINC_BUCKETS] = {0};
    for (uint64_t i = 0; i < n; i++) {
        if (res[i].outcome == SAMPLE_ERROR) continue;
        int mc = roots[i].min_candidats;
        int b = (mc == POSSIBILITY_MIN_CANDIDATS_UNKNOWN || mc < 0) ? MINC_BUCKETS - 1
              : (mc >= 10 ? 10 : mc);
        mc_n[b]++;
        mc_closed[b] += (res[i].outcome == SAMPLE_CLOSED);
    }
    printf("\nfermetures par min_candidats de la racine :\n");
    for (int b = 0; b < MINC_BUCKETS; b++) {
        if (mc_n[b] == 0) continue;
        char label[16];
        if (b == MINC_BUCKETS - 1) snprintf(label, sizeof(label), "inconnu");
        else if (b == 10) snprintf(label, sizeof(label), ">= 10");
        else snprintf(label, sizeof(label), "%d", b);
        printf("  %-8s %8" PRIu64 " racines, %6.2f %% fermées\n", label, mc_n[b],
               100.0 * (double)mc_closed[b] / (double)mc_n[b]);
    }

    if (population > 0 && sum.n > 0) {
        double P = (double)population;
        print_human(P, h1, sizeof(h1));
        printf("\nextrapolation à la population de %s racines (%" PRIu64 ") :\n", h1, population);
        print_human(P * (double)sum.closed / (double)sum.n, h1, sizeof(h1));
        print_human(P * sum.closed_lo, h2, sizeof(h2));
        print_human(P * sum.closed_hi, h3, sizeof(h3));
        printf("  racines réfutables sous %ld nœuds : %s  [%s ; %s]\n", budget, h1, h2, h3);
        double total = P * sum.mean_nodes;
        double half = 1.96 * P * sum.sem_nodes;
        print_human(total, h1, sizeof(h1));
        print_human(total - half > 0 ? total - half : 0, h2, sizeof(h2));
        print_human(total + half, h3, sizeof(h3));
        double years = (sum.nodes_per_second > 0.0)
                     ? total / sum.nodes_per_second / (365.25 * 86400.0) : 0.0;
        print_human(years, h4, sizeof(h4));
        if (sum.censored) {
            printf("  coût d'épuisement du stock : AU MOINS %s nœuds (≈ %s années-cœur au débit"
                   " mesuré)\n", h1, h4);
            printf("  -> BORNE BASSE : %" PRIu64 " racine(s) ont atteint le plafond, leur coût réel"
                   " est inconnu (>= %ld). Relever --budget resserre la borne, sans jamais la"
                   " transformer en estimation tant qu'une racine reste ouverte.\n",
                   sum.budget, budget);
        } else {
            printf("  coût d'épuisement du stock : %s nœuds [%s ; %s] (≈ %s années-cœur au débit"
                   " mesuré)\n", h1, h2, h3, h4);
            printf("  -> toutes les racines tirées se sont fermées : estimation sans biais, mais"
                   " une queue lourde rare peut échapper à %" PRIu64 " tirages.\n", sum.n);
        }
    } else if (population == 0) {
        printf("\npas d'extrapolation : taille de la population inconnue (ni .meta, ni --population)\n");
    }

    free(roots);
    free(res);
    free(depth);
    free(scratch);
    return 0;
}

/* ------------------------------------------------------------------------- */

static void usage(void)
{
    printf("Usage : bench_sample --from-back <f> [options]\n"
           "  --from-back <f>    stock serveur (.back, compact ou hérité, zstd si compilé ZSTD=1)\n"
           "  --sample <n>       tire n racines UNIFORMÉMENT (réservoir, une passe) ; sans cette\n"
           "                     option, toutes les racines du fichier sont prises (cas d'un\n"
           "                     échantillon déjà tiré)\n"
           "  --seed <s>         graine du tirage (défaut 1 : un tirage se rejoue à l'identique)\n"
           "  --min-pieces <n>   ne tire que parmi les racines d'au moins n pièces posées\n"
           "  --max-pieces <n>   ... et d'au plus n pièces posées\n"
           "  --out <f>          écrit l'échantillon en .back, plus <f>.meta (population, graine)\n"
           "  --measure          tente de fermer chaque racine de l'échantillon\n"
           "  --budget <n>       plafond de nœuds par racine (défaut 10000000)\n"
           "  --workers <n>      processus de mesure (défaut : un par cœur en ligne)\n"
           "  --csv <f>          résultat par racine (pièces, min_candidats, issue, nœuds, temps)\n"
           "  --population <n>   taille de la population pour l'extrapolation (défaut : celle\n"
           "                     du tirage, ou population= du fichier .meta de --from-back)\n"
           "  --pieces <f>       fichier de pièces (défaut data/pieces.csv)\n");
}

int main(int argc, char **argv)
{
    /* Obligatoire avant tout usage de datamanager.c (lié via TEST_MODULES). */
    datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT);

    const char *pieces = "data/pieces.csv";
    const char *back = NULL, *out = NULL, *csv = NULL;
    uint64_t sample_n = 0, population = 0, seed = 1;
    long budget = 10000000;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int workers = ncpu > 0 ? (int)ncpu : 1;
    int do_measure = 0;
    int min_pieces = 0, max_pieces = ETERN_PARTS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--from-back") == 0 && i + 1 < argc)       back = argv[++i];
        else if (strcmp(argv[i], "--sample") == 0 && i + 1 < argc)     sample_n = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)       seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--min-pieces") == 0 && i + 1 < argc) min_pieces = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-pieces") == 0 && i + 1 < argc) max_pieces = atoi(argv[++i]);
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)        out = argv[++i];
        else if (strcmp(argv[i], "--measure") == 0)                    do_measure = 1;
        else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc)     budget = atol(argv[++i]);
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc)    workers = atoi(argv[++i]);
        else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)        csv = argv[++i];
        else if (strcmp(argv[i], "--population") == 0 && i + 1 < argc) population = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--pieces") == 0 && i + 1 < argc)     pieces = argv[++i];
        else {
            usage();
            return EXIT_FAILURE;
        }
    }
    if (back == NULL || (!do_measure && out == NULL) || budget <= 0 || workers < 1) {
        usage();
        return EXIT_FAILURE;
    }

    sample_t s;
    memset(&s, 0, sizeof(s));
    s.seed = seed;
    s.min_pieces = min_pieces;
    s.max_pieces = max_pieces;
    if (sample_n > 0 && sample_alloc(&s, sample_n) != 0) {
        fprintf(stderr, "mémoire insuffisante pour un échantillon de %" PRIu64 "\n", sample_n);
        return EXIT_FAILURE;
    }

    stock_profile_t prof;
    double t0 = now_seconds();
    if (draw_sample(back, &s, &prof) != 0) {
        return EXIT_FAILURE;
    }
    print_profile(back, &prof, &s);
    printf("lecture : %.1f s\n", now_seconds() - t0);

    if (out != NULL && write_sample(out, &s, back) != 0) {
        return EXIT_FAILURE;
    }
    if (!do_measure) {
        return EXIT_SUCCESS;
    }

    if (population == 0) {
        /* Un échantillon déjà tiré porte sa population dans son .meta ; un
           tirage fait ici la connaît ; un fichier pris en entier EST sa
           population. */
        population = (sample_n > 0) ? s.population : read_meta_population(back);
        if (population == 0 && sample_n == 0) {
            population = s.population;
        }
    }

    counters = calloc(NB_THREADS, sizeof(*counters));
    lastfilesize = calloc(NB_THREADS, sizeof(*lastfilesize));
    if (counters == NULL || lastfilesize == NULL) {
        fprintf(stderr, "allocation des compteurs impossible\n");
        return EXIT_FAILURE;
    }
    init_id_parts(g_idParts);
    stop_on_solution = 0;
    set_server_ip(NULL);  /* aucun serveur : ni délégation ni envoi réseau */

    struct array_part *parts = read_parts((char *)pieces);
    if (parts == NULL) {
        fprintf(stderr, "lecture de %s impossible\n", pieces);
        return EXIT_FAILURE;
    }
    struct array_part *rot = rotate_all_parts(parts);
    map_big_array *map = buildBigArray(rot, search_max_face(rot));
    memset(&g_client, 0, sizeof(g_client));
    g_client.all_rotate_part = rot;
    g_client.map_part = map;

    int rc = measure(&s, population, budget, workers, csv);
    free(s.recs);
    free(s.lens);
    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
