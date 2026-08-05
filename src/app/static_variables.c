#include "app/static_variables.h"
#include <stdlib.h>
#include <string.h>

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
volatile unsigned long long fc_pruned_at[FORWARD_CHECK_K + 1] = {0};
#endif // FORWARD_CHECK_K > 0

int pruner_mode = 0;

int stop_on_solution = 0;

int gpu_requested = 0;

int help_requested = 0;

int expand_min_level = 0;

int headless_mode = 0;

int pruner_batch_size = PRUNER_BATCH_SIZE;

#ifdef WITH_CUDA
int gpu_pruner_mode = 0;
#endif // WITH_CUDA

volatile unsigned long long pruner_checked = 0;

volatile unsigned long long pruner_removed = 0;

volatile unsigned long long pruner_cells_studied = 0;

volatile unsigned long long fc_cells_studied = 0;

unsigned long long *counters = NULL;

struct client_statistics *fork_statistics = NULL;

unsigned long long *lastfilesize = NULL;
char *lastcheck = NULL;
pthread_mutex_t lastcheck_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Voir la doc dans static_variables.h.
 *
 * Section critique volontairement réduite au strict échange de pointeur : la
 * construction du rapport (potentiellement coûteuse : plusieurs strcat/sprintf)
 * a déjà eu lieu dans le buffer local de l'appelant, hors du verrou.
 */
void lastcheck_publish(char *new_report) {
    pthread_mutex_lock(&lastcheck_mutex);
    free(lastcheck);
    lastcheck = new_report;
    pthread_mutex_unlock(&lastcheck_mutex);
}

/**
 * @brief Voir la doc dans static_variables.h.
 */
useconds_t request_is_pause(int r) {
    if (r == REQUEST_PAUSE) return PAUSE_POLL_SLEEP_US;
    if (r == REQUEST_ADMIN_PAUSE) return ADMIN_PAUSE_POLL_SLEEP_US;
    return 0;
}

/**
 * @brief Voir la doc dans static_variables.h.
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

unsigned long long non_null_possibilities = 0;
volatile uint16_t max_result = 0;

volatile int request = REQUEST_CONTINUE;

long inst_unknow = 0;

int NB_THREADS = 10;

int version = VERSION;

int parent_pid = -1;

pid_t *childrens_pid = NULL;

struct sockaddr_un *main_addr = NULL;

int *main_socket_id = NULL;

char **forkId = NULL;

int SERVER_PORT = 2020;

int HTTP_PORT = 0;

const char *HTTP_TOKEN_FILE = NULL;

char HTTP_ADMIN_TOKEN[HTTP_ADMIN_TOKEN_MAX] = "";

volatile unsigned long long server_shots_per_second = 0;

unsigned long long max_search_by_sec = 0;

int max_stock_by_thread = MAX_STOCK_BY_THREAD;

// Faim du serveur (réponse INST_NEED_WORK), accès via __atomic_* uniquement.
int server_hunger = 0;

int communication_in_progress = 0;

#ifdef DEBUG_SOCKET
int opened_tcp = 0;
#endif // DEBUG_SOCKET

int tcp_timeout = DEFAULT_TCP_TIMEOUT;

int fork_checker_socket_id = -1;

int server = 0;

int server_rmnonext_timing = 30;

unsigned long long bench_target_nodes = 0;

unsigned long long bench_parse_nodes_env(const char *env_value)
{
    if (env_value == NULL || env_value[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    unsigned long long value = strtoull(env_value, &end, 10);
    if (end == env_value) {
        return 0; // pas un nombre
    }
    return value;
}

int bench_should_stop(unsigned long long target_nodes, unsigned long long nodes_done)
{
    return target_nodes > 0 && nodes_done >= target_nodes;
}

int parse_cli_options(int argc, const char *argv[])
{
    int w = 0;
    for (int r = 0; r < argc; r++) {
        if (strcmp(argv[r], "--stop-on-solution") == 0) {
            stop_on_solution = 1;
        } else if (strcmp(argv[r], "--gpu") == 0) {
            // Exécution GPU du pruner : le flag est interprété par le mode
            // `pruner` dans main() (erreur explicite sur un build sans CUDA).
            gpu_requested = 1;
        } else if (strcmp(argv[r], "--headless") == 0) {
            // Exécution sans console interactive : le flag est lu dans main()
            // avant chaque appel à run_console() (serveur, client, mode test).
            headless_mode = 1;
        } else if (strcmp(argv[r], "--help") == 0 || strcmp(argv[r], "-h") == 0) {
            // Aide CLI : le flag est lu dans main() avant le dispatch des modes
            // (affichage de l'aide générale puis EXIT_SUCCESS).
            help_requested = 1;
        } else if (strcmp(argv[r], "--expand-level") == 0) {
            // Option valuée : le niveau suit dans l'argument suivant. Les deux
            // tokens sont retirés d'argv (non recopiés) pour ne pas perturber le
            // parsing positionnel des modes. Valeur absente ou non numérique
            // (atoi → 0) : option ignorée, expand_min_level reste à 0.
            if (r + 1 < argc) {
                expand_min_level = atoi(argv[r + 1]);
                if (expand_min_level < 0) {
                    expand_min_level = 0;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--http-port") == 0) {
            // Option valuée, même schéma que --expand-level. Valeur absente,
            // non numérique ou hors [1, 65535] : option ignorée, HTTP_PORT
            // reste à 0 (API désactivée) plutôt que d'ouvrir un port au hasard.
            if (r + 1 < argc) {
                int port = atoi(argv[r + 1]);
                if (port >= 1 && port <= 65535) {
                    HTTP_PORT = port;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--http-token-file") == 0) {
            // Option valuée, même schéma que --http-port. Le chemin est stocké
            // tel quel (pointeur dans argv, jamais copié — même convention que
            // parts_files) ; le chargement effectif (lecture + vérification des
            // permissions) se fait plus tard dans main(), via http_token_load
            // (src/net/http_server.h), pour rester cohérent avec le style
            // "parse_cli_options ne fait que retirer/mémoriser les options" de
            // cette fonction (aucune I/O ici).
            if (r + 1 < argc) {
                HTTP_TOKEN_FILE = argv[r + 1];
                r++; // consomme aussi la valeur
            }
        } else {
            argv[w++] = argv[r];
        }
    }
    return w;
}
