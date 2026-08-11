/**
 * @file client_config.h
 * @brief Configuration client persistée (fichier texte clé=valeur).
 *
 * Module de parsing/écriture, lecture au démarrage appliquée aux globales
 * (priorité CLI > fichier > défauts), et les commandes console
 * `config`/`configSave` (`--config-file`, cf. static_variables.h).
 */
#ifndef client_config_h
#define client_config_h

#include <stddef.h>

/**
 * @brief Configuration client, un booléen `has_*` par clé optionnelle.
 *
 * Une clé absente du fichier (ou non encore capturée) laisse son `has_*` à 0
 * et sa valeur à zéro/NULL — jamais une valeur par défaut implicite : c'est à
 * l'appelant (`client_config_apply_to_globals`) de décider quoi faire d'une
 * clé absente. Les champs chaîne (`server_host`, `parts_file`) sont toujours
 * `strdup`és (jamais un pointeur dans le buffer de lecture ou dans `argv`) :
 * voir `client_config_free`.
 */
typedef struct {
    int has_nb_forks;
    int nb_forks;

    int has_server_host;
    char *server_host;

    int has_parts_file;
    char *parts_file;

    int has_max_stock_by_thread;
    int max_stock_by_thread;

    int has_limit;
    unsigned long long limit;

    int has_pruner_batch;
    int pruner_batch;
} client_config_t;

/// Résultat de `client_config_parse_line`.
typedef enum {
    CLIENT_CONFIG_LINE_SET = 0,       ///< Clé reconnue, valeur valide, appliquée.
    CLIENT_CONFIG_LINE_IGNORED,       ///< Ligne vide ou commentaire (`#`) : rien à faire, pas une erreur.
    CLIENT_CONFIG_LINE_UNKNOWN_KEY,   ///< Clé non reconnue, ou ligne sans `=` (forme invalide).
    CLIENT_CONFIG_LINE_INVALID_VALUE, ///< Clé reconnue, valeur non convertible/hors domaine.
} client_config_line_status_t;

/// Résultat de `client_config_load`.
typedef enum {
    CLIENT_CONFIG_ABSENT = 0, ///< Fichier absent/illisible : PAS une erreur, cfg inchangée.
    CLIENT_CONFIG_LOADED = 1, ///< Fichier ouvert et parcouru (même si aucune clé valide dedans).
} client_config_load_status_t;

/// Chemin par défaut du fichier de configuration, même convention que les `.back`.
#define CLIENT_CONFIG_DEFAULT_PATH "./eternityii-client.conf"

/**
 * @brief Initialise @p cfg à l'état "aucune clé connue" (tous les `has_*` à 0).
 */
void client_config_init(client_config_t *cfg);

/**
 * @brief Libère les champs chaîne alloués de @p cfg (`server_host`, `parts_file`)
 *        et remet leurs `has_*` à 0. Sans effet sur les champs scalaires.
 *
 * Idempotent : peut être appelée plusieurs fois, ou sur une configuration déjà
 * initialisée par `client_config_init` sans jamais avoir été chargée.
 */
void client_config_free(client_config_t *cfg);

/**
 * @brief Parse UNE ligne au format `clé = valeur` (espaces autour du `=`
 *        ignorés, commentaire `#` jusqu'à fin de ligne — en tête de ligne ou
 *        après la valeur — ignoré) et met à jour @p cfg en conséquence.
 *
 * Fonction pure au sens de ce dépôt (cf. `parse_cli_options`) : aucune I/O,
 * seul @p cfg est modifié. Clés reconnues : `nb_forks` (entier > 0),
 * `server_host` (chaîne non vide), `parts_file` (chaîne non vide),
 * `max_stock_by_thread` (entier >= 0), `limit` (entier >= 0), `pruner_batch`
 * (entier, borné via `pruner_batch_clamp` — jamais invalide une fois
 * numérique, cf. command_lines.h). Une valeur déjà présente pour une clé
 * chaîne est remplacée (l'ancienne copie est libérée) : la DERNIÈRE occurrence
 * d'une clé dans un fichier l'emporte.
 *
 * @param line Ligne à parser (peut contenir un `\n`/`\r` de fin, ignoré).
 * @param cfg  Configuration mise à jour en cas de `CLIENT_CONFIG_LINE_SET`.
 * @return     Le statut de la ligne (voir `client_config_line_status_t`).
 */
client_config_line_status_t client_config_parse_line(const char *line, client_config_t *cfg);

/**
 * @brief Charge un fichier de configuration clé=valeur dans @p cfg.
 *
 * Lecture TOLÉRANTE : un fichier
 * absent ou illisible n'est pas une erreur (`CLIENT_CONFIG_ABSENT`, @p cfg
 * inchangée) — le process ne refuse jamais de démarrer à cause de ce fichier.
 * Une ligne à clé inconnue ou à valeur invalide est journalisée
 * (avertissement) puis ignorée ; le chargement continue avec les lignes
 * suivantes. @p cfg n'est PAS réinitialisée par cet appel : appeler
 * `client_config_init` avant si un état propre est voulu.
 *
 * @param path Chemin du fichier (NULL traité comme absent).
 * @param cfg  Configuration mise à jour ligne par ligne (déjà initialisée par l'appelant).
 * @return     `CLIENT_CONFIG_LOADED` si le fichier a pu être ouvert et parcouru,
 *             `CLIENT_CONFIG_ABSENT` sinon.
 */
client_config_load_status_t client_config_load(const char *path, client_config_t *cfg);

/**
 * @brief Formate @p cfg en texte clé=valeur (une ligne par clé PRÉSENTE — une
 *        clé à `has_* == 0` n'émet aucune ligne, jamais une valeur par défaut).
 *
 * Tampon toujours terminé par '\0' si @p out_size > 0. Fonction pure,
 * réutilisée à la fois par `client_config_save` et par la commande console
 * `config` (affichage).
 *
 * @param cfg      Configuration à formater.
 * @param out      Tampon de sortie.
 * @param out_size Taille du tampon.
 * @return         Le nombre d'octets écrits (hors '\0'), ou -1 si le tampon
 *                 était trop petit (contenu alors tronqué mais valide).
 */
int client_config_format(const client_config_t *cfg, char *out, size_t out_size);

/**
 * @brief Écrit @p cfg dans @p path, en écriture atomique (`.tmp` puis
 *        `rename()`, même patron que `backup()`, src/core/datamanager.c).
 *
 * @param path Chemin du fichier final.
 * @param cfg  Configuration à écrire (seules les clés à `has_* == 1` sont émises).
 * @return     0 en cas de succès, -1 sinon (déjà journalisé).
 */
int client_config_save(const char *path, const client_config_t *cfg);

/**
 * @brief Applique les clés de @p cfg aux globales correspondantes, mais
 *        UNIQUEMENT pour celles qu'aucun argument positionnel de la ligne de
 *        commande n'a déjà fournies — priorité CLI > fichier > défauts.
 *
 * @p argc est celui déjà consommé par `parse_client_args` (src/app/app_runtime.h)
 * pour ce même appel : cette fonction relit les mêmes seuils positionnels
 * (`argc >= 3` pour l'hôte serveur, `argc >= 4` pour `nb_forks`, etc. — cf.
 * `parse_client_args`) plutôt que de les redécouvrir, afin de ne jamais
 * diverger de ce que `parse_client_args` a réellement consommé. Lit le
 * global `pruner_mode` (src/app/static_variables.h) pour savoir si `argv[4]`
 * désigne `parts_file` (pruner) ou `max_stock_by_thread` (client de recherche).
 *
 * Les champs chaîne appliqués sont `strdup`és une seconde fois avant
 * affectation à la globale (jamais un pointeur partagé avec @p cfg) : @p cfg
 * reste entièrement possédée par l'appelant, qui peut la libérer normalement
 * après cet appel via `client_config_free`. Les globales ainsi écrites
 * (`parts_files`, `*server_host`) ne sont, comme le reste des chemins issus
 * d'options CLI de ce projet, jamais libérées — valables pour toute la durée
 * du process.
 *
 * @param cfg         Configuration chargée depuis le fichier.
 * @param argc        Nombre d'arguments positionnels restants (après retrait
 *                    des options globales par `parse_cli_options`).
 * @param server_host Pointeur vers la variable locale de l'appelant contenant
 *                    l'hôte serveur déjà résolu par `parse_client_args` — mis
 *                    à jour si le fichier fournit `server_host` et qu'aucun
 *                    argument positionnel ne l'a fait.
 */
void client_config_apply_to_globals(const client_config_t *cfg, int argc, const char **server_host);

/**
 * @brief Applique INCONDITIONNELLEMENT les clés de @p cfg aux globales
 *        correspondantes — contrairement à `client_config_apply_to_globals`,
 *        aucun seuil `argc` n'est consulté : chaque clé présente (`has_* == 1`)
 *        écrase la globale correspondante.
 *
 * Utilisée pour appliquer immédiatement, sans redémarrage du process, une
 * configuration saisie en cours de session via `config <clé> <valeur>` — un
 * ordre explicite de l'opérateur pendant l'exécution est par construction
 * plus récent que tout argument positionnel donné au lancement, donc toujours
 * prioritaire. Sans effet sur les clés absentes de @p cfg.
 *
 * @param cfg         Configuration à appliquer (typiquement la configuration
 *                    "en préparation" de `fork_orchestrator`).
 * @param server_host Pointeur vers la variable de l'appelant contenant l'hôte
 *                    serveur courant — mis à jour si `cfg` fournit `server_host`.
 */
void client_config_apply_direct(const client_config_t *cfg, const char **server_host);

/**
 * @brief Capture la configuration EFFECTIVE actuelle (valeurs réellement en
 *        vigueur) depuis les globales, dans un `client_config_t` neuf.
 *
 * Contrairement à `client_config_load` (ce qui a été LU dans un fichier), ceci
 * reflète toujours l'état COURANT (y compris après un `limit`/`maxStockByThread`/
 * `prunerBatch` exécuté depuis la console) — utilisé par les commandes `config`
 * (affichage) et `configSave` (persistance), qui doivent refléter la réalité,
 * pas un instantané de démarrage.
 *
 * @param out         Configuration résultat (réinitialisée par cet appel).
 * @param server_host Hôte serveur courant (NULL ou chaîne vide : absent du
 *                    résultat, ex. mode serveur/test où la notion n'a pas de
 *                    sens) — voir `g_client_server_host`.
 */
void client_config_capture_effective(client_config_t *out, const char *server_host);

/// Résultat de `client_config_diff`.
typedef enum {
    CLIENT_CONFIG_DIFF_HOT_ONLY = 0,      ///< Aucune clé stagée ne requiert de redémarrage : diffusion IPC seule.
    CLIENT_CONFIG_DIFF_NEEDS_RESTART = 1, ///< Au moins une clé stagée (nb_forks/server_host/parts_file) requiert un redémarrage des fils.
} client_config_diff_t;

/**
 * @brief Compare la configuration EN PRÉPARATION (@p staged) à la
 *        configuration EFFECTIVE (@p current) pour décider, côté `configApply`,
 *        entre une simple diffusion IPC (`HOT_ONLY`) et un arrêt +
 *        reconstruction + re-fork complet (`NEEDS_RESTART`).
 *
 * Fonction pure : seules `nb_forks`, `server_host` et `parts_file` peuvent
 * déclencher `NEEDS_RESTART` (elles conditionnent respectivement le
 * dimensionnement des tableaux de fils, la cible réseau et la map de
 * recherche partagée COW — aucune des trois ne peut changer sans arrêter
 * les fils existants). Une clé stagée absente de @p staged, ou présente mais
 * identique à @p current, ne déclenche jamais de redémarrage à elle seule.
 * Les clés à chaud (`max_stock_by_thread`/`limit`/`pruner_batch`) n'influencent
 * jamais le résultat : elles sont toujours diffusables par IPC.
 *
 * @param current Configuration EFFECTIVE actuelle (typiquement
 *                `client_config_capture_effective`).
 * @param staged  Configuration EN PRÉPARATION (`config <clé> <valeur>`).
 * @return        `CLIENT_CONFIG_DIFF_NEEDS_RESTART` si `nb_forks`,
 *                `server_host` ou `parts_file` est stagée avec une valeur
 *                différente de (ou absente de) @p current ;
 *                `CLIENT_CONFIG_DIFF_HOT_ONLY` sinon (y compris si rien n'est
 *                stagé du tout — un `configApply` sans rien préparer est un
 *                no-op inoffensif).
 */
client_config_diff_t client_config_diff(const client_config_t *current, const client_config_t *staged);

/**
 * @brief Hôte serveur effectif du process CLIENT/PRUNER courant, résolu une
 *        seule fois par `handle_client` (src/app/main.c) juste après
 *        `parse_client_args`/`client_config_apply_to_globals`.
 *
 * `NULL` en mode serveur ou test (la notion d'hôte serveur n'existe pas pour
 * ces modes). Existe uniquement pour que les commandes `config`/`configSave`
 * (exécutées depuis le thread console du process PARENT) puissent refléter
 * cette valeur, qui autrement ne vivrait que dans la pile de `handle_client`.
 */
extern const char *g_client_server_host;

#endif /* client_config_h */
