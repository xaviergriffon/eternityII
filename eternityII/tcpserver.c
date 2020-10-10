#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> /* close */
#include <netdb.h> /* gethostbyname */
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(s) close(s)
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
typedef struct in_addr IN_ADDR;
#endif
#include "tcpserver.h"

int create_tcp_server(int port, int nb_max_clients)
{
	int socket_id;
	int optval = 1;
	
	
	if(-1 == (socket_id = socket(PF_INET, SOCK_STREAM, 0)))
	{
		fprintf(stderr, "Impossible de créer un socket\n");
		exit(EXIT_FAILURE);
	}
	
	setsockopt(socket_id, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
	//struct timeval tv;
	//tv.tv_sec = 60;
	//tv.tv_usec = 0;
	// Voir pour mettre le timeout sur la session
	//setsockopt(socket_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));
	
	//setsockopt(socket_id, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(int));
	/* Affectation d'une adresse */
	struct sockaddr_in sockname;
	memset((char *)&sockname, 0, sizeof(struct sockaddr_in));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(INADDR_ANY);
	
	if(-1 == (bind(socket_id, (struct sockaddr *) &sockname, sizeof(struct sockaddr_in))))
	{
		fprintf(stderr, "Erreur sur bind()\n");
		exit(EXIT_FAILURE);
	}
	
	/* mise en écoute de la socket */
	if(-1 == (listen(socket_id, nb_max_clients)))
	{
		fprintf(stderr, "Erreur sur listen()\n");
		exit(EXIT_FAILURE);
	}
	return (socket_id);
}
