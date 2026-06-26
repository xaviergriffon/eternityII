#ifndef logger_h
#define logger_h

/** @brief Affiche un message d'erreur suivi du message système associé à `errno` (sur stderr). */
void log_errno(const char *format, ...);

/** @brief Affiche un message d'erreur sur stderr. */
void log_error(const char *format, ...);

/**
 * @brief Journalise un message d'erreur fatal via log_error puis termine le
 *        process avec exit(EXIT_FAILURE).
 *
 * Funnel unique des erreurs fatales : centralise le motif répété
 * `log_error(...); exit(EXIT_FAILURE);`. Marqué `noreturn`, donc l'appelant n'a
 * pas à gérer le retour (ni à ajouter un exit() défensif derrière).
 *
 * Conçu comme point de couture pour les tests : les chemins fataux deviennent
 * testables via fork (cf. tests/fork_assert.h) — le fils appelle fatal_error et
 * sort, le parent inspecte WEXITSTATUS — et offrent un point central unique pour
 * une future gestion globale (dump de crash, sortie structurée…).
 */
__attribute__((noreturn, format(printf, 1, 2)))
void fatal_error(const char *format, ...);

/** @brief Affiche un message informatif sur stdout. */
void log_info(const char *format, ...);

/** @brief Affiche un message de débogage sur stdout. */
void log_debug(const char *format, ...);

/** @brief Affiche un message destiné à l'affichage interactif de la console. */
void log_console(const char *format, ...);

/**
 * @brief Enregistre un événement notable (nouveau record, demande non satisfaite,
 *        solution...) dans la zone d'affichage fixe et dans le journal `events.log`.
 *
 * Le message est horodaté, conservé dans un buffer circulaire (les N derniers sont
 * affichés dans la zone fixe en haut du terminal) et ajouté au fichier de log.
 * Si la zone fixe n'est pas active (sortie non interactive), le message est imprimé
 * normalement.
 */
void log_event(const char *format, ...);
/**
 * @brief Met à jour le bandeau de statistiques « live » (vitesse, stock, record…).
 *
 * En mode ncurses, remplace le contenu d'une ligne d'état fixe rafraîchie en
 * continu par le thread de statistiques. En mode ANSI (sans ncurses), c'est un
 * no-op : les statistiques restent consultables via la commande `check`.
 */
void log_status(const char *format, ...);
/** @brief Vide le tampon de sortie standard (pour `log_console`). */
void flush_console(void);
/** @brief Vide le tampon de sortie standard (pour `log_debug`). */
void flush_debug(void);
/** @brief Vide le tampon d'erreur standard (pour `log_error` / `log_errno`). */
void flush_error(void);
/** @brief Vide le tampon de sortie standard (pour `log_info`). */
void flush_info(void);
/** @brief Efface le contenu affiché par la console interactive. */
void clear_console(void);
/** @brief Installe la zone d'affichage fixe (région de défilement ANSI). À appeler depuis le thread console. */
void status_zone_init(void);
/** @brief Restaure le terminal (région de défilement complète). Enregistré via atexit par status_zone_init. */
void status_zone_teardown(void);

#ifdef USE_NCURSES
/**
 * @brief Boucle interactive de la console implémentée avec ncurses.
 *
 * Implémentée dans `logger_ncurses.c`. Remplace la boucle prompt/getcmdline
 * classique : gère les fenêtres ncurses (sortie scrollable, zone Events, ligne
 * de saisie), lit les caractères au clavier, dispatche les commandes via
 * `do_command_line`. Ne retourne pas (appelle `exit` quand l'utilisateur quitte).
 */
void nc_console_loop(void);
#endif /* USE_NCURSES */

#endif /* logger_h */
