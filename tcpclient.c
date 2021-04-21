#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> /* close */
#include <netdb.h> /* gethostbyname */

#include "tcpclient.h"
#include "static_variables.h"

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
		fprintf(stderr, "Unable to identify the machine '%s'\n",hostname);
		return -1;
	}

#ifdef DEBUG_SOCKET
	if (opened_tcp > 0) {
		printf("socket déjà en cours !!!\n");
	}
#endif // DEBUG_SOCKET
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
	tv.tv_sec = tcp_timeout;
	tv.tv_usec = 0;
	setsockopt(socket_id, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));
	setsockopt(socket_id, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(struct timeval));
	
	
	//setsockopt(socket_id, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(int));
	
	/* connexion au serveur */
	sockname.sin_family = host_address->h_addrtype;
	sockname.sin_port = htons(port);
	memcpy((char*) &(sockname.sin_addr.s_addr), host_address->h_addr_list[0], host_address->h_length);
	int t;
	for(t = 1; t <= NB_ATTEMPTS; t++)
	{

		if(-1 == (connect(socket_id, (struct sockaddr *)&sockname, sizeof(struct sockaddr_in))))
		{
			fprintf(stderr, "The socket can not connect to server '%s' attempt:%i\n", hostname,t);
			close(socket_id);
#ifdef DEBUG_SOCKET
			opened_tcp--;
#endif // DEBUG_SOCKET
			if (t == NB_ATTEMPTS) {
				socket_id = -1;
				break;
			} else {
				sleep(1);
			}
			
		} else
		{
#ifdef DEBUG_SOCKET
			opened_tcp++;
#endif // DEBUG_SOCKET
			break;
		}
	}
	return (socket_id);
}
