/*
 * Banc « CÔTÉ TROUVER » : combien coûte l'ATTEINTE d'une solution ?
 *
 * Pourquoi ce banc existe, à côté de tests/bench/bench_refutation.c :
 *
 *   - `bench_refutation` mesure le coût de la PREUVE qu'un sous-arbre est mort.
 *     Dans un sous-arbre mort, TOUS les candidats d'une case sont essayés, quel
 *     que soit leur ordre : le compte de nœuds d'une réfutation est donc
 *     rigoureusement INDÉPENDANT de l'ordre des valeurs.
 *   - `bench_search.sh` mesure un débit (nœuds/s) depuis la genèse, avec
 *     `max_result` en garde-fou : il ne voit pas davantage l'ordre des valeurs.
 *
 * Or c'est précisément l'ordre des valeurs (quelle pièce essayer d'abord sur
 * une case) et le choix du point de départ qui décident À QUEL MOMENT la
 * branche portant la solution est atteinte. Le puzzle réel n'ayant jamais été
 * résolu, rien ne permet de mesurer cela sur `data/pieces.csv` : ce banc tourne
 * donc sur des CLONES à solution connue (`tools/gen_clone.py`), construits
 * autour d'une solution plantée.
 *
 * Conception complète : docs/conception/banc_resolution_clones.md.
 *
 * Ce que ce fichier n'ajoute PAS au chemin de production :
 *   - l'ordre des valeurs est un `#ifdef ETII_BENCH_HOOKS` d'etii_search.c,
 *     défini par cette seule unité de compilation (cf. la doc du hook) ;
 *   - une exécution = un PROCESSUS FILS, donc `stop_on_solution` est exercé tel
 *     quel (`record_solution` sort par `exit()`) sans qu'aucun retour
 *     « solution trouvée » n'ait à être ajouté au moteur.
 *
 * Compilation/exécution : `make bench-solve` (voir le makefile). La taille du
 * plateau est celle du binaire : `make bench-solve CPPFLAGS=-DETERN_PARTS=100`
 * pour des clones 10×10.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Le hook d'ordre des valeurs doit être visible AVANT l'inclusion du moteur :
 * etii_search.c déclare `etii_bench_value_order` sous ce drapeau et l'appelle
 * à l'ouverture de chaque niveau. La définition, elle, vient plus bas dans
 * cette même unité de compilation. */
#define ETII_BENCH_HOOKS 1

/* L'unité de compilation complète : le moteur est `static`. Même technique que
 * tests/bench/bench_refutation.c et tests/core/test_etii_search.c — d'où
 * l'absence d'etii_search.c de la liste des modules liés (cf. makefile). */
#include "core/etii_search.c"

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/datamanager.h"
#include "ui/logger.h"
#include "app/etii_client.h"

#include "bench_solve_stats.h"
#include "cross_mask.h"

#define MAX_INSTANCES 64
#define MAX_SEEDS     16
#define MAX_RUNS      (MAX_INSTANCES * MAX_SEEDS)

/* ==========================================================================
 * Politiques comparées (§3.4 du document de conception)
 * ========================================================================== */

/** @brief Ordre dans lequel les candidats d'une case sont essayés. */
typedef enum {
    /** Ordre naturel du compartiment : c'est EXACTEMENT la production
     *  (l'ordre d'arène, §4.8 de docs/conception/elagage_recherche.md, dit
     *  « rare_first »). Le hook rend NULL, aucune indirection. */
    VO_NATURAL = 0,
    /** L'ordre inverse — le contrôle « common_first » vis-à-vis du précédent. */
    VO_REVERSE,
    /** Permutation tirée au sort. Contrôle OBLIGATOIRE : §4.14 a montré qu'un
     *  ordre « naturel » peut perdre contre le hasard, donc aucune politique
     *  ordonnée ne se juge sans lui. */
    VO_RANDOM,
    /** Valeur la MOINS contraignante : maximise la somme des candidats encore
     *  libres sur les voisines vides une fois la pièce posée. */
    VO_LCV,
    /** Valeur la PLUS contraignante : la même somme, minimisée. */
    VO_MCV,
} value_order_t;

/** @brief D'où part la recherche (§3.4, axe « point de départ »). */
typedef enum {
    /** La genèse de production : `search_possiblity_light` choisit lui-même la
     *  case (MRV) et la développe. C'est ce que fait `first_possibility`. */
    ROOT_GENESIS = 0,
    /** Développe la case vide la plus proche du centre du plateau. */
    ROOT_CENTER,
    /** Développe le premier coin vide. */
    ROOT_BORDER,
} root_policy_t;

/**
 * @brief Ordre des CASES (variables), via le hook `ETII_BENCH_CELL_HOOKS`.
 *
 * Axe distinct de l'ordre des valeurs, et le seul qui puisse répondre à la
 * question du §8 du document de conception : la recherche met plus de 100
 * pièces à RENCONTRER les indices (mesuré sur stock de production), alors
 * qu'ils sont posés dès la genèse. Les confronter tôt aide-t-il à TROUVER ?
 *
 * `bench_refutation` ne pouvait pas y répondre : il mesure le coût de fermeture
 * d'un sous-arbre MORT, à des profondeurs (≥ 130 pièces) où les indices sont
 * déjà rencontrés. Ce banc-ci part de la genèse, c'est-à-dire du régime où la
 * question se pose.
 *
 * Deux géométries, deux hauteurs de clé, et un contrôle aléatoire PAR DENSITÉ
 * (leçon du §7.4 : un contrôle ne vaut qu'à la densité du bras qu'il contrôle).
 */
typedef enum {
    CO_NONE = 0,        /**< L'ordre de production. */
    CO_CROSS_KEY,       /**< Croix, sous `count` : départage à score MRV égal. */
    CO_CROSS_FIRST,     /**< Croix, au-dessus de `count`. */
    CO_HALO_KEY,        /**< Halo des indices (20 cases au 16×16), sous `count`. */
    CO_HALO_FIRST,      /**< Halo, au-dessus de `count` — le bras minimal. */
    CO_RANDC_KEY,       /**< Contrôle : tirage à la densité de la CROIX. */
    CO_RANDC_FIRST,
    CO_RANDH_KEY,       /**< Contrôle : tirage à la densité du HALO. */
    CO_RANDH_FIRST,
} cell_order_t;

typedef struct {
    const char *name;
    value_order_t value_order;
    cell_order_t  cell_order;
} policy_t;

static const policy_t ALL_POLICIES[] = {
    /* Axe « ordre des valeurs » — campagne PR5, close (résultat négatif). */
    { "natural", VO_NATURAL, CO_NONE },
    { "reverse", VO_REVERSE, CO_NONE },
    { "random",  VO_RANDOM,  CO_NONE },
    { "lcv",     VO_LCV,     CO_NONE },
    { "mcv",     VO_MCV,     CO_NONE },
    /* Axe « ordre des cases » — toutes à l'ordre de valeurs de PRODUCTION, pour
     * que la comparaison ne porte que sur un axe à la fois. */
    { "cross-key",  VO_NATURAL, CO_CROSS_KEY   },
    { "cross-mrv",  VO_NATURAL, CO_CROSS_FIRST },
    { "halo-key",   VO_NATURAL, CO_HALO_KEY    },
    { "halo-mrv",   VO_NATURAL, CO_HALO_FIRST  },
    { "randc-key",  VO_NATURAL, CO_RANDC_KEY   },
    { "randc-mrv",  VO_NATURAL, CO_RANDC_FIRST },
    { "randh-key",  VO_NATURAL, CO_RANDH_KEY   },
    { "randh-mrv",  VO_NATURAL, CO_RANDH_FIRST },
};
#define NB_ALL_POLICIES ((int)(sizeof(ALL_POLICIES) / sizeof(ALL_POLICIES[0])))
/** @brief Politiques jouées quand `--policies` n'est pas donné : les cinq
 *  ordres de VALEURS, soit les cinq premières entrées d'`ALL_POLICIES`. */
#define NB_DEFAULT_POLICIES 5

static const struct { const char *name; root_policy_t root; } ALL_ROOTS[] = {
    { "genesis", ROOT_GENESIS },
    { "center",  ROOT_CENTER  },
    { "border",  ROOT_BORDER  },
};
#define NB_ALL_ROOTS ((int)(sizeof(ALL_ROOTS) / sizeof(ALL_ROOTS[0])))

/* ==========================================================================
 * État du hook d'ordre des valeurs (fils uniquement)
 * ========================================================================== */

static value_order_t g_value_order = VO_NATURAL;

/* Masques d'ordre des cases, reconstruits À CHAQUE INSTANCE : la croix est
 * géométrique mais le halo est une propriété de l'INSTANCE (ses indices), et
 * chaque clone a les siens. */
static uint8_t g_cm_cross[ETERN_PARTS];
static uint8_t g_cm_halo[ETERN_PARTS];
static uint8_t g_cm_randc[ETERN_PARTS];
static uint8_t g_cm_randh[ETERN_PARTS];
static int     g_cm_cross_n = 0;
static int     g_cm_halo_n = 0;

/**
 * @brief (Re)construit les quatre masques d'ordre des cases pour une instance.
 *
 * Le halo est dérivé du plateau de GENÈSE — les cases vides voisines d'une
 * case posée, c'est-à-dire exactement celles sur lesquelles une contrainte
 * d'indice porte. Les deux tirages de contrôle partent de graines décalées :
 * issus de la même, l'un serait un préfixe de l'autre et deux contrôles
 * corrélés ne font qu'un seul contrôle (§7.4).
 */
static void bench_cell_masks_bind(const struct possibility_packet *genesis, uint64_t seed)
{
    g_cm_cross_n = cross_fill(g_cm_cross);
    g_cm_halo_n  = cross_fill_halo(g_cm_halo, genesis->grid);
    cross_fill_random(g_cm_randc, g_cm_cross_n, seed);
    cross_fill_random(g_cm_randh, g_cm_halo_n, seed ^ 0x9E3779B97F4A7C15ULL);
}

/** @brief Arme le hook d'ordre des cases (CO_NONE le désarme). */
static void bench_arm_cell_order(cell_order_t co)
{
    const uint8_t *m = NULL;
    int first = 0;

    switch (co) {
        case CO_CROSS_KEY:   m = g_cm_cross; break;
        case CO_CROSS_FIRST: m = g_cm_cross; first = 1; break;
        case CO_HALO_KEY:    m = g_cm_halo;  break;
        case CO_HALO_FIRST:  m = g_cm_halo;  first = 1; break;
        case CO_RANDC_KEY:   m = g_cm_randc; break;
        case CO_RANDC_FIRST: m = g_cm_randc; first = 1; break;
        case CO_RANDH_KEY:   m = g_cm_randh; break;
        case CO_RANDH_FIRST: m = g_cm_randh; first = 1; break;
        case CO_NONE:        break;
    }
    etii_bench_cell_bias  = (m != NULL && !first) ? m : etii_bench_no_bias;
    etii_bench_cell_first = (m != NULL &&  first) ? m : NULL;
}

static uint64_t      g_rng_state   = 1;
static uint16_t     *g_order_buf   = NULL;   /* [ETERN_PARTS][g_order_stride] */
static int           g_order_stride = 0;
static map_big_array   *g_map = NULL;
static struct array_part *g_rot = NULL;
static int8_t            g_all_face = 0;

/** @brief xorshift64* : générateur reproductible, indépendant de la libc. */
static uint64_t bench_rand(void)
{
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/**
 * @brief Somme des candidats encore libres sur les voisines VIDES de (cx,cy)
 *        une fois `cand` posée — le score de LCV/MCV.
 *
 * Réutilise les primitives du moteur (`what_search_in_grid_to_key`,
 * `map_bucket_id_mask`, `map_mask_free_count`) plutôt qu'un calcul maison :
 * la mesure doit porter sur le POUVOIR de l'heuristique, pas sur une
 * implémentation particulière, et aucune divergence de convention de faces
 * n'est alors possible. Repli par parcours du compartiment quand le masque est
 * absent (map hors gabarit), exactement comme `mrv_free_candidates`.
 *
 * @param scratch Plateau de travail, `cand` DÉJÀ posée en (cx,cy).
 * @param used    Masque des pièces utilisées, bit de `cand` DÉJÀ levé.
 */
static int bench_neighbour_freedom(struct possibility_packet *scratch,
                                   const uint64_t *used, int cx, int cy)
{
    static const int dx[4] = {0, 1, 0, -1};
    static const int dy[4] = {-1, 0, 1, 0};
    int total = 0;
    for (int d = 0; d < 4; d++) {
        int nx = cx + dx[d], ny = cy + dy[d];
        if (nx < 0 || ny < 0 || nx >= ETERN_SIZE || ny >= ETERN_SIZE) {
            continue;
        }
        if (scratch->grid[nx][ny] != -2) {
            continue;
        }
        key_part key;
        what_search_in_grid_to_key(g_rot, scratch, (int8_t)nx, (int8_t)ny, &key, g_all_face);
        const uint64_t *mask = map_bucket_id_mask(g_map, &key);
        if (mask != NULL) {
            total += map_mask_free_count(mask, g_map->id_mask_words, used);
        } else {
            struct array_part *cands = get_parts_bigarray_with_key(g_map, &key);
            for (int s = 0; s < cands->size; s++) {
                int16_t id = cands->parts[s].id;
                if (id != 0 && !((used[(id - 1) / 64] >> ((id - 1) % 64)) & 1ULL)) {
                    total++;
                }
            }
        }
    }
    return total;
}

/**
 * @brief Le point d'entrée déclaré par etii_search.c sous ETII_BENCH_HOOKS.
 *
 * Rend NULL pour la politique de production (aucune indirection, aucune
 * allocation, aucun calcul : le banc mesure alors exactement l'arbre que
 * `./eternityII` explore — c'est le verrou de PR3).
 */
static const uint16_t *etii_bench_value_order(const struct possibility_packet *board,
                                              const struct array_part *bucket,
                                              int cx, int cy, int depth)
{
    if (g_value_order == VO_NATURAL || bucket == NULL || bucket->size <= 1) {
        return NULL;
    }
    const int n = bucket->size;
    uint16_t *out = g_order_buf + (size_t)depth * (size_t)g_order_stride;

    if (g_value_order == VO_REVERSE) {
        for (int i = 0; i < n; i++) {
            out[i] = (uint16_t)(n - 1 - i);
        }
        return out;
    }
    if (g_value_order == VO_RANDOM) {
        for (int i = 0; i < n; i++) {
            out[i] = (uint16_t)i;
        }
        for (int i = n - 1; i > 0; i--) {
            int j = (int)(bench_rand() % (uint64_t)(i + 1));
            uint16_t tmp = out[i];
            out[i] = out[j];
            out[j] = tmp;
        }
        return out;
    }

    /* LCV / MCV : score par candidat, puis tri par insertion STABLE — à score
     * égal l'ordre de production est conservé, donc la politique ne mesure que
     * ce qu'elle départage réellement. */
    struct possibility_packet scratch;
    memcpy(&scratch, board, sizeof(scratch));
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, board);

    static int scores[4 * ETERN_PARTS + 4];
    for (int s = 0; s < n; s++) {
        int16_t id = bucket->parts[s].id;
        if (id == 0 || ((used[(id - 1) / 64] >> ((id - 1) % 64)) & 1ULL)) {
            /* Candidat que la boucle chaude sautera de toute façon : score
             * neutre, sa place dans l'ordre n'a aucun effet observable. */
            scores[s] = -1;
            continue;
        }
        int position = id - 1;
        scratch.grid[cx][cy] = (int16_t)id_for_rotated_part((uint16_t)id, bucket->parts[s].rotation);
        mrv_used_set(used, position);
        scores[s] = bench_neighbour_freedom(&scratch, used, cx, cy);
        mrv_used_clear(used, position);
        scratch.grid[cx][cy] = -2;
    }

    const int sign = (g_value_order == VO_LCV) ? -1 : 1; /* LCV : score décroissant */
    for (int i = 0; i < n; i++) {
        out[i] = (uint16_t)i;
    }
    for (int i = 1; i < n; i++) {
        uint16_t key = out[i];
        int key_score = sign * scores[key];
        int j = i - 1;
        while (j >= 0 && sign * scores[out[j]] > key_score) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return out;
}

/* ==========================================================================
 * Chargement d'une instance
 * ========================================================================== */

typedef struct {
    char pieces[512];
    char indices[512];   /* vide = instance sans indice */
    char label[64];
} instance_t;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** @brief Lit le `ntiles:` d'un fichier de pièces sans le charger. */
static int read_ntiles(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    int np = -1;
    if (fscanf(f, "ntiles: %d", &np) != 1) {
        np = -1;
    }
    fclose(f);
    return np;
}

/** @brief Plateau vide (toutes cases à -2, aucune pièce utilisée). */
static void make_empty_board(struct possibility_packet *b)
{
    memset(b, 0, sizeof(*b));
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            b->grid[x][y] = -2;
        }
    }
    b->alloc = 0;
    b->x = dirx[0];
    b->y = diry[0];
}

/**
 * @brief Plateau de genèse : vide, plus les indices de l'instance s'il y en a.
 *
 * Reproduit la pose d'indices de `first_possibility` (même lecture, même
 * indexation `id + ETERN_PARTS*rotation`) sans passer par le datamanager :
 * ce banc est mono-racine par construction (§4 du document de conception).
 */
static void build_genesis(struct possibility_packet *out, const char *indices_path,
                          struct array_part *rot)
{
    make_empty_board(out);
    if (indices_path == NULL || indices_path[0] == '\0') {
        return;
    }
    struct array_index *indices = read_indices(indices_path);
    for (int i = 0; i < indices->size; i++) {
        struct board_index *hint = &indices->indices[i];
        int position = hint->id + ETERN_PARTS * hint->rotation;
        if (position < 0 || position >= rot->size || rot->parts[position].id != hint->id) {
            fprintf(stderr, "indices : pièce %i rotation %i introuvable dans %s\n",
                    hint->id, hint->rotation, indices_path);
            exit(EXIT_FAILURE);
        }
        if (hint->x >= ETERN_SIZE || hint->y >= ETERN_SIZE) {
            fprintf(stderr, "indices : case (%i,%i) hors du plateau\n", hint->x, hint->y);
            exit(EXIT_FAILURE);
        }
        out->grid[hint->x][hint->y] = (int16_t)position;
        set_face_used(out->b_faceused, (uint16_t)(hint->id - 1), 1);
    }
    free_array_index(indices);
    out->alloc = (uint16_t)possibility_placed_count(out);
}

/**
 * @brief Case vide sur laquelle une politique de RACINE demande de développer.
 *
 * `ROOT_CENTER` prend la case vide la plus proche du centre géométrique,
 * `ROOT_BORDER` le premier coin vide, puis à défaut la première case de bord.
 * Développer une case, quelle qu'elle soit, reste EXHAUSTIF (tous ses
 * candidats deviennent des racines) : changer de case change l'ordre dans
 * lequel l'espace est visité, jamais l'espace visité.
 *
 * @return 1 si une case a été choisie.
 */
static int choose_root_cell(const struct possibility_packet *board, root_policy_t policy,
                            int *out_x, int *out_y)
{
    int best_x = -1, best_y = -1;
    long best_score = -1;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (board->grid[x][y] != -2) {
                continue;
            }
            long score;
            if (policy == ROOT_CENTER) {
                /* Distance (au carré, ×4 pour rester entière) au centre du
                 * plateau ; on MINIMISE, d'où le signe. */
                long ddx = 2 * x - (ETERN_SIZE - 1);
                long ddy = 2 * y - (ETERN_SIZE - 1);
                score = -(ddx * ddx + ddy * ddy);
            } else {
                /* ROOT_BORDER : un coin vaut mieux qu'un bord, un bord mieux
                 * que l'intérieur. */
                int on_x_edge = (x == 0 || x == ETERN_SIZE - 1);
                int on_y_edge = (y == 0 || y == ETERN_SIZE - 1);
                score = (long)on_x_edge + (long)on_y_edge;
            }
            if (best_x < 0 || score > best_score) {
                best_score = score;
                best_x = x;
                best_y = y;
            }
        }
    }
    if (best_x < 0) {
        return 0;
    }
    *out_x = best_x;
    *out_y = best_y;
    return 1;
}

/**
 * @brief Développe `genesis` sur la case (cx,cy) : une racine par candidat.
 *
 * @return nombre de racines produites (écrites dans `out`, borné par `max_out`).
 */
static int expand_at_cell(const struct possibility_packet *genesis, int cx, int cy,
                          map_big_array *map, struct array_part *rot,
                          struct possibility_packet *out, int max_out)
{
    struct possibility_packet scratch;
    memcpy(&scratch, genesis, sizeof(scratch));
    key_part key;
    what_search_in_grid_to_key(rot, &scratch, (int8_t)cx, (int8_t)cy, &key,
                               (int8_t)map->sizearrayM);
    struct array_part *cands = get_parts_bigarray_with_key(map, &key);
    int count = 0;
    for (int s = 0; s < cands->size && count < max_out; s++) {
        int16_t id = cands->parts[s].id;
        if (id == 0 || is_face_used(scratch.b_faceused, (uint16_t)(id - 1))) {
            continue;
        }
        memcpy(&out[count], genesis, sizeof(out[count]));
        out[count].grid[cx][cy] = (int16_t)id_for_rotated_part((uint16_t)id, cands->parts[s].rotation);
        set_face_used(out[count].b_faceused, (uint16_t)(id - 1), 1);
        out[count].alloc = (uint16_t)possibility_placed_count(&out[count]);
        count++;
    }
    return count;
}

/**
 * @brief Rattache le hook à une instance (map, rotations, tampons d'ordre).
 *
 * Appelée dans le PARENT, une fois par instance : les fils héritent l'état par
 * copie sur écriture, et l'auto-test ci-dessous tourne dans le parent.
 */
static void bench_hook_bind(map_big_array *map, struct array_part *rot)
{
    g_map = map;
    g_rot = rot;
    g_all_face = (int8_t)map->sizearrayM;
    free(g_order_buf);
    g_order_stride = rot->size + 1;
    g_order_buf = calloc((size_t)ETERN_PARTS * (size_t)g_order_stride, sizeof(*g_order_buf));
    if (g_order_buf == NULL) {
        fprintf(stderr, "allocation des tampons d'ordre impossible\n");
        exit(EXIT_FAILURE);
    }
}

/* ==========================================================================
 * Auto-test de l'instrument
 * ========================================================================== */

/**
 * @brief Contrôle DIRECT : chaque politique rend-elle bien une PERMUTATION ?
 *
 * C'est la propriété que l'auto-test par comptage de nœuds (ci-dessous) ne
 * vérifiait qu'indirectement — et l'indirection s'est révélée fragile, cf. sa
 * doc. Ici on appelle le point d'entrée sur les vrais compartiments d'un
 * plateau et on vérifie que les indices rendus sont une bijection de
 * [0, taille[ : aucun candidat perdu, aucun dupliqué, aucun indice hors
 * compartiment. Ce contrôle-ci ne suppose RIEN du moteur, il ne regarde que le
 * hook — il reste donc valide quel que soit l'ordre de variable.
 *
 * @return 0 si toutes les permutations sont valides, -1 sinon.
 */
static int bench_check_permutations(const policy_t *policies, int nb_policies,
                                    const struct possibility_packet *board)
{
    static uint8_t seen[4 * ETERN_PARTS + 4];
    const value_order_t saved = g_value_order;
    int checked = 0, rc = 0;

    /* Arrêt au PREMIER défaut : une permutation fausse l'est en général sur
     * toutes les cases, et cent messages identiques n'apprennent rien de plus
     * que le premier. */
    for (int cx = 0; cx < ETERN_SIZE && rc == 0; cx++) {
        for (int cy = 0; cy < ETERN_SIZE && rc == 0; cy++) {
            if (board->grid[cx][cy] != -2) {
                continue;
            }
            struct possibility_packet probe;
            memcpy(&probe, board, sizeof(probe));
            key_part key;
            what_search_in_grid_to_key(g_rot, &probe, (int8_t)cx, (int8_t)cy, &key, g_all_face);
            struct array_part *bucket = get_parts_bigarray_with_key(g_map, &key);
            if (bucket == NULL || bucket->size <= 1) {
                continue;
            }
            for (int p = 0; p < nb_policies && rc == 0; p++) {
                g_value_order = policies[p].value_order;
                g_rng_state = 0x9E3779B97F4A7C15ULL;
                const uint16_t *order = etii_bench_value_order(board, bucket, cx, cy, 0);
                if (order == NULL) {
                    continue;   /* ordre naturel du compartiment : rien à vérifier */
                }
                checked++;
                memset(seen, 0, (size_t)bucket->size);
                for (int i = 0; i < bucket->size; i++) {
                    if (order[i] >= (uint16_t)bucket->size || seen[order[i]]) {
                        fprintf(stderr, "AUTO-TEST ÉCHOUÉ : « %s » ne rend pas une"
                                " permutation sur la case (%d,%d) — compartiment de %d"
                                " candidats, rang %d vaut %u%s. Des candidats sont perdus"
                                " ou dupliqués : les chiffres de ce run sont à jeter.\n",
                                policies[p].name, cx, cy, bucket->size, i,
                                (unsigned)order[i],
                                order[i] < (uint16_t)bucket->size ? " (déjà vu)" : " (hors compartiment)");
                        rc = -1;
                        break;
                    }
                    seen[order[i]] = 1;
                }
            }
        }
    }
    g_value_order = saved;
    if (rc == 0) {
        printf("auto-test : %d permutations vérifiées (bijection sur le compartiment)\n",
               checked);
    }
    return rc;
}

/**
 * @brief Sur un sous-arbre MORT, toutes les politiques explorent-elles le même
 *        nombre de nœuds — et sinon, POURQUOI ?
 *
 * L'intention initiale : dans un sous-arbre sans solution, tous les candidats
 * de chaque case sont essayés, donc le compte de nœuds ne dépend pas de leur
 * ordre ; un désaccord trahissait une permutation fausse. Cette lecture
 * repose sur une hypothèse qui n'était pas écrite : que **l'ordre des
 * VARIABLES ne dépend pas de l'ordre des VALEURS**. Elle est vraie du moteur
 * actuel, elle ne l'est pas d'un moteur dont le départage de cases APPREND de
 * la recherche (§4.14 de docs/conception/elagage_recherche.md : un poids
 * d'échec par case, accumulé dans l'ordre où les échecs surviennent — donc
 * dans un ordre que la politique de valeurs détermine). Mesuré sur un tel
 * moteur : la même racine morte ferme en 168 152 à 225 683 nœuds selon la
 * politique, sans qu'aucune permutation soit fausse.
 *
 * Le contrôle est donc scindé. `bench_check_permutations` ci-dessus vérifie
 * DIRECTEMENT ce qui importe — les permutations en sont bien — et c'est lui
 * qui est fatal. Ce qui reste ici est un **détecteur de couplage** : les
 * permutations étant déjà validées, un désaccord de comptage ne peut plus
 * signifier qu'une chose, à savoir que le moteur lie l'ordre des variables à
 * l'ordre des valeurs. C'est une propriété du moteur, pas un bogue — donc
 * signalée et non fatale. Elle a une conséquence qu'il faut connaître avant de
 * lire les résultats : sur un tel moteur, « l'ordre des valeurs est neutre
 * pour la réfutation » cesse d'être vrai (§7.2 du document de conception).
 *
 * Tourne EN PROCESSUS COURANT (pas de fork) sur la première racine que la
 * politique de référence ferme dans `budget` nœuds. Aucune racine fermée dans
 * ce budget : auto-test non concluant, signalé mais non bloquant.
 *
 * **Le processus courant n'est PAS le fils** : il n'a pas fait le `chdir` vers
 * `workdir` ni détourné sa sortie standard. Or fermer une racine, c'est
 * l'explorer entièrement, et `stop_on_solution` étant à 0 ici (il FAUT aller
 * jusqu'à l'épuisement), toute solution rencontrée en chemin passe par
 * `log_solution` : un fichier `solution_<pid>_<seq>` dans le répertoire
 * courant et la grille complète sur la sortie standard. Sur une campagne de
 * 60 instances cela déversait des milliers de fichiers et de plateaux dans le
 * répertoire d'où le banc était lancé (le dépôt), et noyait son propre
 * récapitulatif. D'où la parenthèse ci-dessous : on se place dans `workdir` et
 * on détourne stdout le temps de l'auto-test, puis on rétablit les deux.
 *
 * @return 0 si tout concorde ou si aucune racine n'a pu être fermée, -1 sur désaccord.
 */
static int bench_selftest(const policy_t *policies, int nb_policies,
                          client_possibility_t *client,
                          struct possibility_packet *roots, int nb_roots,
                          int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                          long budget, const char *workdir)
{
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "auto-test : getcwd impossible\n");
        return -1;
    }
    int saved_stdout = dup(STDOUT_FILENO);
    int sink = open("/dev/null", O_WRONLY);
    if (saved_stdout < 0 || sink < 0 || chdir(workdir) != 0) {
        fprintf(stderr, "auto-test : impossible d'isoler le répertoire de travail\n");
        if (saved_stdout >= 0) close(saved_stdout);
        if (sink >= 0) close(sink);
        return -1;
    }
    fflush(stdout);
    dup2(sink, STDOUT_FILENO);
    close(sink);

    const value_order_t saved_order = g_value_order;
    int closed_root = -1;
    unsigned long long reference = 0;

    stop_on_solution = 0;   /* le sous-arbre doit être fermé, pas interrompu */
    g_value_order = policies[0].value_order;
    bench_arm_cell_order(policies[0].cell_order);
    for (int r = 0; r < nb_roots && closed_root < 0; r++) {
        struct possibility_packet work = roots[r];
        unsigned long long nodes = 0;
        request = REQUEST_CONTINUE;
        counters[0] = 0;
        if (search_packet_backtracking_mrv(client, &work, idParts, budget, 0, &nodes)
            == BT_CORE_EXHAUSTED) {
            closed_root = r;
            reference = nodes;
        }
    }
    /* Rejeu de la racine fermée sous les autres politiques — TOUJOURS dans la
     * parenthèse isolée : c'est la partie qui explore le plus, donc celle qui
     * rencontre le plus de solutions à journaliser. */
    int coupled = 0;
    int disagreements[NB_ALL_POLICIES];
    int inconclusive[NB_ALL_POLICIES];
    unsigned long long counts[NB_ALL_POLICIES];
    bt_core_result_t statuses[NB_ALL_POLICIES];
    memset(disagreements, 0, sizeof(disagreements));
    memset(inconclusive, 0, sizeof(inconclusive));
    memset(counts, 0, sizeof(counts));
    memset(statuses, 0, sizeof(statuses));
    if (closed_root >= 0) {
        for (int p = 1; p < nb_policies; p++) {
            g_value_order = policies[p].value_order;
            bench_arm_cell_order(policies[p].cell_order);
            g_rng_state = 0x9E3779B97F4A7C15ULL;
            struct possibility_packet work = roots[closed_root];
            unsigned long long nodes = 0;
            request = REQUEST_CONTINUE;
            counters[0] = 0;
            statuses[p] = search_packet_backtracking_mrv(client, &work, idParts,
                                                         budget * 4, 0, &nodes);
            counts[p] = nodes;
            /* Deux invariants de FORCE DIFFÉRENTE, et les confondre rendrait
             * l'auto-test inutilisable dès qu'un bras d'ordre des CASES entre
             * dans la comparaison :
             *
             *  - une racine MORTE reste morte sous tout bras (ordre, pas
             *    correction) — invariant dur, vrai pour tous ;
             *  - elle coûte le MÊME nombre de nœuds — vrai seulement à ordre
             *    des CASES égal. Un bras qui change l'ordre des variables
             *    change l'arbre exploré : y voir un « couplage » serait un
             *    faux positif, et c'est tout l'objet de la mesure. */
            const int same_cells = (policies[p].cell_order == policies[0].cell_order);
            if (statuses[p] != BT_CORE_EXHAUSTED) {
                /* Ne pas fermer dans le budget de l'auto-test n'est une ANOMALIE
                 * que si le bras explore le MÊME arbre. Un bras d'ordre des
                 * cases en explore un autre, et il peut légitimement y être
                 * beaucoup plus cher — la campagne de réfutation en a mesuré un
                 * à 27× la référence. L'exiger sous 4× ferait crier au couplage
                 * à chaque exécution, c'est-à-dire rendrait l'auto-test inutile. */
                if (same_cells) {
                    disagreements[p] = 1;
                    coupled = 1;
                } else {
                    inconclusive[p] = 1;
                }
            } else if (same_cells && nodes != reference) {
                disagreements[p] = 1;
                coupled = 1;
            }
        }
    }

    /* Fin de la parenthèse : on rétablit AVANT toute écriture destinée à
     * l'utilisateur, et avant de rendre la main. */
    g_value_order = saved_order;
    bench_arm_cell_order(CO_NONE);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    if (chdir(cwd) != 0) {
        fprintf(stderr, "auto-test : retour au répertoire d'origine impossible\n");
        return -1;
    }

    if (closed_root < 0) {
        printf("auto-test : aucune racine fermée en %ld nœuds — non concluant,"
               " relancer avec --selftest-budget plus grand\n\n", budget);
        return 0;
    }
    if (coupled) {
        printf("auto-test : sur la racine MORTE #%d, le nombre de nœuds DÉPEND de la"
               " politique de valeurs :\n", closed_root);
        printf("%-14s %14llu   (référence)\n", policies[0].name, reference);
        for (int p = 1; p < nb_policies; p++) {
            printf("%-14s %14llu%s\n", policies[p].name, counts[p],
                   statuses[p] != BT_CORE_EXHAUSTED ? "   (non fermée !)" : "");
        }
        printf("Les permutations ayant déjà été validées une par une, cela veut dire que\n"
               "ce moteur lie l'ordre des VARIABLES à l'ordre des VALEURS (départage de\n"
               "cases appris, p.ex.), OU qu'un bras a cessé de fermer une racine morte.\n"
               "Ce n'est pas un bogue, mais deux lectures tombent :\n"
               "  - « l'ordre des valeurs est neutre pour la réfutation » (§7.2) ;\n"
               "  - la comparaison de politiques mesure ici DEUX effets à la fois.\n"
               "(Les bras d'ordre des CASES ne sont PAS tenus au même nombre de nœuds :\n"
               " ils changent l'arbre par construction. Seule leur FERMETURE est exigée.)\n\n");
        return 0;
    }
    int nb_same_cells = 0, nb_other = 0, nb_inconclusive = 0;
    for (int p = 0; p < nb_policies; p++) {
        if (policies[p].cell_order == policies[0].cell_order) {
            nb_same_cells++;
        } else if (inconclusive[p]) {
            nb_inconclusive++;
        } else {
            nb_other++;
        }
    }
    printf("auto-test : racine morte #%d fermée en %llu nœuds, identique pour les"
           " %d politique(s) de MÊME ordre de cases", closed_root, reference, nb_same_cells);
    if (nb_other > 0) {
        printf(" ; fermée aussi par %d bras d'ordre de cases, en un nombre de nœuds"
               " différent — attendu, ils changent l'arbre", nb_other);
    }
    if (nb_inconclusive > 0) {
        printf(" ; %d bras n'ont pas fermé sous %ld nœuds — NON CONCLUANT pour eux,"
               " pas une anomalie (--selftest-budget plus grand pour trancher)",
               nb_inconclusive, budget * 4);
    }
    printf("\n\n");
    return 0;
}

/* ==========================================================================
 * Une exécution = un processus fils
 * ========================================================================== */

typedef enum {
    RUN_SOLVED = 0,   /* record_solution a fait exit(EXIT_SUCCESS) */
    RUN_BUDGET,       /* plafond de nœuds atteint */
    RUN_EXHAUSTED,    /* espace épuisé sans solution (racines partielles) */
    RUN_STOPPED,      /* arrêt demandé */
    RUN_FAILED,       /* le fils est mort sans rapporter */
} run_status_t;

/** @brief Ce que le fils écrit sur le tube avant de mourir. */
typedef struct {
    unsigned long long nodes;
    double seconds;
    int roots;          /* nombre de racines déjà consommées */
} run_report_t;

/* Codes de sortie du fils. 0 est RÉSERVÉ à `record_solution` (exit(EXIT_SUCCESS)) :
 * c'est justement ce qui permet de reconnaître une solution sans rien ajouter
 * au moteur (§4 du document de conception). */
#define CHILD_EXIT_BUDGET    10
#define CHILD_EXIT_EXHAUSTED 11
#define CHILD_EXIT_STOPPED   12

static int         g_report_fd = -1;
static run_report_t g_report;
static double      g_child_t0 = 0.0;

/**
 * @brief Rapporte nœuds et temps, quel que soit le CHEMIN de sortie du fils.
 *
 * Enregistré par `atexit` : c'est ce qui rend le cas « solution trouvée »
 * mesurable sans toucher au moteur — `record_solution` sort par `exit()` au
 * beau milieu de la boucle chaude, et cette fonction s'exécute quand même.
 * `counters[0]` est le compteur de nœuds que la boucle incrémente
 * (`counters[client->compteur]`), cumulé sur toutes les racines consommées.
 */
static void bench_child_report(void)
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

typedef struct {
    value_order_t value_order;
    cell_order_t  cell_order;
    root_policy_t root_policy;
    uint64_t      seed;
    long          budget;
    const char   *workdir;
} run_config_t;

/**
 * @brief Corps du fils : pose la genèse, cherche, sort. NE REVIENT JAMAIS.
 */
static void run_child(const run_config_t *cfg,
                      map_big_array *map, struct array_part *rot,
                      struct possibility_packet *roots, int nb_roots,
                      int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    /* Première instruction du fils, comme l'exige la règle de sûreté du fork
     * (AGENTS.md) : un fils hérite la chaîne `atexit()` de son parent, y
     * compris la remise en état du terminal. Ce banc ne démarre aucune console,
     * donc rien n'est aujourd'hui enregistré — l'appel est là pour que ça reste
     * vrai le jour où un module lié en enregistrerait un. */
    status_zone_disown_child();

    /* Répertoire de travail dédié : `log_solution` écrit `solution_<pid>_<seq>`
     * dans le répertoire courant, et `events.log` avec lui. */
    if (chdir(cfg->workdir) != 0) {
        _exit(CHILD_EXIT_STOPPED);
    }
    /* `log_solution` imprime le plateau trouvé sur la sortie standard : une
     * centaine de lignes par exécution, qui noieraient le tableau du banc.
     * Détourné vers un fichier du répertoire de travail — la preuve est
     * conservée, le récapitulatif reste lisible. stderr reste ouvert : une
     * vraie erreur doit rester visible immédiatement. */
    if (freopen("bench_solve_children.log", "a", stdout) == NULL) {
        _exit(CHILD_EXIT_STOPPED);
    }

    /* g_map/g_rot/g_order_buf sont déjà en place : bench_hook_bind les a posés
     * dans le parent, le fils en hérite par copie sur écriture. */
    g_value_order = cfg->value_order;
    bench_arm_cell_order(cfg->cell_order);
    g_rng_state = cfg->seed != 0 ? cfg->seed : 0x9E3779B97F4A7C15ULL;

    client_possibility_t client;
    memset(&client, 0, sizeof(client));
    client.compteur = 0;
    client.all_rotate_part = rot;
    client.map_part = map;

    counters[0] = 0;
    max_result = 0;
    request = REQUEST_CONTINUE;
    /* Le chemin réel est exercé tel quel : une solution fait sortir le fils. */
    stop_on_solution = 1;

    g_child_t0 = now_seconds();
    memset(&g_report, 0, sizeof(g_report));
    atexit(bench_child_report);

    int status = CHILD_EXIT_EXHAUSTED;
    for (int r = 0; r < nb_roots; r++) {
        g_report.roots = r + 1;
        long remaining = cfg->budget > 0
            ? cfg->budget - (long)counters[0]
            : 0;
        if (cfg->budget > 0 && remaining <= 0) {
            status = CHILD_EXIT_BUDGET;
            break;
        }
        unsigned long long nodes = 0;
        /* allow_delegate = 0 : aucun serveur, et céder une partie du
         * sous-arbre fausserait la mesure — même raison que bench_refutation. */
        bt_core_result_t rc = search_packet_backtracking_mrv(&client, &roots[r], idParts,
                                                             remaining, 0, &nodes);
        if (rc == BT_CORE_BUDGET) {
            status = CHILD_EXIT_BUDGET;
            break;
        }
        if (rc == BT_CORE_STOPPED) {
            status = CHILD_EXIT_STOPPED;
            break;
        }
        /* BT_CORE_EXHAUSTED : racine morte, on passe à la suivante. */
    }
    /* exit() et non _exit() : l'atexit ci-dessus DOIT s'exécuter (et sur macOS
     * _exit sauterait aussi le vidage gcov, cf. AGENTS.md). */
    exit(status);
}

/** @brief Lance une exécution et récupère son rapport. */
static run_status_t run_once(const run_config_t *cfg,
                             map_big_array *map, struct array_part *rot,
                             struct possibility_packet *roots, int nb_roots,
                             int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                             run_report_t *out)
{
    int fds[2];
    if (pipe(fds) != 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    /* stdout/stderr vidés AVANT le fork : un tampon non vidé serait écrit deux
     * fois (règle générale du dépôt sur le fork, cf. AGENTS.md). */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        close(fds[0]);
        g_report_fd = fds[1];
        run_child(cfg, map, rot, roots, nb_roots, idParts);
        _exit(CHILD_EXIT_STOPPED); /* inatteignable */
    }

    close(fds[1]);
    run_report_t report;
    memset(&report, 0, sizeof(report));
    size_t got = 0;
    while (got < sizeof(report)) {
        ssize_t n = read(fds[0], (char *)&report + got, sizeof(report) - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fds[0]);

    int wstatus = 0;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
        /* signal : on réessaie */
    }
    *out = report;
    if (got != sizeof(report)) {
        return RUN_FAILED;
    }
    if (!WIFEXITED(wstatus)) {
        return RUN_FAILED;
    }
    switch (WEXITSTATUS(wstatus)) {
        case EXIT_SUCCESS:        return RUN_SOLVED;
        case CHILD_EXIT_BUDGET:   return RUN_BUDGET;
        case CHILD_EXIT_EXHAUSTED:return RUN_EXHAUSTED;
        default:                  return RUN_STOPPED;
    }
}

static const char *status_label(run_status_t s)
{
    switch (s) {
        case RUN_SOLVED:    return "RÉSOLU";
        case RUN_BUDGET:    return "plafond";
        case RUN_EXHAUSTED: return "épuisé";
        case RUN_STOPPED:   return "arrêt";
        case RUN_FAILED:    return "ÉCHEC";
    }
    return "?";
}

/* ==========================================================================
 * Grille de mesure et bilan
 * ========================================================================== */

typedef struct {
    double nodes[MAX_RUNS];
    /** Temps mural de l'exécution. Sert au COÛT PAR NŒUD (§3.5 c) : une
     *  politique qui divise le nombre de nœuds par deux mais coûte 60 % de plus
     *  par nœud n'est pas un gain — et seule la comparaison des deux colonnes
     *  le dit. */
    double seconds[MAX_RUNS];
    int    solved[MAX_RUNS];
    int    count;
} series_t;

static void print_tally(const policy_t *policies, int nb_policies,
                        const series_t *series, long budget)
{
    printf("\n%-10s %8s %14s %14s %12s %14s\n",
           "politique", "résolus", "médiane nœuds", "moy. géom.", "temps total", "nœuds/s");
    for (int p = 0; p < nb_policies; p++) {
        const series_t *s = &series[p];
        double solved_nodes[MAX_RUNS];
        int nb_solved = 0;
        /* Débit agrégé sur TOUTES les exécutions, résolues ou non : c'est un
         * coût par nœud de la politique, pas une mesure de son succès. Une
         * exécution au plafond y contribue autant qu'une autre — c'est même
         * celle qui le mesure le mieux, le plafond fixant le nombre de nœuds. */
        double total_seconds = 0.0, total_nodes = 0.0;
        for (int i = 0; i < s->count; i++) {
            total_seconds += s->seconds[i];
            total_nodes += s->nodes[i];
            if (s->solved[i]) {
                solved_nodes[nb_solved++] = s->nodes[i];
            }
        }
        double sorted[MAX_RUNS];
        memcpy(sorted, solved_nodes, sizeof(double) * (size_t)nb_solved);
        bench_stats_sort(sorted, nb_solved);
        printf("%-10s %4d/%-3d %14.0f %14.0f %10.3f s %14.0f\n",
               policies[p].name, nb_solved, s->count,
               bench_stats_median_sorted(sorted, nb_solved),
               bench_stats_geomean(solved_nodes, nb_solved),
               total_seconds,
               total_seconds > 0.0 ? total_nodes / total_seconds : 0.0);
    }

    /* Courbe de survie : la médiane seule ne dit rien dès qu'une moitié des
     * exécutions bute sur le plafond. */
    printf("\ncourbe de survie (part des exécutions résolues sous N nœuds) :\n");
    printf("%-10s", "politique");
    for (double t = 1e4; t <= (double)budget; t *= 10.0) {
        printf(" %11.0e", t);
    }
    printf(" %11s\n", "plafond");
    for (int p = 0; p < nb_policies; p++) {
        printf("%-10s", policies[p].name);
        for (double t = 1e4; t <= (double)budget; t *= 10.0) {
            printf(" %10.0f%%",
                   100.0 * bench_stats_survival(series[p].nodes, series[p].solved,
                                                series[p].count, t));
        }
        printf(" %10.0f%%\n",
               100.0 * bench_stats_survival(series[p].nodes, series[p].solved,
                                            series[p].count, (double)budget));
    }

    /* Comparaison appariée contre la politique de PRODUCTION (la première
     * demandée) : la seule lecture honnête, toutes explorent la même instance. */
    printf("\ncomparaison appariée contre « %s » (référence) :\n", policies[0].name);
    printf("%-10s %8s %8s %8s %11s\n", "politique", "gagne", "perd", "égal", "indécis");
    for (int p = 1; p < nb_policies; p++) {
        int w = 0, l = 0, t = 0, u = 0;
        bench_stats_paired(series[p].nodes, series[p].solved,
                           series[0].nodes, series[0].solved,
                           series[p].count, &w, &l, &t, &u);
        printf("%-10s %8d %8d %8d %11d\n", policies[p].name, w, l, t, u);
    }

    /* Analyse de redémarrage : AUCUNE implémentation, une simple lecture de la
     * distribution (§3.4). Elle ne vaut que pour les politiques aléatoires —
     * relancer une politique déterministe rejouerait la même exécution. */
    printf("\ncoût attendu d'un redémarrage à seuil (lecture de la distribution,\n"
           "aucun mécanisme : ne vaut que pour une politique ALÉATOIRE) :\n");
    printf("%-10s %13s %9s %16s %18s\n",
           "politique", "seuil", "P(succès)", "E[coût] (nœuds)", "E sans redémarrage");
    for (int p = 0; p < nb_policies; p++) {
        /* La référence est une MOYENNE, pas la médiane. `expected_nodes` est un
         * coût ESPÉRÉ ; le comparer à une médiane sur une distribution à queue
         * lourde fausse la lecture au détriment du redémarrage, la médiane
         * étant très en dessous de la moyenne. Une exécution non résolue est
         * comptée à sa valeur observée (le plafond), donc la référence est
         * alors une BORNE INFÉRIEURE de E[N] — signalée par « >= ». */
        double sum_nodes = 0.0;
        int censored = 0;
        for (int i = 0; i < series[p].count; i++) {
            sum_nodes += series[p].nodes[i];
            if (!series[p].solved[i]) {
                censored++;
            }
        }
        double baseline = series[p].count > 0 ? sum_nodes / (double)series[p].count : 0.0;
        const char *bound = (censored > 0) ? ">=" : "  ";
        for (double c = 1e4; c <= (double)budget; c *= 10.0) {
            bench_restart_t r = bench_stats_restart(series[p].nodes, series[p].solved,
                                                    series[p].count, c);
            if (r.expected_nodes < 0) {
                continue;
            }
            printf("%-10s %13.0e %8.0f%% %16.0f %15s %.0f\n",
                   policies[p].name, c, 100.0 * r.p_success, r.expected_nodes,
                   bound, baseline);
        }
    }
}

static void usage(void)
{
    printf("Usage : bench_solve [options]\n"
           "  --pieces <f>        une instance (clone de tools/gen_clone.py)\n"
           "  --indices <f>       ses indices (défaut : aucun)\n"
           "  --instance-dir <d>  toutes les instances pieces_*.csv d'un répertoire,\n"
           "                      appariées avec indices_*.csv de même suffixe\n"
           "  --budget <n>        plafond de nœuds par exécution (défaut 50000000)\n"
           "  --policies <liste>  politiques comparées (défaut : les cinq ordres de VALEURS ;\n"
           "                      le PREMIER sert de référence appariée)\n"
           "                      ordres de VALEURS : natural,reverse,random,lcv,mcv\n"
           "                      ordres de CASES (tous à l'ordre de valeurs de production) :\n"
           "                        cross-key,cross-mrv : la croix séparatrice (88 cases en 16x16)\n"
           "                        halo-key,halo-mrv   : le halo des indices de l'instance (20)\n"
           "                        randc-*,randh-*     : contrôles aléatoires aux MÊMES densités\n"
           "                      -key = départage à score MRV égal ; -mrv = avant toutes les autres\n"
           "  --cross-seed <n>    graine des contrôles aléatoires d'ordre des cases (défaut 1)\n"
           "  --root <nom>        point de départ parmi genesis,center,border (défaut genesis)\n"
           "  --seeds <n>         graines par instance pour les politiques aléatoires (défaut 1)\n"
           "  --workdir <d>       répertoire de travail des fils (défaut : un mkdtemp)\n"
           "  --selftest-budget <n> plafond de l'auto-test de l'instrument (défaut 200000,\n"
           "                      0 = auto-test désactivé)\n"
           "\n"
           "La taille du plateau est celle du binaire : make bench-solve"
           " CPPFLAGS=-DETERN_PARTS=100\n"
           "pour des clones 10x10. Une instance dont le ntiles ne vaut pas %d est refusée.\n",
           ETERN_PARTS);
}

/** @brief Ajoute les instances `pieces_*.csv` d'un répertoire. */
static int scan_instance_dir(const char *dir, instance_t *out, int max_out)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "ouverture du répertoire %s impossible\n", dir);
        exit(EXIT_FAILURE);
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max_out) {
        if (strncmp(e->d_name, "pieces_", 7) != 0) {
            continue;
        }
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".csv") != 0) {
            continue;
        }
        snprintf(out[count].pieces, sizeof(out[count].pieces), "%s/%s", dir, e->d_name);
        snprintf(out[count].label, sizeof(out[count].label), "%.*s",
                 (int)(len - 7 - 4), e->d_name + 7);
        snprintf(out[count].indices, sizeof(out[count].indices), "%s/indices_%s",
                 dir, e->d_name + 7);
        /* Pas d'indices appariés : instance sans indice, pas une erreur. */
        if (access(out[count].indices, R_OK) != 0) {
            out[count].indices[0] = '\0';
        }
        count++;
    }
    closedir(d);
    /* Ordre de `readdir` non spécifié : on trie pour que deux exécutions du
     * banc alignent les mêmes instances sur les mêmes lignes. */
    for (int i = 1; i < count; i++) {
        instance_t key = out[i];
        int j = i - 1;
        while (j >= 0 && strcmp(out[j].pieces, key.pieces) > 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

int main(int argc, char **argv)
{
    /* Pools alloués dynamiquement : appel OBLIGATOIRE avant tout usage de
     * datamanager.c (lié via TEST_MODULES), même si ce banc n'en exerce
     * aucune fonction de pool. Même préambule que bench_refutation. */
    datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT);

    instance_t instances[MAX_INSTANCES];
    int nb_instances = 0;
    long budget = 50000000;
    int nb_seeds = 1;
    long selftest_budget = 200000;
    const char *workdir = NULL;
    root_policy_t root_policy = ROOT_GENESIS;
    uint64_t cross_seed = 1;
    const char *root_name = "genesis";

    policy_t policies[NB_ALL_POLICIES];
    /* Défaut : les NB_DEFAULT_POLICIES premières entrées, c'est-à-dire les cinq
     * ordres de VALEURS. Les bras d'ordre des CASES sont déclarés mais se
     * demandent par `--policies` : une invocation existante de ce banc doit
     * continuer de mesurer exactement ce qu'elle mesurait, et comparer treize
     * politiques coûte plus du double. L'ORDRE des entrées est donc
     * significatif (même discipline que NB_DEFAULT_ENGINES dans
     * tests/bench/bench_refutation.c). */
    int nb_policies = NB_DEFAULT_POLICIES;
    memcpy(policies, ALL_POLICIES, NB_DEFAULT_POLICIES * sizeof(ALL_POLICIES[0]));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pieces") == 0 && i + 1 < argc) {
            if (nb_instances >= MAX_INSTANCES) { fprintf(stderr, "trop d'instances\n"); return EXIT_FAILURE; }
            snprintf(instances[nb_instances].pieces, sizeof(instances[0].pieces), "%s", argv[++i]);
            instances[nb_instances].indices[0] = '\0';
            snprintf(instances[nb_instances].label, sizeof(instances[0].label), "%s",
                     strrchr(instances[nb_instances].pieces, '/')
                         ? strrchr(instances[nb_instances].pieces, '/') + 1
                         : instances[nb_instances].pieces);
            nb_instances++;
        } else if (strcmp(argv[i], "--indices") == 0 && i + 1 < argc) {
            if (nb_instances == 0) { fprintf(stderr, "--indices doit suivre --pieces\n"); return EXIT_FAILURE; }
            snprintf(instances[nb_instances - 1].indices, sizeof(instances[0].indices), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--instance-dir") == 0 && i + 1 < argc) {
            nb_instances += scan_instance_dir(argv[++i], instances + nb_instances,
                                              MAX_INSTANCES - nb_instances);
        } else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) {
            budget = atol(argv[++i]);
        } else if (strcmp(argv[i], "--seeds") == 0 && i + 1 < argc) {
            nb_seeds = atoi(argv[++i]);
            if (nb_seeds < 1) nb_seeds = 1;
            if (nb_seeds > MAX_SEEDS) nb_seeds = MAX_SEEDS;
        } else if (strcmp(argv[i], "--selftest-budget") == 0 && i + 1 < argc) {
            selftest_budget = atol(argv[++i]);
        } else if (strcmp(argv[i], "--workdir") == 0 && i + 1 < argc) {
            workdir = argv[++i];
        } else if (strcmp(argv[i], "--cross-seed") == 0 && i + 1 < argc) {
            cross_seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root_name = argv[++i];
            int found = 0;
            for (int r = 0; r < NB_ALL_ROOTS; r++) {
                if (strcmp(root_name, ALL_ROOTS[r].name) == 0) {
                    root_policy = ALL_ROOTS[r].root;
                    found = 1;
                }
            }
            if (!found) { usage(); return EXIT_FAILURE; }
        } else if (strcmp(argv[i], "--policies") == 0 && i + 1 < argc) {
            nb_policies = 0;
            char *copy = strdup(argv[++i]);
            for (char *tok = strtok(copy, ","); tok != NULL; tok = strtok(NULL, ",")) {
                for (int p = 0; p < NB_ALL_POLICIES; p++) {
                    if (strcmp(tok, ALL_POLICIES[p].name) == 0) {
                        policies[nb_policies++] = ALL_POLICIES[p];
                    }
                }
            }
            free(copy);
            if (nb_policies == 0) { usage(); return EXIT_FAILURE; }
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }

    if (nb_instances == 0) {
        usage();
        return EXIT_FAILURE;
    }

    char tmpl[] = "/tmp/etii_bench_solve_XXXXXX";
    if (workdir == NULL) {
        workdir = mkdtemp(tmpl);
        if (workdir == NULL) {
            perror("mkdtemp");
            return EXIT_FAILURE;
        }
    } else if (access(workdir, X_OK | W_OK) != 0) {
        /* Un `--workdir` inexistant ou non traversable faisait échouer le
         * `chdir` de CHAQUE fils, qui sortait alors sans avoir rien exécuté :
         * le banc affichait 30 lignes « ÉCHEC / 0 nœud / 0,000 s » sans le
         * moindre diagnostic, et la campagne entière ressemblait à un moteur
         * qui ne résout plus rien. Le chemin est donc éprouvé UNE fois, dans le
         * parent, avant le premier fork. */
        fprintf(stderr, "--workdir %s : répertoire inutilisable (%s). Les fils ne"
                " pourraient pas s'y placer et n'exécuteraient rien.\n",
                workdir, strerror(errno));
        return EXIT_FAILURE;
    }

    counters = calloc(1, sizeof(*counters));
    lastfilesize = calloc(1, sizeof(*lastfilesize));
    lastroot = calloc(1, sizeof(*lastroot));
    lastdepth = calloc(1, sizeof(*lastdepth));
    if (counters == NULL || lastfilesize == NULL || lastroot == NULL || lastdepth == NULL) {
        fprintf(stderr, "allocation des compteurs impossible\n");
        return EXIT_FAILURE;
    }
    /* Aucun serveur : jamais de délégation ni d'envoi réseau depuis ce banc
     * (send_solution devient un no-op, cf. datamanager.c). */
    set_server_ip(NULL);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    init_id_parts(idParts);

    printf("\nbanc « côté trouver » : coût de l'ATTEINTE d'une solution\n");
    printf("plateau %dx%d (%d pièces)   plafond : %ld nœuds par exécution   racine : %s\n",
           ETERN_SIZE, ETERN_SIZE, ETERN_PARTS, budget, root_name);
    printf("%d instance(s) x %d politique(s) x %d graine(s) = %d exécutions\n\n",
           nb_instances, nb_policies, nb_seeds, nb_instances * nb_policies * nb_seeds);
    printf("%-18s %-10s %6s %14s %10s %8s %s\n",
           "instance", "politique", "graine", "nœuds", "temps", "racines", "statut");

    series_t series[NB_ALL_POLICIES];
    memset(series, 0, sizeof(series));

    /* Toutes les racines d'une instance tiennent en mémoire : le développement
     * d'une seule case borne leur nombre par la taille d'un compartiment. */
    static struct possibility_packet roots[4 * ETERN_PARTS + 4];

    for (int inst_i = 0; inst_i < nb_instances; inst_i++) {
        instance_t *inst = &instances[inst_i];
        int np = read_ntiles(inst->pieces);
        if (np != ETERN_PARTS) {
            fprintf(stderr, "%s : ntiles=%d, ce binaire est compilé pour %d pièces"
                    " (make bench-solve CPPFLAGS=-DETERN_PARTS=%d)\n",
                    inst->pieces, np, ETERN_PARTS, np > 0 ? np : ETERN_PARTS);
            return EXIT_FAILURE;
        }

        struct array_part *parts = read_parts(inst->pieces);
        struct array_part *rot = rotate_all_parts(parts);
        map_big_array *map = buildBigArray(rot, search_max_face(rot));

        struct possibility_packet genesis;
        build_genesis(&genesis, inst->indices[0] ? inst->indices : NULL, rot);

        int nb_roots;
        if (root_policy == ROOT_GENESIS) {
            /* La genèse de PRODUCTION : même appel que first_possibility.
             * `File` alloué au TAS, jamais sur la pile : free_file libère aussi
             * la structure elle-même (free(suite)) — un `File` local y serait
             * un free() sur une adresse de pile. */
            File *file = malloc(sizeof(File));
            if (file == NULL) {
                fprintf(stderr, "allocation de la file de genèse impossible\n");
                return EXIT_FAILURE;
            }
            init_file(file, sizeof(struct possibility_packet));
            struct possibility_packet seed_board = genesis;
            search_possiblity_light(file, &seed_board, map, rot, idParts);
            nb_roots = 0;
            while (file->size > 0 && nb_roots < (int)(sizeof(roots) / sizeof(roots[0]))) {
                scroll(file, &roots[nb_roots]);
                nb_roots++;
            }
            free_file(file);
        } else {
            int cx = 0, cy = 0;
            if (!choose_root_cell(&genesis, root_policy, &cx, &cy)) {
                fprintf(stderr, "%s : aucune case vide pour la racine\n", inst->pieces);
                return EXIT_FAILURE;
            }
            nb_roots = expand_at_cell(&genesis, cx, cy, map, rot, roots,
                                      (int)(sizeof(roots) / sizeof(roots[0])));
        }
        if (nb_roots == 0) {
            fprintf(stderr, "%s : la genèse ne produit aucune racine\n", inst->pieces);
            return EXIT_FAILURE;
        }

        bench_hook_bind(map, rot);
        bench_cell_masks_bind(&genesis, cross_seed);

        if (selftest_budget > 0 && nb_policies > 1) {
            client_possibility_t probe;
            memset(&probe, 0, sizeof(probe));
            probe.compteur = 0;
            probe.all_rotate_part = rot;
            probe.map_part = map;
            /* Contrôle DIRECT d'abord : il est fatal, et il désambiguïse le
             * verdict du second (un désaccord de comptage ne peut plus être
             * imputé à une permutation fausse une fois celle-ci validée). */
            if (bench_check_permutations(policies, nb_policies, &roots[0]) != 0) {
                return EXIT_FAILURE;
            }
            if (bench_selftest(policies, nb_policies, &probe, roots, nb_roots,
                               idParts, selftest_budget, workdir) != 0) {
                return EXIT_FAILURE;
            }
        }

        for (int p = 0; p < nb_policies; p++) {
            /* La grille est (instance × graine) pour TOUTES les politiques : la
             * comparaison appariée lit `series[p].nodes[i]` contre
             * `series[0].nodes[i]`, donc l'indice i doit désigner la même
             * exécution partout. Une politique DÉTERMINISTE n'est EXÉCUTÉE
             * qu'une fois — la graine ne change rien pour elle — mais son
             * résultat remplit quand même ses `nb_seeds` cases : sans cela, ses
             * séries seraient plus courtes et l'appariement comparerait des
             * instances différentes (constaté : `random` gagnait/perdait contre
             * une case jamais remplie). Le poids de chaque instance reste
             * identique d'une politique à l'autre, médiane et courbe de survie
             * sont donc inchangées par cette duplication. */
            run_report_t rep;
            run_status_t st = RUN_FAILED;
            memset(&rep, 0, sizeof(rep));
            for (int s = 0; s < nb_seeds; s++) {
                if (s == 0 || policies[p].value_order == VO_RANDOM) {
                    run_config_t cfg;
                    cfg.value_order = policies[p].value_order;
                    cfg.cell_order = policies[p].cell_order;
                    cfg.root_policy = root_policy;
                    cfg.seed = (uint64_t)(s + 1) * 0x9E3779B97F4A7C15ULL + (uint64_t)inst_i;
                    cfg.budget = budget;
                    cfg.workdir = workdir;

                    st = run_once(&cfg, map, rot, roots, nb_roots, idParts, &rep);

                    printf("%-18.18s %-10s %6d %14llu %8.3f s %8d %s\n",
                           inst->label, policies[p].name, s, rep.nodes, rep.seconds,
                           rep.roots, status_label(st));
                    fflush(stdout);
                }
                if (series[p].count < MAX_RUNS) {
                    series[p].nodes[series[p].count] = (double)rep.nodes;
                    series[p].seconds[series[p].count] = rep.seconds;
                    series[p].solved[series[p].count] = (st == RUN_SOLVED);
                    series[p].count++;
                }
            }
        }

        free_bigarray(map);
        free_array_part(rot);
        free_array_part(parts);
    }

    print_tally(policies, nb_policies, series, budget);
    printf("\nrépertoire de travail des fils (solutions écrites) : %s\n", workdir);
    return EXIT_SUCCESS;
}
