#ifndef local_socket_h
#define local_socket_h
#include <sys/un.h>
#include <stdio.h>

int local_socket_new(const char *filename);
int create_client_unix(const char *filename);

struct sockaddr_un *build_sockaddr(const char *filename);
int create_udp_local_socket(struct sockaddr_un *svaddr);

void send_command_to_childs(char *command);
size_t size_of_sockaddr_un(struct sockaddr_un *svaddr);
#endif // local_socket_h
