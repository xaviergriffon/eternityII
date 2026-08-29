/**
 * @file known_clients_registry.h
 * @brief Registre des clients CONNUS (cumul, PR4), distinct de
 *        `control_registry.h` (sessions vivantes, pilotage).
 *
 * Deux registres serveur qui ne se recouvrent pas.
 *
 * |              | `control_registry` (existant)      | ce registre (nouveau)          |
 * |--------------|-------------------------------------|---------------------------------|
 * | Indexé par   | slot de session (réutilisé)         | `machine_uid` (clé de cumul)    |
 * | Durée de vie | la session TCP                      | la vie du serveur (PR5 : + persisté) |
 * | Contenu      | hello, file de commandes, dernier `CTRL_STATS` | totaux cumulés, première/dernière vue, statut |
 * | Rôle         | piloter                              | mesurer                         |
 *
 * `control_registry` est vidé à la déconnexion ; celui-ci ne l'est
 * précisément PAS — une entrée reste visible (statut « déconnecté ») tant que
 * la borne `MAX_KNOWN_CLIENTS` n'impose pas son éviction.
 *
 * Pourquoi `machine_uid` et pas `client_uid` comme clé : `machine_uid` est le
 * SEUL identifiant qui survit au redémarrage d'un processus client (cf.
 * `client_identity.h`) — c'est donc la seule clé stable pour un cumul qui doit
 * lui-même survivre à un redémarrage du serveur (PR5, persistance ci-dessous).
 * `client_uid` reste la clé de SESSION : une même
 * machine peut avoir plusieurs sessions actives simultanément (ex. un client
 * de recherche et un pruner sur le même hôte), chacune suivie séparément dans
 * le petit tableau `sessions[]` d'une entrée, borné par
 * `KNOWN_CLIENT_MAX_SESSIONS`.
 *
 * Persisté depuis PR5 sur un fichier `.back` dédié (`known_clients_registry_save`/
 * `_load` ci-dessous), tolérant en lecture : un fichier absent, illisible ou
 * d'un format inconnu fait simplement repartir le cumul de zéro, jamais
 * échouer le démarrage du serveur. Branché sur les mêmes points d'appel que
 * le reste du stock (autobackup, `--stop-on-solution`, commandes console
 * `backup`/`restore` — voir `src/app/etii_server.c` et `src/ui/command_lines.c`).
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
 *        (appelé par `communicate_with_client_step`, src/app/etii_server.c,
 *        juste après un `control_registry_register` réussi sur
 *        `INST_CONTROL_HELLO` — mêmes données, `hello.identity`).
 *
 * Crée l'entrée si `identity->machine_uid` est vu pour la première fois
 * (`first_seen` = maintenant), ou met à jour une entrée existante : `label`/
 * `peer_ip`/`mode` (dernière valeur déclarée gagne), incrémente
 * `nb_connections_total` et `nb_active_sessions`, ouvre un slot de session
 * pour `identity->client_uid` (baseline des compteurs pruner à 0 — cf. doc du
 * .c). Si le registre est plein ET qu'aucune entrée déconnectée n'est
 * disponible pour éviction, la machine n'est simplement PAS suivie (avertit
 * en log) : cette absence ne doit jamais empêcher la session de contrôle de
 * fonctionner, ce registre est purement observationnel.
 *
 * @param identity Identité déclarée de la session (non NULL).
 * @param peer_ip  Adresse IP du pair (`client_t.peer_ip`), `NULL` accepté
 *                 (stockée comme `""`).
 */
void known_clients_registry_on_connect(const client_identity_t *identity, const char *peer_ip);

/**
 * @brief Met à jour le cumul de la machine désignée par `machine_uid` avec un
 *        `CTRL_STATS` frais reçu pour la session `client_uid` (appelé par
 *        `control_session_poll_stats`, src/app/etii_server.c, juste après
 *        `control_registry_record_stats`).
 *
 * No-op silencieux si la machine ou la session ne sont pas trouvées (ex.
 * registre plein au moment du connect, ou appel hors séquence) : ce registre
 * ne doit jamais faire échouer un échange réseau réel.
 *
 * @param machine_uid Nonce machine (16 octets, `MACHINE_UID_BYTES`).
 * @param client_uid  Nonce de session (16 octets, `CLIENT_UID_BYTES`) —
 *                     désigne le slot de session à mettre à jour.
 * @param stats       Statistiques décodées (non NULL attendu).
 */
void known_clients_registry_on_stats(const uint8_t *machine_uid, const uint8_t *client_uid,
                                      const control_stats_t *stats);

/**
 * @brief Signale la fin de la session `client_uid` de la machine
 *        `machine_uid` (appelé par `run_control_session`,
 *        src/app/etii_server.c, AVANT `control_registry_unregister` — cette
 *        fonction a besoin de l'identité encore présente dans
 *        `control_registry` pour résoudre le slot).
 *
 * Accumule la durée de cette session dans `cumulative_uptime_seconds`,
 * décrémente `nb_active_sessions`, libère le slot de session. L'entrée de la
 * machine N'EST PAS supprimée : elle reste consultable, marquée déconnectée
 * dès que `nb_active_sessions` atteint 0.
 *
 * @param machine_uid Nonce machine (16 octets).
 * @param client_uid  Nonce de session (16 octets).
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
 * @brief Charge `filename` (écrit par `known_clients_registry_save`) et
 *        FUSIONNE chaque enregistrement dans le registre en mémoire — jamais
 *        un remplacement complet, contrairement à `restore()`
 *        (`src/core/datamanager.c`) sur le stock de possibilités :
 *         - machine absente du registre (cas normal, redémarrage du
 *           serveur) : nouvelle entrée créée, marquée déconnectée
 *           (`nb_active_sessions = 0`), avec les totaux du fichier ;
 *         - machine déjà présente (une session s'est reconnectée avant que
 *           `restore` ne soit exécuté) : les compteurs cumulés du fichier
 *           s'AJOUTENT à ceux déjà en mémoire (jamais un écrasement, qui
 *           ferait régresser un cumul déjà mesuré depuis le démarrage du
 *           serveur) ; `label`/`peer_ip`/`mode`/le statut connecté restent
 *           ceux, plus récents, déjà en mémoire.
 *
 * Tolérant en lecture, comme documenté en tête de ce fichier : un fichier
 * absent ou dont l'en-tête ne correspond pas à `KNOWN_CLIENTS_FILE_MAGIC`
 * fait échouer l'appel SANS toucher au registre (retour -1) ; un fichier
 * tronqué en cours d'enregistrements applique ceux lus jusque-là et s'arrête
 * proprement (retour 0) — jamais de crash sur un fichier corrompu ou d'une
 * version antérieure du format.
 *
 * @param filename Chemin du fichier source.
 * @return         0 si l'en-tête a pu être lu et validé (y compris en cas de
 *                 troncature partielle des enregistrements), -1 si le fichier
 *                 est absent, illisible, ou d'un format inconnu.
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
 * @brief Compteur monotone de mutations persistées du registre : incrémenté à chaque
 *        appel de `known_clients_registry_on_connect`/`_on_stats`/
 *        `_on_disconnect` qui modifie réellement une entrée (jamais sur un
 *        rejet — registre plein, identité NULL, machine/session introuvable).
 *        Ne rentre jamais à zéro (n'est jamais réinitialisé par `restore`,
 *        propriété volontaire : seule la valeur COURANTE importe, comparée
 *        par égalité à un instantané précédent — `check_server_step` s'en
 *        sert pour savoir si `known_clients_registry_save` a du travail réel
 *        à faire depuis sa dernière écriture, sans dupliquer cette logique.
 *        Ne compte PAS les mutations en mémoire pure des champs volatils
 *        (`nb_active_sessions`, `sessions[]`) séparément de celles des champs
 *        persistés : les deux catégories changent ensemble aux mêmes points
 *        d'appel, une distinction plus fine n'aurait aucune valeur pratique
 *        ici.
 */
unsigned long long known_clients_registry_mutation_count(void);

#endif /* eternityII_known_clients_registry_h */
