/**
 * @file etii_control.h
 * @brief Canal de contrôle côté client : le processus parent (celui qui
 *        fork les process de recherche, jamais un fork lui-même) ouvre une
 *        connexion TCP additionnelle dédiée vers le serveur, s'annonce via
 *        `INST_CONTROL_HELLO`, puis répond aux trames `CTRL_*` envoyées par
 *        le serveur (initiateur sur cette connexion précise).
 *
 * Cadrage du hello : `INST_CONTROL_HELLO` suivi d'un `int32_t` longueur puis
 * du payload `control_hello_t` encodé, via `send_all` — le format cadré
 * standard déjà utilisé par le reste du protocole.
 */
#ifndef eternityII_etii_control_h
#define eternityII_etii_control_h

#include <stdint.h>

#include "net/control_protocol.h"

/**
 * @brief Agrège `fork_statistics[]` (et le record global `max_result`) dans
 *        `out`, en structure binaire plutôt qu'en chaîne formatée pour
 *        l'affichage console.
 */
void control_channel_build_stats(control_stats_t *out);

/**
 * @brief Le canal de contrôle doit-il continuer à servir ?
 *
 * Vrai tant que le process tourne normalement — et, une fois l'arrêt
 * demandé, tant qu'il reste au moins un fork de travail vivant.
 *
 * Le canal de contrôle est ouvert par le seul process parent. À l'arrêt, il
 * se fermait dès `REQUEST_STOP`, avant que les forks aient fini de vider
 * leur file : le serveur en concluait la mort du client et lui reprenait
 * une possibilité dont les forks avaient déjà poussé les enfants — parent
 * et enfants se retrouvaient tous deux en stock. Ce prédicat ferme cette
 * course côté client (le serveur se protège symétriquement via
 * `owner_client_alive`).
 */
int control_channel_keeps_serving(void);

/**
 * @brief Traite une trame de contrôle déjà reçue (corps testable par
 *        socketpair sans passer par `ctrl_recv_frame`).
 *
 * `CTRL_PING` → répond `CTRL_ACK`. `CTRL_GET_STATS` → agrège les stats et
 * répond `CTRL_STATS`. `CTRL_COMMAND` → défense en profondeur : revérifie
 * `control_command_allowed` (ce client ne fait jamais confiance aveuglément
 * à ce qui arrive sur ce socket) avant d'exécuter via `do_command_line`,
 * puis répond `CTRL_RESULT` (résultat négatif si refusée, non exécutée).
 * Toute autre valeur de `cmd` : journalisée et ignorée.
 *
 * @param payload Pour `CTRL_COMMAND`, n'est pas garanti null-terminé : ce
 *                module en fait une copie bornée avant usage comme chaîne C.
 * @return 0 si traitée et réponse envoyée avec succès, -1 en cas d'échec
 *         d'envoi réseau (connexion à considérer perdue).
 */
int control_channel_handle_frame(int socket_id, uint8_t cmd, const void *payload, int32_t len);

/**
 * @brief Boucle du thread de canal de contrôle (processus parent
 *        uniquement). Se (re)connecte avec back-off exponentiel tant que
 *        `request != REQUEST_STOP`, effectue le handshake de version, envoie
 *        le hello, puis sert les trames du serveur via
 *        `control_channel_handle_frame`.
 *
 * `HANDSHAKE_VERSION_REJECTED` arrête ce thread sans poser
 * `request = REQUEST_STOP` : ce n'est pas à ce thread annexe de tuer le
 * process principal pour un problème propre à ce canal.
 *
 * @param param `control_channel_params_t *` alloué par
 *              `start_control_channel` (libéré dès que les champs sont
 *              copiés localement).
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
