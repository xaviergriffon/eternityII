/**
 * @file command_lines.h
 * @brief Méthodes pour prendre en charge des instructions en format texte.
 */
#ifndef command_lines_h
#define command_lines_h

#include <stdio.h>

/**
 * @brief Execute la commande au format texte.
 *
 * @param[in] command instruction au format texte.
 * @return 0 si l'instruction a été correctement interpretée.
 *          Sinon -1 pour commande inconnue ou un négatif correspondant au code erreur de la commande.
 */
int do_command_line(char *command);

/**
 * @brief Calcule la transition d'état pour les commandes `pause`/`resume`, sans effet de bord.
 *
 * Extrait de `pause_interpreter`/`resume_interpreter` pour être testable sans
 * passer par le global `request` — même philosophie que `parse_cli_options`
 * (src/app/static_variables.c) : la logique de décision reste une fonction pure,
 * les interpréteurs se contentent de lire/écrire le global et de logger.
 *
 * Règles :
 * - `want_pause == 1` (commande `pause`) : depuis `REQUEST_CONTINUE` ou
 *   `REQUEST_PAUSE`, bascule vers `REQUEST_ADMIN_PAUSE`. Déjà en
 *   `REQUEST_ADMIN_PAUSE` ou en `REQUEST_STOP` : no-op (état inchangé).
 * - `want_pause == 0` (commande `resume`) : depuis `REQUEST_ADMIN_PAUSE`,
 *   bascule vers `REQUEST_CONTINUE`. Tout autre état : no-op (état inchangé) —
 *   notamment `REQUEST_PAUSE`, qui n'est du ressort que du régulateur de débit
 *   (`control_step`, src/app/etii_client.c).
 *
 * @param current    Valeur courante de `request`.
 * @param want_pause  1 pour une demande de pause admin, 0 pour une reprise.
 * @return            Le nouvel état de `request` (peut être égal à `current`).
 */
int admin_pause_transition(int current, int want_pause);
#endif /* command_lines_h */
