/**
 * @file        local_socket.h
 * @brief       Contient des méthodes permettants de communiquer via des "sockets" AF_UNIX et PF_UNIX.
 */
#ifndef local_socket_h
#define local_socket_h
#include <stddef.h>
#include <sys/un.h>

/**
 * @brief Taille du plus gros datagramme IPC parent<->fork (octet de type compris).
 *
 * Couvre les trois familles de messages d'ipc_protocol.h : IPC_MSG_STATS
 * (struct client_statistics — croît avec FC_STAT_MAX_K), IPC_MSG_BEST_BOARD
 * (struct possibility_packet) et IPC_MSG_LOG_* (IPC_LINE_MAX). Sert à
 * dimensionner les tampons SO_SNDBUF/SO_RCVBUF des sockets IPC (cf.
 * build_udp_local_socket).
 */
size_t ipc_max_datagram(void);

/**
 * @brief Construction d'une adresse de socket AF_UNIX vers le fichier
 * @return L'adresse du socket AF_UNIX
 */
struct sockaddr_un *build_sockaddr(const char *filename);
/**
 * @brief Construit un socket en PF_UNIX vers l'adresse
 * @return l'identifiant du socket
 */
int build_udp_local_socket(struct sockaddr_un *svaddr);
/**
 * @brief Transmet une commande aux process fils
 * 
 * @param command commande à transmettre
 */
void send_command_to_childs(char *command);
#endif // local_socket_h
