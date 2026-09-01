#include "app/app_static_variables.h"
#include "core/core_static_variables.h"
#include <stdlib.h>
#include <string.h>

int NB_THREADS = 10;
int g_active_forks = 0;

int pruner_mode = 0;

int pruner_forks_requested = -1;

int auto_roles_requested = 0;
int local_dispatch_enabled = 0;

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

int analysed_lease_seconds = ANALYSED_LEASE_DEFAULT_SECONDS;

#ifdef WITH_CUDA
int gpu_pruner_mode = 0;
#endif // WITH_CUDA

char *lastcheck = NULL;
pthread_mutex_t lastcheck_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Voir la doc dans app_static_variables.h.
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

struct client_statistics *fork_statistics = NULL;

time_t *fork_last_activity = NULL;

long inst_unknow = 0;

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

const char *server_config_file_path = "./eternityii-server.conf";

client_identity_t g_client_identity_template;

volatile unsigned long long server_shots_per_second = 0;
volatile unsigned long long server_last_backup_duration_ms = 0;

unsigned long long max_search_by_sec = 0;

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
        } else if (strcmp(argv[r], "--pruner-forks") == 0) {
            // Option valuée : dosage recherche/contrôle demandé pour le lot de
            // forks à venir (résolu contre NB_THREADS, pas encore connu ici,
            // par resolve_pruner_forks — cf. app_static_variables.h). Le
            // sentinel -1 (« non demandé ») doit rester réservé à l'absence de
            // l'option : une valeur négative fournie explicitement est donc
            // ramenée à 0 plutôt que traitée comme absente.
            if (r + 1 < argc) {
                int n = atoi(argv[r + 1]);
                pruner_forks_requested = (n < 0) ? 0 : n;
                r++; // consomme aussi la valeur
            }
        } else if (strcmp(argv[r], "--local-dispatch") == 0) {
            // Drapeau booléen, même schéma que --auto-roles : lu par le mode
            // client (main.c, fork_orchestrator.c) pour armer le courtier du
            // parent et les crochets des forks.
            local_dispatch_enabled = 1;
        } else if (strcmp(argv[r], "--split-shallow-first") == 0) {
            // Sens de cession des frères non explorés. Drapeau booléen, lu par
            // le moteur de recherche (delegate_shallow_first, core).
            delegate_shallow_first = 1;
        } else if (strcmp(argv[r], "--auto-roles") == 0) {
            // Drapeau booléen, même schéma que --stop-on-solution : lu par
            // le mode serveur uniquement (main.c), avant le lancement du
            // thread check_server.
            auto_roles_requested = 1;
        } else if (strcmp(argv[r], "--config-file") == 0) {
            // Option valuée, même schéma. Chargement effectif (lecture, puis
            // application aux globales) dans handle_client/handle_server
            // (src/app/main.c), via client_config_load/server_config_load —
            // aucune I/O ici. Une seule option pour les deux : au plus un mode
            // s'exécute par process, donc écrire la même valeur dans les deux
            // globales est sans effet pour celle du mode qui ne tourne pas
            // (jamais lue). Sans cette option, chaque mode garde son propre
            // chemin par défaut (client_config_file_path/server_config_file_path).
            if (r + 1 < argc) {
                client_config_file_path = argv[r + 1];
                server_config_file_path = argv[r + 1];
                r++; // consomme aussi la valeur
            }
        } else {
            argv[w++] = argv[r];
        }
    }
    return w;
}
