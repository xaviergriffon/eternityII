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

#define MAX_DEPTHS 32
#define MAX_BACK_ROOTS 4096

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

/** @brief Résultat d'une tentative de fermeture. */
typedef struct {
    bt_core_result_t status;
    unsigned long long nodes;
    double seconds;
    uint16_t max_result_reached;
} closure_t;

/**
 * @brief Tente de fermer le sous-arbre de `root` avec un moteur donné.
 *
 * `allow_delegate = 0` dans les deux cas : céder une partie du sous-arbre
 * romprait la preuve elle-même (cf. la doc de `search_packet_backtracking_core`).
 *
 * @param root    Racine (non modifiée).
 * @param dynamic 1 : moteur MRV ; 0 : moteur à ordre fixe.
 * @param budget  Plafond de nœuds.
 */
static closure_t close_subtree(struct possibility_packet *root, int dynamic, long budget)
{
    closure_t out;
    struct possibility_packet work;
    memcpy(&work, root, sizeof(work));

    counters[0] = 0;
    max_result = 0;
    request = REQUEST_CONTINUE;

    unsigned long long nodes = 0;
    double t0 = now_seconds();
    out.status = dynamic
        ? search_packet_backtracking_mrv(&g_client, &work, g_idParts, budget, 0, &nodes)
        : search_packet_backtracking_core(&g_client, &work, g_idParts, budget, 0, &nodes);
    out.seconds = now_seconds() - t0;
    out.nodes = nodes;
    out.max_result_reached = max_result;
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
 * deux moteurs n'avantage a priori.
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

static void print_row(const char *label, int depth, closure_t fixed, closure_t mrv)
{
    printf("%-14s %5d | %-6s %12llu %9.3f s | %-6s %12llu %9.3f s",
           label, depth,
           status_label(fixed.status), fixed.nodes, fixed.seconds,
           status_label(mrv.status), mrv.nodes, mrv.seconds);
    if (fixed.status == BT_CORE_EXHAUSTED && mrv.status == BT_CORE_EXHAUSTED) {
        printf(" | ×%.2f nœuds  ×%.2f temps",
               mrv.nodes > 0 ? (double)fixed.nodes / (double)mrv.nodes : 0.0,
               mrv.seconds > 0 ? fixed.seconds / mrv.seconds : 0.0);
    }
    printf("\n");
    fflush(stdout);
}

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
           "  --max-pieces <n>   ... et d'au plus n pièces posées\n");
}

int main(int argc, char **argv)
{
    const char *pieces = "data/pieces.csv";
    const char *back = NULL;
    int min_pieces = 0, max_pieces = ETERN_PARTS;
    long budget = 5000000;
    long seed_nodes = 2000000;
    int max_roots = 20;
    int depths[MAX_DEPTHS] = {150, 165, 175, 180, 185};
    int nb_depths = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pieces") == 0 && i + 1 < argc)          pieces = argv[++i];
        else if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc)     budget = atol(argv[++i]);
        else if (strcmp(argv[i], "--seed-nodes") == 0 && i + 1 < argc) seed_nodes = atol(argv[++i]);
        else if (strcmp(argv[i], "--from-back") == 0 && i + 1 < argc)  back = argv[++i];
        else if (strcmp(argv[i], "--max-roots") == 0 && i + 1 < argc)  max_roots = atoi(argv[++i]);
        else if (strcmp(argv[i], "--min-pieces") == 0 && i + 1 < argc)  min_pieces = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-pieces") == 0 && i + 1 < argc)  max_pieces = atoi(argv[++i]);
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

    printf("banc de réfutation : coût de la PREUVE qu'un sous-arbre est mort\n");
    printf("pièces : %s   plafond : %ld nœuds par tentative\n\n", pieces, budget);
    printf("%-14s %5s | %-6s %12s %11s | %-6s %12s %11s\n",
           "racine", "pièces", "fixe", "nœuds", "temps", "MRV", "nœuds", "temps");
    printf("---------------------------------------------------------------------------------------\n");

    if (back != NULL) {
        // Racines réelles : le stock d'un serveur, tel qu'il est sauvegardé.
        FILE *f = fopen(back, "r");
        if (f == NULL) {
            fprintf(stderr, "ouverture de %s impossible\n", back);
            return EXIT_FAILURE;
        }
        struct possibility_packet pkt;
        int n = 0, closed_fixed = 0, closed_mrv = 0, both_closed = 0;
        unsigned long long both_nodes_fixed = 0, both_nodes_mrv = 0;
        double both_sec_fixed = 0.0, both_sec_mrv = 0.0;
        int min_alloc = ETERN_PARTS, max_alloc = 0;
        long long sum_alloc = 0, total = 0;
        while (fread(&pkt, sizeof(pkt), 1, f) == 1) {
            total++;
            int p = placed_count(&pkt);
            if (p < min_alloc) min_alloc = p;
            if (p > max_alloc) max_alloc = p;
            sum_alloc += p;
            if (n >= max_roots || p < min_pieces || p > max_pieces) {
                continue; // on continue à lire pour le profil de profondeur
            }
            normalize_possibility_packet(&pkt);
            closure_t fx = close_subtree(&pkt, 0, budget);
            closure_t mv = close_subtree(&pkt, 1, budget);
            closed_fixed += (fx.status == BT_CORE_EXHAUSTED);
            closed_mrv += (mv.status == BT_CORE_EXHAUSTED);
            if (fx.status == BT_CORE_EXHAUSTED && mv.status == BT_CORE_EXHAUSTED) {
                both_closed++;
                both_nodes_fixed += fx.nodes; both_nodes_mrv += mv.nodes;
                both_sec_fixed += fx.seconds; both_sec_mrv += mv.seconds;
            }
            char label[32];
            snprintf(label, sizeof(label), "back#%d", n);
            print_row(label, p, fx, mv);
            n++;
        }
        fclose(f);
        printf("\nstock lu : %lld possibilités, pièces posées min/moy/max = %d / %.1f / %d\n",
               total, min_alloc, total > 0 ? (double)sum_alloc / (double)total : 0.0, max_alloc);
        printf("fermetures dans le plafond : ordre fixe %d/%d, MRV %d/%d\n",
               closed_fixed, n, closed_mrv, n);
        if (both_closed > 0) {
            printf("racines fermées par les DEUX (%d) : nœuds fixe %llu vs MRV %llu (×%.2f),"
                   " temps fixe %.3f s vs MRV %.3f s (×%.2f)\n",
                   both_closed, both_nodes_fixed, both_nodes_mrv,
                   both_nodes_mrv > 0 ? (double)both_nodes_fixed / (double)both_nodes_mrv : 0.0,
                   both_sec_fixed, both_sec_mrv,
                   both_sec_mrv > 0 ? both_sec_fixed / both_sec_mrv : 0.0);
        }
    } else {
        // Racines fabriquées : préfixes d'une descente MRV profonde.
        struct possibility_packet root;
        make_empty_board(&root);
        best_board_init(&g_search_best_board);
        max_result = 0;
        request = REQUEST_CONTINUE;
        unsigned long long seeded = 0;
        search_packet_backtracking_mrv(&g_client, &root, g_idParts, seed_nodes, 0, &seeded);

        struct possibility_packet deep;
        uint16_t deep_alloc = 0;
        if (!best_board_get(&g_search_best_board, &deep, &deep_alloc)) {
            fprintf(stderr, "aucun plateau profond produit par la descente\n");
            return EXIT_FAILURE;
        }
        printf("# descente MRV : %llu nœuds -> plateau profond de %d pièces posées\n\n",
               seeded, placed_count(&deep));

        for (int i = 0; i < nb_depths; i++) {
            struct possibility_packet r;
            build_prefix_root(&deep, depths[i], &r);
            closure_t fx = close_subtree(&r, 0, budget);
            closure_t mv = close_subtree(&r, 1, budget);
            print_row("préfixe", placed_count(&r), fx, mv);
        }
    }

    free_bigarray(map);
    free_array_part(rot);
    return EXIT_SUCCESS;
}
