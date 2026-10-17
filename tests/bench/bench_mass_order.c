/*
 * Banc « EXPLORATION PAR MASSE » : explorer les racines d'une coupe par masse
 * estimée décroissante fait-il trouver la solution plus TÔT, sondes comprises,
 * que la production ?
 *
 * S5 (docs/conception/etude_arbre_expansion_profonde.md) a établi que la masse
 * à mi-profondeur CLASSE la racine de la solution très haut ; le §7 de
 * docs/conception/banc_resolution_clones.md ne retient une piste que si elle
 * gagne en TEMPS. Ce banc joue, sur UNE instance (un clone à solution plantée,
 * binaire compilé à sa taille), quatre bras à la suite :
 *
 *   A  production : recherche depuis la genèse ;
 *   B  coupe exacte (premier niveau à >= 1 000 nœuds), ordre d'énumération ;
 *   C  la même coupe, permutée au hasard (contrôle : effet de l'ORDRE ou de la
 *      coupe ?) ;
 *   D  la même coupe, par masse décroissante estimée par --probes sondes par
 *      racine — le temps des sondes est AJOUTÉ à celui de D.
 *
 * Chaque exécution est un processus FILS où `stop_on_solution` sort réellement
 * par exit() (même mécanique que tests/bench/bench_solve.c : un `atexit`
 * rapporte nœuds et temps, quel que soit le chemin de sortie). Une ligne
 * « MO … » par instance, avec la lecture de chaque paire décisive
 * (`sample_pair_outcome`). Le résumé par cellule est fait par
 * tests/bench/mass_order_campaign.sh.
 *
 * Compilation : cible `make bench-mass-order` (un binaire par taille de clone).
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Le moteur est `static` : même technique que bench_solve et bench_sample. */
#include "core/etii_search.c"

#include "bench/sample_stats.h"
#include "bench/tree_probe.h"
#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "ui/logger.h"

#define CUT_MIN 1000
#define CUT_CAP 5000000ULL

#define CHILD_EXIT_BUDGET    10
#define CHILD_EXIT_EXHAUSTED 11
#define CHILD_EXIT_STOPPED   12

typedef struct {
    unsigned long long nodes;
    double seconds;
} run_report_t;

typedef struct {
    int solved;
    unsigned long long nodes;
    double seconds;
    const char *status;
} arm_result_t;

static int16_t g_ids[ETERN_PARTS + 1][PART_SIZES];
static int g_report_fd = -1;
static run_report_t g_report;
static double g_child_t0 = 0.0;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Rapporte nœuds et temps, quel que soit le chemin de sortie du fils — dont
   `record_solution`, qui sort par exit() au milieu de la boucle chaude. */
static void child_report(void)
{
    if (g_report_fd < 0) {
        return;
    }
    g_report.nodes = counters[0];
    g_report.seconds = now_seconds() - g_child_t0;
    ssize_t w = write(g_report_fd, &g_report, sizeof(g_report));
    (void)w;
    close(g_report_fd);
    g_report_fd = -1;
}

/* Corps du fils : explore les racines dans l'ordre donné, sort. Ne revient pas. */
static void run_child(const char *workdir, map_big_array *map, struct array_part *rot,
                      const struct possibility_packet *roots, const uint64_t *order, uint64_t n, long budget)
{
    status_zone_disown_child();
    if (chdir(workdir) != 0 || freopen("children.log", "a", stdout) == NULL) {
        _exit(CHILD_EXIT_STOPPED);
    }
    client_possibility_t client;
    memset(&client, 0, sizeof(client));
    client.all_rotate_part = rot;
    client.map_part = map;
    counters[0] = 0;
    max_result = 0;
    request = REQUEST_CONTINUE;
    stop_on_solution = 1;
    g_child_t0 = now_seconds();
    memset(&g_report, 0, sizeof(g_report));
    atexit(child_report);
    int status = CHILD_EXIT_EXHAUSTED;
    for (uint64_t i = 0; i < n; i++) {
        long remaining = budget - (long)counters[0];
        if (remaining <= 0) {
            status = CHILD_EXIT_BUDGET;
            break;
        }
        struct possibility_packet work = roots[order != NULL ? order[i] : i];
        unsigned long long nodes = 0;
        bt_core_result_t rc = search_packet_backtracking_mrv(&client, &work, g_ids, remaining, 0, &nodes);
        if (rc == BT_CORE_BUDGET) { status = CHILD_EXIT_BUDGET; break; }
        if (rc == BT_CORE_STOPPED) { status = CHILD_EXIT_STOPPED; break; }
    }
    exit(status);  /* exit() : l'atexit doit s'exécuter */
}

static arm_result_t run_arm(const char *workdir, map_big_array *map, struct array_part *rot,
                            const struct possibility_packet *roots, const uint64_t *order, uint64_t n, long budget)
{
    arm_result_t res = { 0, 0, 0.0, "échec" };
    int fds[2];
    if (pipe(fds) != 0) {
        return res;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        return res;
    }
    if (pid == 0) {
        close(fds[0]);
        g_report_fd = fds[1];
        run_child(workdir, map, rot, roots, order, n, budget);
        _exit(CHILD_EXIT_STOPPED);
    }
    close(fds[1]);
    run_report_t rep;
    memset(&rep, 0, sizeof(rep));
    size_t got = 0;
    while (got < sizeof(rep)) {
        ssize_t r = read(fds[0], (char *)&rep + got, sizeof(rep) - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
    if (got != sizeof(rep) || !WIFEXITED(st)) {
        return res;
    }
    res.nodes = rep.nodes;
    res.seconds = rep.seconds;
    switch (WEXITSTATUS(st)) {
        case EXIT_SUCCESS:         res.solved = 1; res.status = "résolue"; break;
        case CHILD_EXIT_BUDGET:    res.status = "plafond"; break;
        case CHILD_EXIT_EXHAUSTED: res.status = "épuisée"; break;
        default:                   res.status = "arrêtée"; break;
    }
    return res;
}

static void build_genesis(struct possibility_packet *g)
{
    memset(g, 0, sizeof(*g));
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            g->grid[x][y] = -2;
    if (indices_file != NULL) {
        struct array_index *ind = read_indices(indices_file);
        for (int i = 0; i < ind->size; i++) {
            struct board_index *h = &ind->indices[i];
            g->grid[h->x][h->y] = g_ids[h->id][h->rotation];
            set_face_used(g->b_faceused, (uint16_t)(h->id - 1), 1);
        }
        free_array_index(ind);
    }
    g->alloc = (uint16_t)possibility_placed_count(g);
    g->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
}

static int read_planted_solution(const char *path, int16_t sol[ETERN_SIZE][ETERN_SIZE])
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    char line[256];
    int cells = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        int x, y, id, r;
        if (line[0] != '#' && sscanf(line, "%d %d %d %d", &x, &y, &id, &r) == 4 && x >= 0 && x < ETERN_SIZE
            && y >= 0 && y < ETERN_SIZE && id > 0 && id <= ETERN_PARTS && r >= 0 && r < 4) {
            sol[x][y] = (int16_t)(id + ETERN_PARTS * r);
            cells++;
        }
    }
    fclose(f);
    return cells == ETERN_PARTS ? 0 : -1;
}

static const double *g_mass;

static int cmp_mass_desc(const void *a, const void *b)
{
    uint64_t i = *(const uint64_t *)a, j = *(const uint64_t *)b;
    if (g_mass[i] != g_mass[j]) return g_mass[i] > g_mass[j] ? -1 : 1;
    return (i > j) - (i < j);  /* ordre d'énumération à égalité : déterministe */
}

static void print_arm(const char *name, const arm_result_t *r)
{
    printf(" %s=%s:%.4f:%llu", name, r->solved ? "R" : "N", r->seconds, r->nodes);
}

int main(int argc, char **argv)
{
    const char *pieces = NULL, *solution = NULL, *label = "-", *workdir = NULL;
    long budget = 20000000;
    int probes = 20;
    uint64_t seed = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pieces") == 0 && i + 1 < argc) pieces = argv[++i];
        else if (strcmp(argv[i], "--indices-file") == 0 && i + 1 < argc) indices_file = argv[++i];
        else if (strcmp(argv[i], "--solution") == 0 && i + 1 < argc) solution = argv[++i];
        else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) label = argv[++i];
        else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) budget = atol(argv[++i]);
        else if (strcmp(argv[i], "--probes") == 0 && i + 1 < argc) probes = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--workdir") == 0 && i + 1 < argc) workdir = argv[++i];
        else {
            fprintf(stderr, "usage : bench_mass_order --pieces P --indices-file I --workdir D [--solution S]"
                            " [--label T] [--budget N] [--probes K] [--seed S]\n");
            return EXIT_FAILURE;
        }
    }
    if (pieces == NULL || workdir == NULL || budget <= 0 || probes <= 0) {
        fprintf(stderr, "--pieces, --workdir requis\n");
        return EXIT_FAILURE;
    }
    counters = calloc(NB_THREADS, sizeof(*counters));
    lastfilesize = calloc(NB_THREADS, sizeof(*lastfilesize));
    set_server_ip(NULL);
    init_id_parts(g_ids);
    struct array_part *parts = read_parts((char *)pieces);
    if (counters == NULL || parts == NULL) return EXIT_FAILURE;
    struct array_part *rot = rotate_all_parts(parts);
    map_big_array *map = prepare_map_part(rot);

    struct possibility_packet genesis;
    build_genesis(&genesis);
    struct possibility_packet *cut = NULL;
    int level = 0;
    uint64_t n = tree_enumerate_cut(&genesis, map, rot, g_ids, CUT_MIN, CUT_CAP, &cut, &level);
    if (n == 0) {
        printf("MO %s erreur=coupe_impossible\n", label);
        return EXIT_FAILURE;
    }

    /* Bras D : masses à mi-profondeur, temps des sondes compté. */
    const int mid = level + (int)ceil(ceil(0.133 * ETERN_PARTS) / 2.0);
    double *mass = calloc((size_t)n, sizeof(*mass));
    uint64_t *by_mass = malloc((size_t)n * sizeof(*by_mass));
    uint64_t *shuffled = malloc((size_t)n * sizeof(*shuffled));
    sample_rng_t rng;
    sample_rng_seed(&rng, seed);
    double t0 = now_seconds();
    double w[TREE_PROBE_LEVELS];
    for (uint64_t i = 0; i < n; i++) {
        double acc = 0.0;
        for (int k = 0; k < probes; k++) {
            memset(w, 0, sizeof w);
            tree_probe(&cut[i], map, rot, g_ids, &rng, w, mid, 0, NULL, NULL);
            acc += w[mid];
        }
        mass[i] = acc / probes;
        by_mass[i] = i;
    }
    g_mass = mass;
    qsort(by_mass, (size_t)n, sizeof(*by_mass), cmp_mass_desc);
    const double probe_s = now_seconds() - t0;

    /* Bras C : permutation de Fisher-Yates, graine de l'instance. */
    for (uint64_t i = 0; i < n; i++) shuffled[i] = i;
    sample_rng_t prng;
    sample_rng_seed(&prng, seed ^ 0xC0FFEEULL);
    for (uint64_t i = n - 1; i > 0; i--) {
        uint64_t j = sample_rng_below(&prng, i + 1);
        uint64_t t = shuffled[i]; shuffled[i] = shuffled[j]; shuffled[j] = t;
    }

    /* Rang (dans l'ordre D) de la racine de la solution, pour lecture. */
    long sol_pos = -1;
    int16_t sol[ETERN_SIZE][ETERN_SIZE];
    if (solution != NULL && read_planted_solution(solution, sol) == 0) {
        for (uint64_t i = 0; i < n; i++) {
            if (tree_matches_solution(&cut[by_mass[i]], sol)) { sol_pos = (long)i; break; }
        }
    }

    arm_result_t A = run_arm(workdir, map, rot, &genesis, NULL, 1, budget);
    arm_result_t B = run_arm(workdir, map, rot, cut, NULL, n, budget);
    arm_result_t C = run_arm(workdir, map, rot, cut, shuffled, n, budget);
    arm_result_t D = run_arm(workdir, map, rot, cut, by_mass, n, budget);
    D.seconds += probe_s;

    printf("MO %s niveau=%d racines=%" PRIu64 " sondes_s=%.4f rang_solution=%ld", label, level, n, probe_s, sol_pos);
    print_arm("A", &A);
    print_arm("B", &B);
    print_arm("C", &C);
    print_arm("D", &D);
    printf(" DA=%d DC=%d BA=%d CA=%d\n",
           sample_pair_outcome(D.solved, D.seconds, A.solved, A.seconds),
           sample_pair_outcome(D.solved, D.seconds, C.solved, C.seconds),
           sample_pair_outcome(B.solved, B.seconds, A.solved, A.seconds),
           sample_pair_outcome(C.solved, C.seconds, A.solved, A.seconds));
    free(mass); free(by_mass); free(shuffled); free(cut);
    return EXIT_SUCCESS;
}
