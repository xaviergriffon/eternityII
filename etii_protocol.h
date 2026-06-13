/**
 * @file etii_protocol.h
 * @brief Méthodes pour les échanges entre client et serveur EternityII
 */
#ifndef eternityII_etii_protocol_h
#define eternityII_etii_protocol_h

#include "possibility.h"

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
