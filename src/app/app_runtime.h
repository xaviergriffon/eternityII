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

/* ---- Parsing d'arguments CLI (main.c) ---- */

/**
 * @brief Parse un entier positif optionnel avec repli explicite.
 *
 * Extrait de `handle_tcpclient` (parsing du nombre de threads, argv[3]) pour
 * être testable hors de main.c (non linké dans le binaire de test).
 *
 * @param arg             Chaîne à parser (ex. argv[i]), ou NULL si absente.
 * @param fallback         Valeur renvoyée si `arg` est NULL ou si l'entier parsé est <= 0.
 * @param out_was_invalid  Sortie (NULL accepté) : 1 si `arg` était fourni mais
 *                         non positif (repli appliqué à cause d'une erreur, pas
 *                         d'une absence d'argument), 0 sinon.
 * @return `atoi(arg)` s'il est strictement positif, sinon `fallback`.
 */
int parse_positive_int_or_default(const char *arg, int fallback, int *out_was_invalid);

/**
 * @brief Classifie et interprète l'argument optionnel « nb_threads » de `tcpserver`.
 *
 * Usage : `tcpserver [nb_threads] [pieces.csv]` — le premier argument DOIT être
 * un nombre de threads, pas un fichier ; une erreur fréquente ("tcpserver
 * data/pieces16.csv") donnerait sinon `atoi(chemin) == 0` → serveur démarré
 * avec 0 thread de communication (accepte les connexions mais ne les sert
 * jamais). Extrait de `handle_tcpserver` pour être testable hors de main.c.
 *
 * @param arg                 argv[2], ou NULL si absent.
 * @param default_nb_threads   Valeur affectée à `*out_nb_threads` si `arg` est
 *                              absent, ou numérique mais invalide (<= 0).
 * @param out_nb_threads       Sortie : nombre de threads retenu.
 * @return TCPSERVER_ARG_AS_COUNT (0) si `arg` est absent ou interprété comme un
 *         compte de threads valide ; TCPSERVER_ARG_INVALID_COUNT (1) si `arg`
 *         ressemble à un nombre mais est invalide (<= 0, repli appliqué) ;
 *         TCPSERVER_ARG_AS_FILENAME (2) si `arg` ne ressemble pas à un nombre
 *         (traité comme le fichier de pièces).
 */
enum {
	TCPSERVER_ARG_AS_COUNT = 0,
	TCPSERVER_ARG_INVALID_COUNT = 1,
	TCPSERVER_ARG_AS_FILENAME = 2,
};
int parse_tcpserver_thread_arg(const char *arg, int default_nb_threads, int *out_nb_threads);

#endif /* app_runtime_h */
