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

/**
 * @brief Borne une valeur de taille de lot pruner à [1, PRUNER_BATCH_MAX].
 *
 * Extrait de `pruner_batch_interpreter` pour être réutilisé par
 * `admin_apply_remote_command` sans dupliquer les bornes.
 *
 * @param v Valeur brute demandée.
 * @return  `v` borné à [1, PRUNER_BATCH_MAX].
 */
int pruner_batch_clamp(int v);

/// `admin_apply_remote_command` a appliqué la commande avec succès.
#define ADMIN_CMD_OK 0
/// Commande reconnue mais arguments manquants/invalides.
#define ADMIN_CMD_BAD_ARGS (-1)
/// Commande absente de la liste blanche `control_command_allowed`.
#define ADMIN_CMD_FORBIDDEN (-2)

/**
 * @brief Applique une commande admin distante (whitelistée) directement sur
 *        l'état serveur, sans passer par `do_command_line`.
 *
 * `do_command_line` (et tous ses interpréteurs) tokenise via `strtok`, qui
 * utilise un curseur global non réentrant : un appel concurrent depuis un
 * thread HTTP (ou tout autre appelant asynchrone) pendant que le thread
 * console ou le canal de contrôle tokenise déjà une ligne corromprait les
 * deux découpages. Cette fonction relit `line` avec `strtok_r` (curseur
 * local) et applique directement les quelques commandes admin sûres, sans
 * toucher à l'état global de `strtok`.
 *
 * Ne couvre que les commandes acceptées par `control_command_allowed`
 * (control_protocol.h) : `pause`, `resume`, `limit <n>`,
 * `maxStockByThread <n>`, `prunerBatch <n>`. Toute autre commande (dont
 * `exit`, `restore`, `import`) est refusée avant même d'être tokenisée.
 *
 * @param line Ligne de commande complète (ex. "limit 1000"), non modifiée.
 * @return     `ADMIN_CMD_OK`, `ADMIN_CMD_FORBIDDEN` (hors liste blanche) ou
 *             `ADMIN_CMD_BAD_ARGS` (commande reconnue, argument manquant/invalide).
 */
int admin_apply_remote_command(const char *line);
#endif /* command_lines_h */
