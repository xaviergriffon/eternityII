/**
 * @file command_lines.h
 * @brief Méthodes pour prendre en charge des instructions en format texte.
 */
#ifndef command_lines_h
#define command_lines_h

#include <stdio.h>

/**
 * @brief Code retour d'interpréteur : argument manquant ou invalide.
 *
 * Quand un interpréteur le renvoie, `do_command_line` affiche automatiquement
 * le rappel d'usage déclaré dans la table des commandes (`usage : limit <n> — …`)
 * puis retourne -1 à son appelant — le contrat externe de `do_command_line`
 * (0 succès / -1 échec) est inchangé. Distinct de -1 pour que seuls les échecs
 * d'arguments déclenchent ce rappel, pas les erreurs d'exécution.
 */
#define CMD_ERR_USAGE (-3)

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
 * (src/app/app_static_variables.c) : la logique de décision reste une fonction pure,
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

/**
 * @brief Borne une valeur de budget de nœuds du pruner à [0, PRUNER_DFS_BUDGET_MAX].
 *
 * Extrait de `pruner_dfs_budget_interpreter` pour être réutilisé par
 * `admin_apply_remote_command` et `client_config_parse_line` sans dupliquer
 * les bornes. Contrairement à `pruner_batch_clamp`, 0 (et toute valeur
 * négative, ramenée à 0) est une valeur BASSE valide : elle désactive la
 * preuve de fermeture bornée (§4.6b), même convention que `limit 0`.
 *
 * @param v Valeur brute demandée.
 * @return  `v` borné à [0, PRUNER_DFS_BUDGET_MAX].
 */
int pruner_dfs_budget_clamp(int v);

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
 * `do_command_line` tokenise via `strtok`, curseur global non réentrant :
 * un appel concurrent depuis un thread HTTP pendant que la console ou le
 * canal de contrôle tokenise déjà une ligne corromprait les deux découpages.
 * Cette fonction relit `line` avec `strtok_r` (curseur local) pour les
 * commandes de `control_command_allowed` uniquement — toute autre (dont
 * `exit`, `restore`, `import`) est refusée avant même d'être tokenisée.
 *
 * `start`/`stopForks`/`configApply`/`configSave`/`clientsCommand`/
 * `clientsWork` sont réservées au serveur : `POST /api/v1/command` n'est
 * atteignable que depuis `runserver`, sans quoi elles agiraient sur les
 * globales/l'orchestrateur du serveur au lieu du no-op voulu.
 *
 * `pause`/`resume` diffusent aussi `CTRL_COMMAND` à toutes les sessions de
 * contrôle actives — le serveur n'a pas de boucle de recherche à mettre en
 * pause lui-même, seuls les clients connectés comptent.
 *
 * @return `ADMIN_CMD_OK`, `ADMIN_CMD_FORBIDDEN` (hors liste blanche) ou
 *         `ADMIN_CMD_BAD_ARGS` (commande reconnue, argument manquant/invalide).
 */
int admin_apply_remote_command(const char *line);

/**
 * @brief Variante de `admin_apply_remote_command` qui accepte EN PLUS les
 *        commandes PRIVILÉGIÉES (`control_command_privileged` : `restore`,
 *        `backup`), destinée exclusivement à `POST /api/v1/command`
 *        (`src/net/http_server.c`) APRÈS que l'appelant a authentifié la
 *        requête par jeton Bearer (`--http-token-file`) — cette fonction
 *        n'authentifie rien elle-même, elle suppose la décision déjà prise.
 *
 * Les commandes de `control_command_allowed` (pause/resume/limit/...) restent
 * déléguées à `admin_apply_remote_command`, sans duplication de logique ni
 * changement de comportement pour elles. Le canal de contrôle binaire
 * (`CTRL_COMMAND`, `src/app/etii_control.c`) n'appelle JAMAIS cette fonction :
 * il reste strictement borné à `control_command_allowed`, des deux côtés —
 * ajouter `restore`/`backup` à l'API HTTP admin ne les rend PAS déclenchables
 * à distance sur un client via le canal de contrôle.
 *
 * Comme `admin_apply_remote_command`, tokenise via `strtok_r` (curseur local)
 * : jamais `strtok`, dont le curseur global serait corrompu par un appel
 * concurrent (thread HTTP, console, canal de contrôle).
 *
 * @param line Ligne de commande complète (ex. "restore", "backup"), non modifiée.
 * @return     `ADMIN_CMD_OK`, `ADMIN_CMD_FORBIDDEN` (hors des deux listes
 *             blanches) ou `ADMIN_CMD_BAD_ARGS`.
 */
int admin_apply_privileged_command(const char *line);

/**
 * @brief Résout un nom de commande (alias inclus, casse ignorée) vers son nom canonique.
 *
 * Extrait pour être testable sans passer par `do_command_line` : la résolution
 * d'alias et l'insensibilité à la casse sont de la logique pure sur la table
 * des commandes.
 *
 * @param name Nom saisi (ex. "quit", "MAXSTOCKBYTHREAD").
 * @return     Nom canonique (ex. "exit", "maxStockByThread"), ou NULL si inconnu.
 */
const char *command_canonical_name(const char *name);

/**
 * @brief Formate l'aide générale : commandes groupées par catégorie, une ligne
 *        `usage  résumé` par commande, alias entre parenthèses.
 *
 * @param out      Tampon de sortie (toujours terminé par '\0', tronqué si trop petit).
 * @param out_size Taille du tampon.
 * @return         0 (toujours).
 */
int help_format_general(char *out, size_t out_size);

/**
 * @brief Formate l'aide d'un sujet : une commande (détail complet — usage,
 *        catégorie, portée, propagation aux fils, complément) ou une catégorie
 *        (sa section de l'aide générale).
 *
 * @param topic    Nom de commande (alias/casse acceptés) ou mot-clé de catégorie
 *                 (`general`, `recherche`, `stock`, `sauvegarde`, `diagnostic`, `clients`).
 * @param out      Tampon de sortie (toujours terminé par '\0', tronqué si trop petit).
 * @param out_size Taille du tampon.
 * @return         0 si le sujet est connu, -1 sinon (tampon vide).
 */
int help_format_topic(const char *topic, char *out, size_t out_size);
#endif /* command_lines_h */
