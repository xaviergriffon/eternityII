#ifndef logger_h
#define logger_h
void log_errno(const char *format, ...);
void log_error(const char *format, ...);
void log_info(const char *format, ...);
void log_debug(const char *format, ...);
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
void flush_console(void);
void flush_debug(void);
void flush_error(void);
void flush_info(void);
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
