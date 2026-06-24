#ifndef app_runtime_h
#define app_runtime_h

#include <sys/types.h>

/*
 * Fonctions « plumbing » du processus, extraites de main.c pour être testables
 * unitairement : main.c définit main() et n'est donc pas linké dans le binaire
 * de test. Ces fonctions ne dépendent que des globales de static_variables.h.
 *
 * Deux familles :
 *   - gestion des signaux (handlers + installation des dispositions) ;
 *   - bootstrap runtime (allocation des compteurs et des contextes enfants,
 *     message d'usage).
 */

/* ---- Signaux ---- */

/** @brief Handler no-op (utilisé pour SIGPIPE). */
void signal_ignored(int sig);

/** @brief Handler d'arrêt : positionne request=REQUEST_STOP, propage aux enfants
 *         (si parent), et exit(0) en mode serveur. */
void signal_end_handler(int sig);

/** @brief Handler SIGCHLD : récolte les enfants terminés (waitpid WNOHANG). */
void sigchld_handler(int signal);

/** @brief Installe sigchld_handler sur SIGCHLD. */
void init_sigchld_sigaction(void);

/** @brief Installe signal_end_handler (SIGINT/HUP/QUIT/TERM) + ignore SIGPIPE. */
void init_signals(void);

/** @brief Débloque SIGINT et installe signal_end_handler (SA_RESTART) pour un thread enfant. */
void configure_child_signals(void);

/** @brief Attend la terminaison de tous les enfants (boucle wait(), tolère EINTR). */
void wait_child(void);

/* ---- Bootstrap runtime ---- */

/** @brief Alloue et remet à zéro les compteurs par thread. @return 0. */
int  init_counters(void);

/** @brief Alloue/initialise les contextes des processus enfants (pids, forkId, stats). */
void init_childs(void);

/** @brief Affiche le message d'usage (arguments invalides). */
void failed_arg(void);

#endif /* app_runtime_h */
