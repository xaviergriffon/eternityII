/*
 * Tests unitaires de tcpserver.c / tcpclient.c.
 *
 * On monte un serveur d'écoute sur un port éphémère (port 0 -> choisi par l'OS),
 * on récupère le port via getsockname, puis on s'y connecte avec
 * create_tcp_client : un vrai aller-retour TCP local, sans dépendance externe.
 *
 * On évite volontairement le chemin « connexion refusée » de create_tcp_client :
 * il réessaie NB_ATTEMPTS fois avec sleep(1) entre chaque (≈ 9 s).
 */
#include "greatest.h"
#include "net/tcpserver.h"
#include "net/tcpclient.h"
#include "app/static_variables.h"

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* create_tcp_server retourne un socket en écoute ; create_tcp_client s'y connecte. */
TEST server_listens_and_client_connects(void)
{
    int listen_fd = create_tcp_server(0, 5); /* port 0 -> éphémère */
    ASSERT(listen_fd >= 0);

    /* récupère le port effectivement attribué */
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    ASSERT_EQ_FMT(0, getsockname(listen_fd, (struct sockaddr *)&addr, &len), "%d");
    int port = ntohs(addr.sin_port);
    ASSERT(port > 0);

    tcp_timeout = 2; /* borne les timeouts d'E/S du client */
    int client_fd = create_tcp_client("127.0.0.1", port);
    ASSERT(client_fd >= 0); /* handshake TCP complété (backlog) */

    close(client_fd);
    close(listen_fd);
    PASS();
}

/* Nom d'hôte non résoluble (.invalid réservé RFC 2606) -> -1, sans réessais. */
TEST client_unresolvable_host_returns_minus_one(void)
{
    int fd = create_tcp_client("nonexistent-host.invalid", 12345);
    ASSERT_EQ_FMT(-1, fd, "%d");
    PASS();
}

SUITE(tcp_suite)
{
    RUN_TEST(server_listens_and_client_connects);
    RUN_TEST(client_unresolvable_host_returns_minus_one);
}
