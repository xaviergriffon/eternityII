/*
 * Tests unitaires de local_socket.c (sockets Unix UDP pour l'IPC parent->enfant).
 *
 * On exerce la logique d'adressage (pure) et un vrai aller-retour DGRAM AF_UNIX
 * sur des chemins temporaires, sans fork : on câble les globaux (parent_pid,
 * forkId, main_socket_id, NB_THREADS) puis on lit le datagramme reçu.
 */
#include "greatest.h"
#include "../local_socket.h"
#include "../static_variables.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>

/* Non déclarée dans local_socket.h (helper interne non statique). */
socklen_t size_of_sockaddr_un(struct sockaddr_un *svaddr);

/* build_sockaddr : famille AF_UNIX et chemin recopié. */
TEST build_sockaddr_sets_family_and_path(void)
{
    struct sockaddr_un *a = build_sockaddr("/tmp/etii_xyz");
    ASSERT(a != NULL);
    ASSERT_EQ_FMT((int)AF_UNIX, (int)a->sun_family, "%d");
    ASSERT_STR_EQ("/tmp/etii_xyz", a->sun_path);
    free(a);
    PASS();
}

/* size_of_sockaddr_un : strlen(path) + sizeof(famille) + 1. */
TEST size_of_sockaddr_un_matches_path_length(void)
{
    struct sockaddr_un *a = build_sockaddr("/tmp/abc");
    socklen_t expected = (socklen_t)(strlen("/tmp/abc") + sizeof(a->sun_family) + 1);
    ASSERT_EQ_FMT((int)expected, (int)size_of_sockaddr_un(a), "%d");
    free(a);
    PASS();
}

/* Aller-retour complet : build_udp_local_socket + send_command_to_childs. */
TEST send_command_to_childs_delivers_datagram(void)
{
    const char *child_path = "/tmp/etii_lsock_child";
    const char *main_path  = "/tmp/etii_lsock_main";

    struct sockaddr_un *child_addr = build_sockaddr(child_path);
    int child_fd = build_udp_local_socket(child_addr);
    ASSERT(child_fd >= 0);
    /* fichier socket bien créé par bind */
    struct stat st;
    ASSERT_EQ_FMT(0, stat(child_path, &st), "%d");

    struct sockaddr_un *main_addr = build_sockaddr(main_path);
    int main_fd = build_udp_local_socket(main_addr);
    ASSERT(main_fd >= 0);

    /* recv borné dans le temps : si l'envoi échoue, on ne bloque pas le runner. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(child_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Câblage des globaux pour que send_command_to_childs s'exécute en « parent ». */
    int      saved_nb = NB_THREADS;
    pid_t    saved_pp = parent_pid;
    char   **saved_fk = forkId;
    int     *saved_ms = main_socket_id;

    NB_THREADS = 1;
    parent_pid = getpid();
    forkId = malloc(sizeof(char *));
    forkId[0] = (char *)child_path;
    main_socket_id = &main_fd;

    send_command_to_childs("hello");

    char buf[64] = { 0 };
    ssize_t n = recvfrom(child_fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
    ASSERT_EQ_FMT(5, (int)n, "%d");
    ASSERT_STR_EQ("hello", buf);

    /* restauration des globaux */
    free(forkId);
    NB_THREADS = saved_nb;
    parent_pid = saved_pp;
    forkId = saved_fk;
    main_socket_id = saved_ms;

    close(child_fd);
    close(main_fd);
    unlink(child_path);
    unlink(main_path);
    free(child_addr);
    free(main_addr);
    PASS();
}

/* Hors parent (parent_pid != getpid()) : send_command_to_childs ne fait rien. */
TEST send_command_to_childs_noop_when_not_parent(void)
{
    pid_t saved_pp = parent_pid;
    int  *saved_ms = main_socket_id;

    parent_pid = getpid() + 100000; /* on n'est pas « le parent » */
    main_socket_id = NULL;          /* non déréférencé puisque la garde échoue */

    send_command_to_childs("ignored"); /* ne doit rien faire, pas de crash */

    parent_pid = saved_pp;
    main_socket_id = saved_ms;
    PASS();
}

SUITE(local_socket_suite)
{
    RUN_TEST(build_sockaddr_sets_family_and_path);
    RUN_TEST(size_of_sockaddr_un_matches_path_length);
    RUN_TEST(send_command_to_childs_delivers_datagram);
    RUN_TEST(send_command_to_childs_noop_when_not_parent);
}
