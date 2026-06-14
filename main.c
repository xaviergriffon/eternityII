#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#include "static_variables.h"
#include "console.h"
#include "possibility.h"

#include "datamanager.h"
#include "tcpserver.h"
#include "tcpclient.h"
#include "part.h"
#include "lifo.h"
#include "etii_protocol.h"
#include "readdata.h"
#include "etii_client.h"
#include "etii_search.h"
#include "etii_server.h"
#include "local_socket.h"
#include "command_lines.h"
#include "etii_statistic.h"
#include "logger.h"
#include "ipc_protocol.h"

void handle_tcpclient(int argc, const char *argv[]);
void handle_tcpserver(int argc, const char *argv[]);
void handle_test(const char *arg);
void failed_arg(void);

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
void init_childs(void);
void init_sigchld_sigaction(void);
void init_signals(void);
void configure_child_signals(void);
void wait_child(void);
int run_checker(int server);
int init_counters(void);
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
        NB_THREADS = atoi(argv[3]);
    }
    if (argc >= 5) {
        if (pruner_mode) {
            // tcppruner [serveur] [nb_threads] [pieces.csv] : pas de stock local
            parts_files = (char *)(argv[4]);
        } else {
            max_stock_by_thread = atoi(argv[4]);
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

    if (argc >= 6) {
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
                    log_error("fork error %i\n", errno);
                    fork_error++;
                    if (fork_error > 10) {
                        log_error("too many fork error %i\n", fork_error);
                        // ON arrête le programme en indiquant via le signal et met l'indice au nombre de threads.
                        request = REQUEST_STOP;
                        c = NB_THREADS;
                    }
                    c--;
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
        // Les forks sont terminés : on peut démarrer les threads du parent.
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
    if (argc >= 3) {
        log_info("arg 2 : %s", argv[2]);
        NB_THREADS = atoi(argv[2]);
    }
    log_info("Nb threads : %i\n", NB_THREADS);
    init_childs();
    init_signals();
    init_counters();
    run_checker(1);
    run_console(1);
    if (argc >= 4) {
        parts_files = (char *)(argv[3]);
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
 * @brief Affiche un message d'erreur indiquant que les arguments sont incorrects.
 */
void failed_arg(void)
{
	log_error("Indiquer parametre suivant :\ntcpserver [nombre de threads] [pieces.csv]\ntcpclient [serveur] [nb_threads] [max_stock] [pieces.csv]\ntcppruner [serveur] [nb_threads] [pieces.csv]\n"
#ifdef WITH_CUDA
	          "gpupruner [serveur] [nb_threads] [pieces.csv]\n"
#endif // WITH_CUDA
	);
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
		log_error("Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
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
 * @brief Initialise les compteurs utilisés dans le programme.
 *
 * Cette fonction configure et initialise tous les compteurs nécessaires
 * au bon fonctionnement du programme. Elle doit être appelée au début du
 * programme avant que les compteurs ne soient utilisés.
 *
 * @return int Retourne 0 si l'initialisation est réussie, ou un code
 * d'erreur non nul en cas d'échec.
 */
int init_counters(void)
{
	counters = malloc(sizeof(unsigned long long) * NB_THREADS);
	lastfilesize = malloc(sizeof(unsigned long long) * NB_THREADS);
	
	for(int c = 0; c < NB_THREADS;c++)
	{
		counters[c] = 0;
		lastfilesize[c] = 0;
	}

	return 0;
}

/** @brief Gestionnaire de signal no-op (utilisé pour SIGPIPE). */
void signal_ignored(int sig) {
#ifdef DEBUG_SIGNAL
    log_debug("catch signal %s\n", strsignal(sig));
#endif
}

/**
 * @brief Gestionnaire de signal d'arrêt (SIGINT, SIGTERM, SIGHUP, SIGQUIT).
 *
 * Positionne `request = REQUEST_STOP` et propage le signal à tous les processus
 * enfants (si le processus courant est le parent). En mode serveur, appelle
 * `exit(0)` directement.
 *
 * @param sig Numéro du signal reçu.
 */
void signal_end_handler(int sig)
{
#ifdef DEBUG_SIGNAL
    log_console("receive signal : %i\n", sig);
    flush_console();
#endif // DEBUG_SIGNAL
	request = REQUEST_STOP;
    if (childrens_pid != NULL && parent_pid == getpid()) {
		for (int c = 0; c < NB_THREADS; c++) {
            if (childrens_pid[c] > 0) {
                kill(childrens_pid[c], sig);
            }
		}
#ifdef DEBUG_SIGNAL
    } else if (childrens_pid != NULL && parent_pid != getpid()) {
        log_info("child %d receive signal %s\n", getpid(), strsignal(sig));
#endif
    }

    if (server == 1) {
        exit(0);
    }
}

/**
 * @brief Gestionnaire de SIGCHLD : récolte les statuts des processus enfants terminés.
 *
 * Appelle `waitpid(-1, WNOHANG)` en boucle pour éviter les zombies. En mode
 * DEBUG_SIGNAL, journalise les codes de sortie et les signaux reçus.
 *
 * @param signal Numéro du signal (toujours SIGCHLD).
 */
void sigchld_handler(int signal) {
	// lecture du statut pour éviter les process zombie
	int status = 0;
#ifdef DEBUG_SIGNAL
    log_debug("sigchld_handler\n");
	pid_t wpid;
    while(0 < (wpid = waitpid(-1, &status, WNOHANG))) {
        log_debug("waitpid %d\n", (int)wpid);
    }
	
    log_debug("Exit status of %d was %d\n", (int)wpid, status);
	if(WIFEXITED(status)) {
		/* The child process exited normally */
		log_debug("Exit value %d\n", WEXITSTATUS(status));
	} else if(WIFSIGNALED(status)) {
		/* The child process was killed by a signal. Note the use of strsignal
			to make the output human-readable. */
		log_debug("Killed by %s\n", strsignal(WTERMSIG(status)));
	}
    flush_debug();
#else
    while(0 < waitpid(-1, &status, WNOHANG));
#endif // DEBUG_SIGNAL
}

/**
 * @brief Initialise les signaux pour les threads enfants.
 */
void init_sigchld_sigaction(void) {
 	struct sigaction sa;
     //memset(&sa, 0, sizeof *sa);
     sa.sa_handler = sigchld_handler;
     sa.sa_flags = SA_SIGINFO|SA_RESTART;
     sigemptyset(&(sa.sa_mask));
     if (sigaction(SIGCHLD, &sa, NULL) != 0) {
         log_error("Problème avec sigaction()\n");
         exit(EXIT_FAILURE);
     }
 }

/**
 * @brief Attend la terminaison de tous les processus enfants (mode client parent).
 *
 * Boucle sur `wait()` jusqu'à ce qu'il n'y ait plus d'enfants. En mode
 * DEBUG_SIGNAL, journalise les codes de sortie de chaque enfant.
 */
void wait_child(void) {
    log_info("start wait_child\n");
    int status = 0;
    pid_t wpid;
    /* Boucle tant que wait() réussit OU est interrompu par un signal.
       Avec ncurses (SIGWINCH au redimensionnement) ou tout autre signal
       sans SA_RESTART, wait() peut retourner -1 avec errno==EINTR : il ne
       faut PAS sortir, sinon le parent terminerait alors que des enfants
       sont encore vivants (qui deviendraient des orphelins). On ne quitte
       que sur ECHILD (plus d'enfants) ou une vraie erreur. */
    while (1) {
        wpid = wait(&status);
        if (wpid > 0) {
#ifdef DEBUG_SIGNAL
            log_debug("Exit status of %d was %d\n", (int)wpid, status);
            if (WIFEXITED(status)) {
                log_debug("Exit value %d\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                log_debug("Killed by %s\n", strsignal(WTERMSIG(status)));
            }
#else
            (void)wpid;
            (void)status;
#endif // DEBUG_SIGNAL
            continue;
        }
        if (wpid == -1 && errno == EINTR) {
            /* Interrompu par un signal (ex: SIGWINCH installé par ncurses
               sans SA_RESTART). On retente. */
            continue;
        }
        /* Plus d'enfants à attendre (ECHILD) ou erreur fatale : on sort. */
        break;
    }
    log_info("end wait_child\n");
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
                    for (int cpt = 0; cpt < NB_THREADS; cpt++) {
                        if (strcmp(claddr->sun_path, forkId[cpt]) == 0) {
                            memcpy(&fork_statistics[cpt], buf + 1,
                                   sizeof(struct client_statistics));
                            break;
                        }
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
            log_error("Problème avec pthread_create()\n");
            free(thread_attributes);
            exit(EXIT_FAILURE);
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
		log_error("run_fork_thread Problème avec pthread_create()\n");
		free(thread_attributes);
		exit(EXIT_FAILURE);
	}
	pthread_attr_destroy(thread_attributes);
	free(thread_attributes);
}

/**
 * @brief Initialise les attributs des threads enfants.
 */
void init_childs(void) {
    childrens_pid = malloc(sizeof(pid_t) * NB_THREADS);
    forkId = malloc(sizeof(char *) * NB_THREADS);
    fork_statistics = malloc(sizeof(struct client_statistics) * NB_THREADS);
    memset(fork_statistics, 0, sizeof(struct client_statistics) * NB_THREADS);
    for (int c = 0; c < NB_THREADS; c++) {
        childrens_pid[c] = -1;
        forkId[c] = malloc(sizeof(char) * 300);
        forkId[c][0] = '\0';
        
        fork_statistics[c].analyses_in_stock = 0;
        fork_statistics[c].possibilities_in_stock = 0;
        fork_statistics[c].shots_per_second = 0;
    }
}

/**
 * @brief Initialise les gestionnaires de signaux pour l'application.
 *
 * Cette fonction configure les gestionnaires de signaux nécessaires pour s'assurer que
 * l'application peut gérer divers signaux de manière appropriée. Elle est
 * généralement appelée pendant la phase d'initialisation du programme.
 */
void init_signals(void) {
    struct sigaction sa;
    sa.sa_handler = signal_end_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    // Configure les signaux pour le processus principal et les threads enfants
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Ignorer SIGPIPE
    signal(SIGPIPE, signal_ignored);
}


/**
 * @brief Configure les signaux pour les threads enfants.
 *
 * Cette fonction est appelée dans chaque thread enfant pour s'assurer qu'ils
 * écoutent les signaux comme SIGINT.
 */
void configure_child_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    struct sigaction sa;
    sa.sa_handler = signal_end_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    // Configure SIGINT pour les threads enfants
    sigaction(SIGINT, &sa, NULL);
}
