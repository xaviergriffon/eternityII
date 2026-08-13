/**
 * @file control_protocol.h
 * @brief Codec du futur canal de contrôle (connexion TCP additionnelle,
 *        SERVEUR initiateur) : constantes de trame + (dé)sérialisation pure.
 *
 * Ce module est autonome : il ne branche ni le serveur ni le client sur le
 * canal de contrôle (ce sera l'objet de PR ultérieures). Il livre :
 *  - le format de trame générique CTRL_* (cmd + len + payload), envoyé/reçu
 *    exclusivement via `send_all`/`recv_all` (etii_protocol.h) — jamais un
 *    `send`/`recv` brut, qui peut ne transférer qu'une partie du message et
 *    désynchroniser tout le flux (cf. etii_protocol.h) ;
 *  - deux structures de payload à champs explicites (`control_hello_t`,
 *    largeur fixe sauf son `identity.label` préfixé par sa longueur, cf.
 *    net/client_identity.h ; `control_stats_t`, largeur fixe) et leurs
 *    encodeurs/décodeurs purs (sans I/O, testables sans socket) ;
 *  - `control_command_allowed`, la liste blanche des commandes console
 *    déclenchables à distance via CTRL_COMMAND.
 *
 * Pourquoi pas `packet`/`possibility_packet` : ceux-ci transportent un seul
 * type de charge utile (`possibility_packet`, structure `packed` mais avec du
 * padding caché malgré `packed` — ne JAMAIS memcmp/hash le struct brut, ne
 * JAMAIS le poser sur le fil sans passer par des champs explicites). Le canal
 * de contrôle doit transporter des messages hétérogènes de tailles variables
 * (hello, stats agrégées, ligne de commande texte) : il lui faut son propre
 * format cadré générique.
 *
 * Limite connue (partagée avec le reste du protocole existant, qui envoie
 * déjà des `int32` bruts sans `htonl`) : l'encodage est l'ordre d'octets
 * natif de la machine — machines homogènes supposées, pas d'interopérabilité
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
/// ses forks, `g_client_aggregate_best_board`, cf. `core/best_board.h`).
/// Émise par `control_session_step` (etii_server.c) uniquement quand un
/// `CTRL_STATS` reçu juste avant rapporte un `max_result` supérieur au
/// meilleur déjà connu du serveur — pas à chaque tour.
#define CTRL_GET_BEST_BOARD 7
/// Réponse à `CTRL_GET_BEST_BOARD` : payload = `uint8_t valid` puis, si
/// `valid != 0`, `sizeof(struct possibility_packet)` octets bruts (même
/// convention que le protocole de travail INST_GET/INST_ADD : struct copié
/// tel quel sur le fil, round-trip valide sur le même build — jamais de
/// comparaison par égalité sur ces octets, cf. la mise en garde sur le
/// padding caché de `possibility_packet` malgré `packed`). `valid == 0` :
/// le client n'a encore aucun enregistrement (cas normal juste après
/// démarrage, avant le premier record local).
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
 * Étendu en v12 (PR2) de l'identité déclarée du client (`identity`) —
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
 *        `POST /api/v1/command`) — SOURCE UNIQUE DE VÉRITÉ dont
 *        `control_command_allowed`/`control_command_privileged`/
 *        `control_command_read_only` (ci-dessous) ne sont que des projections.
 *
 * Deux axes orthogonaux existent pour une commande admise sur au moins une
 * surface, et cet enum les fusionne en UNE valeur plutôt que de les laisser
 * se déduire par croisement de plusieurs listes :
 *  - « relayable vers un client » (CTRL_COMMAND, `clientsCommand`/`clientsCmd`)
 *    vs « strictement serveur/HTTP » (jamais poussée à un client) ;
 *  - « lecture pure » vs « modifie un état » — le SEUL axe qui compte encore
 *    pour l'authentification HTTP (`--http-token-file`), depuis que
 *    l'exigence « toute commande de modification doit être authentifiée » a
 *    aligné le niveau d'auth de TOUTES les commandes modifiantes, qu'elles
 *    soient relayables ou non. Avant cet alignement, "allowed" (relayable) et
 *    "privileged" (jamais relayée) codaient AUSSI, implicitement, deux
 *    niveaux d'auth distincts sur l'API HTTP — ce n'est plus le cas
 *    aujourd'hui, d'où la confusion à lire seulement leurs noms.
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
 * @brief Classifie `command_name` (fonction pure, cœur-métier, sans
 *        dépendance réseau) — compare uniquement son premier mot (avant un
 *        éventuel espace/argument). `command_name == NULL` ou vide retourne
 *        `CTRL_CMD_UNKNOWN` explicitement, jamais de déréférencement.
 *
 * `config` SANS argument (simple affichage) reste classé
 * `CTRL_CMD_WRITE_RELAYABLE` comme toute autre invocation de `config` : cette
 * fonction ne distingue pas les variantes d'une même commande, seulement son
 * premier mot — donc `config` seul exige aussi un jeton sur l'API HTTP, bien
 * qu'il s'agisse alors d'un simple affichage.
 *
 * @param command_name Nom (ou ligne complète) de la commande à classifier.
 * @return              La classe de `command_name` (@ref control_command_class_t).
 */
control_command_class_t control_command_classify(const char *command_name);

/**
 * @brief Commande relayable à distance vers un CLIENT — via le canal de
 *        contrôle binaire (`CTRL_COMMAND`, poussé par le SERVEUR) ou la
 *        console `clientsCommand`/`clientsCmd`. Équivaut à
 *        `control_command_classify(command_name) != CTRL_CMD_UNKNOWN &&
 *        != CTRL_CMD_WRITE_SERVER_ONLY` (c.-à-d. `CTRL_CMD_READ_ONLY` ou
 *        `CTRL_CMD_WRITE_RELAYABLE`).
 *
 * "clientsCommand"/"clientsCmd" et "clientsWork" sont des commandes SERVEUR
 * (elles agissent sur `control_registry`, jamais sur les forks de recherche
 * d'un client) : les admettre ici les rend exécutables via
 * `admin_apply_remote_command` (POST /api/v1/command). Les autoriser aussi
 * côté canal de contrôle (`CTRL_COMMAND`, poussé par le SERVEUR vers un
 * client) est inoffensif par construction : sur un client, `control_registry`
 * est toujours vide, donc leur exécution y est un no-op silencieux — même
 * raisonnement déjà appliqué à pause/resume (cf. leurs interpréteurs dans
 * command_lines.c).
 *
 * "start"/"stopForks"/"configApply"/"config"/"configSave" pilotent à
 * distance le cycle de vie des fils de recherche d'un CLIENT
 * (`fork_orchestrator.h`) : elles n'ont de sens que poussées vers un client
 * (jamais un serveur, où elles sont de toute façon masquées — cf.
 * `command_is_client_only`, command_lines.c). "exit" n'entre PAS dans cette
 * liste et n'y entrera jamais.
 *
 * NE distingue PAS lecture/écriture — voir `control_command_read_only` pour
 * cet axe, orthogonal.
 *
 * @param command_name Nom (ou ligne complète) de la commande à vérifier.
 * @return              1 si la commande est autorisée à distance, 0 sinon
 *                      (y compris pour `command_name == NULL` ou vide).
 */
int control_command_allowed(const char *command_name);

/**
 * @brief Commande SERVEUR-SEULEMENT déclenchable uniquement par
 *        `POST /api/v1/command` (API REST admin, `src/net/http_server.c`)
 *        après authentification par jeton Bearer (`--http-token-file`) —
 *        JAMAIS via le canal de contrôle binaire (`CTRL_COMMAND`), qui reste
 *        strictement borné à `control_command_allowed`. Équivaut à
 *        `control_command_classify(command_name) == CTRL_CMD_WRITE_SERVER_ONLY`.
 *
 * Contient "restore" et "backup" (les deux seules commandes de
 * `command_lines.c` capables de remplacer/écraser l'état du serveur, fichiers
 * `.back`) ainsi que "sortAsc", "sortDesc", "sortDescMulti", "split" et
 * "regroup" : ces cinq dernières ne remplacent aucun fichier mais réorganisent
 * en bloc, sous verrou, l'ensemble du stock de possibilités du serveur — un
 * effet de bord suffisamment large (et potentiellement coûteux, `sortDescMulti`
 * est multi-thread) pour n'avoir de sens que côté serveur, jamais relayée à
 * un client. Toujours disjointe de `control_command_allowed` — une commande
 * n'est jamais dans les deux listes à la fois.
 *
 * Ce prédicat n'implique PLUS, à lui seul, un niveau d'authentification HTTP
 * supérieur à `control_command_allowed` — les deux exigent aujourd'hui le
 * même jeton Bearer dès qu'elles modifient un état (voir
 * `control_command_class_t` ci-dessus pour la raison). Sa seule portée
 * réelle restante est : jamais relayable à un client.
 *
 * Même style que `control_command_allowed` : compare uniquement le premier
 * mot de `command_name`, gère `NULL` explicitement (retourne 0).
 *
 * @param command_name Nom (ou ligne complète) de la commande à vérifier.
 * @return              1 si la commande est serveur-seulement, 0 sinon (y
 *                      compris pour `command_name == NULL` ou vide).
 */
int control_command_privileged(const char *command_name);

/**
 * @brief Commande de LECTURE PURE (ne modifie aucun état, ni local ni
 *        distant) — utilisé EXCLUSIVEMENT par `POST /api/v1/command`
 *        (`src/net/http_server.c`) pour décider si l'authentification par
 *        jeton Bearer est requise. Équivaut à
 *        `control_command_classify(command_name) == CTRL_CMD_READ_ONLY`.
 *
 * Ne contient que "clientsWork" : une consultation pure (lit une attribution
 * déjà enregistrée côté serveur, n'envoie jamais rien à un client). Toute
 * commande admise par `control_command_allowed` OU `control_command_privileged`
 * qui n'est PAS `control_command_read_only` modifie un état et doit donc être
 * authentifiée sur l'API HTTP admin (voir `handle_command_route`,
 * `src/net/http_server.c` — ce module n'a connaissance ni de l'API HTTP ni du
 * jeton lui-même).
 *
 * N'a AUCUN effet sur le canal de contrôle binaire (`CTRL_COMMAND`) ni sur la
 * console : ces deux chemins n'ont pas de notion d'authentification (l'un est
 * initié par le serveur lui-même vers un client déjà connecté, l'autre suppose
 * un accès shell déjà de confiance) et continuent de consulter uniquement
 * `control_command_allowed`, inchangé.
 *
 * Même style que `control_command_allowed`/`control_command_privileged` :
 * compare uniquement le premier mot de `command_name`, gère `NULL`
 * explicitement (retourne 0).
 *
 * @param command_name Nom (ou ligne complète) de la commande à vérifier.
 * @return              1 si la commande est un pur read, 0 sinon (y compris
 *                      pour `command_name == NULL` ou vide).
 */
int control_command_read_only(const char *command_name);

#endif
