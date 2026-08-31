/**
 * @file known_clients_registry.h
 * @brief Registre des clients connus (cumul), distinct de
 *        `control_registry.h` (sessions vivantes, pilotage).
 *
 * Deux registres serveur qui ne se recouvrent pas.
 *
 * |              | `control_registry`                 | ce registre                    |
 * |--------------|-------------------------------------|---------------------------------|
 * | Indexé par   | slot de session (réutilisé)         | `machine_uid` (clé de cumul)    |
 * | Durée de vie | la session TCP                      | la vie du serveur (persisté)    |
 * | Contenu      | hello, file de commandes, dernier `CTRL_STATS` | totaux cumulés, première/dernière vue, statut |
 * | Rôle         | piloter                              | mesurer                         |
 *
 * `control_registry` est vidé à la déconnexion ; celui-ci ne l'est
 * précisément pas — une entrée reste visible (statut « déconnecté ») tant que
 * la borne `MAX_KNOWN_CLIENTS` n'impose pas son éviction.
 *
 * Clé `machine_uid` et pas `client_uid` : `machine_uid` est le seul
 * identifiant qui survit au redémarrage d'un processus client, donc la
 * seule clé stable pour un cumul qui doit lui-même survivre à un
 * redémarrage du serveur. `client_uid` reste la clé de session : une même
 * machine peut avoir plusieurs sessions actives (recherche + pruner sur le
 * même hôte), suivies dans `sessions[]`, bornée par
 * `KNOWN_CLIENT_MAX_SESSIONS`.
 *
 * Persisté sur un fichier `.back` dédié, tolérant en lecture : un fichier
 * absent, illisible ou d'un format inconnu fait repartir le cumul de zéro,
 * jamais échouer le démarrage du serveur. Branché sur les mêmes points
 * d'appel que le reste du stock (autobackup, `--stop-on-solution`,
 * `backup`/`restore`).
 */
#ifndef eternityII_known_clients_registry_h
#define eternityII_known_clients_registry_h

#include <stdint.h>
#include <time.h>

#include "net/client_identity.h"
#include "net/control_protocol.h"
#include "app/app_static_variables.h"   /* MAX_KNOWN_CLIENTS, PEER_IP_MAX_LEN */

/**
 * @brief Vue légère (sans mutex ni détail interne) d'une machine connue, pour
 *        la commande console `knownClients` et `GET /api/v1/known-clients`.
 */
typedef struct {
    /// Nonce machine persistant (`client_identity_t.machine_uid`), encodé en
    /// hexadécimal — clé de cumul de cette entrée.
    char machine_uid_hex[2 * MACHINE_UID_BYTES + 1];
    /// Dernier libellé déclaré vu pour cette machine (`--name`, ou hostname).
    /// Affichage seul, jamais une clé — une machine peut changer de label
    /// d'une exécution à l'autre.
    char label[CLIENT_LABEL_MAX];
    /// Dernière adresse IP du pair observée pour cette machine.
    char peer_ip[PEER_IP_MAX_LEN];
    /// Dernier mode observé (cf. `CLIENT_MODE_*`, client_identity.h).
    uint8_t mode;
    /// 1 si au moins une session de cette machine est actuellement active
    /// (`nb_active_sessions > 0`), 0 sinon (« déconnecté » : l'entrée reste
    /// visible, elle n'est pas supprimée à la dernière déconnexion).
    int connected;
    /// Nombre de sessions actuellement actives pour cette machine (0 si
    /// déconnectée ; peut dépasser 1, ex. recherche + pruner simultanés).
    int nb_active_sessions;
    /// Parmi `nb_active_sessions`, combien déclarent le rôle recherche
    /// (`CLIENT_MODE_SEARCH`).
    /// `nb_active_search + nb_active_prune == nb_active_sessions` toujours.
    int nb_active_search;
    /// Parmi `nb_active_sessions`, combien déclarent le rôle contrôle
    /// (`CLIENT_MODE_PRUNER` ou `CLIENT_MODE_GPU_PRUNER`).
    int nb_active_prune;
    /// Nombre total de connexions (hellos de contrôle) observées pour cette
    /// machine depuis le démarrage du serveur, toutes sessions confondues.
    int nb_connections_total;
    /// Première fois que cette machine a été vue (première connexion).
    time_t first_seen;
    /// Dernière activité observée (connexion, statistiques, ou déconnexion).
    time_t last_seen;
    /// Somme, sur toutes les sessions passées ET en cours de cette machine,
    /// des possibilités vérifiées par le pruner (`control_stats_t.pruner_checked`).
    /// Chaque session redémarre son propre compteur à 0 (process relancé) :
    /// le cumul est calculé par accroissement (delta) observé à chaque
    /// `CTRL_STATS`, jamais par une simple somme des valeurs instantanées —
    /// cf. le fichier .c pour le détail.
    uint64_t total_pruner_checked;
    /// Idem pour les possibilités éliminées par le pruner.
    uint64_t total_pruner_removed;
    /// Meilleur résultat (nombre de cases placées) jamais rapporté par cette
    /// machine, toutes sessions confondues (pic, pas une somme).
    uint64_t best_max_result;
    /// Somme des durées de connexion des sessions déjà terminées de cette
    /// machine (secondes). N'inclut PAS la durée de la session en cours tant
    /// qu'elle n'est pas terminée.
    uint64_t cumulative_uptime_seconds;
} known_client_info_t;

/**
 * @brief Signale une nouvelle session de contrôle pour la machine `identity`
 *        (appelé juste après un `control_registry_register` réussi sur
 *        `INST_CONTROL_HELLO`).
 *
 * Crée l'entrée si `identity->machine_uid` est vu pour la première fois, ou
 * met à jour une entrée existante (`label`/`peer_ip`/`mode`), incrémente
 * `nb_connections_total`/`nb_active_sessions`, ouvre un slot de session. Si
 * le registre est plein sans entrée déconnectée à évincer, la machine n'est
 * simplement pas suivie (log) — ce registre est purement observationnel, ne
 * doit jamais empêcher la session de contrôle de fonctionner.
 */
void known_clients_registry_on_connect(const client_identity_t *identity, const char *peer_ip);

/**
 * @brief Met à jour le cumul de la machine `machine_uid` avec un
 *        `CTRL_STATS` frais reçu pour la session `client_uid`.
 *
 * No-op silencieux si la machine ou la session ne sont pas trouvées : ce
 * registre ne doit jamais faire échouer un échange réseau réel.
 */
void known_clients_registry_on_stats(const uint8_t *machine_uid, const uint8_t *client_uid,
                                      const control_stats_t *stats);

/**
 * @brief Signale la fin de la session `client_uid` de la machine
 *        `machine_uid` (appelé avant `control_registry_unregister`, dont
 *        cette fonction a besoin pour résoudre le slot).
 *
 * Accumule la durée dans `cumulative_uptime_seconds`, décrémente
 * `nb_active_sessions`, libère le slot. L'entrée de la machine n'est pas
 * supprimée : reste consultable, marquée déconnectée à 0 session active.
 */
void known_clients_registry_on_disconnect(const uint8_t *machine_uid, const uint8_t *client_uid);

/**
 * @brief Marqueur de format du fichier `.back` de ce registre (PR5). Vérifié
 *        en tête de fichier par `known_clients_registry_load` ; un magic
 *        différent (fichier d'une autre nature, ou corrompu) fait échouer le
 *        chargement — jamais un octet interprété au hasard. Pas de champ de
 *        version séparé : ce module suit la même convention que les autres
 *        fichiers `.back` du projet (round-trip sur le même build, jamais de
 *        garantie de compatibilité inter-versions du code).
 */
#define KNOWN_CLIENTS_FILE_MAGIC 0x314c434bu /* "KCL1", little-endian */

/**
 * @brief En-tête du fichier `.back` de ce registre : magic puis nombre
 *        d'enregistrements `known_client_record_t` qui suivent.
 */
typedef struct {
    uint32_t magic;
    uint32_t count;
} known_clients_file_header_t;

/**
 * @brief Un enregistrement persisté : uniquement les champs CUMULÉS d'une
 *        machine connue (jamais l'état de session vivante — `nb_active_sessions`,
 *        `sessions[]` — qui n'a aucun sens après un redémarrage du serveur,
 *        cf. arbitrage "les baux ne sont pas persistés" du document de
 *        conception, section 4.7, appliqué ici par analogie).
 *
 * Champs de largeur fixe explicite (comme `control_protocol.h`), même si ce
 * n'est pas un format réseau : `time_t`/`int` ont une largeur dépendante du
 * build, écrire ces types bruts rendrait un fichier PR5 illisible par un
 * binaire compilé différemment sur la même machine (ex. build 32 vs 64 bits).
 */
typedef struct {
    uint8_t machine_uid[MACHINE_UID_BYTES];
    char label[CLIENT_LABEL_MAX];
    char peer_ip[PEER_IP_MAX_LEN];
    uint8_t mode;
    uint32_t nb_connections_total;
    int64_t first_seen;
    int64_t last_seen;
    uint64_t total_pruner_checked;
    uint64_t total_pruner_removed;
    uint64_t best_max_result;
    uint64_t cumulative_uptime_seconds;
} known_client_record_t;

/**
 * @brief Sérialise les champs cumulés de toutes les machines connues dans
 *        `filename` (écriture atomique : fichier temporaire `.tmp` puis
 *        `rename`, comme `best_board_save`/`backup()`). Les champs de
 *        session vivante (`nb_active_sessions`, `connected`, `sessions[]`)
 *        ne sont pas écrits — voir `known_client_record_t`.
 *
 * @param filename Chemin du fichier de destination.
 * @return         0 en cas de succès, -1 sinon (échec I/O).
 */
int known_clients_registry_save(const char *filename);

/**
 * @brief Charge `filename` et FUSIONNE chaque enregistrement dans le
 *        registre en mémoire — jamais un remplacement complet :
 *         - machine absente : nouvelle entrée créée, marquée déconnectée,
 *           avec les totaux du fichier ;
 *         - machine déjà présente (reconnectée avant `restore`) : les
 *           compteurs du fichier s'ajoutent à ceux en mémoire (jamais un
 *           écrasement, qui ferait régresser un cumul déjà mesuré) ;
 *           `label`/`peer_ip`/`mode`/statut restent ceux, plus récents,
 *           déjà en mémoire.
 *
 * Tolérant en lecture : en-tête invalide fait échouer l'appel sans toucher
 * au registre (-1) ; un fichier tronqué applique les enregistrements lus
 * jusque-là et s'arrête proprement (0).
 *
 * @return 0 si l'en-tête a pu être lu et validé (même en cas de troncature
 *         partielle), -1 si le fichier est absent, illisible, ou invalide.
 */
int known_clients_registry_load(const char *filename);

/**
 * @brief Recopie un instantané des machines connues dans `out` (au plus
 *        `max` entrées), pour la commande console `knownClients` et
 *        `GET /api/v1/known-clients`.
 *
 * @param out Tableau destination.
 * @param max Capacité de `out`.
 * @return    Nombre d'entrées effectivement copiées (0 si `out == NULL` ou
 *            `max <= 0`).
 */
int known_clients_registry_snapshot(known_client_info_t *out, int max);

/**
 * @brief Nombre de machines actuellement suivies par le registre (connectées
 *        et déconnectées confondues).
 */
int known_clients_registry_count(void);

/**
 * @brief Compteur monotone de mutations persistées du registre : incrémenté
 *        à chaque appel de `_on_connect`/`_on_stats`/`_on_disconnect` qui
 *        modifie réellement une entrée (jamais sur un rejet).
 *
 * Ne rentre jamais à zéro (jamais réinitialisé par `restore`) : seule la
 * valeur courante importe, comparée par égalité à un instantané précédent —
 * `check_server_step` s'en sert pour savoir si `known_clients_registry_save`
 * a du travail réel depuis sa dernière écriture.
 */
unsigned long long known_clients_registry_mutation_count(void);

#endif /* eternityII_known_clients_registry_h */
