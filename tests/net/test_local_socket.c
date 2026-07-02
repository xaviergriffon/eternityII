/*
 * Tests unitaires de local_socket.c (sockets Unix UDP pour l'IPC parent->enfant).
 *
 * On exerce la logique d'adressage (pure) et un vrai aller-retour DGRAM AF_UNIX
 * sur des chemins temporaires, sans fork : on câble les globaux (parent_pid,
 * forkId, main_socket_id, NB_THREADS) puis on lit le datagramme reçu.
 */
#include "greatest.h"
#include "net/local_socket.h"
#include "app/static_variables.h"

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

/* build_udp_local_socket : bind échoue quand le répertoire parent n'existe pas.
 * unlink/remove renvoient ENOENT (ignoré), puis bind retourne ENOENT → -1.
 * Couvre lignes 67-69 de local_socket.c (branche bind échoue → return -1). */
TEST build_udp_local_socket_bind_fails(void)
{
    /* Chemin volontairement invalide : répertoire parent absent. */
    const char *path = "/tmp/etii_ls_no_such_parent_dir_xyz/sock";
    struct sockaddr_un *addr = build_sockaddr(path);
    ASSERT(addr != NULL);

    int fd = build_udp_local_socket(addr);
    ASSERT_EQ_FMT(-1, fd, "%d");

    free(addr);
    PASS();
}

/* send_command_to_childs : sendto échoue quand la destination n'existe pas.
 * On configure les globaux comme dans le test « delivers_datagram » mais on ne
 * crée PAS le socket destinataire → sendto retourne -1 (ENOENT) → log_errno.
 * Couvre lignes 90-93 de local_socket.c (branche sendto échoue). */
TEST send_command_to_childs_sendto_fails(void)
{
    const char *main_path = "/tmp/etii_ls_sf_main";
    const char *ghost_path = "/tmp/etii_ls_sf_ghost_no_socket";

    struct sockaddr_un *main_addr_local = build_sockaddr(main_path);
    int main_fd = build_udp_local_socket(main_addr_local);
    ASSERT(main_fd >= 0);

    int      saved_nb = NB_THREADS;
    pid_t    saved_pp = parent_pid;
    char   **saved_fk = forkId;
    int     *saved_ms = main_socket_id;

    NB_THREADS = 1;
    parent_pid = getpid();
    forkId = malloc(sizeof(char *));
    forkId[0] = (char *)ghost_path; /* aucun socket ici */
    main_socket_id = &main_fd;

    /* sendto vers ghost_path → ENOENT → log_errno → pas de crash */
    send_command_to_childs("test");

    free(forkId);
    NB_THREADS = saved_nb;
    parent_pid = saved_pp;
    forkId = saved_fk;
    main_socket_id = saved_ms;

    close(main_fd);
    unlink(main_path);
    free(main_addr_local);
    PASS();
}

/* build_udp_local_socket : le fd du socket ne doit PAS fuir quand bind()
 * échoue après la création du socket. On répète l'échec ~50 fois (même
 * chemin invalide : répertoire parent absent) puis on ouvre un vrai fichier
 * : si chaque appel avait fui son fd, le numéro obtenu ici serait décalé
 * d'environ 50 par rapport à la baseline. On vérifie qu'il en reste proche
 * (fuite cumulée absente). Couvre le close(socket_id) ajouté sur les
 * chemins d'erreur remove()/bind() de build_udp_local_socket. */
TEST build_udp_local_socket_does_not_leak_fd_on_bind_failure(void)
{
    const char *path = "/tmp/etii_ls_no_such_parent_dir_leak/sock";

    /* Baseline : fd d'un fichier ouvert avant la boucle d'échecs. */
    char baseline_path[] = "/tmp/etii_ls_leak_baseline_XXXXXX";
    int baseline_fd = mkstemp(baseline_path);
    ASSERT(baseline_fd >= 0);

    for (int i = 0; i < 50; i++) {
        struct sockaddr_un *addr = build_sockaddr(path);
        int fd = build_udp_local_socket(addr);
        ASSERT_EQ_FMT(-1, fd, "%d"); /* bind échoue : répertoire parent absent */
        free(addr);
    }

    /* Si les 50 tentatives avaient fui leur fd, celui-ci serait très au-delà
     * de la baseline. Sans fuite, l'écart reste faible (quelques fds tout au
     * plus, pour d'éventuelles ressources internes du runner de test). */
    char probe_path[] = "/tmp/etii_ls_leak_probe_XXXXXX";
    int probe_fd = mkstemp(probe_path);
    ASSERT(probe_fd >= 0);

    int gap = probe_fd - baseline_fd;
    ASSERT(gap >= 0);
    ASSERT(gap < 10); /* bien en-deçà des 50 fuites possibles si close() manquait */

    close(baseline_fd);
    close(probe_fd);
    unlink(baseline_path);
    unlink(probe_path);
    PASS();
}

SUITE(local_socket_suite)
{
    RUN_TEST(build_sockaddr_sets_family_and_path);
    RUN_TEST(size_of_sockaddr_un_matches_path_length);
    RUN_TEST(send_command_to_childs_delivers_datagram);
    RUN_TEST(send_command_to_childs_noop_when_not_parent);
    RUN_TEST(build_udp_local_socket_bind_fails);
    RUN_TEST(send_command_to_childs_sendto_fails);
    RUN_TEST(build_udp_local_socket_does_not_leak_fd_on_bind_failure);
}
