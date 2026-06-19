#ifndef command_history_h
#define command_history_h

/*
 * Historique en mémoire des commandes saisies dans la console interactive.
 * Ring buffer simple, partagé par les deux builds (ANSI et ncurses). Accédé
 * uniquement depuis le thread console, donc aucun mutex.
 *
 * Indexation pour history_get : 0 = commande la plus récente, 1 = précédente, etc.
 */

/** @brief Ajoute une commande à l'historique. Ignore les chaînes vides et
 *         les doublons consécutifs (comme un shell). */
void history_add(const char *line);

/** @brief Retourne la commande à l'index donné (0 = la plus récente), ou
 *         NULL si index hors bornes. Le pointeur reste valide jusqu'au
 *         prochain history_add. */
const char *history_get(int index);

/** @brief Nombre actuel d'entrées dans l'historique (≤ HISTORY_MAX). */
int history_size(void);

#endif /* command_history_h */
