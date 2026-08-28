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
#include "fork_assert.h"
#include "net/tcpserver.h"
#include "net/tcpclient.h"
#include "app/app_static_variables.h"

#include <stdlib.h>
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

/* Port déjà occupé par un socket en écoute (sans SO_REUSEADDR) : le fils
 * hérite du socket bloquant via fork(), tente une nouvelle écoute sur le même
 * port → bind échoue (EADDRINUSE) → create_tcp_server appelle exit(EXIT_FAILURE).
 * run_in_fork capture ce code de sortie.
 * Couvre lignes 50-51 de tcpserver.c (branche bind échoue → exit). */
static int g_blocked_port = 0;
static void try_server_on_blocked_port(void)
{
    create_tcp_server(g_blocked_port, 1);
}

TEST server_bind_fails_exits(void)
{
    /* Occupe le port sans SO_REUSEADDR : même un socket neuf avec SO_REUSEADDR
       ne peut pas se lier à un port activement en écoute. */
    int blocker = socket(PF_INET, SOCK_STREAM, 0);
    ASSERT(blocker >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_EQ_FMT(0, bind(blocker, (struct sockaddr *)&addr, sizeof(addr)), "%d");
    ASSERT_EQ_FMT(0, listen(blocker, 1), "%d");
    socklen_t len = sizeof(addr);
    getsockname(blocker, (struct sockaddr *)&addr, &len);
    g_blocked_port = ntohs(addr.sin_port);

    /* Le fils hérite de blocker et essaie de lier le même port → exit(EXIT_FAILURE). */
    int exit_code = run_in_fork(try_server_on_blocked_port, NULL);
    ASSERT_EQ_FMT(EXIT_FAILURE, exit_code, "%d");

    close(blocker);
    PASS();
}

/* create_tcp_server_bound : succès sur loopback, port éphémère, jamais exit. */
TEST server_bound_listens_on_loopback(void)
{
    int listen_fd = create_tcp_server_bound(INADDR_LOOPBACK, 0, 5);
    ASSERT(listen_fd >= 0);

    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    ASSERT_EQ_FMT(0, getsockname(listen_fd, (struct sockaddr *)&addr, &len), "%d");
    ASSERT_EQ_FMT((int32_t)htonl(INADDR_LOOPBACK), (int32_t)addr.sin_addr.s_addr, "%d");

    close(listen_fd);
    PASS();
}

/* Port déjà occupé : create_tcp_server_bound renvoie -1 SANS jamais appeler
 * exit (contrairement à create_tcp_server) — testable directement, sans fork. */
TEST server_bound_bind_failure_returns_minus_one(void)
{
    int blocker = socket(PF_INET, SOCK_STREAM, 0);
    ASSERT(blocker >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_EQ_FMT(0, bind(blocker, (struct sockaddr *)&addr, sizeof(addr)), "%d");
    ASSERT_EQ_FMT(0, listen(blocker, 1), "%d");
    socklen_t len = sizeof(addr);
    getsockname(blocker, (struct sockaddr *)&addr, &len);
    int blocked_port = ntohs(addr.sin_port);

    int result = create_tcp_server_bound(INADDR_ANY, blocked_port, 1);
    ASSERT_EQ_FMT(-1, result, "%d");

    close(blocker);
    PASS();
}

SUITE(tcp_suite)
{
    RUN_TEST(server_listens_and_client_connects);
    RUN_TEST(client_unresolvable_host_returns_minus_one);
    RUN_TEST(server_bind_fails_exits);
    RUN_TEST(server_bound_listens_on_loopback);
    RUN_TEST(server_bound_bind_failure_returns_minus_one);
}
