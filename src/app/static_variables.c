#include "app/static_variables.h"
#include <stdlib.h>
#include <string.h>

#if ETERN_PARTS == 256

// Parcours issu d'un glouton statique piloté par un score de risque (position
// pondérée par 1/taille-du-pool-restant au moment de la pose, cf.
// docs/conception/elagage_recherche.md §3.2) : à chaque étape, la case dont le
// pool de candidats restants est le plus petit est posée. Mesuré −1,3 % de
// score de risque par rapport à l'ordre précédent (bord/intérieur entrelacé)
// sur data/pieces.csv, comparable au meilleur autre candidat testé (coins
// immédiats en premier, −1,4 %). Structure résultante : le cadre (60 cases)
// est posé en un seul passage rapide plutôt qu'entrelacé avec l'intérieur,
// suivi d'un balayage systématique de l'intérieur.
uint8_t directions[ETERN_PARTS] = {
    135, 34, 45, 210, 221, 0, 240, 15, 255, 16, 32, 48, 64, 80, 96, 112,
    128, 144, 160, 176, 192, 208, 224, 1, 241, 2, 242, 3, 243, 4, 244, 5,
    245, 6, 246, 7, 247, 8, 248, 9, 249, 10, 250, 11, 251, 12, 252, 13,
    14, 253, 254, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 207, 223,
    239, 237, 238, 222, 226, 225, 209, 17, 33, 18, 49, 65, 81, 97, 113, 129,
    145, 161, 177, 193, 19, 35, 227, 211, 20, 36, 228, 212, 21, 37, 229, 213,
    22, 38, 230, 214, 23, 39, 231, 215, 24, 40, 232, 216, 25, 41, 233, 217,
    26, 42, 234, 218, 27, 43, 235, 236, 219, 220, 28, 44, 29, 30, 46, 62,
    78, 94, 110, 126, 142, 158, 174, 190, 206, 205, 204, 203, 202, 201, 200, 199,
    198, 197, 196, 195, 194, 178, 162, 146, 130, 114, 98, 82, 66, 50, 51, 67,
    83, 99, 115, 131, 147, 163, 179, 52, 68, 84, 100, 116, 132, 148, 164, 180,
    53, 69, 85, 101, 117, 133, 149, 165, 181, 54, 70, 86, 102, 118, 134, 119,
    150, 151, 166, 182, 55, 71, 87, 103, 167, 183, 56, 72, 88, 104, 120, 136,
    152, 168, 184, 57, 73, 89, 105, 121, 137, 153, 169, 185, 58, 74, 90, 106,
    122, 138, 154, 170, 186, 59, 75, 91, 107, 123, 139, 155, 171, 187, 60, 76,
    92, 108, 124, 140, 156, 172, 188, 61, 77, 93, 109, 125, 141, 157, 173, 189};

uint8_t dirx[ETERN_PARTS] = {
    7, 2, 13, 2, 13, 0, 0, 15, 15, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,
    5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    14, 13, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 13, 14, 14, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
    6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
    10, 10, 10, 10, 11, 11, 11, 12, 11, 12, 12, 12, 13, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 11, 10, 9, 8, 7,
    6, 5, 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 7,
    6, 7, 6, 6, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13};

uint8_t diry[ETERN_PARTS] = {
    8, 2, 2, 13, 13, 0, 15, 0, 15, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 0, 15, 0, 15, 0, 15, 0, 15, 0,
    15, 0, 15, 0, 15, 0, 15, 0, 15, 0, 15, 0, 15, 0, 15, 0,
    0, 15, 15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 14, 14, 13, 14, 14, 13, 1, 2, 1, 3, 4, 5, 6, 7, 8,
    9, 10, 11, 12, 1, 2, 14, 13, 1, 2, 14, 13, 1, 2, 14, 13,
    1, 2, 14, 13, 1, 2, 14, 13, 1, 2, 14, 13, 1, 2, 14, 13,
    1, 2, 14, 13, 1, 2, 14, 14, 13, 13, 1, 2, 1, 1, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 3, 4,
    5, 6, 7, 8, 9, 10, 11, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    3, 4, 5, 6, 7, 8, 9, 10, 11, 3, 4, 5, 6, 7, 8, 7,
    9, 9, 10, 11, 3, 4, 5, 6, 10, 11, 3, 4, 5, 6, 7, 8,
    9, 10, 11, 3, 4, 5, 6, 7, 8, 9, 10, 11, 3, 4, 5, 6,
    7, 8, 9, 10, 11, 3, 4, 5, 6, 7, 8, 9, 10, 11, 3, 4,
    5, 6, 7, 8, 9, 10, 11, 3, 4, 5, 6, 7, 8, 9, 10, 11};

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

int pruner_mode = 0;

int stop_on_solution = 0;

int gpu_requested = 0;

int help_requested = 0;

int expand_min_level = 0;
int expand_max_stock = EXPAND_MAX_STOCK;
int expand_max_levels = EXPAND_MAX_LEVELS;
int rebalance_budget = REBALANCE_BUDGET_DEFAULT;
int stock_files_requested = 0;
int stock_max_ram_mb = 0;
const char *stock_spill_dir = "./eternityii-spill";

int headless_mode = 0;

int pruner_batch_size = PRUNER_BATCH_SIZE;

int pruner_dfs_budget = PRUNER_DFS_BUDGET_DEFAULT;

int analysed_lease_seconds = ANALYSED_LEASE_DEFAULT_SECONDS;

#ifdef WITH_CUDA
int gpu_pruner_mode = 0;
#endif // WITH_CUDA

volatile unsigned long long pruner_checked = 0;

volatile unsigned long long pruner_removed = 0;

volatile unsigned long long pruner_cells_studied = 0;

volatile unsigned long long pruner_dfs_closed = 0;

volatile unsigned long long pruner_dfs_nodes = 0;

volatile unsigned long long fc_cells_studied = 0;

unsigned long long *counters = NULL;

struct client_statistics *fork_statistics = NULL;

time_t *fork_last_activity = NULL;

volatile int server_io_active = 0;

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
int g_active_forks = 0;

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

const char *client_label = NULL;

const char *machine_uid_file_path = "./eternityii-machine_uid";

const char *client_config_file_path = "./eternityii-client.conf";

client_identity_t g_client_identity_template;

volatile unsigned long long server_shots_per_second = 0;
volatile unsigned long long server_last_backup_duration_ms = 0;

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

int mrv_enabled = MRV_DEFAULT_ENABLED;

int global_dead_check = 0;

int singleton_conflict_check = 0;

int mrv_parse_env(const char *env_value)
{
    if (env_value != NULL && strcmp(env_value, "0") == 0) {
        return 0;
    }
    if (env_value != NULL && strcmp(env_value, "1") == 0) {
        return 1;
    }
    // Absente ou valeur non reconnue : le défaut du programme, jamais une
    // désactivation silencieuse d'un moteur adopté.
    return MRV_DEFAULT_ENABLED;
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
        } else if (strcmp(argv[r], "--expand-max-stock") == 0) {
            // Option valuée, même schéma que --expand-level. Contrairement à
            // expand_min_level (0 = expansion désactivée, valeur légitime),
            // un plafond <= 0 n'a pas de sens utile : valeur absente ou <= 0
            // ignorée, expand_max_stock garde sa valeur par défaut
            // (EXPAND_MAX_STOCK) ou celle déjà fixée par un usage antérieur.
            if (r + 1 < argc) {
                int max_stock = atoi(argv[r + 1]);
                if (max_stock > 0) {
                    expand_max_stock = max_stock;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--expand-max-levels") == 0) {
            // Option valuée, même schéma que --expand-max-stock : un plafond
            // <= 0 empêcherait toute expansion (aucune passe), donc valeur
            // absente ou <= 0 ignorée, expand_max_levels garde sa valeur par
            // défaut (EXPAND_MAX_LEVELS) ou celle déjà fixée.
            if (r + 1 < argc) {
                int max_levels = atoi(argv[r + 1]);
                if (max_levels > 0) {
                    expand_max_levels = max_levels;
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
        } else if (strcmp(argv[r], "--name") == 0) {
            // Option valuée, même schéma que --http-token-file : pointeur
            // direct dans argv, jamais copié. Résolue plus tard par
            // init_client_identity (app_runtime.h), avant tout fork.
            if (r + 1 < argc) {
                client_label = argv[r + 1];
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--machine-uid-file") == 0) {
            // Option valuée, même schéma. Chargement effectif (lecture/écriture
            // du fichier) dans init_client_identity, pas ici (aucune I/O dans
            // parse_cli_options).
            if (r + 1 < argc) {
                machine_uid_file_path = argv[r + 1];
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--rebalance-budget") == 0) {
            // Option valuée, même schéma que --expand-max-stock : un budget
            // <= 0 n'a pas de sens utile (aucun rééquilibrage), valeur
            // absente ou <= 0 ignorée, rebalance_budget garde sa valeur par
            // défaut (REBALANCE_BUDGET_DEFAULT) ou celle déjà fixée.
            if (r + 1 < argc) {
                int budget = atoi(argv[r + 1]);
                if (budget > 0) {
                    rebalance_budget = budget;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--stock-files") == 0) {
            // Option valuée, même schéma que --expand-max-stock : appliquée
            // plus tard par main() (datamanager_configure_stock_files), pas
            // ici (aucune dépendance sur core/datamanager.h dans ce fichier).
            // <n> <= 0 ignoré (garde 0 = défaut NB_FILE_POSSIBILITY_DEFAULT).
            if (r + 1 < argc) {
                int n = atoi(argv[r + 1]);
                if (n > 0) {
                    stock_files_requested = n;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--stock-max-ram") == 0) {
            // Option valuée, même schéma que --expand-max-stock : appliquée
            // plus tard par main() (datamanager_configure_ram_limit), pas ici
            // (aucune dépendance sur core/datamanager.h dans ce fichier).
            // <mo> <= 0 ignoré (garde 0 = illimité, comportement inchangé).
            if (r + 1 < argc) {
                int mo = atoi(argv[r + 1]);
                if (mo > 0) {
                    stock_max_ram_mb = mo;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--stock-spill-dir") == 0) {
            // Option valuée, même schéma que --machine-uid-file : pointeur
            // direct dans argv, jamais copié. Création/purge effective du
            // répertoire différée à stock_spill_configure
            // (core/stock_spill.h), appelée par runserver (app/etii_server.c)
            // -- aucune dépendance sur core/stock_spill.h dans ce fichier,
            // aucune I/O ici.
            if (r + 1 < argc) {
                stock_spill_dir = argv[r + 1];
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--tcp-timeout") == 0) {
            // Option valuée, même schéma que --http-port : un timeout <= 0
            // n'a pas de sens utile (des sockets qui n'expirent jamais),
            // valeur absente ou <= 0 ignorée, tcp_timeout garde sa valeur
            // par défaut (DEFAULT_TCP_TIMEOUT) ou celle déjà fixée.
            if (r + 1 < argc) {
                int timeout = atoi(argv[r + 1]);
                if (timeout > 0) {
                    tcp_timeout = timeout;
                }
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--config-file") == 0) {
            // Option valuée, même schéma. Chargement effectif (lecture, puis
            // application aux globales) dans handle_client (src/app/main.c),
            // via client_config_load/client_config_apply_to_globals — aucune
            // I/O ici.
            if (r + 1 < argc) {
                client_config_file_path = argv[r + 1];
                r++; // consomme aussi la valeur
            }
        } else {
            argv[w++] = argv[r];
        }
    }
    return w;
}
