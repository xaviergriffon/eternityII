/*
 * app_runtime.c — fonctions de plomberie du processus extraites de main.c
 * (gestion des signaux + bootstrap runtime), regroupées ici pour être testables
 * unitairement. Voir app_runtime.h. Le comportement est strictement identique à
 * l'original : les corps ont été déplacés verbatim depuis main.c.
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <pthread.h>

#include "app/app_runtime.h"
#include "app/static_variables.h"
#include "app/etii_statistic.h"
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
