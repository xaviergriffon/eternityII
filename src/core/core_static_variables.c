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

#else
uint8_t directions[ETERN_PARTS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

uint8_t dirx[ETERN_PARTS] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};

uint8_t diry[ETERN_PARTS] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};

#endif
#if FORWARD_CHECK_K > 0
volatile unsigned long long fc_pruned = 0;
volatile unsigned long long fc_attempts = 0;
volatile unsigned long long fc_pruned_at[FC_STAT_MAX_K + 1] = {0};
volatile unsigned long long fc_singleton_conflict = 0;
#endif // FORWARD_CHECK_K > 0

volatile unsigned long long fc_cells_studied = 0;

#ifdef ETII_STAT_CORNER_ZONES
volatile unsigned long long cz_surrounded_incomplete[CZ_CORNERS] = {0};
volatile unsigned long long cz_nodes = 0;
volatile unsigned long long cz_holes_hist[CZ_ZONE_CELLS + 1] = {0};
volatile unsigned long long cz_depth_sum = 0;
volatile unsigned long long cz_depth_min = 0;
volatile unsigned long long cz_depth_max = 0;
volatile unsigned long long cz_subtree_nodes = 0;
#endif

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
#else
char* parts_files = "./data/pieces16.csv";
#endif // ETERN_PARTS == 256

// Indices officiels du puzzle 256 pièces : lu par first_possibility (possibility.c)
// uniquement quand ETERN_PARTS == 256. Le chemin reste défini inconditionnellement
// pour rester surchargeable sans #if côté appelant.
char* indices_file = "./data/indices.csv";

unsigned long long non_null_possibilities = 0;

volatile int request = REQUEST_CONTINUE;

int max_stock_by_thread = MAX_STOCK_BY_THREAD;

int shallow_root_abandon_depth = SHALLOW_ROOT_ABANDON_DEPTH;

volatile unsigned long long shallow_root_abandoned = 0;

volatile int server_io_active = 0;

// Faim du serveur (réponse INST_NEED_WORK), accès via __atomic_* uniquement.
int server_hunger = 0;

int singleton_conflict_check = 0;
