/**
 * @file server_config.h
 * @brief Configuration serveur persistée (fichier texte clé=valeur),
 *        pendant serveur de `client_config.h`.
 *
 * Même convention que le client (priorité CLI > fichier > défauts) mais sans
 * décompte d'auto-démarrage ni réapplication à chaud : le serveur n'a pas
 * d'orchestrateur de fils différé, le fichier est donc lu une seule fois, au
 * tout début de `main()`/`handle_server`.
 *
 * Couvre toutes les options de démarrage propres au mode serveur : le
 * nombre de threads et le fichier de pièces (positionnels), et les options
 * valuées `--expand-level`, `--expand-max-stock`, `--expand-max-levels`,
 * `--http-port`, `--http-token-file`, `--stock-files`, `--stock-max-ram`,
 * `--stock-spill-dir`, `--rebalance-budget`, `--tcp-timeout`,
 * `--sort-interval`, `--sort-direction`, ainsi que les drapeaux
 * `--auto-roles`, `--stop-on-solution`, `--headless`, `--sort-enabled`.
 */
#ifndef server_config_h
#define server_config_h

#include <stddef.h>

/**
 * @brief Configuration serveur, un booléen `has_*` par clé optionnelle.
 *
 * Une clé absente du fichier laisse son `has_*` à 0 et sa valeur à zéro/NULL —
 * jamais une valeur par défaut implicite : c'est à l'appelant
 * (`server_config_apply_*`) de décider quoi faire d'une clé absente. Les
 * champs chaîne (`parts_file`, `http_token_file`, `stock_spill_dir`) sont
 * toujours `strdup`és (jamais un pointeur dans le buffer de lecture ni dans
 * `argv`) : voir `server_config_free`.
 */
typedef struct {
    int has_nb_threads;
    int nb_threads;

    int has_parts_file;
    char *parts_file;

    int has_expand_level;
    int expand_level;

    int has_expand_max_stock;
    int expand_max_stock;

    int has_expand_max_levels;
    int expand_max_levels;

    int has_http_port;
    int http_port;

    int has_http_token_file;
    char *http_token_file;

    int has_stock_files;
    int stock_files;

    int has_stock_max_ram;
    int stock_max_ram;

    int has_stock_spill_dir;
    char *stock_spill_dir;

    int has_rebalance_budget;
    int rebalance_budget;

    int has_tcp_timeout;
    int tcp_timeout;

    int has_auto_roles;
    int auto_roles;

    int has_stop_on_solution;
    int stop_on_solution;

    int has_headless;
    int headless;

    int has_sort_enabled;
    int sort_enabled;

    int has_sort_interval;
    int sort_interval;

    int has_sort_direction;
    int sort_direction;
} server_config_t;

/// Résultat de `server_config_parse_line`.
typedef enum {
    SERVER_CONFIG_LINE_SET = 0,       ///< Clé reconnue, valeur valide, appliquée.
    SERVER_CONFIG_LINE_IGNORED,       ///< Ligne vide ou commentaire (`#`) : rien à faire, pas une erreur.
    SERVER_CONFIG_LINE_UNKNOWN_KEY,   ///< Clé non reconnue, ou ligne sans `=` (forme invalide).
    SERVER_CONFIG_LINE_INVALID_VALUE, ///< Clé reconnue, valeur non convertible/hors domaine.
} server_config_line_status_t;

/// Résultat de `server_config_load`.
typedef enum {
    SERVER_CONFIG_ABSENT = 0, ///< Fichier absent/illisible : PAS une erreur, cfg inchangée.
    SERVER_CONFIG_LOADED = 1, ///< Fichier ouvert et parcouru (même si aucune clé valide dedans).
} server_config_load_status_t;

/// Chemin par défaut du fichier de configuration, même convention que le client.
#define SERVER_CONFIG_DEFAULT_PATH "./eternityii-server.conf"

/**
 * @brief Initialise @p cfg à l'état "aucune clé connue" (tous les `has_*` à 0).
 */
void server_config_init(server_config_t *cfg);

/**
 * @brief Libère les champs chaîne alloués de @p cfg (`parts_file`,
 *        `http_token_file`, `stock_spill_dir`) et remet leurs `has_*` à 0.
 *        Sans effet sur les champs scalaires. Idempotent.
 */
void server_config_free(server_config_t *cfg);

/**
 * @brief Parse une ligne au format `clé = valeur` (mêmes règles de
 *        tokenisation que `client_config_parse_line`).
 *
 * Clés reconnues : `nb_threads`, `parts_file`, `expand_level`,
 * `expand_max_stock`, `expand_max_levels`, `http_port` ([1, 65535]),
 * `http_token_file`, `stock_files`, `stock_max_ram`, `stock_spill_dir`,
 * `rebalance_budget`, `tcp_timeout`, `sort_interval`, `sort_direction`
 * (`asc`/`desc`), `auto_roles`/`stop_on_solution`/`headless`/`sort_enabled`
 * (0 ou 1). La dernière occurrence d'une clé l'emporte.
 *
 * @return Le statut de la ligne (voir `server_config_line_status_t`).
 */
server_config_line_status_t server_config_parse_line(const char *line, server_config_t *cfg);

/**
 * @brief Charge un fichier de configuration clé=valeur dans @p cfg.
 *
 * Lecture TOLÉRANTE, même contrat que `client_config_load` : un fichier
 * absent ou illisible n'est pas une erreur (`SERVER_CONFIG_ABSENT`, @p cfg
 * inchangée) ; une ligne à clé inconnue ou à valeur invalide est journalisée
 * (avertissement) puis ignorée, le chargement continue. @p cfg n'est PAS
 * réinitialisée par cet appel.
 *
 * @param path Chemin du fichier (NULL traité comme absent).
 * @param cfg  Configuration mise à jour ligne par ligne (déjà initialisée par l'appelant).
 * @return     `SERVER_CONFIG_LOADED` si le fichier a pu être ouvert et parcouru,
 *             `SERVER_CONFIG_ABSENT` sinon.
 */
server_config_load_status_t server_config_load(const char *path, server_config_t *cfg);

/**
 * @brief Formate @p cfg en texte clé=valeur (une ligne par clé PRÉSENTE).
 *        Même contrat que `client_config_format`.
 *
 * @return Le nombre d'octets écrits (hors '\0'), ou -1 si le tampon était trop petit.
 */
int server_config_format(const server_config_t *cfg, char *out, size_t out_size);

/**
 * @brief Écrit @p cfg dans @p path, en écriture atomique (`.tmp` puis
 *        `rename()`, même patron que `client_config_save`).
 *
 * @return 0 en cas de succès, -1 sinon (déjà journalisé).
 */
int server_config_save(const char *path, const server_config_t *cfg);

/**
 * @brief Applique à leurs globales toutes les clés de @p cfg sauf les deux
 *        positionnelles (`nb_threads`/`parts_file`) — uniquement pour celles
 *        qu'un argument CLI n'a pas déjà fournies.
 *
 * Appelée dans `main()` juste après `parse_cli_options`, avant le dispatch
 * de mode — et non depuis `handle_server` — car trois de ces clés
 * (`HTTP_TOKEN_FILE`, `stock_files_requested`/`stock_max_ram_mb`) sont
 * consommées inconditionnellement par `main()` juste après ce point ; une
 * résolution plus tardive arriverait trop tard.
 *
 * Le champ chaîne appliqué (`stock_spill_dir`) est `strdup`é une seconde
 * fois (jamais un pointeur partagé avec @p cfg).
 */
void server_config_apply_pre_dispatch(const server_config_t *cfg);

/**
 * @brief Applique les deux clés positionnelles de @p cfg (`nb_threads`,
 *        `parts_file`) aux globales correspondantes, uniquement pour celles
 *        qu'aucun argument CLI n'a déjà fournies.
 *
 * Contrairement aux clés de `server_config_apply_pre_dispatch`, savoir si
 * la CLI les a fournies dépend d'une logique de fallback
 * (`parse_server_thread_arg`) que seule `handle_server` a déjà exécutée au
 * moment de l'appel — d'où @p cli_gave_nb_threads/@p cli_gave_parts_file
 * plutôt qu'un seuil `argc`.
 *
 * @param cli_gave_nb_threads Vrai si la CLI a déjà fourni `nb_threads`.
 * @param cli_gave_parts_file Vrai si la CLI a déjà fourni le fichier de pièces.
 */
void server_config_apply_to_globals(const server_config_t *cfg, int cli_gave_nb_threads, int cli_gave_parts_file);

/**
 * @brief Capture la configuration effective actuelle depuis les globales,
 *        dans un `server_config_t` neuf.
 *
 * Contrairement à `server_config_load`, reflète toujours l'état courant du
 * serveur — utilisé par `config`/`configSave`. `stock_files` reflète
 * `nb_file_possibility` (compte réellement actif) plutôt que le sentinel
 * `stock_files_requested`. `http_port`/`http_token_file`/`stock_max_ram`
 * sont omis quand leur sentinel « non configuré » est en vigueur.
 */
void server_config_capture_effective(server_config_t *out);

#endif /* server_config_h */
