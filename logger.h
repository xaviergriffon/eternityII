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
void flush_console(void);
void flush_debug(void);
void flush_error(void);
void flush_info(void);
void clear_console(void);
/** @brief Installe la zone d'affichage fixe (région de défilement ANSI). À appeler depuis le thread console. */
void status_zone_init(void);
/** @brief Restaure le terminal (région de défilement complète). Enregistré via atexit par status_zone_init. */
void status_zone_teardown(void);
#endif /* logger_h */
