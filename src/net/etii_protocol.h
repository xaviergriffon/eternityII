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
/**
 * @}
 */

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
 * @brief Fermeture de la connection
 * 
 * @param socket_id identifiant du socket
 */
void close_socket(int socket_id);

#endif
