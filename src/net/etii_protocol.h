/**
 * @file etii_protocol.h
 * @brief Méthodes pour les échanges entre client et serveur EternityII
 */
#ifndef eternityII_etii_protocol_h
#define eternityII_etii_protocol_h

#include "core/possibility.h"

/**
 * @defgroup Instructions Liste des instructions
 * @{
 */
#define INST_ERROR -1
#define INST_ADD 1
#define INST_GET 2
#define INST_SOLUTION 3
#define INST_END 4
#define INST_CONSIDERED 5
#define INST_NULL 6
#define INST_POSSIBILITY_ANALYSED 7
#define INST_TEST_CONNECTED 8
#define INST_CHECK_VERSION 9
#define INST_SUPPORTED_VERSION 10
#define INST_UNSUPPORTED_VERSION 11
/// Demande d'une possibilité à vérifier (client pruner) : le serveur sert le
/// pool non vérifié uniquement, sans repli sur le pool vérifié.
#define INST_GET_TO_CHECK 12
/// Demande par LOT de possibilités à vérifier (client pruner). Le client envoie
/// l'instruction suivie d'un `int32` N (nombre souhaité, borné par lui-même pour
/// maîtriser sa mémoire). Le serveur répond un `int32` K (0 ≤ K ≤ N) puis, si
/// K > 0, K × `sizeof(possibility_packet)` octets contigus. Un seul aller-retour
/// pour tout le lot (cf. INST_GET_TO_CHECK qui en exige un par possibilité).
#define INST_GET_TO_CHECK_BATCH 13
/// Acquittement par LOT de possibilités analysées (client pruner). Le client
/// envoie l'instruction, un `int32` M, puis M paquets contigus. Le serveur
/// répond un unique INST_CONSIDERED.
#define INST_POSSIBILITY_ANALYSED_BATCH 14
/// Sonde de « faim » du serveur (v8). Le client envoie l'instruction seule ;
/// le serveur répond un `int32` N ≥ 0 : le nombre de possibilités qu'il
/// souhaiterait recevoir pour ne pas laisser d'autres clients sans travail
/// (0 = stock suffisant). Émise par le thread d'alimentation du client à la
/// place du keepalive : elle sert aussi de preuve d'activité de la session.
#define INST_NEED_WORK 15
/// Annonce d'un canal de contrôle (v9). Un client (le processus PARENT, pas
/// les forks de recherche) s'annonce comme canal de contrôle après le
/// handshake de version. Ouvre une session où le SERVEUR devient l'initiateur
/// des échanges suivants (cf. control_protocol.h).
#define INST_CONTROL_HELLO 16
/**
 * @}
 */

/**
 * @brief Verdict de l'interprétation de la réponse du serveur au handshake de version.
 * @see handshake_verdict
 */
typedef enum {
    HANDSHAKE_OK = 0,           /**< INST_SUPPORTED_VERSION : version acceptée, poursuivre. */
    HANDSHAKE_VERSION_REJECTED, /**< INST_UNSUPPORTED_VERSION : refus réel → arrêter le client. */
    HANDSHAKE_RETRY             /**< Toute autre réponse (timeout/INST_END, fermeture, octet
                                     inattendu) : échec transitoire → réessayer plus tard. */
} handshake_verdict_t;

/**
 * @brief Interprète la réponse du serveur au contrôle de version (fonction pure).
 *
 * Sépare la DÉCISION (3 cas distincts) de l'effet de bord (arrêt du client vs
 * nouvelle tentative). Le point clé : un `INST_END` de timeout — renvoyé par
 * `recv_instruction` quand le serveur saturé ne répond pas — ne doit JAMAIS être
 * confondu avec un refus de version. Isolée ici, cette logique est testable sans
 * réseau ni état global.
 *
 * @param result Octet renvoyé par `recv_instruction` après l'envoi de INST_CHECK_VERSION.
 * @return       Le verdict correspondant.
 */
handshake_verdict_t handshake_verdict(int8_t result);

/**
 * @brief Structure pour les échanges
 */
typedef struct
{
    /// Instruction de l'échange
    uint8_t instruction;
    /// Possibilité fournie
    struct possibility_packet possibility;

} __attribute__((__packed__)) packet;

/**
 * @brief Réception d'un instruction
 * 
 * @param socket_id identifiant du socket à écouter
 * @return int8_t instruction réceptionnée
 *    @see Instructions
 */
int8_t recv_instruction(int socket_id);

/**
 * @brief Envoie une instruction via le socket
 * 
 * @param socket_id identifiant du socket
 * @param instruction instruction
 * @return long la taille du message envoyé (<= 0 == erreur)
 *    @see Instructions
 */
long send_instruction(int socket_id, int8_t instruction);

/**
 * @brief Reçoit exactement `len` octets (boucle sur les réceptions partielles).
 *
 * Indispensable pour les échanges par lot : un bloc de plusieurs Mo arrive
 * fragmenté en plusieurs segments TCP, l'hypothèse « un paquet = un `recv` »
 * (valable pour un seul `possibility_packet`) ne tient plus. Réessaie sur EINTR.
 *
 * @param socket_id Descripteur du socket connecté.
 * @param buf       Tampon de destination (au moins `len` octets).
 * @param len       Nombre d'octets à recevoir.
 * @return          `len` si tout a été reçu, le total partiel si le pair ferme
 *                  la connexion avant, -1 sur erreur.
 */
long recv_all(int socket_id, void *buf, size_t len);

/**
 * @brief Envoie exactement `len` octets (boucle sur les envois partiels).
 *
 * Pendant de `recv_all` : un grand `send` peut n'écrire qu'une partie du tampon.
 * Réessaie sur EINTR.
 *
 * @param socket_id Descripteur du socket connecté.
 * @param buf       Tampon source.
 * @param len       Nombre d'octets à envoyer.
 * @return          `len` si tout a été envoyé, -1 sur erreur.
 */
long send_all(int socket_id, const void *buf, size_t len);

/**
 * @brief Indique si le socket est connecté
 *
 * @param socket_id identifiant du socket
 * @return int 1 si connecté et sinon 0
 */
int is_connected(int socket_id);

/**
 * @brief Interroge le serveur sur sa « faim » (INST_NEED_WORK).
 *
 * Envoie l'instruction puis lit la réponse `int32` : le nombre de possibilités
 * que le serveur souhaiterait recevoir (0 = stock suffisant). Tient lieu de
 * keepalive : un échange réussi prouve la session vivante. En cas d'échec
 * d'envoi ou de réception, le socket est fermé (shutdown + close), comme le
 * fait `is_connected`.
 *
 * @param socket_id Descripteur du socket connecté.
 * @return          La faim du serveur (≥ 0), ou -1 si la connexion est rompue
 *                  (le socket est alors fermé).
 */
int32_t poll_server_hunger(int socket_id);

/**
 * @brief Fermeture de la connection
 * 
 * @param socket_id identifiant du socket
 */
void close_socket(int socket_id);

#endif
