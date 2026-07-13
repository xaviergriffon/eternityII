/*
 * app_runtime.c — fonctions de plomberie du processus extraites de main.c
 * (gestion des signaux + bootstrap runtime), regroupées ici pour être testables
 * unitairement. Voir app_runtime.h. Le comportement est strictement identique à
 * l'original : les corps ont été déplacés verbatim depuis main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#include "app/app_runtime.h"
#include "app/static_variables.h"
#include "app/etii_client.h"
#include "app/etii_server.h"
#include "app/etii_statistic.h"
#include "core/datamanager.h"
#include "core/best_board.h"
#include "net/local_socket.h"
#include "net/ipc_protocol.h"
#include "ui/command_lines.h"
#include "ui/logger.h"

/**
 * @brief Affiche un message d'erreur indiquant que les arguments sont incorrects.
 */
void failed_arg(void)
{
	log_error("Indiquer parametre suivant :\ntcpserver [nombre de threads] [pieces.csv]\ntcpclient [serveur] [nb_threads] [max_stock] [pieces.csv]\ntcppruner [serveur] [nb_threads] [pieces.csv]\n"
#ifdef WITH_CUDA
	          "gpupruner [serveur] [nb_threads] [pieces.csv]\n"
#endif // WITH_CUDA
	          "Option (n'importe où) : --stop-on-solution "
	          "(s'arrêter à la 1re solution ; défaut : continuer)\n"
	);
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

int parse_positive_int_or_default(const char *arg, int fallback, int *out_was_invalid)
{
	if (out_was_invalid != NULL) {
		*out_was_invalid = 0;
	}
	if (arg == NULL) {
		return fallback;
	}
	int n = atoi(arg);
	if (n > 0) {
		return n;
	}
	if (out_was_invalid != NULL) {
		*out_was_invalid = 1;
	}
	return fallback;
}

int parse_tcpserver_thread_arg(const char *arg, int default_nb_threads, int *out_nb_threads)
{
	if (arg == NULL) {
		*out_nb_threads = default_nb_threads;
		return TCPSERVER_ARG_AS_COUNT;
	}
	int n = atoi(arg);
	char c0 = arg[0];
	int looks_numeric = (c0 == '+' || c0 == '-' || (c0 >= '0' && c0 <= '9'));
	if (n > 0) {
		*out_nb_threads = n;
		return TCPSERVER_ARG_AS_COUNT;
	}
	*out_nb_threads = default_nb_threads;
	if (looks_numeric) {
		/* Nombre fourni mais non valide (0 ou négatif) : on garde le défaut. */
		return TCPSERVER_ARG_INVALID_COUNT;
	}
	/* Pas un nombre : traité comme le fichier de pièces, nombre de threads inchangé. */
	return TCPSERVER_ARG_AS_FILENAME;
}

const char *parse_tcpclient_args(int argc, const char *argv[])
{
    NB_THREADS = 1;
    const char *serverIp = "localhost";
    if (argc >= 3) {
        serverIp = argv[2];
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
    // argv[5] = pieces.csv pour un client de recherche. Pour un pruner, argv[5]
    // est la taille de lot (cf. plus haut) et le fichier de pièces reste argv[4].
    if (!pruner_mode && argc >= 6) {
        parts_files = (char *)(argv[5]);
    }
#ifdef DEBUG_IN_MONO_PROCESS
    NB_THREADS = 1;
#endif
    return serverIp;
}

void backup_failed_exit(void)
{
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

/** @brief Gestionnaire de signal no-op (utilisé pour SIGPIPE). */
void signal_ignored(int sig) {
    (void)sig;
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
	(void)signal;
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
    /* Pas de SA_RESTART : on veut que les appels bloquants (accept, recvfrom,
       wait) renvoient EINTR sur réception d'un signal d'arrêt, afin que leurs
       boucles puissent constater request==REQUEST_STOP et sortir proprement. */
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

/* ---- IPC parent<->enfants (sockets Unix UDP locales) ---- */

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
    // Débit des études de prunage : même fenêtre glissante, sur pruner_cells_studied
    unsigned long long oldPPS[5];
    for (int c = 0; c < 5; c++) {
        oldSPS[c] = 0;
        oldPPS[c] = 0;
    }
    int s = 0;
    int t;
    unsigned long long last_counter = 0;
    unsigned long long last_prune_cells = 0;
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

        // Même mécanique pour le débit des études de prunage : cumul des cases
        // étudiées par les contrôles de possibilité (pruner, rmnonext) ET par
        // le forward-checking de la recherche — flux disjoint de `counters`.
        unsigned long long prune_cells = pruner_cells_studied
            + __atomic_load_n(&fc_cells_studied, __ATOMIC_RELAXED);
        unsigned long long pps = 0;
        if (prune_cells >= last_prune_cells) {
            pps = prune_cells - last_prune_cells;
        } else {
            // le compteur a fait un tour
            pps = ((pps - 1) - last_prune_cells) + prune_cells;
        }
        last_prune_cells = prune_cells;
        oldPPS[s] = pps;

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

        int mp = 0;
        for (int i = 0; i < 5; i++) {
            if (oldPPS[i] > 0) {
                mp++;
                pps += oldPPS[i];
            }
        }
        if (mp > 0) {
            pps = pps / mp;
        } else {
            pps = 0;
        }
        statistic->pruner_cells_per_second = pps;

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
        statistic->pruner_cells_studied = pruner_cells_studied;
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

        // Représentation du meilleur plateau LOCAL à ce fork : envoyée en plus
        // des stats, mais UNIQUEMENT quand ce fork bat son propre record
        // (jamais à chaque tour, contrairement à IPC_MSG_STATS ci-dessus) —
        // cf. core/best_board.h. `last_sent_best_board` est statique au thread :
        // un fork ne renvoie donc le plateau qu'une seule fois par record.
        {
            static uint16_t last_sent_best_board = 0;
            struct possibility_packet local_board;
            uint16_t local_alloc = 0;
            if (best_board_get(&g_search_best_board, &local_board, &local_alloc)
                && local_alloc > last_sent_best_board) {
                char boardbuf[1 + sizeof(struct possibility_packet)];
                boardbuf[0] = IPC_MSG_BEST_BOARD;
                memcpy(boardbuf + 1, &local_board, sizeof(local_board));
                sendto(fork_checker_socket_id, boardbuf, sizeof boardbuf, MSG_DONTWAIT,
                       (struct sockaddr *) main_addr, sizeof(struct sockaddr_un));
                last_sent_best_board = local_alloc;
            }
        }
		sleep(1);
	}
    free(statistic);

	return NULL;
}

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

        /* recvfrom() n'est PAS garanti terminer sun_path par un octet nul :
         * le noyau n'écrit que `len` octets (convention SUN_LEN, qui exclut
         * le terminateur). Sans ce nul explicite, find_fork_index() (qui
         * compare via strcmp) peut lire au-delà de l'adresse reçue, dans la
         * mémoire non initialisée de `claddr` (malloc, jamais remis à zéro). */
        {
            size_t addr_path_len = (size_t)len - offsetof(struct sockaddr_un, sun_path);
            if (addr_path_len >= sizeof(claddr->sun_path)) {
                addr_path_len = sizeof(claddr->sun_path) - 1;
            }
            claddr->sun_path[addr_path_len] = '\0';
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

            case IPC_MSG_BEST_BOARD:
                if (numBytes >= (ssize_t)(1 + sizeof(struct possibility_packet))) {
                    struct possibility_packet board;
                    memcpy(&board, buf + 1, sizeof(board));
                    // Agrégat du process PARENT sur ses forks : même règle
                    // « premier à dépasser gagne » que g_search_best_board
                    // côté fork (cf. core/best_board.h). C'est cette instance
                    // que le canal de contrôle sert en réponse à
                    // CTRL_GET_BEST_BOARD (etii_control.c).
                    best_board_try_record(&g_client_aggregate_best_board, &board, board.alloc);
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
