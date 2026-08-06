/**
 * @file control_registry.h
 * @brief Registre des sessions de contrôle (canal `INST_CONTROL_HELLO`, v9).
 *
 * Une session de contrôle vit sur la MÊME connexion TCP qu'une session de
 * travail classique (même slot du pool `client_t` d'`etii_server.c`), mais
 * bascule après le hello dans un mode où c'est le SERVEUR qui initie les
 * échanges (`CTRL_PING`, `CTRL_GET_STATS`, `CTRL_COMMAND`, cf.
 * `control_protocol.h`). Ce registre est donc VOLONTAIREMENT indépendant du
 * pool `client_t` : il ne gère aucune socket, seulement l'état "session de
 * contrôle" associé — pid annoncé, mode, et une petite file de commandes en
 * attente pour le thread de session (posées par la console via les commandes
 * `clientsCmd`/`clientsStats`, et par `pause`/`resume` qui diffusent
 * systématiquement à ce registre en plus de leur effet local, dépilées par
 * `run_control_session`/`control_session_step`).
 *
 * Conçu pour être testable sans thread réseau : la logique de file/registre
 * (mutex + `pthread_cond_t` par session, protégeant une petite file circulaire
 * bornée) est exercée directement depuis les tests par des appels séquentiels
 * (mono-thread) et un scénario poste/attend (multi-thread, condvar).
 */
#ifndef eternityII_control_registry_h
#define eternityII_control_registry_h

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "net/control_protocol.h"
#include "app/static_variables.h"   /* PEER_IP_MAX_LEN */

/// Capacité de la file de commandes en attente d'UNE session (tableau
/// circulaire borné). Largement suffisant : les commandes `clientsCmd`/
/// `clientsStats` sont des actes ponctuels de console, pas un flux continu.
#define CONTROL_SESSION_QUEUE_CAP 16

/// Longueur maximale (avec le terminateur nul) d'une ligne de commande postée
/// via `CTRL_COMMAND` — alignée sur les lignes de commande console usuelles,
/// largement sous `CTRL_PAYLOAD_MAX` (control_protocol.h).
#define CONTROL_COMMAND_LINE_MAX 256

/**
 * @brief Vue légère (sans mutex ni détail interne) d'une session de contrôle
 *        active, pour la commande console `clients` (`control_registry_snapshot`).
 */
typedef struct {
    /// PID du processus parent annoncé au hello.
    int32_t pid;
    /// Nombre de forks de recherche gérés par ce parent.
    int32_t nb_forks;
    /// Mode du client : 0 = recherche, 1 = pruner, 2 = pruner GPU.
    uint8_t mode;
    /// Adresse IP du pair de la connexion TCP (`accept()`, non falsifiable —
    /// contrairement au reste du hello, qui reste déclaratif), copiée
    /// telle quelle depuis `client_t.peer_ip` (etii_server.h) à l'enregistrement.
    char peer_ip[PEER_IP_MAX_LEN];
    /// Dernière activité observée (dernier hello, post ou touch).
    time_t last_activity;
    /// 1 si `stats`/`stats_time` proviennent d'un `CTRL_STATS` déjà reçu
    /// (cf. `control_registry_record_stats`), 0 si aucune statistique n'a
    /// encore été récoltée pour cette session (ex. juste après le hello).
    int has_stats;
    /// Dernières statistiques agrégées reçues (valides seulement si `has_stats`).
    control_stats_t stats;
    /// Horodatage Unix de la réception de `stats` (valide seulement si `has_stats`).
    time_t stats_time;
} control_session_info_t;

/**
 * @brief Enregistre une nouvelle session de contrôle dans le registre.
 *
 * @param socket_id Socket de la session (informatif : ce registre n'agit
 *                  jamais sur la socket, c'est l'appelant qui la possède).
 * @param peer_ip   Adresse IP du pair (`client_t.peer_ip`, etii_server.h),
 *                  copiée dans le slot. `NULL` accepté (stocké comme `""`) —
 *                  aucun appelant réel du serveur ne le fait, mais les tests
 *                  qui n'exercent que le hello n'ont pas à la fournir.
 * @param hello     Hello décodé (`control_hello_decode`), copié dans le slot.
 * @return          L'indice du slot alloué (0 ≤ idx < MAX_CONTROL_SESSIONS),
 *                  ou -1 si le registre est plein ou `hello == NULL`.
 */
int control_registry_register(int socket_id, const char *peer_ip, const control_hello_t *hello);

/**
 * @brief Libère le slot `index` (session terminée, propre ou brutale).
 *
 * No-op si `index` est hors bornes : sûr à appeler en fin de session même sur
 * un indice jamais enregistré avec succès (-1).
 *
 * @param index Indice renvoyé par `control_registry_register`.
 */
void control_registry_unregister(int index);

/**
 * @brief Poste une commande dans la file d'attente de la session `index`.
 *
 * Signale la `pthread_cond_t` de la session : un `control_registry_wait_command`
 * bloqué dessus se réveille immédiatement au lieu d'attendre le prochain ping.
 *
 * @param index        Indice de la session cible.
 * @param cmd          Commande de trame (cf. `CTRL_*`, control_protocol.h).
 * @param command_line Ligne de commande texte (pertinente pour `CTRL_COMMAND` ;
 *                      `NULL` accepté pour les commandes sans payload comme
 *                      `CTRL_GET_STATS`).
 * @return             0 si postée, -1 si `index` invalide/session inactive ou
 *                      file pleine.
 */
int control_registry_post_command(int index, uint8_t cmd, const char *command_line);

/**
 * @brief Attend jusqu'à `timeout_ms` qu'une commande arrive pour la session
 *        `index`, la dépile, et la renvoie.
 *
 * Implémenté via `pthread_cond_timedwait` : se réveille dès qu'une commande
 * est postée (pas d'attente active), ou au plus tard après `timeout_ms`.
 *
 * @param index          Indice de la session.
 * @param out_cmd        Reçoit la commande dépilée (si retour 0).
 * @param out_command_line Tampon destination pour la ligne de commande
 *                       (tronquée si trop petit, toujours terminé par '\0').
 * @param bufsize        Taille de `out_command_line`.
 * @param timeout_ms     Délai maximal d'attente, en millisecondes.
 * @return               0 si une commande a été dépilée, 1 si timeout
 *                       (aucune commande : l'appelant enverra un `CTRL_PING`),
 *                       -1 si erreur (index invalide ou session inactive).
 */
int control_registry_wait_command(int index, uint8_t *out_cmd, char *out_command_line,
                                   size_t bufsize, int timeout_ms);

/**
 * @brief Met à jour l'horodatage de dernière activité d'une session (appelé à
 *        chaque échange réussi : ping/ack, commande exécutée, stats reçues).
 *
 * @param index Indice de la session. No-op si hors bornes ou inactive.
 */
void control_registry_touch(int index);

/**
 * @brief Nombre de sessions de contrôle actuellement enregistrées.
 */
int control_registry_count(void);

/**
 * @brief Met en cache les dernières statistiques reçues (`CTRL_STATS`) pour la
 *        session `index`, pour qu'un lecteur synchrone (ex. `GET /api/v1/clients`
 *        de l'API HTTP admin) puisse les relire sans attendre un aller-retour
 *        réseau. Appelée par `control_session_step` (`src/app/etii_server.c`)
 *        juste après un décodage `CTRL_STATS` réussi.
 *
 * @param index Indice de la session. No-op si hors bornes ou session inactive.
 * @param stats Statistiques décodées à mettre en cache (copiées, jamais NULL
 *              attendu de l'appelant).
 */
void control_registry_record_stats(int index, const control_stats_t *stats);

/**
 * @brief Recopie un instantané des sessions actives dans `out` (au plus `max`
 *        entrées), pour la commande console `clients`.
 *
 * @param out Tableau destination.
 * @param max Capacité de `out`.
 * @return    Nombre d'entrées effectivement copiées (0 si `out == NULL` ou
 *            `max <= 0`).
 */
int control_registry_snapshot(control_session_info_t *out, int max);

/**
 * @brief Poste `cmd`/`command_line` à TOUTES les sessions actives (pour
 *        `clientsCmd`, et pour `pause`/`resume` qui diffusent systématiquement,
 *        cf. `pause_interpreter`/`resume_interpreter` dans `command_lines.c`).
 *
 * Si `cmd` vaut `CTRL_COMMAND` et que le premier mot de `command_line` est
 * `"pause"` ou `"resume"`, l'état de pause désiré du registre (cf.
 * `control_registry_desired_pause_state`) est mis à jour en conséquence, AVANT
 * la diffusion — ainsi tout client qui se connecte APRÈS cet appel démarre
 * automatiquement dans le même état, sans qu'il faille rejouer la commande.
 *
 * @return Nombre de sessions auxquelles la commande a bien été postée.
 */
int control_registry_broadcast_command(uint8_t cmd, const char *command_line);

/**
 * @brief Poste `CTRL_GET_STATS` (sans payload) à toutes les sessions actives
 *        (pour la commande console `clientsStats`).
 *
 * @return Nombre de sessions sollicitées.
 */
int control_registry_broadcast_get_stats(void);

/**
 * @brief Indique si un sondage automatique `CTRL_GET_STATS` est dû pour la
 *        session `index` (aucun sondage manuel `clientsStats`/HTTP n'exclut
 *        celui-ci : les deux partagent le même but, tirer un `CTRL_STATS`
 *        frais pour repérer un nouveau record côté client — cf.
 *        `CONTROL_AUTO_STATS_INTERVAL_SEC`, static_variables.h).
 *
 * Effet de bord si le sondage est dû : marque l'instant présent comme
 * dernière tentative, pour que l'appel suivant ne redevienne dû qu'après un
 * nouvel `interval_sec`. Cette marque avance aussi bien sur un sondage
 * automatique réussi qu'échoué (le prochain keepalive retentera de toute
 * façon) — elle n'a pas besoin d'attendre un `CTRL_STATS` décodé.
 *
 * @param index        Indice de la session.
 * @param interval_sec Intervalle minimal (secondes) entre deux sondages.
 * @return              1 si le sondage est dû (et vient d'être marqué comme
 *                      tenté), 0 sinon (trop tôt, index invalide, ou
 *                      `interval_sec <= 0`).
 */
int control_registry_auto_stats_due(int index, int interval_sec);

/**
 * @brief État de pause désiré courant du registre : 0 = résumé (défaut),
 *        1 = en pause. Mis à jour par `control_registry_broadcast_command`
 *        lors d'un `pause`/`resume` console (ou `clientsCmd pause|resume`
 *        équivalent), et appliqué par `control_registry_register` à toute
 *        nouvelle session enregistrée (une commande `CTRL_COMMAND "pause"` est
 *        pré-postée dans sa file avant même le premier `CTRL_PING`).
 *
 * Exposé principalement pour les tests ; la console/`etii_server.c` n'ont pas
 * besoin de le consulter directement.
 */
int control_registry_desired_pause_state(void);

#endif /* eternityII_control_registry_h */
