/*
 * Banc de RÉFUTATION : combien coûte la PREUVE qu'une possibilité ne mène à
 * aucune solution ?
 *
 * Pourquoi ce banc existe, à côté de tests/bench/bench_search.sh :
 *
 *   - `bench_search.sh` mesure un DÉBIT (nœuds/s) et, comme garde-fou, la
 *     profondeur atteinte (`max_result`). Ces deux grandeurs sont des proxys
 *     imparfaits : le débit ne dit pas si les nœuds explorés servent à quelque
 *     chose, et `max_result` récompense le fait de descendre loin dans une
 *     branche — or descendre loin dans une branche qui ne mène nulle part n'est
 *     PAS l'objectif du solveur.
 *   - Le vrai travail du moteur est l'inverse : établir le plus TÔT possible
 *     qu'une possibilité est morte, pour ne jamais développer son sous-arbre.
 *     La mesure qui correspond à cet objectif est le coût de FERMETURE d'un
 *     sous-arbre (nœuds et temps jusqu'à `BT_CORE_EXHAUSTED`), à racine
 *     IDENTIQUE entre deux moteurs. C'est ce que mesure ce banc.
 *
 * La primitive existe déjà : `search_packet_backtracking_core` /
 * `search_packet_backtracking_mrv` acceptent toutes deux un plafond de nœuds et
 * renvoient `BT_CORE_EXHAUSTED` (sous-arbre entièrement exploré : mort prouvé,
 * sauf solution signalée) ou `BT_CORE_BUDGET` (indéterminé) — c'est la même
 * primitive que la preuve bornée du pruner (§4.6b de
 * docs/conception/elagage_recherche.md). Ce fichier n'est qu'un harnais autour
 * d'elles ; il n'ajoute aucun code au chemin de production.
 *
 * Deux sources de racines, correspondant à deux questions différentes :
 *
 *   --from-back <fichier>  Les possibilités d'un VRAI stock serveur (fichier
 *                          `.back`). Répond à « le stock d'un serveur en cours
 *                          fournit-il des exemples exploitables ? » — c'est-à-
 *                          dire des sous-arbres assez petits pour être fermés.
 *   (défaut)               Racines fabriquées : une descente est menée par le
 *                          moteur MRV jusqu'à un plateau profond, puis on en
 *                          extrait des préfixes de profondeur croissante
 *                          (`--depths`). Répond à « à partir de quelle
 *                          profondeur un sous-arbre devient-il réfutable, et
 *                          quel moteur le réfute le moins cher ? »
 *
 * Chaque racine est soumise aux DEUX moteurs (ordre fixe et ordre dynamique),
 * avec le même plafond de nœuds : c'est une comparaison appariée, la seule
 * lecture honnête (les deux explorent le même sous-arbre, seul l'ordre change).
 *
 * Compilation/exécution : `make bench-refutation` (voir le makefile).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* L'unité de compilation complète : les deux moteurs sont `static`. Même
 * technique que tests/core/test_etii_search.c — d'où le retrait d'etii_search.c
 * de la liste des modules liés (cf. makefile). */
#include "core/etii_search.c"

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "app/etii_client.h"
#include "core/datamanager.h"

/* etii_search.c inclut déjà "app/gpu_pruner.h" sous WITH_CUDA (protégé par son
 * propre garde d'inclusion) ; répété ici pour rendre la dépendance explicite à
 * la lecture de ce fichier. */
#ifdef WITH_CUDA
#include "app/gpu_pruner.h"
#endif

#define MAX_DEPTHS 32

static client_possibility_t g_client;
static int16_t g_idParts[ETERN_PARTS + 1][PART_SIZES];

/** @brief Alloue les compteurs globaux que la boucle de recherche incrémente. */
static void alloc_counters(void)
{
    counters = calloc(NB_THREADS, sizeof(*counters));
    lastfilesize = calloc(NB_THREADS, sizeof(*lastfilesize));
    if (counters == NULL || lastfilesize == NULL) {
        fprintf(stderr, "allocation des compteurs impossible\n");
        exit(EXIT_FAILURE);
    }
}

/** @brief idParts comme dans autosearch : idParts[p][r] = p + ETERN_PARTS*r. */
static void fill_idparts(void)
{
    for (int p = 0; p <= ETERN_PARTS; p++) {
        int base = p;
        for (int r = 0; r < PART_SIZES; r++) {
            g_idParts[p][r] = (int16_t)base;
            base += ETERN_PARTS;
        }
    }
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

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/**
 * @brief Une variante de moteur à comparer.
 *
 * Trois axes, volontairement SÉPARÉS : les deux moteurs historiques
 * confondaient ordre de parcours et portée de la détection de case morte
 * (fixe = LOCALE, 4 voisines via `bt_forward_check` ; dynamique = GLOBALE,
 * tout le plateau via `mrv_choose_cell`) — `global_check` isole cet axe en
 * armant le balayage global sur l'ordre fixe (§4.7, ablation). `singleton_check`
 * est un troisième mécanisme, orthogonal aux deux premiers puisqu'il vit dans
 * `bt_forward_check`, donc actif pour les deux moteurs dès qu'il est levé
 * (§4.4, conflit de singletons / théorème de Hall |S|=2).
 */
typedef struct {
    const char *name;
    /** 1 : ordre dynamique (MRV) ; 0 : ordre de parcours fixe `directions[]`. */
    int dynamic;
    /** 1 : ajoute le balayage global de case morte à l'ordre fixe (sans effet si `dynamic`). */
    int global_check;
    /** 1 : arme le conflit de singletons dans bt_forward_check (§4.4, les deux moteurs). */
    int singleton_check;
} engine_t;

/** @brief Résultat d'une tentative de fermeture. */
typedef struct {
    bt_core_result_t status;
    unsigned long long nodes;
    double seconds;
} closure_t;

/**
 * @brief Tente de fermer le sous-arbre de `root` avec un moteur donné.
 *
 * `allow_delegate = 0` dans tous les cas : céder une partie du sous-arbre
 * romprait la preuve elle-même (cf. la doc de `search_packet_backtracking_core`).
 *
 * @param root   Racine (non modifiée).
 * @param eng    Variante de moteur.
 * @param budget Plafond de nœuds.
 */
static closure_t close_subtree(const struct possibility_packet *root, const engine_t *eng, long budget)
{
    closure_t out;
    struct possibility_packet work;
    memcpy(&work, root, sizeof(work));

    counters[0] = 0;
    max_result = 0;
    request = REQUEST_CONTINUE;
    global_dead_check = eng->global_check;
    singleton_conflict_check = eng->singleton_check;

    unsigned long long nodes = 0;
    double t0 = now_seconds();
    out.status = eng->dynamic
        ? search_packet_backtracking_mrv(&g_client, &work, g_idParts, budget, 0, &nodes)
        : search_packet_backtracking_core(&g_client, &work, g_idParts, budget, 0, &nodes);
    out.seconds = now_seconds() - t0;
    out.nodes = nodes;
    global_dead_check = 0;
    singleton_conflict_check = 0;
    return out;
}

static const char *status_label(bt_core_result_t r)
{
    switch (r) {
        case BT_CORE_EXHAUSTED: return "FERMÉ";
        case BT_CORE_BUDGET:    return "budget";
        case BT_CORE_STOPPED:   return "arrêt";
    }
    return "?";
}

/** @brief Nombre de cases remplies d'un plateau. */
static int placed_count(const struct possibility_packet *b)
{
    int n = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (b->grid[x][y] != -2) n++;
        }
    }
    return n;
}

/**
 * @brief Construit une racine en ne gardant, d'un plateau profond, que ses `k`
 *        premières cases remplies DANS L'ORDRE DE PARCOURS `directions[]`.
 *
 * Toute restriction d'un plateau cohérent reste cohérente (on ne fait que
 * retirer des contraintes), et prendre le préfixe du parcours donne exactement
 * la forme qu'un client à ordre fixe produirait — donc une racine qu'aucun des
 * moteurs n'avantage a priori. Ces racines-là sont, contrairement au stock
 * réel, des sous-arbres RÉELLEMENT VIVANTS : c'est le cas dur.
 */
static void build_prefix_root(const struct possibility_packet *deep, int k,
                              struct possibility_packet *out)
{
    make_empty_board(out);
    int kept = 0;
    for (int i = 0; i < ETERN_PARTS && kept < k; i++) {
        int16_t v = deep->grid[dirx[i]][diry[i]];
        if (v == -2) {
            continue;
        }
        out->grid[dirx[i]][diry[i]] = v;
        // La valeur de grille est un indice de rotation `id + ETERN_PARTS*r` :
        // l'identifiant se retrouve par le reste (cf. id_for_rotated_part).
        int id = v % ETERN_PARTS;
        if (id == 0) id = ETERN_PARTS;
        set_face_used(out->b_faceused, (uint16_t)(id - 1), 1);
        kept++;
    }
    bt_canonicalize_packet(out);
}

/* ==========================================================================
 * Sortie : un tableau par racine (colonnes = moteurs), puis un bilan.
 * ========================================================================== */

static void print_header(const engine_t *engines, int nb)
{
    printf("%-12s %6s", "racine", "pièces");
    for (int e = 0; e < nb; e++) {
        printf(" | %-11s %12s %9s", engines[e].name, "nœuds", "temps");
    }
    printf("\n");
    for (int i = 0; i < 19 + nb * 37; i++) putchar('-');
    printf("\n");
}

static void print_row(const char *label, int pieces, int nb, const closure_t *res)
{
    printf("%-12s %6d", label, pieces);
    for (int e = 0; e < nb; e++) {
        printf(" | %-11s %12llu %7.3f s",
               status_label(res[e].status), res[e].nodes, res[e].seconds);
    }
    printf("\n");
    fflush(stdout);
}

/** @brief Bilan agrégé d'un moteur sur un échantillon de racines. */
typedef struct {
    int closed;
    unsigned long long nodes;
    double seconds;
} tally_t;

static void print_tally(const engine_t *engines, int nb, const tally_t *t, int roots,
                        const tally_t *common, int nb_common)
{
    // Débit AGRÉGÉ (toutes racines confondues, fermées ou non) : c'est
    // l'équivalent, sur stock RÉEL, du nœuds/s de tests/bench/bench_search.sh
    // — la mesure qui répond à « ce mécanisme change-t-il l'efficacité de la
    // boucle chaude elle-même ? », distincte de la question de fermeture
    // bornée ci-dessous (§4.6b vs §4.4 : deux questions, deux instruments).
    printf("\n%-14s %8s %8s %14s %16s %16s\n",
           "moteur", "fermées", "sur", "temps total", "fermetures/s", "nœuds/s (total)");
    for (int e = 0; e < nb; e++) {
        printf("%-14s %8d %8d %12.3f s %16.2f %16.0f\n",
               engines[e].name, t[e].closed, roots, t[e].seconds,
               t[e].seconds > 0 ? (double)t[e].closed / t[e].seconds : 0.0,
               t[e].seconds > 0 ? (double)t[e].nodes / t[e].seconds : 0.0);
    }
    // Le tableau ci-dessus est biaisé par le plafond : un moteur qui renonce
    // vite dépense peu de temps sur les racines qu'il ne ferme pas, et son
    // ratio « fermetures/s » s'en trouve flatté. Le sous-ensemble fermé par
    // TOUS les moteurs est la seule comparaison appariée, sans plafond en jeu.
    printf("\ncomparaison appariée — les %d racines fermées par TOUS les moteurs :\n", nb_common);
    printf("%-14s %14s %14s\n", "moteur", "nœuds", "temps");
    for (int e = 0; e < nb; e++) {
        printf("%-14s %14llu %12.3f s\n", engines[e].name, common[e].nodes, common[e].seconds);
    }
}

#ifdef WITH_CUDA
/**
 * @brief Variante GPU de `--pruner-profile` : rejoue le contrôle superficiel
 *        du pruner GPU (`gpu_pruner_check_batch`, une seule passe — cf.
 *        `src/app/gpu_pruner.cu`) sur le même échantillon régulier d'un
 *        stock réel, PAR LOTS de `gpu_batch` (comme `autoprune_gpu` en
 *        production, jamais possibilité par possibilité — le débit mesuré
 *        n'aurait sinon aucun sens vis-à-vis de la taille réelle des
 *        lancements kernel).
 *
 * N'a PAS d'équivalent DFS borné : le GPU ne fait, en production, que le
 * contrôle superficiel (§4.6b documente pourquoi un DFS divergent par thread
 * convient mal au modèle SIMT — jamais tenté côté GPU). La comparaison
 * pertinente n'est donc pas GPU-vs-DFS mais GPU (une passe) vs CPU (point
 * fixe, `possibility_all_has_a_next_counted`) sur l'état de départ EXACT
 * (`orig[i]`, jamais muté) : c'est la divergence documentée mais jamais
 * chiffrée en §4.6a / docs/pruner_gpu_cuda.md.
 *
 * `checked` est forcé à 0 sur l'état de départ soumis aux deux côtés : le
 * contrôle CPU (`possibility_all_has_a_next_counted`) recalcule toujours
 * indépendamment de ce champ, alors que le kernel GPU court-circuite dessus
 * (`p->checked == 1` -> vivant sans recalcul, cf. `prune_kernel`). Sans ce
 * forçage la mesure confondrait la divergence "une passe vs point fixe" avec
 * celle, sans intérêt ici, du court-circuit `checked`.
 */
static void run_pruner_profile_gpu(const char *back, int pruner_profile, int gpu_batch,
                                    map_big_array *map, struct array_part *rot)
{
    FILE *f = fopen(back, "r");
    if (f == NULL) {
        fprintf(stderr, "ouverture de %s impossible\n", back);
        exit(EXIT_FAILURE);
    }
    struct possibility_packet pkt;
    long long total = 0;
    while (fread(&pkt, sizeof(pkt), 1, f) == 1) total++;
    long long stride = (total > pruner_profile) ? total / pruner_profile : 1;
    printf("stock : %lld possibilités\n", total);
    printf("profil GPU (contrôle une passe, lots de %d) sur %d possibilités"
           " échantillonnées 1 sur %lld :\n\n", gpu_batch, pruner_profile, stride);

    struct possibility_packet *orig = malloc(sizeof(*orig) * (size_t)pruner_profile);
    struct possibility_packet *work = malloc(sizeof(*work) * (size_t)pruner_profile);
    uint8_t *alive = malloc((size_t)pruner_profile);
    uint32_t *cells = malloc(sizeof(*cells) * (size_t)pruner_profile);
    if (orig == NULL || work == NULL || alive == NULL || cells == NULL) {
        fprintf(stderr, "allocation impossible\n");
        exit(EXIT_FAILURE);
    }

    rewind(f);
    long long index = 0;
    int sampled = 0;
    while (fread(&pkt, sizeof(pkt), 1, f) == 1) {
        long long i = index++;
        if (sampled >= pruner_profile) break;
        if (i % stride != 0) continue;
        pkt.checked = 0;
        orig[sampled] = pkt;
        work[sampled] = pkt;
        sampled++;
    }
    fclose(f);

    if (gpu_pruner_init(map, rot) != 0) {
        fprintf(stderr, "gpu_pruner_init a échoué (pas de GPU CUDA détecté ?)\n");
        exit(EXIT_FAILURE);
    }

    double t0 = now_seconds();
    for (int base = 0; base < sampled; base += gpu_batch) {
        int cnt = sampled - base;
        if (cnt > gpu_batch) cnt = gpu_batch;
        gpu_pruner_check_batch(work + base, cnt, alive + base, cells + base);
    }
    double gpu_seconds = now_seconds() - t0;

    long long gpu_dead = 0, gpu_alive = 0, gpu_solutions = 0;
    unsigned long long cells_total = 0;
    long long false_dead = 0, missed_cascade = 0;
    for (int i = 0; i < sampled; i++) {
        cells_total += cells[i];
        if (work[i].alloc >= ETERN_PARTS) {
            gpu_solutions++;
        } else if (alive[i]) {
            gpu_alive++;
        } else {
            gpu_dead++;
        }

        struct possibility_packet cpu_copy = orig[i];
        int cpu_alive = possibility_all_has_a_next_counted(&cpu_copy, map, rot, NULL);
        if (!alive[i] && cpu_alive) {
            false_dead++;
        } else if (alive[i] && !cpu_alive) {
            missed_cascade++;
        }
    }

    gpu_pruner_shutdown();
    free(orig);
    free(work);
    free(alive);
    free(cells);

    printf("%-28s %8lld  (%.1f %% de l'échantillon)\n", "mortes au contrôle GPU (1 passe) :",
           gpu_dead, sampled > 0 ? 100.0 * (double)gpu_dead / (double)sampled : 0.0);
    printf("%-28s %8lld  (%.1f %%)\n", "survivent (GPU, 1 passe) :",
           gpu_alive, sampled > 0 ? 100.0 * (double)gpu_alive / (double)sampled : 0.0);
    printf("%-28s %8lld  (%.1f %%)\n", "solutions rencontrées :",
           gpu_solutions, sampled > 0 ? 100.0 * (double)gpu_solutions / (double)sampled : 0.0);
    printf("\ndébit GPU : %.0f possibilités/s, %.2f cases examinées/possibilité"
           " (%.3f s total, %d possibilités, lot=%d)\n",
           gpu_seconds > 0 ? (double)sampled / gpu_seconds : 0.0,
           sampled > 0 ? (double)cells_total / (double)sampled : 0.0,
           gpu_seconds, sampled, gpu_batch);
    printf("\ndivergence vs CPU (point fixe) sur le MÊME état de départ (%d possibilités) :\n", sampled);
    printf("  GPU mort / CPU vivant (FAUX MORT, doit être nul)            : %lld\n", false_dead);
    printf("  GPU vivant / CPU mort (cascade en fin de balayage manquée,\n"
           "  attendue et documentée, §4.6a/docs/pruner_gpu_cuda.md)      : %lld\n", missed_cascade);

    if (false_dead > 0) {
        fprintf(stderr, "\nERREUR : %lld faux mort(s) détecté(s) — le GPU élimine une possibilité"
                " que le CPU juge vivante. Ce n'est alors plus une condition nécessaire"
                " (cf. §5 de docs/conception/elagage_recherche.md) : à corriger avant tout,"
                " jamais seulement documenter.\n", false_dead);
        exit(EXIT_FAILURE);
    }
}
#endif // WITH_CUDA

static void usage(void)
{
    printf("Usage : bench_refutation [options]\n"
           "  --pieces <f>       fichier de pièces (défaut data/pieces.csv)\n"
           "  --budget <n>       plafond de nœuds par tentative de fermeture (défaut 5000000)\n"
           "  --seed-nodes <n>   nœuds de la descente MRV produisant le plateau profond (défaut 2000000)\n"
           "  --depths a,b,c     profondeurs des racines fabriquées (défaut 150,165,175,180,185)\n"
           "  --from-back <f>    prend les racines dans un stock serveur (.back) au lieu de les fabriquer\n"
           "  --max-roots <n>    nombre de racines lues d'un .back (défaut 20)\n"
           "  --min-pieces <n>   ne retient d'un .back que les racines d'au moins n pièces posées\n"
           "  --max-pieces <n>   ... et d'au plus n pièces posées\n"
           "  --kpi <n>          mode KPI : échantillonne n racines RÉGULIÈREMENT réparties dans le\n"
           "                     .back (aucun filtre de profondeur — c'est ce que le serveur sert\n"
           "                     réellement), n'imprime que le bilan fermetures/seconde\n"
           "  --engines <liste>  moteurs à comparer parmi fixe,fixe+global,fixe+singleton,mrv (défaut : tous)\n"
           "  --pruner-profile <n> rejoue le VRAI pipeline du pruner (autoprune_step) sur n\n"
           "                     possibilités échantillonnées régulièrement dans le .back :\n"
           "                     part morte au contrôle superficiel seul, part fermée par la\n"
           "                     preuve DFS bornée (§4.6b, --budget) parmi le reste, part qui\n"
           "                     survit intacte — répond à §4.6b (le budget ferme-t-il ?) et à\n"
           "                     §4.9 (combien un pruner en service éliminerait-il déjà seul ?)\n"
           "  --gpu              (avec --pruner-profile, build CUDA uniquement) rejoue le\n"
           "                     contrôle GPU une passe (gpu_pruner_check_batch) au lieu du\n"
           "                     pipeline CPU superficiel+DFS ; mesure le taux d'élimination,\n"
           "                     le débit et la divergence vs le point fixe CPU (§4.6a)\n"
           "  --gpu-batch <n>    taille de lot soumise à gpu_pruner_check_batch (défaut 100,\n"
           "                     proche de PRUNER_BATCH_SIZE — sans effet sans --gpu)\n");
}

int main(int argc, char **argv)
{
    const char *pieces = "data/pieces.csv";
    const char *back = NULL;
    int min_pieces = 0, max_pieces = ETERN_PARTS;
    long budget = 5000000;
    long seed_nodes = 2000000;
    int max_roots = 20;
    int kpi = 0;
    int pruner_profile = 0;
    int gpu = 0;
    int gpu_batch = 100;
    int depths[MAX_DEPTHS] = {150, 165, 175, 180, 185};
    int nb_depths = 5;

    engine_t all_engines[4] = {
        { "fixe",           0, 0, 0 },
        { "fixe+global",    0, 1, 0 },
        { "fixe+singleton", 0, 0, 1 },
        { "MRV",            1, 0, 0 },
    };
    engine_t engines[4];
    int nb_engines = 4;
    memcpy(engines, all_engines, sizeof(all_engines));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pieces") == 0 && i + 1 < argc)           pieces = argv[++i];
        else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc)      budget = atol(argv[++i]);
        else if (strcmp(argv[i], "--seed-nodes") == 0 && i + 1 < argc)  seed_nodes = atol(argv[++i]);
        else if (strcmp(argv[i], "--from-back") == 0 && i + 1 < argc)   back = argv[++i];
        else if (strcmp(argv[i], "--max-roots") == 0 && i + 1 < argc)   max_roots = atoi(argv[++i]);
        else if (strcmp(argv[i], "--min-pieces") == 0 && i + 1 < argc)  min_pieces = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-pieces") == 0 && i + 1 < argc)  max_pieces = atoi(argv[++i]);
        else if (strcmp(argv[i], "--kpi") == 0 && i + 1 < argc)         kpi = atoi(argv[++i]);
        else if (strcmp(argv[i], "--pruner-profile") == 0 && i + 1 < argc) pruner_profile = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gpu") == 0)                          gpu = 1;
        else if (strcmp(argv[i], "--gpu-batch") == 0 && i + 1 < argc)    gpu_batch = atoi(argv[++i]);
        else if (strcmp(argv[i], "--engines") == 0 && i + 1 < argc) {
            nb_engines = 0;
            char *copy = strdup(argv[++i]);
            for (char *tok = strtok(copy, ","); tok != NULL; tok = strtok(NULL, ",")) {
                for (int e = 0; e < 4; e++) {
                    if (strcmp(tok, all_engines[e].name) == 0) {
                        engines[nb_engines++] = all_engines[e];
                    }
                }
            }
            free(copy);
            if (nb_engines == 0) { usage(); return EXIT_FAILURE; }
        }
        else if (strcmp(argv[i], "--depths") == 0 && i + 1 < argc) {
            nb_depths = 0;
            char *copy = strdup(argv[++i]);
            for (char *tok = strtok(copy, ","); tok != NULL && nb_depths < MAX_DEPTHS; tok = strtok(NULL, ",")) {
                depths[nb_depths++] = atoi(tok);
            }
            free(copy);
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }

#ifndef WITH_CUDA
    if (gpu) {
        fprintf(stderr, "--gpu exige un build CUDA (make bench-refutation-gpu) ; ce binaire"
                " a été compilé sans support CUDA (pas de repli silencieux vers le CPU).\n");
        return EXIT_FAILURE;
    }
#endif
    if (gpu_batch < 1) gpu_batch = 1;

    alloc_counters();
    fill_idparts();
    stop_on_solution = 0;
    // Aucun serveur : jamais de délégation ni d'envoi réseau depuis ce banc.
    set_server_ip(NULL);

    struct array_part *parts = read_parts((char *)pieces);
    if (parts == NULL) {
        fprintf(stderr, "lecture de %s impossible\n", pieces);
        return EXIT_FAILURE;
    }
    struct array_part *rot = rotate_all_parts(parts);
    map_big_array *map = buildBigArray(rot, search_max_face(rot));

    memset(&g_client, 0, sizeof(g_client));
    g_client.compteur = 0;
    g_client.all_rotate_part = rot;
    g_client.map_part = map;

    printf("\nbanc de réfutation : coût de la PREUVE qu'un sous-arbre est mort\n");
    printf("pièces : %s   plafond : %ld nœuds par racine et par moteur\n", pieces, budget);

    tally_t tally[4], common[4];
    memset(tally, 0, sizeof(tally));
    memset(common, 0, sizeof(common));
    int nb_common = 0;
    closure_t res[4];
    int roots_done = 0;

    if (pruner_profile > 0) {
        if (back == NULL) {
            fprintf(stderr, "--pruner-profile exige --from-back\n");
            return EXIT_FAILURE;
        }
#ifdef WITH_CUDA
        if (gpu) {
            run_pruner_profile_gpu(back, pruner_profile, gpu_batch, map, rot);
            free_bigarray(map);
            free_array_part(rot);
            return EXIT_SUCCESS;
        }
#endif
        FILE *f = fopen(back, "r");
        if (f == NULL) {
            fprintf(stderr, "ouverture de %s impossible\n", back);
            return EXIT_FAILURE;
        }
        struct possibility_packet pkt;
        long long total = 0;
        while (fread(&pkt, sizeof(pkt), 1, f) == 1) total++;
        long long stride = (total > pruner_profile) ? total / pruner_profile : 1;
        printf("stock : %lld possibilités\n", total);
        printf("profil du VRAI pipeline pruner (autoprune_step) sur %d possibilités"
               " échantillonnées 1 sur %lld, plafond DFS %ld nœuds :\n\n",
               pruner_profile, stride, budget);

        rewind(f);
        long long index = 0, sampled = 0;
        long long dead_superficial = 0, closed_by_dfs = 0, survives = 0, solutions = 0;
        unsigned long long dfs_nodes_total = 0;
        double dfs_seconds_total = 0.0;
        // Chronométrage du seul contrôle superficiel (possibility_all_has_a_next_counted),
        // symétrique de la mesure de débit GPU (run_pruner_profile_gpu ci-dessus) : sert
        // de terme de comparaison CPU séquentiel / GPU par lots, sans jamais inclure le
        // temps DFS (chronométré séparément, dfs_seconds_total) ni le coût fixe de
        // démarrage (lecture pièces.csv, construction de la map, double lecture du .back).
        unsigned long long superficial_cells_total = 0;
        double superficial_seconds_total = 0.0;
        while (fread(&pkt, sizeof(pkt), 1, f) == 1) {
            long long i = index++;
            if (sampled >= pruner_profile) break;
            if (i % stride != 0) continue;
            sampled++;

            struct possibility_packet work;
            memcpy(&work, &pkt, sizeof(work));
            unsigned int cells_studied = 0;
            // Même appel, mêmes arguments que autoprune_step (etii_search.c) :
            // c'est le contrôle superficiel réel du pruner, pas une simulation.
            double ts0 = now_seconds();
            int has_next = possibility_all_has_a_next_counted(&work, g_client.map_part,
                                                               g_client.all_rotate_part,
                                                               &cells_studied);
            superficial_seconds_total += now_seconds() - ts0;
            superficial_cells_total += cells_studied;
            if (work.alloc >= ETERN_PARTS) {
                solutions++;
                continue;
            }
            if (!work.checked && has_next && budget > 0) {
                unsigned long long dfs_nodes = 0;
                double t0 = now_seconds();
                bt_core_result_t dfs = search_packet_backtracking_budgeted(&g_client, &work,
                                                                           g_idParts, budget,
                                                                           &dfs_nodes);
                dfs_seconds_total += now_seconds() - t0;
                dfs_nodes_total += dfs_nodes;
                if (dfs == BT_CORE_EXHAUSTED) {
                    closed_by_dfs++;
                    continue;
                }
            }
            if (work.checked || has_next) {
                survives++;
            } else {
                dead_superficial++;
            }
        }
        fclose(f);

        long long eliminated = dead_superficial + closed_by_dfs;
        printf("%-28s %8lld  (%.1f %% de l'échantillon)\n", "mortes au contrôle superficiel :",
               dead_superficial, sampled > 0 ? 100.0 * (double)dead_superficial / (double)sampled : 0.0);
        printf("%-28s %8lld  (%.1f %%) — %llu nœuds DFS, %.3f s\n", "fermées par la preuve DFS :",
               closed_by_dfs, sampled > 0 ? 100.0 * (double)closed_by_dfs / (double)sampled : 0.0,
               dfs_nodes_total, dfs_seconds_total);
        printf("%-28s %8lld  (%.1f %%)\n", "solutions rencontrées :",
               solutions, sampled > 0 ? 100.0 * (double)solutions / (double)sampled : 0.0);
        printf("%-28s %8lld  (%.1f %%)\n", "survivent intactes :",
               survives, sampled > 0 ? 100.0 * (double)survives / (double)sampled : 0.0);
        printf("\n%-28s %8lld  (%.1f %% de l'échantillon éliminé, superficiel + DFS)\n",
               "total éliminé :", eliminated,
               sampled > 0 ? 100.0 * (double)eliminated / (double)sampled : 0.0);
        printf("\ndébit du contrôle superficiel CPU (séquentiel, hors DFS) : %.0f possibilités/s,"
               " %.2f cases examinées/possibilité (%.3f s total, %lld possibilités)\n",
               superficial_seconds_total > 0 ? (double)sampled / superficial_seconds_total : 0.0,
               sampled > 0 ? (double)superficial_cells_total / (double)sampled : 0.0,
               superficial_seconds_total, sampled);

        free_bigarray(map);
        free_array_part(rot);
        return EXIT_SUCCESS;
    }

    if (back != NULL) {
        FILE *f = fopen(back, "r");
        if (f == NULL) {
            fprintf(stderr, "ouverture de %s impossible\n", back);
            return EXIT_FAILURE;
        }
        // Première passe : profil du stock (et, en mode KPI, le pas
        // d'échantillonnage — il faut connaître le total pour répartir).
        struct possibility_packet pkt;
        long long total = 0, sum_pieces = 0, not_canonical = 0, inconsistent = 0;
        int min_seen = ETERN_PARTS, max_seen = 0;
        while (fread(&pkt, sizeof(pkt), 1, f) == 1) {
            int p = placed_count(&pkt);
            total++;
            sum_pieces += p;
            if (p < min_seen) min_seen = p;
            if (p > max_seen) max_seen = p;
            // Contrôle du stock lui-même : tout paquet servi par le serveur doit
            // être cohérent (`check_possibility`) et déjà canonique
            // (`normalize_possibility_packet` n'a rien à réparer). C'est la
            // vérification, sur données RÉELLES, de la re-canonisation des
            // paquets délégués en ordre dynamique (§4.7).
            if (check_possibility(&pkt, g_client.all_rotate_part) < 0) inconsistent++;
            if (normalize_possibility_packet(&pkt) != 0) not_canonical++;
        }
        printf("stock : %lld possibilités, pièces posées min/moy/max = %d / %.1f / %d\n",
               total, min_seen, total > 0 ? (double)sum_pieces / (double)total : 0.0, max_seen);
        printf("intégrité du stock : %lld incohérent(s), %lld non canonique(s)\n",
               inconsistent, not_canonical);
        long long stride = (kpi > 0 && total > kpi) ? total / kpi : 1;
        if (kpi > 0) {
            printf("mode KPI : %d racines échantillonnées 1 sur %lld, aucun filtre de profondeur\n",
                   kpi, stride);
        }
        printf("\n");
        if (!kpi) {
            print_header(engines, nb_engines);
        }

        rewind(f);
        long long index = 0;
        while (fread(&pkt, sizeof(pkt), 1, f) == 1) {
            int p = placed_count(&pkt);
            long long i = index++;
            if (roots_done >= (kpi > 0 ? kpi : max_roots)) break;
            if (kpi > 0) {
                if (i % stride != 0) continue;
            } else if (p < min_pieces || p > max_pieces) {
                continue;
            }
            normalize_possibility_packet(&pkt);
            for (int e = 0; e < nb_engines; e++) {
                res[e] = close_subtree(&pkt, &engines[e], budget);
                tally[e].closed += (res[e].status == BT_CORE_EXHAUSTED);
                tally[e].nodes += res[e].nodes;
                tally[e].seconds += res[e].seconds;
            }
            int all_closed = 1;
            for (int e = 0; e < nb_engines; e++) {
                all_closed &= (res[e].status == BT_CORE_EXHAUSTED);
            }
            if (all_closed) {
                nb_common++;
                for (int e = 0; e < nb_engines; e++) {
                    common[e].nodes += res[e].nodes;
                    common[e].seconds += res[e].seconds;
                }
            }
            if (!kpi) {
                char label[32];
                snprintf(label, sizeof(label), "back#%d", roots_done);
                print_row(label, p, nb_engines, res);
            }
            roots_done++;
        }
        fclose(f);
    } else {
        struct possibility_packet root;
        make_empty_board(&root);
        best_board_init(&g_search_best_board);
        max_result = 0;
        request = REQUEST_CONTINUE;
        global_dead_check = 0;
        unsigned long long seeded = 0;
        search_packet_backtracking_mrv(&g_client, &root, g_idParts, seed_nodes, 0, &seeded);

        struct possibility_packet deep;
        uint16_t deep_alloc = 0;
        if (!best_board_get(&g_search_best_board, &deep, &deep_alloc)) {
            fprintf(stderr, "aucun plateau profond produit par la descente\n");
            return EXIT_FAILURE;
        }
        printf("descente MRV : %llu nœuds -> plateau profond de %d pièces posées\n\n",
               seeded, placed_count(&deep));
        print_header(engines, nb_engines);

        for (int i = 0; i < nb_depths; i++) {
            struct possibility_packet r;
            build_prefix_root(&deep, depths[i], &r);
            for (int e = 0; e < nb_engines; e++) {
                res[e] = close_subtree(&r, &engines[e], budget);
                tally[e].closed += (res[e].status == BT_CORE_EXHAUSTED);
                tally[e].nodes += res[e].nodes;
                tally[e].seconds += res[e].seconds;
            }
            int all_closed = 1;
            for (int e = 0; e < nb_engines; e++) {
                all_closed &= (res[e].status == BT_CORE_EXHAUSTED);
            }
            if (all_closed) {
                nb_common++;
                for (int e = 0; e < nb_engines; e++) {
                    common[e].nodes += res[e].nodes;
                    common[e].seconds += res[e].seconds;
                }
            }
            print_row("préfixe", placed_count(&r), nb_engines, res);
            roots_done++;
        }
    }

    print_tally(engines, nb_engines, tally, roots_done, common, nb_common);

    // Compteur global exact plutôt qu'une inférence par comptage de nœuds :
    // répond directement à « ce mécanisme s'est-il seulement déclenché ? »
    // (§4.4), la question que §4.4/§4.6b ont montré ne pas pouvoir se
    // trancher sur un protocole non représentatif.
    for (int e = 0; e < nb_engines; e++) {
        if (engines[e].singleton_check) {
            printf("\nfc_singleton_conflict (%s) : %llu déclenchement(s) sur cette exécution\n",
                   engines[e].name, (unsigned long long)fc_singleton_conflict);
            break;
        }
    }

    free_bigarray(map);
    free_array_part(rot);
    return EXIT_SUCCESS;
}
