/*
 * Suite de l'estimateur de Knuth (tests/bench/tree_probe.c).
 *
 * Ce qui fausserait une estimation sans qu'aucun chiffre n'ait l'air faux : un
 * poids mal multiplié, un enfant tiré non uniformément, ou un niveau décalé.
 * Le seul contrôle qui attrape les trois est l'EXACTITUDE contre l'énumération
 * complète d'un vrai arbre : le 4×4 entier en build 16, le haut de l'arbre
 * depuis la genèse (indices posés) en build 256.
 */
#include "greatest.h"
#include "bench/tree_probe.h"
#include "core/lifo.h"
#include "core/part.h"
#include "core/readdata.h"
#include "core/core_static_variables.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Le 4×4 est un petit arbre : seuil de coupe et profondeur des tests à sa taille. */
#if ETERN_PARTS == 16
#define CUT_MIN 10
#define DEEPER 2
#else
#define CUT_MIN 100
#define DEEPER 4
#endif

/* Moyenne et erreur type d'observations connues. */
TEST estimate_reports_mean_and_standard_error(void)
{
    tree_estimate_t e;
    tree_estimate_init(&e);
    double w[TREE_PROBE_LEVELS];
    const double obs[4] = { 2.0, 4.0, 6.0, 8.0 };
    for (int i = 0; i < 4; i++) {
        memset(w, 0, sizeof w);
        w[3] = obs[i];
        w[5] = (i == 0) ? 10.0 : 0.0;  /* une seule observation atteint le niveau 5 */
        tree_estimate_add(&e, w);
    }
    double m, se;
    tree_estimate_level(&e, 3, 1.0, &m, &se);
    ASSERT(fabs(m - 5.0) < 1e-12);
    ASSERT(fabs(se - sqrt((20.0 / 3.0) / 4.0)) < 1e-12);
    tree_estimate_level(&e, 5, 2.0, &m, &se);  /* les zéros comptent dans la moyenne */
    ASSERT(fabs(m - 5.0) < 1e-12);
    ASSERT_EQ_FMT(1ULL, (unsigned long long)e.reached[5], "%llu");

    tree_estimate_t f;
    tree_estimate_init(&f);
    tree_estimate_merge(&f, &e);
    tree_estimate_merge(&f, &e);
    tree_estimate_level(&f, 3, 1.0, &m, &se);
    ASSERT(fabs(m - 5.0) < 1e-12);
    ASSERT_EQ_FMT(8ULL, (unsigned long long)f.obs, "%llu");
    PASS();
}

static map_big_array *g_map;
static struct array_part *g_rot;
static int16_t g_ids[ETERN_PARTS + 1][4];

static void load_real_map(void)
{
    if (g_map != NULL) return;
    g_rot = rotate_all_parts(read_parts(parts_files));
    g_map = prepare_map_part(g_rot);
    for (int p = 0; p <= ETERN_PARTS; p++)
        for (int r = 0; r < 4; r++)
            g_ids[p][r] = (int16_t)(p + ETERN_PARTS * r);
}

static void genesis(struct possibility_packet *g)
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
}

/* Énumération COMPLÈTE de l'arbre de l'expansion, niveau par niveau. La
   dernière case est comptée, pas développée (comme la sonde : développer un
   plateau à une case vide termine le processus sur une solution). */
static void exact_count(struct possibility_packet *p, unsigned long long cnt[TREE_PROBE_LEVELS], int max_level)
{
    int l = possibility_placed_count(p);
    cnt[l]++;
    if (l >= max_level || l >= ETERN_PARTS) return;
    if (l == ETERN_PARTS - 1) {
        for (int x = 0; x < ETERN_SIZE; x++)
            for (int y = 0; y < ETERN_SIZE; y++) {
                if (p->grid[x][y] != -2) continue;
                key_part key;
                what_search_in_grid_to_key(g_rot, p, (int8_t)x, (int8_t)y, &key, (int8_t)g_map->sizearrayM);
                struct array_part *b = get_parts_bigarray_with_key(g_map, &key);
                for (int s = 0; s < b->size; s++) {
                    int16_t id = b->parts[s].id;
                    cnt[ETERN_PARTS] += (id != 0 && !is_face_used(p->b_faceused, (uint16_t)(id - 1)));
                }
            }
        return;
    }
    File kids;
    init_file(&kids, sizeof(struct possibility_packet));
    unsigned long long pruned = 0;
    search_possiblity_light_pruned(&kids, p, g_map, g_rot, g_ids, &pruned);
    struct possibility_packet k;
    while (scroll(&kids, &k)) exact_count(&k, cnt, max_level);
}

/* L'estimateur retrouve, à chaque niveau, le nombre EXACT de nœuds de l'arbre
 * de l'expansion — à quelques erreurs types près, graine fixée. Contre-épreuve
 * intégrée : des niveaux non vides doivent avoir été comparés, et la sonde ne
 * dépasse pas son niveau d'arrêt. */
TEST probes_recover_the_exact_tree_profile(void)
{
    load_real_map();
    struct possibility_packet root;
    genesis(&root);
#if ETERN_PARTS == 16
    const int max_level = ETERN_PARTS;     /* le 4×4 entier, solutions comprises */
    const int probes = 200000;
#else
    const int max_level = 13;              /* le haut de l'arbre depuis la genèse */
    const int probes = 20000;
#endif
    unsigned long long exact[TREE_PROBE_LEVELS];
    memset(exact, 0, sizeof exact);
    exact_count(&root, exact, max_level);

    tree_estimate_t e;
    tree_estimate_init(&e);
    sample_rng_t rng;
    sample_rng_seed(&rng, 20261018u);
    double w[TREE_PROBE_LEVELS];
    for (int i = 0; i < probes; i++) {
        memset(w, 0, sizeof w);
        int last = tree_probe(&root, g_map, g_rot, g_ids, &rng, w, max_level, 0, NULL, NULL);
        ASSERT(last <= max_level);
        tree_estimate_add(&e, w);
    }
    int compared = 0;
    for (int l = 0; l <= max_level; l++) {
        double m, se;
        tree_estimate_level(&e, l, 1.0, &m, &se);
        if (exact[l] == 0) {
            ASSERT(m == 0.0);
            continue;
        }
        compared++;
        double err = fabs(m - (double)exact[l]);
        if (err > 5.0 * se + 1e-9) {
            FAILm("estimation hors de 5 erreurs types");
        }
        ASSERT(se < 0.2 * (double)exact[l]);  /* assez de sondes pour que l'égalité dise quelque chose */
    }
    ASSERT(compared >= 6);
#if ETERN_PARTS == 16
    ASSERT(exact[ETERN_PARTS] > 0);  /* le 4×4 a des solutions : le dernier niveau est comparé */
#endif
    PASS();
}

/* La capture rend le nœud atteint au niveau demandé et son poids, et rien
 * si la sonde meurt avant. */
TEST probe_captures_the_node_reached_at_a_level(void)
{
    load_real_map();
    struct possibility_packet root, got;
    genesis(&root);
    sample_rng_t rng;
    sample_rng_seed(&rng, 7u);
    double w[TREE_PROBE_LEVELS];
    int target = root.alloc + 3, captured = 0;
    for (int i = 0; i < 200; i++) {
        memset(w, 0, sizeof w);
        double cw = -1.0;
        int last = tree_probe(&root, g_map, g_rot, g_ids, &rng, w, target, target, &got, &cw);
        if (last >= target) {
            captured++;
            ASSERT_EQ_FMT(target, possibility_placed_count(&got), "%d");
            ASSERT(cw > 0.0 && fabs(cw - w[target]) < 1e-9);
        } else {
            ASSERT(cw == 0.0);
        }
    }
    ASSERT(captured > 0);
    PASS();
}

/* La coupe est l'énumération exacte du premier niveau à au moins min_nodes
 * nœuds ; elle refuse un niveau qui dépasse son plafond. */
TEST cut_is_the_exact_first_level_above_the_threshold(void)
{
    load_real_map();
    struct possibility_packet root;
    genesis(&root);
    unsigned long long exact[TREE_PROBE_LEVELS];
    memset(exact, 0, sizeof exact);
    exact_count(&root, exact, root.alloc + 8 < ETERN_PARTS ? root.alloc + 8 : ETERN_PARTS - 1);
    int want = -1;
    for (int l = root.alloc; l < TREE_PROBE_LEVELS; l++) {
        if (exact[l] >= CUT_MIN) { want = l; break; }
    }
    ASSERT(want > 0);
    struct possibility_packet *cut = NULL;
    int level = 0;
    uint64_t n = tree_enumerate_cut(&root, g_map, g_rot, g_ids, CUT_MIN, 1000000, &cut, &level);
    ASSERT_EQ_FMT(want, level, "%d");
    ASSERT_EQ_FMT(exact[want], (unsigned long long)n, "%llu");
    for (uint64_t i = 0; i < n; i++) {
        ASSERT_EQ_FMT(want, possibility_placed_count(&cut[i]), "%d");
    }
    free(cut);
    ASSERT_EQ_FMT(0ULL, (unsigned long long)tree_enumerate_cut(&root, g_map, g_rot, g_ids, CUT_MIN, CUT_MIN / 2, &cut, &level), "%llu");
    ASSERT(cut == NULL);
    PASS();
}

/* Les sous-arbres d'une coupe sont disjoints et couvrent l'arbre : pour tout
 * nœud plus profond, UNE et une seule racine de la coupe lui est conforme.
 * C'est ce qui fait de « la racine de la solution » une notion bien définie
 * en S5. Contre-épreuve : changer une case posée fait perdre la conformité. */
TEST exactly_one_cut_root_matches_any_deeper_node(void)
{
    load_real_map();
    struct possibility_packet root;
    genesis(&root);
    struct possibility_packet *cut = NULL;
    int level = 0;
    uint64_t n = tree_enumerate_cut(&root, g_map, g_rot, g_ids, CUT_MIN, 1000000, &cut, &level);
    ASSERT(n >= CUT_MIN);
    sample_rng_t rng;
    sample_rng_seed(&rng, 99u);
    int checked = 0;
    for (int t = 0; t < 200 && checked < 30; t++) {
        double w[TREE_PROBE_LEVELS];
        memset(w, 0, sizeof w);
        struct possibility_packet deep;
        double cw = 0.0;
        int target = level + DEEPER;
        tree_probe(&root, g_map, g_rot, g_ids, &rng, w, target, target, &deep, &cw);
        if (cw == 0.0) continue;
        int16_t sol[ETERN_SIZE][ETERN_SIZE];
        for (int x = 0; x < ETERN_SIZE; x++)
            for (int y = 0; y < ETERN_SIZE; y++)
                sol[x][y] = deep.grid[x][y] != -2 ? deep.grid[x][y] : (int16_t)9999;
        int matches = 0;
        uint64_t hit = 0;
        for (uint64_t i = 0; i < n; i++) {
            if (tree_matches_solution(&cut[i], sol)) { matches++; hit = i; }
        }
        ASSERT_EQ_FMT(1, matches, "%d");
        struct possibility_packet bad = cut[hit];
        for (int x = 0; x < ETERN_SIZE; x++)
            for (int y = 0; y < ETERN_SIZE; y++)
                if (bad.grid[x][y] != -2) { bad.grid[x][y] = (int16_t)(bad.grid[x][y] + 1); x = ETERN_SIZE; break; }
        ASSERT_EQ(0, tree_matches_solution(&bad, sol));
        checked++;
    }
    ASSERT(checked >= 10);
    free(cut);
    PASS();
}

SUITE(tree_probe_suite)
{
    RUN_TEST(estimate_reports_mean_and_standard_error);
    RUN_TEST(probes_recover_the_exact_tree_profile);
    RUN_TEST(probe_captures_the_node_reached_at_a_level);
    RUN_TEST(cut_is_the_exact_first_level_above_the_threshold);
    RUN_TEST(exactly_one_cut_root_matches_any_deeper_node);
}
