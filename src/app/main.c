#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>

#include "app/static_variables.h"
#include "ui/console.h"
#include "core/possibility.h"

#include "core/datamanager.h"
#include "core/part.h"
#include "core/readdata.h"
#include "app/etii_client.h"
#include "app/etii_server.h"
#include "app/etii_control.h"
#include "app/app_runtime.h"
#include "app/client_config.h"
#include "net/http_server.h"
#include "net/local_socket.h"
#include "ui/command_lines.h"
#include "app/etii_statistic.h"
#include "ui/logger.h"
#include "net/ipc_protocol.h"

void handle_client(int argc, const char *argv[]);
void handle_server(int argc, const char *argv[]);
void handle_test(const char *arg);

/**
 * @brief Point d'entrée du programme.
 *
 * Ceci est la fonction principale où commence l'exécution du programme.
 *
 * @param argc Le nombre d'arguments de la ligne de commande.
 * @param argv Un tableau de chaînes terminées par un caractère nul représentant les arguments de la ligne de commande.
 * @return Un entier représentant le statut de sortie du programme.
 *         En général, retourner 0 indique une exécution réussie.
 */
int main(int argc, const char *argv[]) {
    parent_pid = getpid();
    log_info("Version %i", version);

    // Options position-indépendantes (ex. --stop-on-solution) : retirées de argv
    // pour ne pas perturber le parsing positionnel des modes, et lues AVANT tout
    // fork → héritées par les process de recherche enfants. Logique extraite
    // (testée unitairement, cf. tests/app/test_static_variables.c).
    argc = parse_cli_options(argc, argv);
    if (help_requested) {
        // --help / -h (n'importe où) : aide générale puis sortie en succès,
        // avant toute initialisation (aucun fork, thread ni socket).
        print_cli_help();
        exit(EXIT_SUCCESS);
    }
    if (stop_on_solution) {
        log_info("option : arrêt à la première solution activé (--stop-on-solution)\n");
    }
    if (headless_mode) {
        log_info("option : console interactive désactivée (--headless)\n");
    }
    if (HTTP_TOKEN_FILE != NULL) {
        // Chargé ici (avant tout fork), quel que soit le mode : même
        // emplacement que les autres options globales. --http-token-file sans
        // --http-port est accepté (le jeton ne sert alors à rien, mais rien
        // n'empêche l'opérateur de préparer sa configuration à l'avance) —
        // un simple avertissement, pas un échec.
        if (HTTP_PORT <= 0) {
            log_info("option : --http-token-file fourni sans --http-port (jeton inutilisé, API HTTP désactivée)\n");
        }
        if (http_token_load(HTTP_TOKEN_FILE, HTTP_ADMIN_TOKEN, sizeof(HTTP_ADMIN_TOKEN)) < 0) {
            // Message d'erreur déjà journalisé par http_token_load (jamais le
            // contenu du jeton). Échec de démarrage explicite : une demande
            // d'authentification mal configurée ne doit jamais dégénérer en
            // silence vers "API sans jeton".
            exit(EXIT_FAILURE);
        }
        log_info("option : jeton d'authentification de l'API HTTP admin chargé (--http-token-file)\n");
    }

    // ETII_BENCH_NODES : variable d'environnement (pas d'option CLI, hors du
    // chemin de production) activant le banc de mesure — voir static_variables.h
    // et tests/bench/bench_search.sh. Lue une seule fois ici, avant tout fork,
    // comme les options CLI ci-dessus.
    bench_target_nodes = bench_parse_nodes_env(getenv("ETII_BENCH_NODES"));
    if (bench_target_nodes > 0) {
        log_info("banc de mesure : arrêt demandé après %llu nœuds (ETII_BENCH_NODES)\n",
                  bench_target_nodes);
    }

    if (argc >= 2 && argv[1] != NULL) {
        // Initialisation avant tout fork/thread de statistiques : pas de
        // concurrence possible ici, mais on passe par lastcheck_publish()
        // pour garder un unique point d'écriture protégé par lastcheck_mutex
        // (cf. static_variables.h).
        lastcheck_publish(calloc(2000, sizeof(char)));

        if (strcmp("client", argv[1]) == 0) {
            handle_client(argc, argv);
        } else if (strcmp("pruner", argv[1]) == 0) {
            // Client pruner : même plomberie que le client de recherche, mais les
            // threads exécutent autoprune et demandent du travail à vérifier
            pruner_mode = 1;
            if (gpu_requested) {
#ifdef WITH_CUDA
                // --gpu : le contrôle des lots est exécuté sur le GPU
                // (cf. gpu_pruner.cu / autoprune_gpu).
                gpu_pruner_mode = 1;
#else
                // Erreur explicite plutôt qu'un repli CPU silencieux : l'
                // utilisateur qui demande le GPU doit savoir qu'il ne l'a pas.
                log_error("--gpu : ce binaire est compilé sans CUDA — "
                          "recompiler avec make CUDA=1\n");
                exit(EXIT_FAILURE);
#endif // WITH_CUDA
            }
            handle_client(argc, argv);
        } else if (strcmp("server", argv[1]) == 0) {
            handle_server(argc, argv);
        } else if (strcmp("help", argv[1]) == 0) {
            // help [sujet] : aide générale, ou détail d'un mode/option. Un
            // sujet inconnu est une erreur d'argument (rappel de l'aide via
            // failed_arg) pour ne pas sortir en succès sur une faute de frappe.
            if (argc > 2) {
                if (print_cli_help_topic(argv[2]) != 0) {
                    log_error("sujet d'aide inconnu : \"%s\"\n", argv[2]);
                    failed_arg();
                    exit(EXIT_FAILURE);
                }
            } else {
                print_cli_help();
            }
        } else if (strcmp("test", argv[1]) == 0) {
            char* file = parts_files;
            if (argc > 2) {
                file = (char *)(argv[2]);
            }
            handle_test(file);
        } else {
            failed_arg();
            exit(EXIT_FAILURE);
        }
        lastcheck_publish(NULL);
    } else {
        failed_arg();
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}

void run_client(const char *hostname, const char *file, int fork_seq);

/**
 * @brief Gère le client TCP.
 *
 * Cette fonction initialise les fils, les signaux, les compteurs, les vérifications, la console, et exécute le client.
 *
 * @param argc Le nombre d'arguments de la ligne de commande.
 * @param argv Un tableau de chaînes terminées par un caractère nul représentant les arguments de la ligne de commande.
 */
void handle_client(int argc, const char *argv[]) {
    log_info("client\n");
    // Parsing positionnel (dépendant de pruner_mode) extrait dans app_runtime.c
    // pour être testable — cf. parse_client_args.
    const char *serverIp = parse_client_args(argc, argv);

    // Configuration client (PR A, docs/conception/cycle_vie_forks.md) : fichier
    // clé=valeur optionnel (--config-file), appliqué UNIQUEMENT aux positions
    // que la ligne de commande n'a pas déjà fournies — priorité CLI > fichier >
    // défauts. Fait avant tout fork, comme les autres options globales. Un
    // fichier absent n'est jamais une erreur (cf. client_config_load).
    client_config_t startup_cfg;
    client_config_init(&startup_cfg);
    if (client_config_load(client_config_file_path, &startup_cfg) == CLIENT_CONFIG_LOADED) {
        log_info("configuration : chargée depuis \"%s\"\n", client_config_file_path);
    }
    client_config_apply_to_globals(&startup_cfg, argc, &serverIp);
    client_config_free(&startup_cfg);
    // Conservé pour les commandes console `config`/`configSave`, exécutées
    // depuis le thread console du process PARENT : serverIp n'est sinon
    // accessible que dans la pile de cette fonction.
    g_client_server_host = serverIp;

    init_childs();
    init_counters();
    init_signals();

    // Identité déclarée (v12) : résolue UNE FOIS ici, avant tout fork, pour
    // que tous les forks héritent (copy-on-write) le même machine_uid/
    // client_uid/label — seul fork_seq diffère, fixé par chaque connexion à
    // l'émission de son propre hello (cf. app_runtime.h).
    init_client_identity();

    // Map de lookup construite ICI, une seule fois, AVANT la boucle de fork() :
    // elle n'est plus jamais écrite ensuite, donc les process de recherche
    // l'héritent en copy-on-write et se partagent physiquement UNE copie au
    // lieu d'en construire chacun la leur (5,06 Mo de `flat` + 1,27 Mo d'index
    // compact + 0,11 Mo d'arène par process). Le parent en reste propriétaire :
    // il est le seul à la libérer, après wait_child().
    // Fait avant la création de la socket locale : un fichier de pièces
    // illisible fait sortir read_parts, autant que ce soit avant d'avoir laissé
    // une socket `etii_main.<pid>` derrière nous.
    search_parts_t shared_parts;
    build_search_parts(&shared_parts, parts_files);
    set_inherited_search_parts(&shared_parts);

    char socket_main[50];
    sprintf(socket_main, "etii_main.%d", getpid());
    main_addr = build_sockaddr(socket_main);
    log_info("socket main : %s\n", socket_main);

    int *socket_id = malloc(sizeof(int));
    *socket_id = build_udp_local_socket(main_addr);
    main_socket_id = socket_id;

    init_sigchld_sigaction();

    // Les tampons stdio sont hérités TELS QUELS par fork() : ce qui n'a pas été
    // écrit sur le flux ici (stdout redirigé vers un fichier = tampon par blocs)
    // serait ré-émis par CHACUN des enfants à sa sortie, dupliquant N fois tout
    // le journal de démarrage. On vide donc les flux juste avant la boucle —
    // encore mono-thread, donc aucun verrou stdio ne peut être pris ailleurs.
    fflush(NULL);

    // IMPORTANT : aucun thread du parent (console, checker, réception stats) ne
    // doit tourner pendant la boucle de fork(). Sinon, si l'un d'eux détient le
    // verrou d'un FILE stdio (ex. la console au milieu d'un printf) au moment du
    // fork, l'enfant hérite de ce verrou verrouillé par un thread qui n'existe
    // pas chez lui : son premier printf se bloque alors définitivement. On reste
    // donc mono-thread jusqu'à la fin des forks, puis on lance les threads.

    pid_t child_pid = -1;
    int fork_error = 0;
    for (int c = 0; c < NB_THREADS; c++) {
        if (parent_pid == getpid()) {
#ifdef DEBUG_IN_MONO_PROCESS
            child_pid = getpid();
#else
            child_pid = fork();
#endif // DEBUG_IN_MONO_PROCESS
            if (child_pid != 0) {
                if (child_pid == -1) {
                    // Échec de création de CE process : on le signale et on
                    // poursuit avec les autres. On NE retente pas le même slot
                    // (pas de c--) et on N'arrête PAS l'application ici : seule
                    // l'absence TOTALE de process l'arrêtera (bilan après la
                    // boucle). Exigence : « indiquer que les process n'ont pas
                    // été créés, ne s'arrêter que si aucun n'a pu l'être ».
                    log_error("fork error : process %i/%i non créé (errno=%i)\n",
                              c + 1, NB_THREADS, errno);
                    fork_error++;
                    childrens_pid[c] = -1;
                    // Ressources visiblement épuisées : inutile d'insister, on
                    // conserve les process déjà créés et on passe à la suite.
                    if (fork_error >= 10) {
                        log_error("création de process interrompue après %i échecs ; "
                                  "poursuite avec les process déjà créés\n", fork_error);
                        break;
                    }
                    continue;
                }
#ifdef DEBUG_THREAD
                log_info("child %i created\n", child_pid);
#endif // DEBUG_THREAD
                int sp_len = sprintf(forkId[c], "etii_fork.%d", child_pid);
                forkId[c][sp_len] = '\0';
                childrens_pid[c] = child_pid;
                int childStatus = 0;
                waitpid(child_pid, &childStatus, WNOHANG);
                if (childStatus != 0) {
                    log_error("child %i error %i\n", child_pid, childStatus);
                    c--;
                    continue;
                }
#ifndef DEBUG_IN_MONO_PROCESS
            } else {
#endif // DEBUG_IN_MONO_PROCESS
#ifdef DEBUG_THREAD
                log_info("NEW thread %i\n", getpid());
#endif // DEBUG_THREAD
                NB_THREADS = 1;
                run_fork_checker(main_addr);
                run_client(serverIp, parts_files, c);

                if (fork_checker_socket_id > 0) {
                    close(fork_checker_socket_id);
                }
                char socket_fork[50];
                int socket_fork_len = sprintf(socket_fork, "etii_fork.%d", getpid());
                socket_fork[socket_fork_len] = '\0';
                struct sockaddr_un *fork_addr = build_sockaddr(socket_fork);
#ifdef DEBUG_LOCAL_SOCKET
                log_debug("remove : %s\n", fork_addr->sun_path);
                flush_debug();
#endif // DEBUG_LOCAL_SOCKET
                remove(fork_addr->sun_path);
                free(fork_addr);
            }
        }
    }

    if (parent_pid == getpid()) {
        // Bilan de création : combien de process enfants ont réellement démarré.
        int created = count_created_forks(childrens_pid, NB_THREADS);
        if (created == 0) {
            // SEUL cas d'arrêt : aucun process n'a pu être créé. Rien à tuer,
            // on libère et on sort proprement.
            log_error("aucun process enfant n'a pu être créé — arrêt de l'application\n");
            close(*socket_id);
            remove(main_addr->sun_path);
            free(main_addr);
            set_inherited_search_parts(NULL);
            free_search_parts(&shared_parts);
            return;
        }
        if (fork_error > 0) {
            log_info("%i/%i process créés ; %i non créés (ressources insuffisantes) — poursuite\n",
                     created, NB_THREADS, fork_error);
        }

        // Les forks sont terminés : on peut démarrer les threads du parent. Ces
        // démarrages sont désormais NON fatals (cf. run_server_thread /
        // run_checker / run_console) : sous forte pression de ressources, le
        // parent tourne en mode dégradé au lieu de planter en laissant les
        // process enfants orphelins.
        if (*socket_id > 0) {
            run_server_thread(socket_id);
        }
        run_checker(0);
        if (!headless_mode) {
            run_console(0);
        }
        // Canal de contrôle (v9) : connexion TCP additionnelle dédiée où le
        // serveur devient l'initiateur des échanges. Non fatal comme les
        // threads ci-dessus si la création échoue (cf. start_control_channel).
        start_control_channel(serverIp, created);

        wait_child();
        close(*socket_id);
#ifdef DEBUG_LOCAL_SOCKET
        log_debug("remove : %s\n", main_addr->sun_path);
        flush_debug();
#endif // DEBUG_LOCAL_SOCKET
        remove(main_addr->sun_path);
        // Tous les process de recherche sont terminés (wait_child) : plus
        // personne ne lit la map partagée, le propriétaire peut la libérer.
        set_inherited_search_parts(NULL);
        free_search_parts(&shared_parts);
    }
    free(main_addr);
}

/**
 * @brief Gère le serveur TCP.
 *
 * Cette fonction initialise les fils, les signaux, les compteurs, les vérifications, la console, et exécute le serveur.
 *
 * @param argc Le nombre d'arguments de la ligne de commande.
 * @param argv Un tableau de chaînes terminées par un caractère nul représentant les arguments de la ligne de commande.
 */
void handle_server(int argc, const char *argv[]) {
    log_info("server\n");
    server = 1;
    NB_THREADS = 80;
    // Usage : server [nb_threads] [pieces.csv]. Le 1er argument est le NOMBRE
    // DE THREADS, pas le fichier. Erreur fréquente : « server data/pieces16.csv »
    // → atoi(chemin) == 0 → serveur démarré avec 0 thread de communication : il
    // accepte les connexions mais ne les sert JAMAIS (stock figé, client inactif).
    // On valide donc l'argument et on récupère le cas du fichier passé à sa place.
    int file_arg = -1; // indice de l'argument « fichier de pièces », si fourni
    if (argc >= 3) {
        log_info("arg 2 : %s\n", argv[2]);
        switch (parse_server_thread_arg(argv[2], NB_THREADS, &NB_THREADS)) {
        case SERVER_ARG_AS_FILENAME:
            // Pas un nombre : l'utilisateur a probablement passé le fichier ici.
            // On garde le nombre de threads par défaut et on traite cet argument
            // comme le fichier de pièces (sinon : serveur muet à 0 thread).
            log_error("1er argument (\"%s\") interprété comme fichier de pièces ; "
                      "le nombre de threads attendu à cette position est absent — "
                      "%i threads par défaut. Usage : server [nb_threads] [pieces.csv]\n",
                      argv[2], NB_THREADS);
            file_arg = 2;
            break;
        case SERVER_ARG_INVALID_COUNT:
            // Nombre fourni mais non valide (0 ou négatif) : on garde le défaut.
            log_error("nombre de threads invalide (\"%s\") — %i threads par défaut\n", argv[2], NB_THREADS);
            if (argc >= 4) file_arg = 3;
            break;
        default: // SERVER_ARG_AS_COUNT
            if (argc >= 4) file_arg = 3;
            break;
        }
    }
    log_info("Nb threads : %i\n", NB_THREADS);
    init_childs();
    init_signals();
    init_counters();
    run_checker(1);
    if (!headless_mode) {
        run_console(1);
    }
    if (file_arg >= 0) {
        parts_files = (char *)(argv[file_arg]);
    }
    runserver(parts_files);
}

void run_auto(const char *file);

/**
 * @brief Gère le test.
 *
 * Cette fonction initialise les compteurs, les vérifications, la console, et exécute le test.
 *
 * @param file fichier à traiter.
 */
void handle_test(const char *file) {
    NB_THREADS = 1;
    // Le mode test bride par défaut à 100000 coups/s (usage interactif). Le
    // banc de mesure (ETII_BENCH_NODES) veut le débit brut de la machine —
    // pas de bridage artificiel dans ce cas.
    max_search_by_sec = bench_target_nodes > 0 ? 0 : 100000;
    init_childs();
    init_counters();
    run_checker(0);
    if (!headless_mode) {
        run_console(0);
    }
    run_auto(file);
}

/**
 * @brief Exécute le client avec le nom d'hôte et le fichier spécifiés.
 *
 * Cette fonction initie une connexion client au nom d'hôte donné et traite
 * le fichier spécifié.
 *
 * @param hostname Le nom d'hôte auquel se connecter.
 * @param file Le fichier à traiter.
 * @param fork_seq Rang de ce fork (0..N-1) parmi ceux de son process parent.
 */
void run_client(const char *hostname, const char *file, int fork_seq)
{
	// On indique au manager de passer par un serveur
	set_server_ip(hostname);

    run_mono_client(file, fork_seq);

	// Sauvegarde de secours si les files ne sont pas vides (anomalie en mode
	// client) — extraite dans app_runtime.c pour être testable.
	backup_failed_exit();
}

/**
 * @brief Exécute le programme en mode automatique.
 *
 * Cette fonction lit les pièces du fichier spécifié, les fait tourner, prépare une carte des pièces,
 * et détermine les premières possibilités. Ensuite, elle exécute le client en mode automatique.
 *
 * @param file Le fichier à traiter.
 */
void run_auto(const char *file)
{
	// On prépare les premières possiblitées en local. Aucun fork dans ce mode :
	// rien n'est publié via set_inherited_search_parts, et run_mono_client
	// construira donc (et libérera) les siennes, comme avant.
	search_parts_t parts;
	build_search_parts(&parts, file);
	first_possibility(parts.map, parts.rotate_parts);
	free_search_parts(&parts);

	// Mode test : mono-processus, aucun vrai fork — fork_seq n'a pas de sens
	// ici (server_ip reste NULL, le hello de travail n'est de toute façon
	// jamais envoyé, cf. check_and_connect_to_server).
	run_mono_client(file, 0);
}

