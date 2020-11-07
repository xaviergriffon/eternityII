#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <winsock2.h>
#define sleep(s) Sleep(s*1000)
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

#include "tcpclient.h"

#define NB_ATTEMPTS 10

int create_tcp_client(const char *hostname, int port)
{
	struct hostent *host_address;
	struct sockaddr_in sockname;
	int optval;
	int socket_id;
	
	/* Recherche de l'adresse de la machine distance */
	if(NULL == (host_address = gethostbyname(hostname)))
	{
		printf("Impossible de d'identifier la machine '%s'\n",hostname);
		fprintf(stderr, "Impossible de d'identifier la machine '%s'\n",hostname);
		return -1;
	}
	
	/* création de la socket */
	if(-1 == (socket_id = socket(PF_INET,SOCK_STREAM, 0)))
	{
		printf("Impossible de créer une socket\n");
		fprintf(stderr, "Impossible de créer une socket\n");
		return -1;
	}
	
	/* Changement d'un paramètre pour rendre la socket réutilisable directement */
	optval = 1;
	setsockopt(socket_id, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
	struct timeval tv;
	tv.tv_sec = 60;
	tv.tv_usec = 0;
	setsockopt(socket_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));
	
	//setsockopt(socket_id, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(int));
	
	/* connexion au serveur */
	sockname.sin_family = host_address->h_addrtype;
	sockname.sin_port = htons(port);
	memcpy((char*) &(sockname.sin_addr.s_addr), host_address->h_addr_list[0], host_address->h_length);
	int t;
	for(t = 1; t <= NB_ATTEMPTS;t++)
	{
		if(-1 == (connect(socket_id, (struct sockaddr *)&sockname, sizeof(struct sockaddr_in))))
		{
			fprintf(stderr, "Impossible de connecter la socket au serveur '%s' tentative:%i\n",hostname,t);
			closesocket(socket_id);
			if (t == NB_ATTEMPTS) {
				socket_id = -1;
				break;
			} else {
				sleep(1);
			}
			
		} else
		{
			break;
		}
	}
	return (socket_id);
}