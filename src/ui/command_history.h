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

#include <stddef.h>

/** @brief Construit le chemin par défaut du fichier d'historique persistant
 *         (`$HOME/.eternityII_history`, repli sur `./.eternityII_history` si
 *         HOME est absent) dans @p buf de taille @p size.
 *  @return @p buf en cas de succès, NULL si @p buf est NULL, @p size nulle ou
 *          si le chemin ne tient pas dans le tampon. */
char *history_default_path(char *buf, size_t size);

/** @brief Charge l'historique depuis @p path, ligne par ligne, dans l'ordre
 *         chronologique (plus ancienne en premier). Respecte HISTORY_MAX et la
 *         dédup via history_add. L'absence du fichier n'est pas une erreur. */
void history_load(const char *path);

/** @brief Écrit l'historique dans @p path, une commande par ligne, dans l'ordre
 *         chronologique (plus ancienne en premier). Écriture atomique
 *         (fichier temporaire + rename) pour ne pas corrompre l'existant.
 *  @return 0 en cas de succès, -1 en cas d'échec (journalisé via log_error). */
int history_save(const char *path);

#endif /* command_history_h */
