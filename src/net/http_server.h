/**
 * @file http_server.h
 * @brief Écouteur réseau de l'API REST admin (thread détaché, connexion
 *        `127.0.0.1` uniquement) : partie impure de l'API HTTP, adossée au
 *        codec pur `net/http_codec.h`.
 *
 * Modèle volontairement minimal : accept séquentiel, une requête par
 * connexion (`Connection: close`), un seul thread — API d'administration
 * occasionnelle, pas un serveur web de production. Démarré uniquement si
 * `HTTP_PORT > 0` (option CLI `--http-port <n>`, cf. app_static_variables.h),
 * depuis `runserver` (src/app/etii_server.c).
 *
 * Authentification (`--http-token-file <chemin>`, `HTTP_ADMIN_TOKEN`, cf.
 * app_static_variables.h) : par défaut (aucun jeton configuré), seule la LECTURE
 * fonctionne — les routes `GET` et la seule commande de lecture pure de
 * `POST /api/v1/command` (`clientsWork`, `control_command_read_only`). Toute
 * commande de MODIFICATION (`pause`, `resume`, `limit`, `maxStockByThread`,
 * `prunerBatch`, `clientsCommand`/`clientsCmd`, plus les privilégiées
 * `restore`/`backup`) exige un en-tête `Authorization: Bearer <jeton>` valide,
 * et reste donc inaccessible tant qu'aucun jeton n'est configuré. Le bind
 * loopback reste la première barrière : un accès distant passe par un
 * tunnel/reverse-proxy explicite, à la charge de l'opérateur.
 */
#ifndef eternityII_http_server_h
#define eternityII_http_server_h

#include "net/http_codec.h"

/**
 * @brief Démarre l'API HTTP REST admin : crée le socket d'écoute sur
 *        `127.0.0.1:<port>` puis lance le thread accepteur en mode détaché.
 *
 * N'expose jamais le service hors de la machine (bind loopback strict, pas
 * `INADDR_ANY`) : un accès distant passe par un tunnel/reverse-proxy explicite,
 * à la charge de l'opérateur. Cf. la note d'authentification en tête de fichier.
 *
 * @param port Port TCP d'écoute (appelant garanti > 0 : `HTTP_PORT == 0`
 *             signifie « API désactivée », vérifié avant l'appel).
 * @return     0 si le socket est en écoute et le thread démarré, -1 sinon
 *             (bind/listen ou création de thread échoués — le port est peut-
 *             être déjà utilisé). L'appelant décide de la gravité.
 */
int http_server_start(int port);

/**
 * @brief Charge et valide le jeton d'authentification Bearer de l'API HTTP
 *        admin depuis un fichier (`--http-token-file <chemin>`).
 *
 * Appelée une seule fois au démarrage, avant tout fork (`main()`), quel que
 * soit le mode (server/client/pruner/test) — même schéma que les autres
 * options globales. Refuse explicitement (retourne -1, message journalisé
 * via `log_error`, jamais le contenu du jeton) :
 *  - un fichier inaccessible (`stat`/`fopen` échoue) ;
 *  - un fichier aux permissions plus larges que propriétaire-seul (`mode &
 *    0077 != 0`, même exigence qu'une clé privée SSH — un jeton lisible par
 *    d'autres comptes de la machine n'apporte aucune garantie) ;
 *  - un fichier vide ou dont la première ligne, une fois les espaces/retours
 *    à la ligne de fin retirés, est vide ;
 *  - un jeton trop long pour `out_size` (`HTTP_ADMIN_TOKEN_MAX`).
 *
 * @param path     Chemin du fichier jeton (`HTTP_TOKEN_FILE`, non NULL garanti
 *                 par l'appelant).
 * @param out      Tampon destination (`HTTP_ADMIN_TOKEN`), rempli et terminé
 *                 par NUL en cas de succès.
 * @param out_size Taille de `out`.
 * @return         Longueur du jeton chargé (>= 1), ou -1 en cas d'échec.
 */
int http_token_load(const char *path, char *out, size_t out_size);

/**
 * @brief Traite une connexion HTTP déjà acceptée : lit une requête complète,
 *        la route, exécute l'action, envoie la réponse, puis rend la main
 *        (la fermeture du descripteur reste à la charge de l'appelant).
 *
 * Non-static à dessein : les tests l'appellent directement sur une extrémité
 * de `socketpair`, sans passer par `accept()` (même esprit que
 * `communicate_with_client_step` dans etii_server.c).
 *
 * @param socket_id Descripteur de la connexion acceptée (déjà connectée,
 *                   idéalement avec `SO_RCVTIMEO`/`SO_SNDTIMEO` déjà posés).
 * @return          0 si une réponse a été envoyée (quel que soit son code de
 *                   statut HTTP), -1 si la connexion s'est fermée ou a expiré
 *                   avant qu'une requête complète soit reçue.
 */
int handle_http_connection(int socket_id);

/**
 * @brief Construit un instantané des statistiques serveur courantes pour
 *        `GET /api/v1/stats` (lecture de globaux/accesseurs datamanager déjà
 *        thread-safe, sans verrou supplémentaire).
 *
 * @param out Vue à remplir (jamais NULL, appelant garanti).
 */
void http_stats_collect(http_stats_view_t *out);

/**
 * @brief Construit un instantané de l'état serveur courant pour
 *        `GET /api/v1/status`.
 *
 * @param out Vue à remplir (jamais NULL, appelant garanti).
 */
void http_status_collect(http_status_view_t *out);

/**
 * @brief Construit un instantané de la répartition du stock par `alloc` pour
 *        `GET /api/v1/stock-distribution`, via `datamanager_stock_distribution`
 *        (`core/datamanager.h`) — même source que la commande console `statistic`.
 *
 * Contrairement à `http_stats_collect` (lectures de compteurs déjà
 * thread-safe), cet appel PARCOURT toutes les files sous verrou : coûteux à la
 * fréquence d'un poll de télémétrie, d'où une route dédiée plutôt qu'un ajout
 * à `GET /api/v1/stats`.
 *
 * @param out Vue à remplir (jamais NULL, appelant garanti).
 */
void http_stock_distribution_collect(http_stock_distribution_view_t *out);

/**
 * @brief Construit un instantané des sessions de contrôle actives (canal
 *        `INST_CONTROL_HELLO`, v9) pour `GET /api/v1/clients`, via
 *        `control_registry_snapshot` (src/app/control_registry.h) — même
 *        source que la commande console `clients`.
 *
 * @param out Tableau destination (au moins `max` entrées).
 * @param max Capacité de `out`.
 * @return    Nombre d'entrées effectivement copiées.
 */
int http_clients_collect(http_client_info_t *out, int max);

/**
 * @brief Construit un instantané du meilleur plateau connu du serveur
 *        (`g_server_best_board`, cf. `core/best_board.h`) pour
 *        `GET /api/v1/best-board`.
 *
 * @param out Vue à remplir (jamais NULL, appelant garanti).
 */
void http_best_board_collect(http_best_board_view_t *out);

/**
 * @brief Construit un instantané des machines connues (registre de cumul,
 *        `app/known_clients_registry.h`) pour `GET /api/v1/known-clients`,
 *        via `known_clients_registry_snapshot` — même source que la commande
 *        console `knownClients`.
 *
 * @param out Tableau destination (au moins `max` entrées).
 * @param max Capacité de `out`.
 * @return    Nombre d'entrées effectivement copiées.
 */
int http_known_clients_collect(http_known_client_info_t *out, int max);

#endif /* eternityII_http_server_h */
