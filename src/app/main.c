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
#include "app/app_runtime.h"
#include "net/local_socket.h"
#include "ui/command_lines.h"
#include "app/etii_statistic.h"
#include "ui/logger.h"
#include "net/ipc_protocol.h"

void handle_tcpclient(int argc, const char *argv[]);
void handle_tcpserver(int argc, const char *argv[]);
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
    if (stop_on_solution) {
        log_info("option : arrêt à la première solution activé (--stop-on-solution)\n");
    }

    if (argc >= 2 && argv[1] != NULL) {
        // Initialisation avant tout fork/thread de statistiques : pas de
        // concurrence possible ici, mais on passe par lastcheck_publish()
        // pour garder un unique point d'écriture protégé par lastcheck_mutex
        // (cf. static_variables.h).
        lastcheck_publish(calloc(2000, sizeof(char)));

        if (strcmp("tcpclient", argv[1]) == 0) {
            handle_tcpclient(argc, argv);
        } else if (strcmp("tcppruner", argv[1]) == 0) {
            // Client pruner : même plomberie que le client de recherche, mais les
            // threads exécutent autoprune et demandent du travail à vérifier
            pruner_mode = 1;
            handle_tcpclient(argc, argv);
        } else if (strcmp("tcpserver", argv[1]) == 0) {
            handle_tcpserver(argc, argv);
#ifdef WITH_CUDA
        } else if (strcmp("gpupruner", argv[1]) == 0) {
            // Pruner GPU : même plomberie que tcppruner, mais le contrôle des
            // lots est exécuté sur le GPU (cf. gpu_pruner.cu / autoprune_gpu).
            pruner_mode = 1;
            gpu_pruner_mode = 1;
            handle_tcpclient(argc, argv);
#endif // WITH_CUDA
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

void run_client(const char *hostname, const char *file);
int run_checker(int server);

/**
 * @brief Gère le client TCP.
 *
 * Cette fonction initialise les fils, les signaux, les compteurs, les vérifications, la console, et exécute le client.
 *
 * @param argc Le nombre d'arguments de la ligne de commande.
 * @param argv Un tableau de chaînes terminées par un caractère nul représentant les arguments de la ligne de commande.
 */
void handle_tcpclient(int argc, const char *argv[]) {
    log_info("client\n");
    NB_THREADS = 1;
    char *serverIp = "localhost";
    if (argc >= 3) {
        serverIp = (char *)argv[2];
    }
    if (argc >= 4) {
        int was_invalid = 0;
        NB_THREADS = parse_positive_int_or_default(argv[3], 1, &was_invalid);
        if (was_invalid) {
            // Un nombre de threads <= 0 (ou non numérique) donnerait 0 process de
            // travail : le client ne ferait rien. On retombe sur 1.
            log_error("nombre de threads invalide (\"%s\") — 1 thread par défaut\n", argv[3]);
        }
    }
    if (argc >= 5) {
        if (pruner_mode) {
            // tcppruner [serveur] [nb_threads] [pieces.csv] [batch] : pas de stock local
            parts_files = (char *)(argv[4]);
        } else {
            max_stock_by_thread = atoi(argv[4]);
        }
    }
    if (pruner_mode && argc >= 6) {
        // Taille du lot d'échange pruner (configurable au démarrage). Bornée pour
        // maîtriser la mémoire du pruner et les tampons GPU.
        pruner_batch_size = atoi(argv[5]);
        if (pruner_batch_size < 1) {
            pruner_batch_size = 1;
        }
        if (pruner_batch_size > PRUNER_BATCH_MAX) {
            pruner_batch_size = PRUNER_BATCH_MAX;
        }
    }
#ifdef DEBUG_IN_MONO_PROCESS
    NB_THREADS = 1;
#endif
    init_childs();
    init_counters();
    init_signals();

    char socket_main[50];
    sprintf(socket_main, "etii_main.%d", getpid());
    main_addr = build_sockaddr(socket_main);
    log_info("socket main : %s\n", socket_main);

    int *socket_id = malloc(sizeof(int));
    *socket_id = build_udp_local_socket(main_addr);
    main_socket_id = socket_id;

    init_sigchld_sigaction();

    // argv[5] = pieces.csv pour un client de recherche. Pour un pruner, argv[5]
    // est la taille de lot (cf. plus haut) et le fichier de pièces reste argv[4] :
    // on ne l'écrase donc pas ici.
    if (!pruner_mode && argc >= 6) {
        parts_files = (char *)(argv[5]);
    }

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
                run_client(serverIp, parts_files);

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
        run_console(0);

        wait_child();
        close(*socket_id);
#ifdef DEBUG_LOCAL_SOCKET
        log_debug("remove : %s\n", main_addr->sun_path);
        flush_debug();
#endif // DEBUG_LOCAL_SOCKET
        remove(main_addr->sun_path);
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
void handle_tcpserver(int argc, const char *argv[]) {
    log_info("server\n");
    server = 1;
    NB_THREADS = 80;
    // Usage : tcpserver [nb_threads] [pieces.csv]. Le 1er argument est le NOMBRE
    // DE THREADS, pas le fichier. Erreur fréquente : « tcpserver data/pieces16.csv »
    // → atoi(chemin) == 0 → serveur démarré avec 0 thread de communication : il
    // accepte les connexions mais ne les sert JAMAIS (stock figé, client inactif).
    // On valide donc l'argument et on récupère le cas du fichier passé à sa place.
    int file_arg = -1; // indice de l'argument « fichier de pièces », si fourni
    if (argc >= 3) {
        log_info("arg 2 : %s\n", argv[2]);
        switch (parse_tcpserver_thread_arg(argv[2], NB_THREADS, &NB_THREADS)) {
        case TCPSERVER_ARG_AS_FILENAME:
            // Pas un nombre : l'utilisateur a probablement passé le fichier ici.
            // On garde le nombre de threads par défaut et on traite cet argument
            // comme le fichier de pièces (sinon : serveur muet à 0 thread).
            log_error("1er argument (\"%s\") interprété comme fichier de pièces ; "
                      "le nombre de threads attendu à cette position est absent — "
                      "%i threads par défaut. Usage : tcpserver [nb_threads] [pieces.csv]\n",
                      argv[2], NB_THREADS);
            file_arg = 2;
            break;
        case TCPSERVER_ARG_INVALID_COUNT:
            // Nombre fourni mais non valide (0 ou négatif) : on garde le défaut.
            log_error("nombre de threads invalide (\"%s\") — %i threads par défaut\n", argv[2], NB_THREADS);
            if (argc >= 4) file_arg = 3;
            break;
        default: // TCPSERVER_ARG_AS_COUNT
            if (argc >= 4) file_arg = 3;
            break;
        }
    }
    log_info("Nb threads : %i\n", NB_THREADS);
    init_childs();
    init_signals();
    init_counters();
    run_checker(1);
    run_console(1);
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
    max_search_by_sec = 100000;
    init_childs();
    init_counters();
    run_checker(0);
    run_console(0);
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
 */
void run_client(const char *hostname, const char *file)
{
	// On indique au manager de passer par un serveur
	set_server_ip(hostname);
	
    run_mono_client(file);
	
	// Comme on est en mode client, on ne devrait plus rien avoir dans les files
	// si c'est le cas, il s'agit d'une erreur
	if (datas_size() > 0) {
		char *def_file = malloc(sizeof(char) * 50);
        sprintf(def_file, "./failed_exit_eternityII_%i.back", getpid());
        char *def_analyse_file = malloc(sizeof(char) * 60);
        sprintf(def_analyse_file, "./failed_exit_eternityII-in_analyse_%i.back", getpid());
		backup(def_file);
        backup_analysed(def_analyse_file);
        free(def_file);
        free(def_analyse_file);
	}
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
	struct array_part *apart= read_parts(file);
	struct array_part *rotateParts = rotate_all_parts(apart);
	// On prépare les premières possiblitées en local
	map_big_array *map_parts = prepare_map_part(rotateParts);
	first_possibility(map_parts, rotateParts);
	free_bigarray(map_parts);
	free_array_part(rotateParts);
	free_array_part(apart);
	
	run_mono_client(file);
}


/**
 * @brief Initialise le thread chargé de faire les statistiques.
 * 
 * @param server 1 si le thread est pour le serveur, 0 pour le client.
 */
int run_checker(int server)
{
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
	pthread_attr_init(thread_attributes);
	pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	/* Création du thread */
	
	void *method= NULL;
	if(server == 1)
	{
		method = check_server;
	} else
	{
		method = check_client_threads;
	}
	
	if(0 != pthread_create(&thread, NULL, method, NULL))
	{
		// Non fatal : sous forte pression de ressources (trop de threads/process
		// demandés), on poursuit sans thread de statistiques plutôt que de
		// planter l'application.
		log_error("run_checker : pthread_create a échoué — pas de thread de statistiques\n");
		free(thread_attributes);
		return -1;
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
	return 0;
}

