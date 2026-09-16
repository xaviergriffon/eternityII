#include "core/core_static_variables.h"

#if ETERN_PARTS == 256

uint8_t directions[ETERN_PARTS] = {0, 1, 2, 18, 34, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    29, 45, 14, 15, 31, 47, 46, 30, 63, 79, 95, 111, 127, 143, 159, 175,
    191, 207, 223, 239, 255, 254, 253, 237, 221, 238, 222, 252, 251, 250, 249, 248,
    247, 246, 245, 244, 243, 242, 226, 210, 241, 240, 224, 208, 209, 225, 192, 176,
    160, 144, 128, 112, 96, 80, 64, 48, 32, 33, 16, 17, 193, 227, 228, 211,
    194, 177, 206, 236, 235, 220, 205, 190, 19, 49, 65, 50, 35, 20, 28, 62,
    27, 44, 61, 78, 21, 22, 23, 24, 25, 26, 81, 94, 97, 110, 113, 126,
    129, 142, 145, 158, 161, 174, 229, 230, 231, 232, 233, 234, 178, 195, 212, 213,
    196, 179, 162, 146, 163, 180, 197, 214, 215, 198, 181, 164, 147, 130, 131, 148,
    165, 182, 199, 183, 166, 149, 132, 133, 150, 167, 151, 134, 135, 189, 204, 219,
    218, 203, 188, 173, 157, 172, 187, 202, 217, 216, 201, 200, 186, 171, 156, 141,
    140, 155, 170, 185, 184, 169, 168, 154, 139, 138, 153, 152, 137, 136, 36, 51,
    66, 82, 67, 52, 37, 38, 53, 68, 83, 98, 114, 99, 115, 84, 69, 54,
    39, 55, 70, 85, 100, 116, 101, 117, 86, 71, 87, 102, 118, 103, 119, 125,
    109, 124, 123, 108, 93, 77, 92, 107, 122, 121, 120, 106, 91, 76, 60, 75,
    90, 105, 104, 89, 88, 74, 59, 43, 58, 42, 73, 72, 57, 41, 56, 40};

uint8_t dirx[ETERN_PARTS] = {0, 1, 2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 13, 13, 14, 15, 15, 15, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 14, 13, 13, 13, 14, 14, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 2, 2, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 3, 4, 3, 2, 1, 14, 12, 11, 12, 13, 14, 3, 1, 1, 2, 3, 4, 12, 14, 11, 12, 13, 14, 5, 6, 7, 8, 9, 10, 1, 14, 1, 14, 1, 14, 1, 14, 1, 14, 1, 14, 5, 6, 7, 8, 9, 10, 2, 3, 4, 5, 4, 3, 2, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 5, 6, 7, 7, 6, 7, 13, 12, 11, 10, 11, 12, 13, 13, 12, 11, 10, 9, 8, 9, 8, 10, 11, 12, 13, 12, 11, 10, 9, 8, 9, 8, 10, 11, 10, 9, 8, 9, 8, 4, 3, 2, 2, 3, 4, 5, 6, 5, 4, 3, 2, 2, 3, 3, 4, 5, 6, 7, 7, 6, 5, 4, 4, 5, 5, 6, 7, 7, 6, 6, 7, 7, 13, 13, 12, 11, 12, 13, 13, 12, 11, 10, 9, 8, 10, 11, 12, 12, 11, 10, 9, 8, 9, 8, 10, 11, 11, 10, 10, 9, 8, 9, 9, 8, 8};

uint8_t diry[ETERN_PARTS] = {0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 2, 2, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 15, 14, 13, 14, 13, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 14, 13, 15, 15, 14, 13, 13, 14, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 2, 1, 1, 12, 14, 14, 13, 12, 11, 12, 14, 14, 13, 12, 11, 1, 3, 4, 3, 2, 1, 1, 3, 1, 2, 3, 4, 1, 1, 1, 1, 1, 1, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 14, 14, 14, 14, 14, 14, 11, 12, 13, 13, 12, 11, 10, 9, 10, 11, 12, 13, 13, 12, 11, 10, 9, 8, 8, 9, 10, 11, 12, 11, 10, 9, 8, 8, 9, 10, 9, 8, 8, 11, 12, 13, 13, 12, 11, 10, 9, 10, 11, 12, 13, 13, 12, 12, 11, 10, 9, 8, 8, 9, 10, 11, 11, 10, 10, 9, 8, 8, 9, 9, 8, 8, 2, 3, 4, 5, 4, 3, 2, 2, 3, 4, 5, 6, 7, 6, 7, 5, 4, 3, 2, 3, 4, 5, 6, 7, 6, 7, 5, 4, 5, 6, 7, 6, 7, 7, 6, 7, 7, 6, 5, 4, 5, 6, 7, 7, 7, 6, 5, 4, 3, 4, 5, 6, 6, 5, 5, 4, 3, 2, 3, 2, 4, 4, 3, 2, 3, 2};

#elif ETERN_PARTS == 16
uint8_t directions[ETERN_PARTS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

uint8_t dirx[ETERN_PARTS] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};

uint8_t diry[ETERN_PARTS] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};

#else

// Tailles de CLONE (64, 100, 144, 196 : cf. ETERN_SIZE dans le .h). Le
// parcours n'est plus qu'un ordre d'énumération depuis VERSION 13 (MRV est le
// seul moteur, il choisit sa case lui-même) : une énumération colonne par
// colonne suffit, exactement celle que le 4×4 ci-dessus écrit à la main.
// La SEULE obligation de `directions[]` depuis VERSION 13 est d'être une
// permutation de 0..ETERN_PARTS-1 (`test_directions`) : plus aucun code n'en
// redéduit de coordonnées. Les deux tables historiques ne l'encodent d'ailleurs
// pas de la même façon (le 16×16 pose `diry*ETERN_SIZE + dirx`, le 4×4
// l'inverse) — ne pas tenter de rétablir une convention commune, elle n'aurait
// aucun lecteur. Ce qui compte, et ce qu'un test verrouille, c'est que
// `(dirx[i], diry[i])` visite chaque case exactement une fois.
//
// Pourquoi un constructeur plutôt qu'un initialiseur littéral : ces tables ne
// sont pas `const` et C n'offre aucun moyen de les CALCULER à la compilation
// (il faudrait 196 valeurs écrites à la main par taille, la faute d'index
// n'étant alors visible qu'à l'exécution). `__attribute__((constructor))`
// s'exécute avant `main` dans tout binaire qui lie ce module — production,
// suites de tests et bancs — donc aucun appelant n'a à s'en souvenir. Les
// tailles 256 et 16 gardent leurs tables littérales : le parcours 16×16 est
// un ordre CHOISI (v11), pas une énumération.
uint8_t directions[ETERN_PARTS];
uint8_t dirx[ETERN_PARTS];
uint8_t diry[ETERN_PARTS];

__attribute__((constructor))
static void core_geometry_init(void)
{
    for (int i = 0; i < ETERN_PARTS; i++) {
        directions[i] = (uint8_t)i;
        dirx[i] = (uint8_t)(i / ETERN_SIZE);
        diry[i] = (uint8_t)(i % ETERN_SIZE);
    }
}

#endif
#if FORWARD_CHECK_K > 0
volatile unsigned long long fc_pruned = 0;
volatile unsigned long long fc_attempts = 0;
volatile unsigned long long fc_pruned_at[FC_STAT_MAX_K + 1] = {0};
volatile unsigned long long fc_singleton_conflict = 0;
#endif // FORWARD_CHECK_K > 0

volatile unsigned long long fc_cells_studied = 0;

int stop_on_solution = 0;

int pruner_batch_size = PRUNER_BATCH_SIZE;

int pruner_dfs_budget = PRUNER_DFS_BUDGET_DEFAULT;

volatile unsigned long long pruner_checked = 0;

volatile unsigned long long pruner_removed = 0;

volatile unsigned long long pruner_cells_studied = 0;

volatile unsigned long long pruner_dfs_closed = 0;

volatile unsigned long long pruner_dfs_nodes = 0;

unsigned long long *counters = NULL;

unsigned long long *lastfilesize = NULL;

int *lastroot = NULL;

int *lastdepth = NULL;

volatile uint16_t max_result = 0;

/**
 * @brief Voir la doc dans core_static_variables.h.
 */
useconds_t request_is_pause(int r) {
    if (r == REQUEST_PAUSE) return PAUSE_POLL_SLEEP_US;
    if (r == REQUEST_ADMIN_PAUSE) return ADMIN_PAUSE_POLL_SLEEP_US;
    return 0;
}

/**
 * @brief Voir la doc dans core_static_variables.h.
 */
int request_keeps_running(int r) {
    return r != REQUEST_STOP;
}

// TODO : deplacer dans un parametre ?
#if ETERN_PARTS == 256
char* parts_files = "./data/pieces.csv";
#elif ETERN_PARTS == 16
char* parts_files = "./data/pieces16.csv";
#else
// Tailles de clone : aucun jeu de pièces n'est livré dans le dépôt (il est
// TIRÉ, cf. tools/gen_clone.py). Ce défaut n'est qu'une convention de nommage
// — un clone est toujours désigné explicitement (argument positionnel du CLI,
// ou --pieces côté banc).
char* parts_files = "./data/pieces" ETII_STRINGIFY(ETERN_PARTS) ".csv";
#endif // ETERN_PARTS

// Indices du puzzle, lus par first_possibility (possibility.c) quand ce
// pointeur est non NULL. NULL = instance sans indice : c'est le cas du 4×4 de
// test (aucun indice n'a jamais été posé pour cette taille) et le défaut des
// clones, qui apportent le leur via --indices-file quand ils en ont un.
#if ETERN_PARTS == 256
char* indices_file = "./data/indices.csv";
#else
char* indices_file = NULL;
#endif // ETERN_PARTS == 256

unsigned long long non_null_possibilities = 0;

volatile int request = REQUEST_CONTINUE;

int max_stock_by_thread = MAX_STOCK_BY_THREAD;

int shallow_root_abandon_depth = SHALLOW_ROOT_ABANDON_DEPTH;

volatile unsigned long long shallow_root_abandoned = 0;

volatile int server_io_active = 0;

// Faim du serveur (réponse INST_NEED_WORK), accès via __atomic_* uniquement.
int server_hunger = 0;

int singleton_conflict_check = 0;
