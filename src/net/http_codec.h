/**
 * @file http_codec.h
 * @brief Codec HTTP/1.1 minimal de l'API REST admin du serveur (parsing de
 *        requête, routage, formatage de réponse et de JSON) — fonctions
 *        pures, sans socket ni allocation, testables sans réseau.
 *
 * Sous-ensemble volontairement restreint : ligne de requête + en-têtes +
 * corps optionnel borné par `Content-Length`. Pas de chunked-encoding, pas de
 * keep-alive, pas de query string. Chaque connexion sert une seule requête
 * (`Connection: close`), cf. `src/net/http_server.h` pour la boucle réseau.
 *
 * Même esprit que `net/control_protocol.h` : zéro dépendance externe, JSON
 * généré par `snprintf` dans un tampon fourni par l'appelant (jamais
 * d'allocation dans le chemin de réponse).
 */
#ifndef eternityII_http_codec_h
#define eternityII_http_codec_h

#include <stddef.h>
#include <stdint.h>

#include "core/datamanager.h"

/// Taille maximale d'une requête acceptée (ligne + en-têtes + corps), au-delà
/// de laquelle la connexion est refusée (413) plutôt que de croître sans borne.
#define HTTP_REQUEST_MAX 8192
/// Taille du tampon de formatage de réponse fourni par l'appelant. Dimensionné
/// pour le plus gros corps produit : `GET /api/v1/best-board` sérialise jusqu'à
/// ETERN_PARTS cases, chacune avec la description complète de la pièce posée
/// (id, rotation, 4 couleurs de bord) — un ordre de grandeur plus gros que les
/// autres routes (compteurs seuls).
#define HTTP_RESPONSE_MAX 32768
/// Longueur maximale (avec terminateur) de la méthode HTTP acceptée.
#define HTTP_METHOD_MAX 8
/// Longueur maximale (avec terminateur) du chemin de la requête accepté.
#define HTTP_PATH_MAX 128
/// Longueur maximale (avec terminateur) de l'en-tête `Authorization` accepté
/// (ex. "Bearer " + un jeton de `HTTP_ADMIN_TOKEN_MAX` octets, cf.
/// static_variables.h — marge incluse pour ne jamais tronquer un jeton valide).
#define HTTP_AUTHORIZATION_MAX 320
/// Longueur maximale (avec terminateur) de l'adresse IP du pair d'une session
/// de contrôle (`http_client_info_t.peer_ip`), formatée côté serveur par
/// `inet_ntop`. Valeur volontairement dupliquée de `PEER_IP_MAX_LEN`
/// (app/static_variables.h) plutôt qu'importée : ce fichier n'a AUCUNE
/// dépendance vers `app/` (cf. en-tête de fichier ci-dessus).
#define HTTP_CLIENT_IP_MAX 46

/**
 * @brief Requête HTTP parsée : méthode, chemin, et vue sur le corps (pas de
 *        copie — `body` pointe dans le tampon source fourni à `http_request_parse`).
 */
typedef struct {
    /// Méthode HTTP ("GET", "POST", ...), toujours terminée par NUL.
    char method[HTTP_METHOD_MAX];
    /// Chemin de la requête ("/api/v1/stats"), toujours terminé par NUL.
    char path[HTTP_PATH_MAX];
    /// Valeur de l'en-tête `Content-Length`, ou 0 si absent.
    int32_t content_length;
    /// Pointeur vers le corps dans le tampon source (NULL si `content_length == 0`).
    const char *body;
    /// Nombre d'octets de corps effectivement disponibles (== content_length si complet).
    int32_t body_len;
    /// Valeur brute de l'en-tête `Authorization` (ex. "Bearer abc123"), chaîne
    /// vide si l'en-tête est absent OU trop long pour ce tampon (traité comme
    /// absent plutôt que tronqué silencieusement — un jeton tronqué ne doit
    /// jamais matcher par accident). Décodée par `http_extract_bearer_token`.
    char authorization[HTTP_AUTHORIZATION_MAX];
} http_request_t;

/**
 * @brief Résultat du parsing d'une requête HTTP.
 */
typedef enum {
    /// Requête complète et valide : `out` est renseigné.
    HTTP_PARSE_OK = 0,
    /// Requête incomplète (en-têtes ou corps pas encore intégralement reçus) :
    /// l'appelant doit continuer à lire le socket et rappeler avec plus de données.
    HTTP_PARSE_NEED_MORE,
    /// Requête malformée (ligne de requête invalide, en-tête `Content-Length`
    /// non numérique/négatif, jeton méthode/chemin trop long).
    HTTP_PARSE_BAD,
    /// Requête (ou `Content-Length` annoncé) dépassant `HTTP_REQUEST_MAX`.
    HTTP_PARSE_TOO_LARGE
} http_parse_result_t;

/**
 * @brief Parse une requête HTTP/1.1 depuis un tampon brut.
 *
 * @param buf Tampon source (pas nécessairement terminé par NUL).
 * @param len Nombre d'octets valides dans `buf` (0 <= len <= HTTP_REQUEST_MAX
 *            attendu de l'appelant ; une valeur plus grande renvoie TOO_LARGE).
 * @param out Requête parsée en sortie (uniquement si HTTP_PARSE_OK est renvoyé).
 * @return    Le résultat du parsing (cf. `http_parse_result_t`).
 */
http_parse_result_t http_request_parse(const char *buf, int32_t len, http_request_t *out);

/**
 * @brief Route logique résolue à partir de la méthode et du chemin.
 */
typedef enum {
    HTTP_ROUTE_STATS,         ///< GET /api/v1/stats
    HTTP_ROUTE_STATUS,        ///< GET /api/v1/status
    HTTP_ROUTE_COMMAND,       ///< POST /api/v1/command
    HTTP_ROUTE_CLIENTS,       ///< GET /api/v1/clients
    HTTP_ROUTE_CLIENTS_STATS, ///< POST /api/v1/clients/stats
    HTTP_ROUTE_BEST_BOARD,    ///< GET /api/v1/best-board
    HTTP_ROUTE_KNOWN_CLIENTS, ///< GET /api/v1/known-clients
    HTTP_ROUTE_STOCK_DISTRIBUTION, ///< GET /api/v1/stock-distribution
    HTTP_ROUTE_NOT_FOUND,     ///< Chemin inconnu (404)
    HTTP_ROUTE_BAD_METHOD     ///< Chemin connu, méthode non supportée (405)
} http_route_t;

/**
 * @brief Résout la route logique pour une méthode et un chemin donnés.
 *
 * @param method Méthode HTTP ("GET", "POST", ...).
 * @param path   Chemin de la requête ("/api/v1/stats").
 * @return       La route résolue (jamais d'ambiguïté : un chemin connu avec
 *               la mauvaise méthode renvoie HTTP_ROUTE_BAD_METHOD, jamais NOT_FOUND).
 */
http_route_t http_route_resolve(const char *method, const char *path);

/**
 * @brief Formate une réponse HTTP/1.1 complète (ligne de statut + en-têtes
 *        fixes + corps) dans `buf`.
 *
 * @param buf       Tampon destination.
 * @param size      Taille de `buf`.
 * @param status    Code de statut HTTP (200, 400, 403, 404, 405, 413).
 * @param json_body Corps JSON (chaîne terminée par NUL), ou NULL/"" pour un corps vide.
 * @return           Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_response_format(char *buf, size_t size, int status, const char *json_body);

/**
 * @brief Formate une réponse HTTP/1.1 401 Unauthorized, avec l'en-tête
 *        `WWW-Authenticate: Bearer` requis par la RFC 7235 — la seule route
 *        de ce codec qui a besoin d'un en-tête supplémentaire au-delà de
 *        `http_response_format`, d'où une fonction dédiée plutôt qu'un
 *        paramètre optionnel sur cette dernière.
 *
 * @param buf       Tampon destination.
 * @param size      Taille de `buf`.
 * @param json_body Corps JSON (chaîne terminée par NUL), ou NULL/"" pour un corps vide.
 * @return          Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_response_format_unauthorized(char *buf, size_t size, const char *json_body);

/**
 * @brief Extrait le jeton d'un en-tête `Authorization: Bearer <jeton>`.
 *
 * Comparaison du schéma ("Bearer") insensible à la casse (RFC 7235 : les noms
 * de schéma d'authentification sont insensibles à la casse), séparateur
 * espace/tabulation(s) tolérant. Rejette (retourne -1, `out` vidé) l'absence
 * d'en-tête, un schéma différent, un jeton vide, ou un jeton qui ne tiendrait
 * pas dans `out` — jamais de troncature silencieuse d'un jeton.
 *
 * @param authorization_header Valeur brute de l'en-tête (`http_request_t.authorization`).
 * @param out                  Tampon destination, rempli et terminé par NUL en cas de succès.
 * @param out_size             Taille de `out`.
 * @return                     Longueur du jeton extrait (>= 0), ou -1.
 */
int http_extract_bearer_token(const char *authorization_header, char *out, size_t out_size);

/**
 * @brief Compare deux chaînes en temps constant (relatif à `max_len`, pas à
 *        leur longueur réelle) : XOR cumulé sur tout `max_len` sans early-exit,
 *        pour ne pas laisser le temps de réponse de `POST /api/v1/command`
 *        fuiter combien de caractères initiaux d'un jeton deviné sont corrects.
 *
 * Les longueurs (`strnlen` bornée à `max_len`) sont comparées via un OR
 * accumulé dans le même drapeau que les octets, jamais par un `return`
 * anticipé sur mismatch de longueur.
 *
 * @param a       Première chaîne (NUL-terminée), ou NULL (retourne 0).
 * @param b       Seconde chaîne (NUL-terminée), ou NULL (retourne 0).
 * @param max_len Nombre d'octets comparés (borne supérieure des deux longueurs).
 * @return        1 si `a` et `b` sont égales, 0 sinon (y compris arguments NULL).
 */
int http_token_equals_constant_time(const char *a, const char *b, size_t max_len);

/**
 * @brief Décision d'autorisation pure pour `POST /api/v1/command` : combine
 *        deux drapeaux calculés par l'appelant et le résultat de la
 *        vérification du jeton — sans connaître `control_protocol.h`, ni le
 *        jeton lui-même, ni le détail de la classification des commandes.
 *
 * Cette fonction ignore tout de la classification par NOM de commande : elle
 * ne voit que deux booléens déjà tranchés par l'appelant (`src/net/http_server.c`),
 * nommés d'après ce qu'ils décident réellement sur CETTE route plutôt que
 * d'après les noms des listes blanches de `control_protocol.h` — ces
 * dernières encodent un axe différent (relayable vers un client ou non, cf.
 * `control_command_class_t`), qui a cessé de correspondre à un niveau
 * d'authentification distinct depuis l'exigence « toute commande de
 * modification doit être authentifiée » : `is_public` et `needs_auth` sont
 * calculés par l'appelant comme
 * `is_public  = control_command_allowed(command) && control_command_read_only(command)`
 * (aujourd'hui : seulement `clientsWork`) et
 * `needs_auth = !is_public && (control_command_allowed(command) || control_command_privileged(command))`
 * — c.-à-d. `control_command_classify(command) != CTRL_CMD_UNKNOWN` et pas
 * `CTRL_CMD_READ_ONLY`, ce qui regroupe `pause`, `resume`, `limit`,
 * `maxStockByThread`, `prunerBatch`, `clientsCommand`/`clientsCmd`,
 * `start`/`stopForks`/`configApply`/`config`/`configSave`
 * (`CTRL_CMD_WRITE_RELAYABLE`) ET `restore`/`backup`/`sortAsc`/`sortDesc`/
 * `sortDescMulti`/`split`/`regroup` (`CTRL_CMD_WRITE_SERVER_ONLY`) sous LA
 * MÊME exigence d'authentification. La logique de CETTE fonction reste
 * inchangée — seule la façon dont l'appelant peuple ses deux entrées a changé.
 *
 * Règles :
 * - `is_public` : toujours OK, sans vérification de jeton.
 * - `needs_auth` : OK seulement si un jeton est configuré ET que
 *   `token_valid` l'atteste ; sinon UNAUTHORIZED (401), qu'un jeton soit
 *   configuré ou non — un serveur sans jeton configuré refuse aussi ces
 *   commandes plutôt que de les exécuter sans aucune preuve d'identité.
 * - Ni l'un ni l'autre (commande hors des deux listes blanches, ex. `exit`,
 *   `import`) : FORBIDDEN (403).
 *
 * @param is_public            Commande exécutable sans authentification (voir ci-dessus).
 * @param needs_auth           Commande nécessitant un jeton Bearer valide (voir ci-dessus).
 * @param has_configured_token 1 si le serveur a chargé un jeton au démarrage
 *                             (`--http-token-file`), 0 sinon.
 * @param token_valid          1 si un jeton Bearer a été fourni ET correspond
 *                             au jeton configuré (comparaison temps constant), 0 sinon.
 * @return                     La décision (cf. `http_cmd_auth_result_t`).
 */
typedef enum {
    HTTP_CMD_AUTH_OK = 0,          ///< Commande autorisée, à exécuter.
    HTTP_CMD_AUTH_FORBIDDEN,       ///< Ni publique ni reconnue -> 403.
    HTTP_CMD_AUTH_UNAUTHORIZED     ///< Modifiante, jeton absent/invalide/non configuré -> 401.
} http_cmd_auth_result_t;

http_cmd_auth_result_t http_command_authorize(int is_public, int needs_auth, int has_configured_token, int token_valid);

/**
 * @brief Extrait la valeur d'une clé JSON de type chaîne dans un objet JSON plat.
 *
 * Extracteur minimal, volontairement non conforme à la norme JSON complète :
 * ne gère aucun échappement (`\"`, `\\`, `\uXXXX`, ...) et REJETTE (retourne
 * -1) toute valeur qui en contient, plutôt que de la mal interpréter. Les
 * commandes admin whitelistées (`control_command_allowed`) sont toutes en
 * `[A-Za-z0-9 ]`, donc ce sous-ensemble suffit à l'usage réel de l'API.
 *
 * @param body     Corps JSON source (pas nécessairement terminé par NUL).
 * @param len      Nombre d'octets valides dans `body`.
 * @param key      Nom de la clé recherchée (sans les guillemets).
 * @param out      Tampon destination, rempli et terminé par NUL en cas de succès.
 * @param out_size Taille de `out`.
 * @return         Longueur de la valeur extraite (>= 0), ou -1 si la clé est
 *                 absente, la valeur non-chaîne/mal formée/tronquée, ou `out` trop petit.
 */
int http_json_extract_string(const char *body, int32_t len, const char *key, char *out, size_t out_size);

/**
 * @brief Vue en lecture des statistiques serveur à sérialiser en JSON par
 *        `http_json_format_stats`. Remplie par `http_stats_collect`
 *        (src/net/http_server.h) à partir des globaux/accesseurs datamanager.
 */
typedef struct {
    unsigned long long shots_per_second;
    unsigned long long possibility_stock;
    unsigned long long checked_stock;
    unsigned long long analysed_stock;
    unsigned long long max_result;
    unsigned long long active_threads;
    unsigned long long pruner_checked;
    unsigned long long pruner_removed;
    /// Possibilités actuellement déportées sur disque (PR2, --stock-max-ram
    /// + --stock-spill-dir), tous pools et toutes files confondus — 0 si le
    /// débordement est désactivé, illimité, ou inactif.
    unsigned long long stock_spilled_packets;
    /// Nombre de fichiers de segment de débordement actuellement sur disque,
    /// tous pools et toutes files confondus.
    unsigned long long stock_spill_segments;
    /// Tailles par file (index 0..nb_file_possibility-1, PR4 : le compte
    /// RÉEL est une variable, ce tableau est dimensionné au plafond de
    /// compilation NB_FILE_POSSIBILITY_MAX), pool non vérifié.
    unsigned long long queue_unchecked[NB_FILE_POSSIBILITY_MAX];
    /// Tailles par file, pool vérifié.
    unsigned long long queue_checked[NB_FILE_POSSIBILITY_MAX];
    /// Tailles par file, pool analysé.
    unsigned long long queue_analysed[NB_FILE_POSSIBILITY_MAX];
} http_stats_view_t;

/**
 * @brief Vue en lecture de l'état serveur à sérialiser en JSON par
 *        `http_json_format_status`. Remplie par `http_status_collect`.
 */
typedef struct {
    /// Libellé d'état ("running", "admin_pause", "regulation_pause", "stopping").
    const char *state;
    long uptime_seconds;
    int version;
    unsigned long long limit;
    int max_stock_by_thread;
    int pruner_batch;
    int pruner_dfs_budget;
    /// Durée (ms) de la dernière sauvegarde automatique exécutée, 0 si aucune n'a encore eu lieu.
    unsigned long long last_backup_duration_ms;
    /// Plafond RAM des deux pools de stock, en Mo (option --stock-max-ram /
    /// commande stockMaxRam) — 0 = illimité. Dérivé de
    /// datamanager_ram_limit_packets() pour l'affichage (jamais l'inverse :
    /// la seule valeur réellement comparée par put_to_pool est en
    /// possibilités, cf. core/datamanager.h).
    unsigned long long stock_ram_limit_mb;
    /// Mo actuellement occupés par les deux pools de stock (estimation, cf.
    /// datamanager_bytes_per_possibility) — 0 quand le stock est vide, jamais
    /// lié au plafond ci-dessus (peut le dépasser légèrement entre deux
    /// vérifications, cf. datamanager.c).
    unsigned long long stock_ram_used_mb;
} http_status_view_t;

/**
 * @brief Sérialise `view` en JSON dans `buf` (cf. schéma documenté dans docs/api_http_rest.md).
 * @return Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_json_format_stats(char *buf, size_t size, const http_stats_view_t *view);

/**
 * @brief Sérialise `view` en JSON dans `buf` (cf. schéma documenté dans docs/api_http_rest.md).
 * @return Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_json_format_status(char *buf, size_t size, const http_status_view_t *view);

/**
 * @brief Vue en lecture de la répartition du stock par nombre de pièces
 *        placées (`alloc`), à sérialiser par `http_json_format_stock_distribution`.
 *        Remplie par `http_stock_distribution_collect` (src/net/http_server.h)
 *        à partir de `datamanager_stock_distribution` — même donnée que la
 *        commande console `statistic`, qui elle ne fait que l'imprimer en logs.
 *
 * Volontairement une requête DÉDIÉE, pas des champs de `GET /api/v1/stats` :
 * comme `best-board`, l'histogramme est un ordre de grandeur plus gros qu'un
 * compteur et impose un parcours complet des files sous verrou — un
 * consommateur qui ne poll que le débit ne doit pas le payer.
 *
 * Structurellement identique à `stock_distribution_t` (core/datamanager.h) :
 * la duplication est assumée, c'est la même règle que `http_client_info_t` vs
 * `control_session_info_t` — le codec expose sa propre vue et `http_server.c`
 * fait la copie.
 */
typedef struct {
    /// Répartition du pool non vérifié, indexée par `alloc` (0..ETERN_PARTS).
    unsigned long long unchecked[STOCK_DISTRIBUTION_LEVELS];
    /// Répartition du pool vérifié (`checked == 1`).
    unsigned long long checked[STOCK_DISTRIBUTION_LEVELS];
    /// Répartition du pool « en cours d'analyse ».
    unsigned long long analysed[STOCK_DISTRIBUTION_LEVELS];
    unsigned long long total_unchecked;
    unsigned long long total_checked;
    unsigned long long total_analysed;
} http_stock_distribution_view_t;

/**
 * @brief Sérialise `view` en JSON dans `buf` (cf. schéma documenté dans docs/api_http_rest.md).
 *
 * **Seuls les niveaux non vides sont listés** : sur les 257 niveaux possibles
 * (`STOCK_DISTRIBUTION_LEVELS` en build 256 pièces), un serveur réel n'en
 * occupe qu'une poignée, et émettre les 257 lignes ferait frôler
 * `HTTP_RESPONSE_MAX` pour ne transporter que des zéros. Un stock entièrement
 * vide donne donc `"levels":[]` — les totaux, eux, sont toujours présents.
 * Le tableau est trié par `alloc` croissant.
 *
 * @return Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_json_format_stock_distribution(char *buf, size_t size, const http_stock_distribution_view_t *view);

/**
 * @brief Vue en lecture d'une session de contrôle active (canal
 *        `INST_CONTROL_HELLO`, v9), pour `GET /api/v1/clients`. Remplie par
 *        `http_clients_collect` (src/net/http_server.h) à partir de
 *        `control_registry_snapshot` (src/app/control_registry.h) — ce struct
 *        évite à `http_codec.h`, pur et sans dépendance `app/`, de connaître
 *        `control_session_info_t`.
 */
/// Longueur maximale (avec terminateur) du libellé déclaré d'un client
/// (`http_client_info_t.label`). Valeur dupliquée de `CLIENT_LABEL_MAX`
/// (net/client_identity.h) plutôt qu'importée : ce fichier n'a AUCUNE
/// dépendance vers `app/` (cf. en-tête de fichier ci-dessus) — et
/// `client_identity.h` en est volontairement une, indépendante d'`app/`.
#define HTTP_CLIENT_LABEL_MAX 32
/// Longueur d'un nonce (machine_uid/client_uid) encodé en hexadécimal, avec
/// terminateur. Dupliquée de `2 * MACHINE_UID_BYTES + 1` (== `CLIENT_UID_BYTES`).
#define HTTP_CLIENT_UID_HEX_MAX 33

typedef struct {
    /// Identifiant de session monotone, jamais réutilisé (cf.
    /// `control_session_info_t.session_no`, app/control_registry.h).
    unsigned long long session_no;
    /// PID du processus parent qui a envoyé le hello.
    int32_t pid;
    /// Nombre de forks de recherche gérés par ce parent.
    int32_t nb_forks;
    /// Mode du client : 0 = recherche, 1 = pruner, 2 = pruner GPU.
    uint8_t mode;
    /// Libellé déclaré (option CLI `--name`, ou nom d'hôte par défaut) —
    /// affichage seul, jamais une clé.
    char label[HTTP_CLIENT_LABEL_MAX];
    /// Nonce machine persistant, encodé en hexadécimal.
    char machine_uid_hex[HTTP_CLIENT_UID_HEX_MAX];
    /// Nonce de session (process parent), encodé en hexadécimal.
    char client_uid_hex[HTTP_CLIENT_UID_HEX_MAX];
    /// Adresse IP du pair de la connexion TCP (non falsifiable, contrairement
    /// au reste du hello — cf. control_registry.h), `""` si inconnue.
    char peer_ip[HTTP_CLIENT_IP_MAX];
    /// Horodatage Unix (secondes) de la dernière activité observée.
    long long last_activity;
    /// 1 si les champs `stats_*`/`stats_time` sont valides (un `CTRL_STATS` a
    /// déjà été reçu pour cette session, via `POST /api/v1/clients/stats` ou la
    /// console `clientsStats`), 0 sinon (aucune donnée encore récoltée).
    int has_stats;
    /// Débit de recherche courant (essais/seconde) au moment de `stats_time`.
    unsigned long long stats_shots_per_second;
    /// Nombre de possibilités en stock local au moment de `stats_time`.
    unsigned long long stats_possibility_stock;
    /// Nombre de possibilités analysées en stock local au moment de `stats_time`.
    unsigned long long stats_analysed_stock;
    /// Meilleur niveau de curseur atteint (cf. `possibility_packet.alloc`) au
    /// moment de `stats_time`.
    unsigned long long stats_max_result;
    /// Nombre de possibilités vérifiées par le pruner (0 hors mode pruner).
    unsigned long long stats_pruner_checked;
    /// Nombre de possibilités éliminées par le pruner (0 hors mode pruner).
    unsigned long long stats_pruner_removed;
    /// Débit de prunage courant (cases étudiées/seconde), pendant « coups/s »
    /// du pruner (0 hors mode pruner).
    unsigned long long stats_pruner_cells_per_second;
    /// Horodatage Unix (secondes) de la réception des statistiques ci-dessus
    /// (valide seulement si `has_stats`).
    long long stats_time;
} http_client_info_t;

/**
 * @brief Sérialise un tableau de sessions de contrôle actives en JSON dans `buf`
 *        (cf. schéma documenté dans docs/api_http_rest.md).
 *
 * @param buf    Tampon destination.
 * @param size   Taille de `buf`.
 * @param infos  Tableau de sessions (peut être vide si `count == 0`).
 * @param count  Nombre d'entrées valides dans `infos`.
 * @return       Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_json_format_clients(char *buf, size_t size, const http_client_info_t *infos, int count);

/**
 * @brief Vue en lecture d'une machine connue du registre de cumul
 *        (`app/known_clients_registry.h`, PR4), pour
 *        `GET /api/v1/known-clients`. Remplie par `http_known_clients_collect`
 *        (src/net/http_server.h) à partir de `known_clients_registry_snapshot` —
 *        même schéma de séparation que `http_client_info_t` ci-dessus : ce
 *        fichier reste sans dépendance vers `app/`.
 */
typedef struct {
    /// Nonce machine persistant, encodé en hexadécimal (clé de cumul).
    char machine_uid_hex[HTTP_CLIENT_UID_HEX_MAX];
    /// Dernier libellé déclaré vu pour cette machine.
    char label[HTTP_CLIENT_LABEL_MAX];
    /// Dernière adresse IP du pair observée pour cette machine.
    char peer_ip[HTTP_CLIENT_IP_MAX];
    /// Dernier mode observé (cf. `CLIENT_MODE_*`, client_identity.h).
    uint8_t mode;
    /// 1 si au moins une session de cette machine est actuellement active, 0
    /// sinon (« déconnecté » : l'entrée reste visible).
    int connected;
    /// Nombre de sessions actuellement actives pour cette machine.
    int nb_active_sessions;
    /// Nombre total de connexions observées depuis le démarrage du serveur.
    int nb_connections_total;
    /// Horodatage Unix (secondes) de la première connexion observée.
    long long first_seen;
    /// Horodatage Unix (secondes) de la dernière activité observée.
    long long last_seen;
    /// Cumul des possibilités vérifiées par le pruner, toutes sessions
    /// passées et en cours de cette machine confondues.
    unsigned long long total_pruner_checked;
    /// Cumul des possibilités éliminées par le pruner.
    unsigned long long total_pruner_removed;
    /// Meilleur niveau de curseur (cf. `possibility_packet.alloc`) jamais rapporté par cette
    /// machine, toutes sessions confondues (pic, pas une somme).
    unsigned long long best_max_result;
    /// Somme des durées de connexion des sessions déjà terminées (secondes).
    unsigned long long cumulative_uptime_seconds;
} http_known_client_info_t;

/**
 * @brief Sérialise un tableau de machines connues en JSON dans `buf` (cf.
 *        schéma documenté dans docs/api_http_rest.md).
 *
 * @param buf    Tampon destination.
 * @param size   Taille de `buf`.
 * @param infos  Tableau de machines (peut être vide si `count == 0`).
 * @param count  Nombre d'entrées valides dans `infos`.
 * @return       Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_json_format_known_clients(char *buf, size_t size, const http_known_client_info_t *infos, int count);

/**
 * @brief Vue en lecture du meilleur plateau connu du serveur (agrégat
 * `g_server_best_board`, cf. `core/best_board.h`), à sérialiser en JSON par
 * `http_json_format_best_board`. Remplie par `http_best_board_collect`
 * (src/net/http_server.h). Volontairement une requête DÉDIÉE, pas un champ
 * de `GET /api/v1/stats` : la représentation complète (256 cases) est un
 * ordre de grandeur plus grosse qu'un compteur, un consommateur qui ne
 * s'intéresse qu'au débit ne doit pas la payer à chaque poll.
 */
/**
 * @brief Description d'une case de `http_best_board_view_t.grid` : la pièce
 * réellement posée (id, rotation, 4 couleurs de bord) — jamais le simple
 * indice brut encodé dans `possibility_packet.grid` (`id + ETERN_PARTS*rotation`,
 * cf. `id_for_rotated_part`), qui ne dit rien de la pièce sans la table des
 * rotations pour le décoder. Même décodage que `save_solution_csv`
 * (`src/core/possibility.c`), la référence existante pour ce calcul.
 */
typedef struct {
    /// -1 si la case est vide (ne devrait pas arriver au-delà de `alloc`, mais
    /// reflète le paquet tel quel) ; sinon l'identifiant réel de la pièce.
    int16_t id;
    /// Rotation appliquée (0-3), valide seulement si `id >= 0`.
    int8_t rotation;
    /// Couleurs des 4 bords de la pièce dans son orientation posée (motifs à
    /// faire correspondre avec les cases voisines), valides seulement si `id >= 0`.
    int8_t top;
    int8_t right;
    int8_t bottom;
    int8_t left;
} http_best_board_cell_t;

typedef struct {
    /// 1 si un plateau a déjà été enregistré (aucun record avant le premier
    /// placement n'existe : `alloc`/`grid` ne sont valides que si `has_board`).
    int has_board;
    /// Niveau du curseur de parcours de ce plateau. BORNE INFÉRIEURE du nombre
    /// de pièces réellement posées : `possibility_all_has_a_next` pose les pièces
    /// forcées sans avancer `alloc` (cf. l'invariant `faceused >= alloc` de
    /// `check_possibility`). Compter les cases non vides de `grid` pour l'exact.
    unsigned alloc;
    /// Grille de descriptions de pièces : `grid[x][y]`, cf. `http_best_board_cell_t`.
    http_best_board_cell_t grid[ETERN_SIZE][ETERN_SIZE];
} http_best_board_view_t;

/**
 * @brief Sérialise `view` en JSON dans `buf` (cf. schéma documenté dans docs/api_http_rest.md).
 * @return Longueur écrite (hors NUL final), ou -1 si `buf` est trop petit.
 */
int http_json_format_best_board(char *buf, size_t size, const http_best_board_view_t *view);

#endif /* eternityII_http_codec_h */
