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
        lastcheck = calloc(2000, sizeof(char));

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
        free(lastcheck);
    } else {
        failed_arg();
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}

void run_client(const char *hostname, const char *file);
void run_server_thread(int *socket_id);
int run_checker(int server);
int run_fork_checker(struct sockaddr_un *main_addr);

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
        int n = atoi(argv[3]);
        if (n > 0) {
            NB_THREADS = n;
        } else {
            // Un nombre de threads <= 0 (ou non numérique) donnerait 0 process de
            // travail : le client ne ferait rien. On retombe sur 1.
            log_error("nombre de threads invalide (\"%s\") — 1 thread par défaut\n", argv[3]);
            NB_THREADS = 1;
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
        int n = atoi(argv[2]);
        char c0 = argv[2][0];
        int looks_numeric = (c0 == '+' || c0 == '-' || (c0 >= '0' && c0 <= '9'));
        if (n > 0) {
            NB_THREADS = n;
            if (argc >= 4) file_arg = 3;
        } else if (looks_numeric) {
            // Nombre fourni mais non valide (0 ou négatif) : on garde le défaut.
            log_error("nombre de threads invalide (\"%s\") — %i threads par défaut\n", argv[2], NB_THREADS);
            if (argc >= 4) file_arg = 3;
        } else {
            // Pas un nombre : l'utilisateur a probablement passé le fichier ici.
            // On garde le nombre de threads par défaut et on traite cet argument
            // comme le fichier de pièces (sinon : serveur muet à 0 thread).
            log_error("1er argument (\"%s\") interprété comme fichier de pièces ; "
                      "le nombre de threads attendu à cette position est absent — "
                      "%i threads par défaut. Usage : tcpserver [nb_threads] [pieces.csv]\n",
                      argv[2], NB_THREADS);
            file_arg = 2;
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
void run_fork_thread(int *socket_id);

/**
 * @brief Thread de statistiques du processus enfant (fork).
 *
 * Crée le socket Unix local de l'enfant (`etii_fork.<pid>`), démarre le thread
 * `fork_udp` pour la réception des commandes IPC, puis toutes les secondes :
 * - calcule le débit moyen sur 5 s (`shots_per_second`),
 * - compte les possibilités en stock et en analyse,
 * - envoie une structure `client_statistics` au parent via `sendto`.
 *
 * @param param Pointeur vers la `sockaddr_un` du socket principal du parent.
 * @return      NULL.
 */
void *fork_checker(void *param) {
	struct sockaddr_un *main_addr = (struct sockaddr_un *)param;
	char socket_fork[50];
    int sp_len = sprintf(socket_fork, "etii_fork.%d", getpid());
    socket_fork[sp_len] = '\0';
    struct sockaddr_un *fork_addr = build_sockaddr(socket_fork);
#ifdef DEBUG_LOCAL_SOCKET
    log_debug("socket fork : %s\n", socket_fork);
#endif // DEBUG_LOCAL_SOCKET
    fork_checker_socket_id = build_udp_local_socket(fork_addr);
    free(fork_addr);

	log_info("fork_checker_socket_id: %i\n", fork_checker_socket_id);
	if (fork_checker_socket_id > 0) {
		int *so = &fork_checker_socket_id;
		run_fork_thread(so);
	}
    
    // TPS tests per second (5 secondes)
    unsigned long long oldSPS[5];
    for (int c = 0; c < 5; c++) {
        oldSPS[c] = 0;
    }
    int s = 0;
    int t;
    unsigned long long last_counter = 0;
    struct client_statistics *statistic = calloc(1, sizeof(struct client_statistics));
	while(request != REQUEST_STOP && fork_checker_socket_id > 0) {
        unsigned long long counter = 0;
        unsigned long long possibilities_in_stock = 0;
        for (t = 0; t < NB_THREADS; t++) {
            counter += counters[t];
            possibilities_in_stock += lastfilesize[t];
        }
        unsigned long long sps = 0;
        if (counter >= last_counter) {
            sps = counter - last_counter;
        } else {
            // le compteur a fait un tour
            sps = ((sps - 1) - last_counter) + counter;
        }
        last_counter = counter;
        oldSPS[s] = sps;
        s++;
        if (s >= 5) {
            s = 0;
        }
        
        // on effectue une moyenne sur 5 secondes
        // les valeurs à 0 ne sont pas comptées
        int m = 0;
        for (int i = 0; i < 5; i++) {
            if (oldSPS[i] > 0) {
                m++;
                sps += oldSPS[i];
            }
        }
        if (m > 0) {
            sps = sps / m;
        } else {
            sps = 0;
        }
        statistic->shots_per_second = sps;
        
        int analyses_in_stock = 0;
        for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
            analyses_in_stock += file_analysed_size(f);
        }
        statistic->analyses_in_stock = analyses_in_stock;
        statistic->possibilities_in_stock = possibilities_in_stock;
        statistic->max_result = max_result;
#if FORWARD_CHECK_K > 0
        // Statistiques du forward-checking : cumuls du processus, agrégés et
        // affichés par le parent dans le rapport de la commande `check`.
        statistic->fc_attempts = __atomic_load_n(&fc_attempts, __ATOMIC_RELAXED);
        statistic->fc_pruned = __atomic_load_n(&fc_pruned, __ATOMIC_RELAXED);
        for (int j = 1; j <= FORWARD_CHECK_K; j++) {
            statistic->fc_pruned_at[j] = __atomic_load_n(&fc_pruned_at[j], __ATOMIC_RELAXED);
        }
#endif // FORWARD_CHECK_K > 0
        // Statistiques du client pruner (restent à zéro en mode recherche)
        statistic->pruner_checked = pruner_checked;
        statistic->pruner_removed = pruner_removed;
        /* On préfixe le datagramme d'un octet de type pour permettre au
           parent de multiplexer stats / logs / événements sur le même
           socket. Voir ipc_protocol.h. */
        char ipcbuf[1 + sizeof(struct client_statistics)];
        ipcbuf[0] = IPC_MSG_STATS;
        memcpy(ipcbuf + 1, statistic, sizeof(struct client_statistics));
#ifdef DEBUG_LOCAL_SOCKET
        if(
#endif // DEBUG_LOCAL_SOCKET

		sendto(fork_checker_socket_id, ipcbuf, sizeof ipcbuf, MSG_DONTWAIT, (struct sockaddr *) main_addr,
                               sizeof(struct sockaddr_un))
#ifdef DEBUG_LOCAL_SOCKET
           != (ssize_t)sizeof ipcbuf ) {
            log_debug("fork_checker cl %d error %i sendto : %s\n", getpid(), errno, strerror(errno));
        }
#else
        ;
#endif // DEBUG_LOCAL_SOCKET
		sleep(1);
	}
    free(statistic);
    
	return NULL;
}

/**
 * @brief Exécute le vérificateur de fork dans un thread détaché.
 *
 * Cette fonction initialise les attributs du thread, définit le thread comme détaché,
 * et crée un nouveau thread pour exécuter le vérificateur de fork. Si la création du thread échoue,
 * elle enregistre un message d'erreur et quitte le programme.
 *
 * @param main_addr Un pointeur vers une structure sockaddr_un contenant l'adresse principale.
 * @return Retourne 0 en cas de succès.
 */
int run_fork_checker(struct sockaddr_un *main_addr)
{
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
	pthread_attr_init(thread_attributes);
	pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	
    // Start the fork checker.
	if(0 != pthread_create(&thread, thread_attributes, fork_checker, main_addr))
	{
		log_error("Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
    
    // Clean up the thread attributes.
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
    
	return 0;
}

/**
 * @brief Thread de réception des statistiques IPC en provenance des processus enfants (parent).
 *
 * Reçoit des structures `client_statistics` via `recvfrom` sur le socket Unix principal,
 * identifie l'enfant émetteur en comparant `sun_path` avec `forkId[]`, et met à jour
 * `fork_statistics[cpt]` par copie mémoire.
 *
 * @param param Pointeur vers l'entier `socket_id` du socket UDP Unix principal.
 * @return      NULL.
 */
void *server_tcp(void *param) {
    int socket_id = *(int*)param;

    struct sockaddr_un *claddr = malloc(sizeof(struct sockaddr_un));
    ssize_t numBytes;
    socklen_t len = sizeof(struct sockaddr_un);

    /* Tampon dimensionné pour le plus gros message attendu :
       1 octet de type + max(struct client_statistics, IPC_LINE_MAX+1). */
    size_t bufsz = 1 + sizeof(struct client_statistics);
    if (bufsz < 1 + IPC_LINE_MAX + 1) bufsz = 1 + IPC_LINE_MAX + 1;
    char *buf = malloc(bufsz);

    while (request != REQUEST_STOP) {
        len = sizeof(struct sockaddr_un);
        numBytes = recvfrom(socket_id, buf, bufsz, 0,
                            (struct sockaddr *) claddr, &len);
        if (numBytes == -1) {
            if (request != REQUEST_STOP) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EBADF) {
#ifdef DEBUG_LOCAL_SOCKET
                    log_debug("srv error invalid descriptor on recvfrom\n");
                    flush_debug();
#endif // DEBUG_LOCAL_SOCKET
                    break;
                }
                log_errno("srv error on recvfrom => ");
                flush_error();
            }
            continue;
        }
        if (numBytes < 1) {
            continue;
        }

        int8_t type = (int8_t)buf[0];
        switch (type) {
            case IPC_MSG_STATS:
                if (numBytes >= (ssize_t)(1 + sizeof(struct client_statistics))) {
                    int cpt = find_fork_index(claddr->sun_path, forkId, NB_THREADS);
                    if (cpt >= 0) {
                        memcpy(&fork_statistics[cpt], buf + 1,
                               sizeof(struct client_statistics));
                    }
                }
                break;

            case IPC_MSG_LOG_INFO:
                buf[numBytes] = '\0';
                log_info("%s", buf + 1);
                break;
            case IPC_MSG_LOG_ERROR:
                buf[numBytes] = '\0';
                log_error("%s", buf + 1);
                break;
            case IPC_MSG_LOG_DEBUG:
                buf[numBytes] = '\0';
                log_debug("%s", buf + 1);
                break;
            case IPC_MSG_LOG_CONSOLE:
                buf[numBytes] = '\0';
                log_console("%s", buf + 1);
                break;
            case IPC_MSG_EVENT:
                buf[numBytes] = '\0';
                log_event("%s", buf + 1);
                break;

            default:
                /* Type inconnu : on ignore silencieusement (compat avenir). */
                break;
        }
    }
    free(claddr);
    free(buf);

    return NULL;
}

/**
 * @brief Exécute le serveur TCP dans un thread détaché.
 *
 * Cette fonction initialise les attributs du thread, définit le thread comme détaché,
 * et crée un nouveau thread pour exécuter le serveur TCP. Si la création du thread échoue,
 * elle enregistre un message d'erreur et quitte le programme.
 *
 * @param socket_id Un pointeur vers un entier contenant l'identifiant du socket.
 */
void run_server_thread(int *socket_id) {
    log_info("srv  socket_id %i\n", *socket_id);
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, server_tcp, socket_id))
        {
            // Non fatal : on poursuit sans thread de réception des statistiques
            // plutôt que de planter (et d'orphaniser les process enfants).
            log_error("run_server_thread : pthread_create a échoué — pas de réception de statistiques\n");
            free(thread_attributes);
            return;
        }
        pthread_attr_destroy(thread_attributes);
        free(thread_attributes);
}

/**
 * @brief Thread de réception des commandes IPC dans les processus enfants.
 *
 * Reçoit des chaînes de commande envoyées par le parent via UDP Unix et les
 * délègue à `do_command_line`. Configure les signaux enfant via
 * `configure_child_signals` au démarrage.
 *
 * @param param Pointeur vers l'entier `socket_id` du socket UDP Unix de l'enfant.
 * @return      NULL.
 */
void *fork_udp(void *param) {
    // Configure les signaux pour ce thread
    configure_child_signals();

	int socket_id = *(int*)param;
    struct sockaddr_un *srv_addr = malloc(sizeof(struct sockaddr_un));
    ssize_t numBytes;
    socklen_t len = sizeof(struct sockaddr_un);
    char *value = malloc(sizeof(char) * 100);
    while (request != REQUEST_STOP) {
        numBytes = recvfrom(socket_id, value, sizeof(char) * 100, 0,
                            (struct sockaddr *) srv_addr, &len);
        if (numBytes == -1) {
            if (request != REQUEST_STOP) {
                log_errno("cl error on recvfrom => ");
                flush_error();
            }
            continue;
        }
		value[numBytes] = '\0';
        do_command_line(value);
    }
    free(srv_addr);
    free(value);
    return NULL;
}

/**
 * @brief Démarre le thread `fork_udp` (réception des commandes IPC) en mode détaché.
 * @param socket_id Pointeur vers le descripteur du socket UDP Unix de l'enfant.
 */
void run_fork_thread(int *socket_id) {
	log_info("cl socket_id %i\n", *socket_id);
	pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if(0 != pthread_create(&thread, thread_attributes, fork_udp, socket_id))
	{
		// Non fatal : ce process enfant tourne sans thread de commandes IPC
		// plutôt que de mourir sous la pression des ressources.
		log_error("run_fork_thread : pthread_create a échoué — pas de commandes IPC pour ce process\n");
		free(thread_attributes);
		return;
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
}

