/**
 * @file etii_control.h
 * @brief Canal de contrôle côté client (PR4) : le processus PARENT (celui qui
 *        fork les process de recherche, jamais un fork lui-même) ouvre une
 *        connexion TCP additionnelle dédiée vers le serveur, s'annonce via
 *        `INST_CONTROL_HELLO`, puis répond aux trames `CTRL_*` envoyées par
 *        le serveur (qui devient l'initiateur sur CETTE connexion précise,
 *        cf. src/net/control_protocol.h).
 *
 * Convention de cadrage du hello, choisie ici faute de format déjà formalisé
 * avec le côté serveur (PR3, développée en parallèle) : `INST_CONTROL_HELLO`
 * (instruction du protocole existant, etii_protocol.h) suivi d'un `int32_t`
 * longueur puis du payload `control_hello_t` encodé, le tout via `send_all`
 * (jamais un `send`/`recv` brut sur un cadrage variable) — c'est le format
 * cadré standard déjà utilisé par le reste du protocole (INST_GET_TO_CHECK_BATCH,
 * INST_POSSIBILITY_ANALYSED_BATCH). Si PR3 a fixé une convention différente,
 * un petit ajustement suffira à réconcilier les deux côtés au moment du merge.
 */
#ifndef eternityII_etii_control_h
#define eternityII_etii_control_h

#include <stdint.h>

#include "net/control_protocol.h"

/**
 * @brief Agrège `fork_statistics[]` (et le record global `max_result`) dans
 *        `out`, sur le modèle de `build_thread_queues_table`
 *        (src/app/etii_client.c) mais en structure binaire plutôt qu'en
 *        chaîne de caractères formatée pour l'affichage console.
 *
 * @param out Structure destination, entièrement réécrite (remise à zéro puis
 *            sommée).
 */
void control_channel_build_stats(control_stats_t *out);

/**
 * @brief Traite UNE trame de contrôle déjà reçue (corps testable par
 *        socketpair sans passer par `ctrl_recv_frame`, sur le modèle de
 *        `communicate_with_client_step`/`control_session_step` déjà utilisés
 *        ailleurs dans le projet pour rendre une boucle réseau testable).
 *
 * - `CTRL_PING` → répond `CTRL_ACK`.
 * - `CTRL_GET_STATS` → agrège les stats courantes et répond `CTRL_STATS`.
 * - `CTRL_COMMAND` → défense en profondeur : revérifie
 *   `control_command_allowed` (le serveur PR3 filtre déjà côté `clientsCmd`,
 *   mais ce client ne fait JAMAIS confiance aveuglément à ce qui arrive sur ce
 *   socket) avant d'exécuter via `do_command_line`, puis répond `CTRL_RESULT`.
 *   Une commande refusée n'est PAS exécutée ; le résultat renvoyé est alors
 *   négatif.
 * - Toute autre valeur de `cmd` : journalisée et ignorée (pas de fermeture de
 *   session pour une trame inattendue non dangereuse).
 *
 * @param socket_id Descripteur du socket connecté (pour la réponse).
 * @param cmd       Commande de trame reçue (cf. @ref ControlCommands).
 * @param payload   Payload reçu, peut être `NULL` si `len == 0`. Pour
 *                   `CTRL_COMMAND`, n'est PAS garanti null-terminé : ce module
 *                   en fait une copie bornée avant de l'utiliser comme chaîne C.
 * @param len       Longueur du payload.
 * @return          0 si la trame a été traitée et la réponse envoyée avec
 *                  succès, -1 en cas d'échec d'envoi réseau (l'appelant doit
 *                  alors considérer la connexion perdue et reconnecter).
 */
int control_channel_handle_frame(int socket_id, uint8_t cmd, const void *payload, int32_t len);

/**
 * @brief Boucle du thread de canal de contrôle (tourne dans le processus
 *        PARENT uniquement). Se (re)connecte avec back-off exponentiel
 *        (sur le modèle de `next_no_work_sleep`, src/app/etii_client.c) tant
 *        que `request != REQUEST_STOP`, effectue le handshake de version
 *        EXACT comme `check_and_connect_to_server` (src/core/datamanager.c),
 *        envoie le hello, puis sert les trames du serveur en boucle via
 *        `control_channel_handle_frame`.
 *
 * `HANDSHAKE_VERSION_REJECTED` arrête CE thread (log clair) sans poser
 * `request = REQUEST_STOP` : ce n'est pas à ce thread annexe de tuer le
 * process principal pour un problème qui ne concerne que ce canal.
 *
 * @param param `control_channel_params_t *` alloué par `start_control_channel`
 *              (libéré par ce thread dès que les champs sont copiés
 *              localement).
 * @return      Toujours `NULL`.
 */
void *run_control_channel(void *param);

/**
 * @brief Démarre le thread détaché du canal de contrôle.
 *
 * Non fatal si `pthread_create` échoue : journalise et poursuit en mode
 * dégradé (sans canal de contrôle), sur le même modèle que
 * `run_server_thread`/`run_checker`/`run_console` (src/app/app_runtime.c,
 * src/ui/console.c) — un canal de pilotage à distance en moins ne doit
 * jamais faire planter le process ni orphaniser les process enfants.
 *
 * @param server_ip Adresse/hôte du serveur (copiée dans les paramètres du
 *                   thread, l'appelant reste propriétaire de la chaîne).
 */
void start_control_channel(const char *server_ip);

/**
 * @brief Force la reconnexion de la session de contrôle en cours.
 *
 * `hello.nb_forks` est désormais relu depuis la globale `g_active_forks` à
 * CHAQUE reconnexion (plus une valeur figée au démarrage du thread) — mais
 * une session déjà établie ne reconnecte pas spontanément juste parce que
 * `g_active_forks` a changé. Cette fonction pose un drapeau consulté par la
 * boucle de service de `run_control_channel` : la session en cours se ferme
 * proprement (même chemin qu'un timeout normal) puis se rouvre avec un hello
 * à jour. Appelée par `orchestrator_spawn_forks` après chaque (re)démarrage
 * des fils. Thread-safe, appelable depuis n'importe quel thread.
 */
void control_channel_request_reconnect(void);

#endif
