/**
 * @file control_protocol.h
 * @brief Codec du canal de contrôle (connexion TCP additionnelle, serveur
 *        initiateur) : constantes de trame + (dé)sérialisation pure.
 *
 * Module autonome, ne branche ni le serveur ni le client. Livre :
 *  - le format de trame générique CTRL_* (cmd + len + payload), envoyé/reçu
 *    exclusivement via `send_all`/`recv_all` — jamais un `send`/`recv` brut,
 *    qui peut ne transférer qu'une partie du message et désynchroniser le flux ;
 *  - deux structures de payload à champs explicites (`control_hello_t`,
 *    `control_stats_t`) et leurs encodeurs/décodeurs purs (sans I/O,
 *    testables sans socket) ;
 *  - `control_command_allowed`, la liste blanche des commandes console
 *    déclenchables à distance via CTRL_COMMAND.
 *
 * Pourquoi pas `possibility_packet` : celui-ci transporte un seul type de
 * charge utile fixe et a du padding caché malgré `packed` — ne jamais le
 * poser sur le fil sans passer par des champs explicites. Le canal de
 * contrôle transporte des messages hétérogènes de tailles variables : il
 * lui faut son propre format cadré générique.
 *
 * Limite connue (partagée avec le reste du protocole) : l'encodage est
 * l'ordre d'octets natif de la machine — pas d'interopérabilité
 * big-endian/little-endian garantie.
 */
#ifndef eternityII_control_protocol_h
#define eternityII_control_protocol_h

#include <stddef.h>
#include <stdint.h>

#include "net/client_identity.h"

/**
 * @defgroup ControlCommands Commandes de trame du canal de contrôle
 * @{
 */
/// Ping simple (vivacité de la session de contrôle).
#define CTRL_PING 1
/// Acquittement générique.
#define CTRL_ACK 2
/// Demande de statistiques agrégées au client.
#define CTRL_GET_STATS 3
/// Réponse contenant des statistiques agrégées (`control_stats_t`).
#define CTRL_STATS 4
/// Commande console à exécuter à distance (payload = ligne de commande texte).
#define CTRL_COMMAND 5
/// Résultat de l'exécution d'une commande à distance.
#define CTRL_RESULT 6
/// Demande la représentation du meilleur plateau connu du client (agrégat de
/// ses forks). Émise uniquement quand un `CTRL_STATS` reçu juste avant
/// rapporte un `max_result` supérieur au meilleur déjà connu du serveur.
#define CTRL_GET_BEST_BOARD 7
/// Réponse à `CTRL_GET_BEST_BOARD` : payload = `uint8_t valid` puis, si
/// `valid != 0`, `sizeof(struct possibility_packet)` octets bruts (struct
/// copié tel quel, jamais de comparaison par égalité — padding caché malgré
/// `packed`). `valid == 0` : le client n'a encore aucun enregistrement.
#define CTRL_BEST_BOARD 8
/**
 * @}
 */

/// Borne de sécurité sur la taille d'un payload de trame de contrôle, alignée
/// sur `IPC_LINE_MAX` (src/net/ipc_protocol.h, 4000) : même ordre de grandeur
/// que les lignes de commande échangées en IPC parent/enfant.
#define CTRL_PAYLOAD_MAX 4000

/**
 * @brief Envoie une trame de contrôle : `uint8_t cmd` + `int32_t len` +
 *        `len` octets de payload, intégralement via `send_all`.
 *
 * @param socket_id Descripteur du socket connecté.
 * @param cmd       Commande de trame (cf. @ref ControlCommands).
 * @param payload   Tampon du payload, peut être `NULL` si `len == 0`.
 * @param len       Nombre d'octets de payload (0 si aucun).
 * @return          0 si la trame a été intégralement envoyée, -1 sinon.
 */
int ctrl_send_frame(int socket_id, uint8_t cmd, const void *payload, int32_t len);

/**
 * @brief Reçoit une trame de contrôle : `uint8_t cmd` + `int32_t len` puis
 *        `len` octets de payload, intégralement via `recv_all`.
 *
 * Alloue `*out_payload` (à libérer par l'appelant) uniquement si `len > 0` ;
 * `*out_payload` vaut `NULL` si `len == 0`. Une longueur hors borne
 * (`len < 0` ou `len > CTRL_PAYLOAD_MAX`) est rejetée SANS tentative
 * d'allocation, pour ne jamais réagir à une taille absurde reçue d'un pair
 * corrompu ou malveillant.
 *
 * @param socket_id   Descripteur du socket connecté.
 * @param out_payload Reçoit l'adresse du payload alloué (ou `NULL`).
 * @param out_len     Reçoit la longueur du payload reçu.
 * @return             La commande reçue (>= 0), ou -1 en cas d'erreur, de
 *                     flux mort, ou de longueur hors borne.
 */
int ctrl_recv_frame(int socket_id, void **out_payload, int32_t *out_len);

/// Taille MINIMALE sur le fil de `control_hello_t` (label d'identité vide).
/// Depuis v12, `identity.label` est de longueur variable (préfixée) : cette
/// borne sert au test de troncature, PAS au dimensionnement d'un tampon
/// d'émission (cf. `CONTROL_HELLO_WIRE_MAX_SIZE` pour cela).
#define CONTROL_HELLO_WIRE_MIN_SIZE (4 + 4 + CLIENT_IDENTITY_WIRE_MIN_SIZE)
/// Taille MAXIMALE sur le fil (label d'identité à sa longueur maximale) : à
/// utiliser pour dimensionner tout tampon d'émission/réception.
#define CONTROL_HELLO_WIRE_MAX_SIZE (4 + 4 + CLIENT_IDENTITY_WIRE_MAX_SIZE)

/**
 * @brief Annonce d'un client se déclarant canal de contrôle (payload de
 *        CTRL_* futur / INST_CONTROL_HELLO).
 *
 * Étendu en v12 de l'identité déclarée du client (`identity`) —
 * `identity.fork_seq` vaut
 * toujours -1 ici : ce hello représente le process PARENT dans son ensemble,
 * jamais un fork particulier (cf. INST_CLIENT_HELLO, net/etii_protocol.h,
 * pour le hello PAR FORK de la connexion de travail).
 */
typedef struct {
    /// PID du processus parent qui s'annonce.
    int32_t pid;
    /// Nombre de forks de recherche gérés par ce parent.
    int32_t nb_forks;
    /// Identité déclarée (machine_uid, client_uid, fork_seq == -1, mode, label).
    client_identity_t identity;
} control_hello_t;

/**
 * @brief Sérialise `hello` dans `buf` (champ par champ, pas de memcpy du
 *        struct entier — le padding caché fausserait la taille sur le fil).
 *
 * @param hello   Structure source.
 * @param buf     Tampon destination.
 * @param bufsize Taille de `buf` (`CONTROL_HELLO_WIRE_MAX_SIZE` recommandé).
 * @return        Le nombre d'octets écrits, ou -1 si `buf` est trop petit
 *                pour le `identity.label` de `hello`.
 */
int32_t control_hello_encode(const control_hello_t *hello, uint8_t *buf, size_t bufsize);

/**
 * @brief Désérialise `control_hello_t` depuis `buf`.
 *
 * @param buf Tampon source.
 * @param len Nombre d'octets disponibles dans `buf`.
 * @param out Structure destination.
 * @return    0 si OK, -1 si `len` est trop court pour les champs fixes ou
 *            pour le `identity.label` qu'il annonce (cf. `client_identity_decode`).
 */
int control_hello_decode(const uint8_t *buf, int32_t len, control_hello_t *out);

/// Taille sur le fil de `control_stats_t` (7 champs `uint64_t`).
#define CONTROL_STATS_WIRE_SIZE (8 * 7)

/**
 * @brief Statistiques agrégées d'un client, transportées en réponse à
 *        CTRL_GET_STATS (payload de CTRL_STATS).
 */
typedef struct {
    /// Débit de recherche courant (essais/seconde).
    uint64_t shots_per_second;
    /// Nombre de possibilités en stock local.
    uint64_t possibility_stock;
    /// Nombre de possibilités analysées en stock local.
    uint64_t analysed_stock;
    /// Meilleur résultat (nombre de cases placées) atteint.
    uint64_t max_result;
    /// Nombre de possibilités vérifiées par le pruner (0 hors mode pruner).
    uint64_t pruner_checked;
    /// Nombre de possibilités éliminées par le pruner (0 hors mode pruner).
    uint64_t pruner_removed;
    /// Débit de prunage courant (cases étudiées/seconde), même moyenne
    /// glissante 5s que `pruner_cells_per_second` (etii_statistic.h) — le
    /// pendant « coups/s » du pruner (0 hors mode pruner).
    uint64_t pruner_cells_per_second;
} control_stats_t;

/**
 * @brief Sérialise `stats` dans `buf` (champ par champ, largeur fixe).
 *
 * @param stats Structure source.
 * @param buf   Tampon destination, au moins `CONTROL_STATS_WIRE_SIZE` octets.
 * @return      Le nombre d'octets écrits (`CONTROL_STATS_WIRE_SIZE`).
 */
int32_t control_stats_encode(const control_stats_t *stats, uint8_t *buf);

/**
 * @brief Désérialise `control_stats_t` depuis `buf`.
 *
 * @param buf Tampon source.
 * @param len Nombre d'octets disponibles dans `buf`.
 * @param out Structure destination.
 * @return    0 si OK, -1 si `len < CONTROL_STATS_WIRE_SIZE`.
 */
int control_stats_decode(const uint8_t *buf, int32_t len, control_stats_t *out);

/**
 * @brief Classe d'une commande console vis-à-vis des surfaces de pilotage
 *        distant (canal de contrôle binaire `CTRL_COMMAND` et API REST admin
 *        `POST /api/v1/command`) — source unique de vérité dont
 *        `control_command_allowed`/`_privileged`/`_read_only` ne sont que
 *        des projections.
 *
 * Deux axes orthogonaux fusionnés en une seule valeur : « relayable vers un
 * client » vs « strictement serveur/HTTP » ; « lecture pure » vs « modifie
 * un état » — le seul axe qui compte encore pour l'authentification HTTP,
 * depuis que toute commande modifiante exige le même niveau d'auth qu'elle
 * soit relayable ou non.
 */
typedef enum {
    /// Ni relayable ni exposée par l'API HTTP admin (ex. "exit", "restore"
    /// via CTRL_COMMAND, ou toute commande inconnue) : refusée partout, avec
    /// un code d'erreur "commande non reconnue" (jamais un problème d'auth).
    CTRL_CMD_UNKNOWN = 0,
    /// Lecture pure, relayable ET exposée par l'API HTTP SANS authentification
    /// (`clientsWork` : seule occupante à ce jour).
    CTRL_CMD_READ_ONLY,
    /// Modifie un état (local ou, via CTRL_COMMAND, celui d'un client
    /// distant) ; relayable par le canal de contrôle binaire ET par
    /// `clientsCommand`/`clientsCmd` ; exposée par l'API HTTP avec
    /// authentification requise (`pause`, `resume`, `limit`,
    /// `maxStockByThread`, `prunerBatch`, `prunerDfsBudget`,
    /// `clientsCommand`/`clientsCmd`, `start`, `stopForks`, `configApply`,
    /// `config`, `configSave`).
    CTRL_CMD_WRITE_RELAYABLE,
    /// Modifie un état SERVEUR en bloc (remplacement de fichiers `.back`, ou
    /// réorganisation sous verrou de tout le stock de possibilités) ; jamais
    /// relayable à un client (n'aurait de toute façon aucun sens côté
    /// client) ; exposée par l'API HTTP avec authentification requise, même
    /// exigence que `CTRL_CMD_WRITE_RELAYABLE` (`restore`, `backup`,
    /// `sortAsc`, `sortDesc`, `sortDescMulti`, `split`, `regroup`).
    CTRL_CMD_WRITE_SERVER_ONLY,
} control_command_class_t;

/**
 * @brief Classifie `command_name` (fonction pure) — compare uniquement son
 *        premier mot. `NULL` ou vide retourne `CTRL_CMD_UNKNOWN`.
 *
 * `config` sans argument (simple affichage) reste classé
 * `CTRL_CMD_WRITE_RELAYABLE` comme toute autre invocation : ne distingue
 * pas les variantes d'une même commande, donc exige aussi un jeton HTTP
 * bien qu'il s'agisse alors d'un simple affichage.
 */
control_command_class_t control_command_classify(const char *command_name);

/**
 * @brief Commande relayable à distance vers un client — via le canal de
 *        contrôle binaire (`CTRL_COMMAND`) ou `clientsCommand`/`clientsCmd`.
 *        Équivaut à `CTRL_CMD_READ_ONLY` ou `CTRL_CMD_WRITE_RELAYABLE`.
 *
 * `clientsCommand`/`clientsCmd`/`clientsWork` sont des commandes serveur
 * (agissent sur `control_registry`) : les autoriser aussi côté canal de
 * contrôle est inoffensif par construction, `control_registry` étant
 * toujours vide sur un client, donc leur exécution y est un no-op silencieux.
 *
 * `start`/`stopForks`/`configApply`/`config`/`configSave` pilotent à
 * distance le cycle de vie des fils de recherche d'un client, sans sens
 * côté serveur (masquées par `command_is_client_only`). `exit` n'entre
 * jamais dans cette liste.
 *
 * Ne distingue pas lecture/écriture — voir `control_command_read_only`.
 *
 * @return 1 si la commande est autorisée à distance, 0 sinon.
 */
int control_command_allowed(const char *command_name);

/**
 * @brief Commande serveur-seulement déclenchable uniquement par
 *        `POST /api/v1/command` après authentification par jeton Bearer —
 *        jamais via le canal de contrôle binaire. Équivaut à
 *        `CTRL_CMD_WRITE_SERVER_ONLY`.
 *
 * Contient `restore`/`backup` (remplacent l'état du serveur, fichiers
 * `.back`) et `sortAsc`/`sortDesc`/`sortDescMulti`/`split`/`regroup`
 * (réorganisent en bloc, sous verrou, tout le stock de possibilités) — un
 * effet de bord trop large pour avoir un sens côté client. Toujours
 * disjointe de `control_command_allowed`.
 *
 * N'implique plus, à lui seul, un niveau d'authentification HTTP supérieur
 * à `control_command_allowed` — les deux exigent le même jeton dès qu'elles
 * modifient un état. Sa seule portée réelle restante : jamais relayable.
 *
 * @return 1 si la commande est serveur-seulement, 0 sinon.
 */
int control_command_privileged(const char *command_name);

/**
 * @brief Commande de lecture pure (ne modifie aucun état) — utilisé
 *        exclusivement par `POST /api/v1/command` pour décider si
 *        l'authentification par jeton est requise. Équivaut à
 *        `CTRL_CMD_READ_ONLY`.
 *
 * Ne contient que `clientsWork` : consultation pure. Toute commande admise
 * ailleurs qui n'est pas read-only modifie un état et doit être authentifiée.
 *
 * Aucun effet sur le canal de contrôle binaire ni sur la console : ces deux
 * chemins n'ont pas de notion d'authentification et continuent de consulter
 * uniquement `control_command_allowed`.
 *
 * @return 1 si la commande est un pur read, 0 sinon.
 */
int control_command_read_only(const char *command_name);

#endif
