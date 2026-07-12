#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> /* close */

#include "net/tcpserver.h"
#include "ui/logger.h"

/**
 * @brief Voir la doc dans tcpserver.h.
 */
int create_tcp_server_bound(uint32_t s_addr_host_order, int port, int nb_max_clients)
{
	int socket_id;
	int optval = 1;

	if(-1 == (socket_id = socket(PF_INET, SOCK_STREAM, 0)))
	{
        log_error("Impossible de créer un socket\n");
		return -1;
	}
	// on permet la réutilisation de la socket après sa fermeture.
	setsockopt(socket_id, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

	//setsockopt(socket_id, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(int));
	/* Affectation d'une adresse */
	struct sockaddr_in sockname;
	memset((char *)&sockname, 0, sizeof(struct sockaddr_in));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(s_addr_host_order);

	if(-1 == (bind(socket_id, (struct sockaddr *) &sockname, sizeof(struct sockaddr_in))))
	{
        log_errno("Erreur sur bind() => ");
        close(socket_id);
		return -1;
	}

	/* mise en écoute de la socket */
    log_info("max clients: %i\n", nb_max_clients);
	if(-1 == (listen(socket_id, nb_max_clients)))
	{
        log_errno("Erreur sur listen() => ");
        close(socket_id);
		return -1;
	}
	return (socket_id);
}

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
	int socket_id = create_tcp_server_bound(INADDR_ANY, port, nb_max_clients);
	if (socket_id == -1) {
		exit(EXIT_FAILURE);
	}
	return socket_id;
}
