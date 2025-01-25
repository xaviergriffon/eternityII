/**
 * @file        local_socket.h
 * @brief       Contient des méthodes permettants de communiquer via des "sockets" AF_UNIX et PF_UNIX.
 */
#ifndef local_socket_h
#define local_socket_h
#include <sys/un.h>
#include <stdio.h>
#include <sys/socket.h>

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
