#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <sys/socket.h>

#include "app/app_static_variables.h"
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
#include "app/server_config.h"
#include "app/fork_orchestrator.h"
#include "app/work_broker.h"
#include "net/http_server.h"
#include "net/local_socket.h"
#include "ui/command_lines.h"
#include "app/etii_statistic.h"
#include "ui/logger.h"
#include "net/ipc_protocol.h"

void handle_client(int argc, const char *argv[]);
void handle_server(int argc, const char *argv[], server_config_t *startup_cfg);
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
    // --config-file (serveur) : chargé ICI, avant les blocs ci-dessous qui
    // appliquent HTTP_TOKEN_FILE/HTTP_PORT/stock_files/stock_max_ram de façon
    // INCONDITIONNELLE, avant même le dispatch de mode par argv[1] — priorité
    // CLI > fichier > défauts, même convention que le client (cf.
    // client_config_load dans handle_client). server_config_apply_pre_dispatch
    // applique ici toutes les clés SAUF les deux positionnelles (nb_threads,
    // parts_file), qui ne peuvent être résolues qu'après le parsing
    // positionnel propre à handle_server (cf. server_config_apply_to_globals,
    // appelée là-bas). Un fichier absent n'est jamais une erreur (cf.
    // server_config_load). server_startup_cfg est passée à handle_server puis
    // libérée là-bas, une fois ses deux clés positionnelles consommées.
    int server_mode_requested = (argc >= 2 && argv[1] != NULL && strcmp(argv[1], "server") == 0);
    server_config_t server_startup_cfg;
    server_config_init(&server_startup_cfg);
    int server_config_loaded_at_boot = 0;
    if (server_mode_requested) {
        server_config_loaded_at_boot =
            (server_config_load(server_config_file_path, &server_startup_cfg) == SERVER_CONFIG_LOADED);
        server_config_apply_pre_dispatch(&server_startup_cfg);
        if (server_config_loaded_at_boot) {
            log_info("option : configuration serveur chargée depuis \"%s\" (--config-file)\n",
                      server_config_file_path);
        }
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
    // chemin de production) activant le banc de mesure — voir app_static_variables.h
    // et tests/bench/bench_search.sh. Lue une seule fois ici, avant tout fork,
    // comme les options CLI ci-dessus.
    bench_target_nodes = bench_parse_nodes_env(getenv("ETII_BENCH_NODES"));
    if (bench_target_nodes > 0) {
        log_info("banc de mesure : arrêt demandé après %llu nœuds (ETII_BENCH_NODES)\n",
                  bench_target_nodes);
    }

    // --stock-files : appliqué ici, avant tout fork/thread, quel que soit le mode — même
    // emplacement que les autres options globales. Appel OBLIGATOIRE et
    // INCONDITIONNEL depuis le passage aux pools alloués dynamiquement
    // (tableaux de pointeurs, cf. datamanager.c) : nb_file_possibility vaut 0
    // tant que cette fonction n'a jamais été appelée, pool NULL non
    // utilisable. stock_files_requested reste à 0 (non demandé) tant que
    // l'option n'est pas fournie : on retombe alors sur
    // NB_FILE_POSSIBILITY_DEFAULT.
    datamanager_configure_stock_files(
        stock_files_requested > 0 ? stock_files_requested : NB_FILE_POSSIBILITY_DEFAULT);
    if (stock_files_requested > 0) {
        log_info("option : %d files de stock (--stock-files)\n", nb_file_possibility);
    }

    // --stock-max-ram : converti UNE SEULE FOIS en nombre de possibilités ici
    // (même emplacement que --stock-files ci-dessus, avant tout fork/thread) ;
    // stock_max_ram_mb reste à 0 (illimité) tant que l'option n'est pas
    // fournie, auquel cas datamanager_configure_ram_limit(0) publie 0 = pas de
    // plafond — comportement historique inchangé.
    datamanager_configure_ram_limit(stock_max_ram_mb);
    if (stock_max_ram_mb > 0) {
        log_info("option : plafond stock %d Mo (--stock-max-ram, ~%llu possibilités)\n",
                  stock_max_ram_mb, datamanager_ram_limit_packets());
    }

    if (argc >= 2 && argv[1] != NULL) {
        // Initialisation avant tout fork/thread de statistiques : pas de
        // concurrence possible ici, mais on passe par lastcheck_publish()
        // pour garder un unique point d'écriture protégé par lastcheck_mutex
        // (cf. app_static_variables.h).
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
            handle_server(argc, argv, &server_startup_cfg);
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

    // Point de sortie normal unique pour les trois modes (client/server/test)
    // une fois leur fonction de gestion revenue — trace la fin du programme,
    // symétrique du "Version %i" loggé au tout début de main(). Ne couvre PAS
    // les deux autres chemins de sortie existants : `exit` interpreter
    // (console/canal de contrôle/API HTTP, cf. command_lines.c, qui a son
    // propre log) et signal_end_handler côté serveur (app_runtime.c), qui
    // appelle exit(0) directement DEPUIS le gestionnaire de signal — logger
    // depuis un signal handler n'est pas async-signal-safe (cf. les usages
    // existants, tous gardés par DEBUG_SIGNAL), donc volontairement pas touché
    // ici. Côté client, un Ctrl-C atteint bien CE log : signal_end_handler s'y
    // contente de positionner request=REQUEST_STOP et de propager le signal
    // aux fils, le process parent revient ensuite normalement jusqu'ici via
    // handle_client()/fork_orchestrator_run().
    log_info("fin du programme (sortie normale)\n");
    exit(EXIT_SUCCESS);
}

/**
 * @brief Gère le client TCP.
 *
 * Cette fonction initialise les fils, les signaux, les compteurs, les
 * vérifications, les threads du parent, puis l'orchestrateur de démarrage
 * différé qui décide QUAND forker.
 *
 * @param argc Le nombre d'arguments de la ligne de commande.
 * @param argv Un tableau de chaînes terminées par un caractère nul représentant les arguments de la ligne de commande.
 */
void handle_client(int argc, const char *argv[]) {
    log_info("client\n");
    // Parsing positionnel (dépendant de pruner_mode) extrait dans app_runtime.c
    // pour être testable — cf. parse_client_args.
    const char *serverIp = parse_client_args(argc, argv);

    // Configuration client : fichier clé=valeur optionnel (--config-file),
    // appliqué UNIQUEMENT aux positions que la ligne de commande n'a pas déjà
    // fournies — priorité CLI > fichier > défauts. Fait avant tout fork,
    // comme les autres options globales. Un fichier absent n'est jamais une
    // erreur (cf. client_config_load). Le statut de chargement décide aussi
    // de l'état initial de l'orchestrateur : COUNTDOWN si un fichier a été
    // trouvé, WAITING_CONFIG sinon.
    // Le log de confirmation (contenu inclus) est émis par fork_orchestrator_run,
    // juste avant le décompte — l'opérateur doit voir la configuration EFFECTIVE
    // (après client_config_apply_to_globals), pas seulement le fichier brut.
    client_config_t startup_cfg;
    client_config_init(&startup_cfg);
    int config_loaded_at_boot = (client_config_load(client_config_file_path, &startup_cfg) == CLIENT_CONFIG_LOADED);
    client_config_apply_to_globals(&startup_cfg, argc, &serverIp);
    client_config_free(&startup_cfg);
    // Conservé pour les commandes console `config`/`configSave`, exécutées
    // depuis le thread console du process PARENT : serverIp n'est sinon
    // accessible que dans la pile de cette fonction.
    g_client_server_host = serverIp;

    // Voir la doc de ensure_stock_files_cover_forks (app_runtime.{h,c}) pour le raisonnement.
    ensure_stock_files_cover_forks(NB_THREADS);

#ifdef WITH_CUDA
    // --pruner-forks incompatible avec --gpu dès qu'il diffère de nb_forks :
    // le contexte CUDA n'est initialisé qu'UNE FOIS par process et son
    // déclenchement ne consulte pas le pruner_mode PAR FORK — un dosage mixte
    // ferait tourner CHAQUE fork sur autoprune_gpu, jamais sur autosearch (cf.
    // gpu_pruner_forks_conflict, app_runtime.h). Échec explicite plutôt qu'un
    // dosage silencieusement ignoré. NB_THREADS/pruner_forks_requested sont
    // désormais résolus (CLI + --config-file), donc évalués ICI.
    if (gpu_pruner_forks_conflict(gpu_pruner_mode, pruner_forks_requested, NB_THREADS)) {
        log_error("--gpu : --pruner-forks %d incompatible avec le pruner GPU (nb_forks=%d) — "
                  "le contexte CUDA n'est initialisé qu'une fois par process, un dosage mixte "
                  "n'aurait aucun sens ; retirer --pruner-forks ou l'ajuster à nb_forks\n",
                  pruner_forks_requested, NB_THREADS);
        exit(EXIT_FAILURE);
    }
#endif // WITH_CUDA

    init_childs();
    init_counters();
    init_signals();

    // Identité déclarée (v12) : résolue UNE FOIS ici, avant tout fork, pour
    // que tous les forks héritent (copy-on-write) le même machine_uid/
    // client_uid/label — seul fork_seq diffère, fixé par chaque connexion à
    // l'émission de son propre hello (cf. app_runtime.h).
    init_client_identity();

    // Map de lookup construite ICI, une seule fois, AVANT tout fork éventuel :
    // elle n'est plus jamais écrite ensuite, donc les process de recherche
    // l'héritent en copy-on-write et se partagent physiquement UNE copie au
    // lieu d'en construire chacun la leur (5,06 Mo de `flat` + 1,27 Mo d'index
    // compact + 0,11 Mo d'arène par process). Le parent en reste propriétaire :
    // il est le seul à la libérer, après le retour de fork_orchestrator_run.
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

    // État initial de l'orchestrateur : posé ICI, pendant que le process est
    // encore mono-thread, AVANT le lancement du moindre thread susceptible de
    // poster un événement (console en tête). Trouvé nécessaire via des tests
    // manuels réels (reproduit de façon fiable sous make test-docker, jamais
    // en local) : quand cette initialisation faisait partie de
    // fork_orchestrator_run elle-même — appelée après le lancement du thread
    // console — un `start` tapé (ou reçu via une FIFO de test) suffisamment
    // tôt gagnait la course contre cette même initialisation, qui écrasait
    // alors sans condition l'état déjà avancé par la console, annulant le
    // fork silencieusement. Cf. fork_orchestrator.h pour le détail complet.
    fork_orchestrator_init_state(config_loaded_at_boot);

    // Les threads du parent (réception stats, checker, console, canal de
    // contrôle) démarrent MAINTENANT — avant tout fork. Le fork lui-même est
    // différé, décidé par l'orchestrateur (fichier de configuration ->
    // décompte de 5 s, sinon attente d'un `start`/`config` en console) et
    // protégé par la quiescence coopérative (`fork_gate`) puisque ces
    // threads tournent déjà au moment où `orchestrator_spawn_forks` forke
    // réellement (cf. src/app/fork_orchestrator.c). Le `fflush(NULL)`
    // protecteur d'avant fork historique est désormais assuré par
    // `fork_gate_acquire_io_locks`.
    //
    // Démarrages NON fatals (cf. run_server_thread / run_checker /
    // run_console / start_control_channel) : sous forte pression de
    // ressources, le parent tourne en mode dégradé plutôt que de planter.
    if (*socket_id > 0) {
        run_server_thread(socket_id);
    }
    run_checker(0);
    if (!headless_mode) {
        run_console(0);
    }
    // Canal de contrôle (v9) : connexion TCP additionnelle dédiée où le
    // serveur devient l'initiateur des échanges. `nb_forks` est désormais
    // relu dynamiquement depuis `g_active_forks` à chaque reconnexion — 0
    // tant qu'aucun fork n'existe encore.
    start_control_channel(serverIp);

    // Courtier de travail (--local-dispatch) : démarré APRÈS les autres threads
    // du parent et AVANT fork_orchestrator_run, comme eux — son thread de relais
    // s'enregistre auprès de fork_gate, donc il doit exister avant la première
    // demande de quiescence. Sans l'option, l'appel ne fait rien.
    work_broker_parent_start(serverIp);

    fork_orchestrator_run(config_loaded_at_boot, &shared_parts);

    // Plus aucun fork ne subsiste : plus personne n'offre. On arrête le relais
    // après avoir tenté un dernier vidage vers le serveur (cf. work_broker.h).
    work_broker_parent_stop();

    close(*socket_id);
#ifdef DEBUG_LOCAL_SOCKET
    log_debug("remove : %s\n", main_addr->sun_path);
    flush_debug();
#endif // DEBUG_LOCAL_SOCKET
    remove(main_addr->sun_path);
    // Plus aucun fork ne subsiste (contrat de fork_orchestrator_run) : plus
    // personne ne lit la map partagée, le propriétaire peut la libérer.
    set_inherited_search_parts(NULL);
    free_search_parts(&shared_parts);
    free(main_addr);
}

/**
 * @brief Gère le serveur TCP.
 *
 * Cette fonction initialise les fils, les signaux, les compteurs, les vérifications, la console, et exécute le serveur.
 *
 * @param argc        Le nombre d'arguments de la ligne de commande.
 * @param argv        Un tableau de chaînes terminées par un caractère nul représentant les arguments de la ligne de commande.
 * @param startup_cfg Configuration serveur déjà chargée par `main()` (--config-file) —
 *                     ses clés positionnelles (`nb_threads`/`parts_file`) sont appliquées
 *                     ici une fois le parsing positionnel CLI connu ; toutes les autres
 *                     clés ont déjà été appliquées par `main()` avant l'appel
 *                     (cf. `server_config_apply_pre_dispatch`). Libérée en sortie de
 *                     cette fonction : `main()` n'y touche plus après cet appel.
 */
void handle_server(int argc, const char *argv[], server_config_t *startup_cfg) {
    log_info("server\n");
    server = 1;
    NB_THREADS = 80;
    // Usage : server [nb_threads] [pieces.csv]. Le 1er argument est le NOMBRE
    // DE THREADS, pas le fichier. Erreur fréquente : « server data/pieces16.csv »
    // → atoi(chemin) == 0 → serveur démarré avec 0 thread de communication : il
    // accepte les connexions mais ne les sert JAMAIS (stock figé, client inactif).
    // On valide donc l'argument et on récupère le cas du fichier passé à sa place.
    int file_arg = -1; // indice de l'argument « fichier de pièces », si fourni
    int cli_gave_nb_threads = 0; // vrai seulement si argv[2] a été accepté comme NOMBRE de threads
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
            cli_gave_nb_threads = 1;
            if (argc >= 4) file_arg = 3;
            break;
        }
    }
    // --config-file (serveur) : les deux seules clés qui restaient à appliquer
    // (nb_threads/parts_file, positionnelles) — le reste a déjà été appliqué
    // par main() avant l'appel, cf. server_config_apply_pre_dispatch. Priorité
    // CLI > fichier > défauts : n'écrase jamais une valeur que la CLI a fournie.
    server_config_apply_to_globals(startup_cfg, cli_gave_nb_threads, file_arg >= 0);
    server_config_free(startup_cfg);
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

