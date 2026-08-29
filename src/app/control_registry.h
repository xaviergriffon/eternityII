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
#include "app/app_static_variables.h"   /* PEER_IP_MAX_LEN */

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
    /// Identifiant de session monotone, jamais réutilisé même après
    /// `control_registry_unregister` (contrairement à l'indice de slot du
    /// registre — « session_no n'est pas un slot »). Clé d'adressage acceptée par
    /// `control_registry_send_command_to` (`clientsCmd --to <session_no>`, PR3).
    uint64_t session_no;
    /// PID du processus parent annoncé au hello.
    int32_t pid;
    /// Nombre de forks de recherche gérés par ce parent.
    int32_t nb_forks;
    /// Mode du client : 0 = recherche, 1 = pruner, 2 = pruner GPU.
    uint8_t mode;
    /// Libellé déclaré (option CLI `--name`, ou nom d'hôte par défaut) —
    /// affichage seul, jamais une clé (cf. `client_identity_t.label`).
    char label[CLIENT_LABEL_MAX];
    /// Nonce machine persistant, encodé en hexadécimal pour l'affichage.
    char machine_uid_hex[2 * MACHINE_UID_BYTES + 1];
    /// Nonce de session (process parent), encodé en hexadécimal.
    char client_uid_hex[2 * CLIENT_UID_BYTES + 1];
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
 * @brief Recopie l'identité déclarée (`client_identity_t`) annoncée au hello
 *        de la session `index` — utilisé par `known_clients_registry.h`
 *        (PR4) pour retrouver
 *        `machine_uid`/`client_uid` à des points d'appel (réception de
 *        `CTRL_STATS`, déconnexion) qui ne détiennent que l'indice de session,
 *        pas le hello complet.
 *
 * @param index Indice de la session.
 * @param out   Structure destination (non modifiée si la fonction échoue).
 * @return      0 si `index` désigne une session active (`out` rempli), -1 sinon.
 */
int control_registry_get_identity(int index, client_identity_t *out);

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
 * @brief Compte PUREMENT, dans un instantané déjà pris (`control_registry_snapshot`),
 *        combien de sessions déclarent le rôle recherche contre le rôle
 *        contrôle (PR2, docs/conception/pilotage_type_client.md).
 *
 * `control_session_info_t.mode` porte déjà cette information depuis v9 ; rien
 * ne la comptait jusqu'ici. `CLIENT_MODE_GPU_PRUNER` est compté avec
 * `CLIENT_MODE_PRUNER` dans `*out_nb_prune` — même distinction binaire
 * recherche/contrôle que `fork_role_t` (`fork_orchestrator.h`), le détail
 * CPU/GPU du pruner n'ayant pas sa place dans un dosage de fork.
 *
 * Fonction PURE (aucune I/O, aucun accès au registre lui-même) : opère sur un
 * tableau déjà fourni par l'appelant, pour rester testable sans mutex ni
 * session réseau — même esprit que `fork_stats_all_zero`
 * (`fork_orchestrator.h`).
 *
 * @param sessions      Instantané de sessions (peut être NULL si `n <= 0`).
 * @param n             Nombre d'entrées valides dans `sessions`.
 * @param out_nb_search Sortie (NULL accepté) : nombre de sessions `CLIENT_MODE_SEARCH`.
 * @param out_nb_prune  Sortie (NULL accepté) : nombre de sessions `CLIENT_MODE_PRUNER`
 *                      ou `CLIENT_MODE_GPU_PRUNER`.
 */
void control_registry_count_roles(const control_session_info_t *sessions, int n,
                                   int *out_nb_search, int *out_nb_prune);

/**
 * @brief Comme `control_registry_count_roles`, mais pondère chaque session
 *        par son nombre de forks déclarés (`nb_forks`) plutôt que de compter
 *        une session comme une seule unité (PR4,
 *        docs/conception/pilotage_type_client.md).
 *
 * `control_registry_count_roles` compte des SESSIONS (une par processus
 * parent connecté) : avec une seule machine cliente connectée, quel que soit
 * son nombre de forks, `nb_search`/`nb_prune` vaut au plus 1 — un garde-fou
 * de la politique automatique (« jamais 0 chercheur ») qui compare ce compte
 * à un plancher de 1 ou 2 se retrouve donc TOUJOURS déclenché dès qu'une
 * seule machine est connectée, même si elle fait tourner des dizaines de
 * forks de recherche (bug réel observé en usage : le stock non vérifié
 * s'accumulait indéfiniment sans jamais déclencher d'augmentation de
 * `pruner_forks`, faute de second client connecté). Cette variante somme
 * `nb_forks` au lieu de compter les sessions, pour que le garde-fou
 * compare un nombre de FORKS à un plancher de forks, pas un nombre de
 * MACHINES à un plancher de machines.
 *
 * Fonction PURE, mêmes garanties que `control_registry_count_roles`.
 *
 * @param sessions           Instantané de sessions (peut être NULL si `n <= 0`).
 * @param n                  Nombre d'entrées valides dans `sessions`.
 * @param out_nb_search_forks Sortie (NULL accepté) : Σ `nb_forks` des sessions
 *                            `CLIENT_MODE_SEARCH`.
 * @param out_nb_prune_forks  Sortie (NULL accepté) : Σ `nb_forks` des sessions
 *                            `CLIENT_MODE_PRUNER`/`CLIENT_MODE_GPU_PRUNER`.
 */
void control_registry_count_role_forks(const control_session_info_t *sessions, int n,
                                        int *out_nb_search_forks, int *out_nb_prune_forks);

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
 * @brief Résout `target` vers l'unique session de contrôle active qu'il
 *        désigne et lui poste `cmd`/`command_line` (adressage
 *        `clientsCmd --to <cible>`, PR3).
 *
 * `target` est essayé, dans cet ordre, comme :
 *  1. un `session_no` décimal (chaîne entièrement numérique) ;
 *  2. un `client_uid` hexadécimal (longueur exacte `2*CLIENT_UID_BYTES`) ;
 *  3. un `label` déclaré (égalité exacte de chaîne).
 *
 * `session_no` et `client_uid` sont tous deux des identifiants jamais
 * réattribués à un titulaire différent (cf. `control_session_info_t.session_no`
 * et `client_identity_t.client_uid`) : une résolution par l'un de ces deux
 * champs ne peut donc jamais frapper le mauvais client — soit la session
 * visée existe encore sous cette même identité (le vrai titulaire), soit elle
 * a disparu et la cible est refusée comme inconnue, jamais silencieusement
 * redirigée vers le nouvel occupant du même slot (« session_no n'est pas un
 * slot »). `label` n'étant PAS garanti unique, une
 * cible qui correspond à plusieurs sessions actives est refusée comme
 * ambiguë plutôt que d'en choisir une arbitrairement.
 *
 * Ne fait AUCUNE vérification de liste blanche elle-même : l'appelant
 * (`clients_cmd_interpreter`, command_lines.c) doit avoir déjà validé
 * `command_line` via `control_command_allowed` avant cet appel, exactement
 * comme pour `control_registry_broadcast_command` — cibler une session
 * n'élargit jamais le jeu de commandes autorisées.
 *
 * @param target       Cible telle que saisie par l'opérateur (non NULL).
 * @param cmd          Commande de trame (cf. `CTRL_*`, control_protocol.h).
 * @param command_line Ligne de commande texte (cf. `control_registry_post_command`).
 * @return             1 si la commande a été postée à exactement une session,
 *                     0 si `target` ne désigne aucune session active ou en
 *                     désigne plusieurs (label ambigu, ou file pleine),
 *                     -1 si `target` est `NULL`.
 */
int control_registry_send_command_to(const char *target, uint8_t cmd, const char *command_line);

/**
 * @brief Résout `target` vers le `client_uid` de l'unique session de contrôle
 *        active qu'il désigne, SANS poster de commande (contrairement à
 *        `control_registry_send_command_to`) — lecture pure pour la
 *        consultation « que travaille X ? » (PR6, attribution des analyses
 *        en cours).
 *
 * Mêmes règles de résolution, dans le même ordre, que
 * `control_registry_send_command_to` : `session_no` décimal, puis
 * `client_uid` hexadécimal complet, puis `label` déclaré (égalité exacte) --
 * voir sa documentation pour le détail des garanties (« session_no n'est pas
 * un slot », ambiguïté d'un `label` partagé).
 *
 * @param target         Cible telle que saisie par l'opérateur (non NULL).
 * @param out_client_uid Out : rempli uniquement si le retour vaut 1.
 * @return               1 si résolu à exactement une session (out rempli),
 *                       0 si `target` ne désigne aucune session active ou en
 *                       désigne plusieurs (label ambigu), -1 si `target` est `NULL`.
 */
int control_registry_resolve_client_uid(const char *target, uint8_t out_client_uid[CLIENT_UID_BYTES]);

/**
 * @brief Applique un dosage recherche/contrôle (PR3,
 *        docs/conception/pilotage_type_client.md) : compose et poste
 *        `"config pruner_forks <pruner_forks>"` puis `"configApply"`, à une
 *        cible unique (résolue exactement comme
 *        `control_registry_send_command_to` : `session_no`, `client_uid`,
 *        puis `label`) si `target` est fourni, ou à TOUTES les sessions
 *        actuellement actives sinon (même convention que
 *        `control_registry_broadcast_command`) — l'ergonomie que le document
 *        de conception attend de `clientsRoles`.
 *
 * MÉMORISE aussi, pour chaque session effectivement touchée, `pruner_forks`
 * comme dosage désiré de sa MACHINE (`machine_uid`, jamais `client_uid` ou
 * `session_no` qui ne survivent pas à un redémarrage de processus) : une
 * machine touchée qui se reconnecte plus tard (même après redémarrage du
 * client) le reçoit automatiquement à l'enregistrement
 * (`control_registry_register`), sans qu'il faille rejouer la commande —
 * calqué sur `g_desired_pause_state`. Une machine jamais touchée par
 * `clientsRoles` n'a pas de dosage désiré mémorisé : elle garde le
 * comportement par défaut de PR1 (rôle impliqué par le mode de lancement).
 *
 * Ne fait AUCUNE vérification de liste blanche elle-même (comme
 * `control_registry_send_command_to`) : `config`/`configApply` sont des
 * commandes FIXES construites ici, déjà dans `write_relayable`
 * (`control_protocol.c`), jamais du texte libre d'opérateur — rien à
 * revalider avant de les poster.
 *
 * @param target       `NULL` ou chaîne vide pour diffuser à toutes les
 *                      sessions actives ; sinon cible telle que saisie par
 *                      l'opérateur (`session_no`/`client_uid`/`label`).
 * @param pruner_forks Dosage visé, tel que reçu de l'opérateur — jamais borné
 *                      ici contre `nb_forks` de la cible (inconnu côté
 *                      serveur) : la résolution finale reste
 *                      `resolve_pruner_forks` côté client (PR1).
 * @return             Nombre de sessions effectivement touchées (postées ET
 *                      toujours actives au moment de mémoriser le dosage) :
 *                      0 ou 1 avec `target`, 0..N sans `target`.
 */
int control_registry_apply_role_dosage(const char *target, int pruner_forks);

/**
 * @brief Indique si `client_uid` correspond à une session de contrôle
 *        ACTUELLEMENT active (enregistrée) dans le registre.
 *
 * Correctif PR7 (bail à expiration) : un test réel a montré qu'un bail purement temporel (échéance
 * fixe depuis l'attribution) réclame le travail d'un client encore vivant dès
 * qu'une possibilité met plus longtemps que `analysed_lease_seconds` à
 * s'analyser — rien ne garantit qu'une analyse tienne dans ce budget. Cette
 * fonction fournit le second critère utilisé par
 * `datamanager_reclaim_expired_leases` : tant que le canal de contrôle du
 * client reste enregistré (preuve directe qu'il est vivant — pings/pongs et
 * `CTRL_STATS` réguliers), son travail n'est JAMAIS réclamé, quelle que soit
 * la durée écoulée. Seule l'absence de session active (déconnexion détectée
 * par `run_control_session`, qui appelle `control_registry_unregister`) lève
 * cette protection.
 *
 * @param client_uid `client_uid` recherché (16 octets, jamais NULL).
 * @return           1 si une session active porte ce `client_uid`, 0 sinon
 *                   (aucune correspondance, ou `client_uid == NULL`).
 */
int control_registry_has_active_client(const uint8_t client_uid[CLIENT_UID_BYTES]);

/**
 * @brief Indique si un sondage automatique `CTRL_GET_STATS` est dû pour la
 *        session `index` (aucun sondage manuel `clientsStats`/HTTP n'exclut
 *        celui-ci : les deux partagent le même but, tirer un `CTRL_STATS`
 *        frais pour repérer un nouveau record côté client — cf.
 *        `CONTROL_AUTO_STATS_INTERVAL_SEC`, app_static_variables.h).
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

/**
 * @brief Dosage recherche/contrôle désiré courant pour la machine
 *        `machine_uid` (PR3) : -1 si aucun dosage n'a jamais été mémorisé pour
 *        elle (comportement par défaut de PR1 inchangé), sinon la dernière
 *        valeur reçue via `control_registry_apply_role_dosage`.
 *
 * Exposé principalement pour les tests ; `control_registry_register` est le
 * seul consommateur en production (pré-poste le dosage dans la file toute
 * neuve d'une session qui s'enregistre).
 *
 * @param machine_uid Nonce machine (16 octets, `MACHINE_UID_BYTES`).
 * @return            Le dosage mémorisé, ou -1 si absent.
 */
int control_registry_desired_pruner_forks(const uint8_t machine_uid[MACHINE_UID_BYTES]);

#endif /* eternityII_control_registry_h */
