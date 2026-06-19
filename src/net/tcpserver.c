#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h> /* close */
#include <netdb.h> /* gethostbyname */
#include <fcntl.h>

#include "net/tcpserver.h"
#include "ui/logger.h"
#include "app/static_variables.h"

/**
 * @brief Crée et met en écoute un socket TCP serveur.
 *
 * Crée une socket SOCK_STREAM, active SO_REUSEADDR, la lie à `INADDR_ANY`
 * sur `port`, puis la passe en écoute avec `nb_max_clients` connexions en
 * attente. Quitte le programme (`exit`) en cas d'erreur de création, bind ou listen.
 *
 * @param port           Port TCP sur lequel écouter.
 * @param nb_max_clients Longueur maximale de la file d'attente de connexions.
 * @return               Descripteur de la socket en écoute.
 */
int create_tcp_server(int port, int nb_max_clients)
{
	int socket_id;
	int optval = 1;
	
	
	if(-1 == (socket_id = socket(PF_INET, SOCK_STREAM, 0)))
	{
        log_error("Impossible de créer un socket\n");
		exit(EXIT_FAILURE);
	}
	// on permet la réutilisation de la socket après sa fermeture.
	setsockopt(socket_id, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
	
	//setsockopt(socket_id, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(int));
	/* Affectation d'une adresse */
	struct sockaddr_in sockname;
	memset((char *)&sockname, 0, sizeof(struct sockaddr_in));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(INADDR_ANY);
	
	if(-1 == (bind(socket_id, (struct sockaddr *) &sockname, sizeof(struct sockaddr_in))))
	{
        log_errno("Erreur sur bind() => ");
		exit(EXIT_FAILURE);
	}
	
	/* mise en écoute de la socket */
    log_info("max clients: %i\n", nb_max_clients);
	if(-1 == (listen(socket_id, nb_max_clients)))
	{
        log_errno("Erreur sur listen() => ");
		exit(EXIT_FAILURE);
	}
	return (socket_id);
}
