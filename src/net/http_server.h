/**
 * @file http_server.h
 * @brief Écouteur réseau de l'API REST admin (thread détaché, connexion
 *        `127.0.0.1` uniquement) : partie impure de l'API HTTP, adossée au
 *        codec pur `net/http_codec.h`.
 *
 * Modèle volontairement minimal : accept séquentiel, une requête par
 * connexion (`Connection: close`), un seul thread — API d'administration
 * occasionnelle, pas un serveur web de production. Démarré uniquement si
 * `HTTP_PORT > 0` (option CLI `--http-port <n>`, cf. static_variables.h),
 * depuis `runserver` (src/app/etii_server.c).
 */
#ifndef eternityII_http_server_h
#define eternityII_http_server_h

#include "net/http_codec.h"

/**
 * @brief Démarre l'API HTTP REST admin : crée le socket d'écoute sur
 *        `127.0.0.1:<port>` puis lance le thread accepteur en mode détaché.
 *
 * N'expose jamais le service hors de la machine (bind loopback strict, pas
 * `INADDR_ANY`) : documenté comme API de confiance, sans authentification —
 * un accès distant passe par un tunnel/reverse-proxy explicite, à la charge
 * de l'opérateur.
 *
 * @param port Port TCP d'écoute (appelant garanti > 0 : `HTTP_PORT == 0`
 *             signifie « API désactivée », vérifié avant l'appel).
 * @return     0 si le socket est en écoute et le thread démarré, -1 sinon
 *             (bind/listen ou création de thread échoués — le port est peut-
 *             être déjà utilisé). L'appelant décide de la gravité.
 */
int http_server_start(int port);

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

#endif /* eternityII_http_server_h */
