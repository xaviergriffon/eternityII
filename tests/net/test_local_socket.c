/*
 * Tests unitaires de local_socket.c (sockets Unix UDP pour l'IPC parent->enfant).
 *
 * On exerce la logique d'adressage (pure) et un vrai aller-retour DGRAM AF_UNIX
 * sur des chemins temporaires, sans fork : on câble les globaux (parent_pid,
 * forkId, main_socket_id, NB_THREADS) puis on lit le datagramme reçu.
 */
#include "greatest.h"
#include "net/local_socket.h"
#include "net/ipc_protocol.h"
#include "core/possibility.h"
#include "app/app_static_variables.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>

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

    /* Trame typée : un octet IPC_MSG_COMMAND puis la commande, SANS son octet
       nul terminal (la longueur est celle du datagramme). */
    char buf[64] = { 0 };
    ssize_t n = recvfrom(child_fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
    ASSERT_EQ_FMT(6, (int)n, "%d");
    ASSERT_EQ_FMT((int)IPC_MSG_COMMAND, (int)(int8_t)buf[0], "%d");
    ASSERT_EQ_FMT(0, memcmp(buf + 1, "hello", 5), "%d");

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

/* ipc_max_datagram : couvre chacun des trois types de message IPC. */
TEST ipc_max_datagram_covers_all_message_types(void)
{
    size_t max_dgram = ipc_max_datagram();
    ASSERT(max_dgram >= 1 + sizeof(struct client_statistics));   /* IPC_MSG_STATS */
    ASSERT(max_dgram >= 1 + sizeof(struct possibility_packet));  /* IPC_MSG_BEST_BOARD */
    ASSERT(max_dgram >= 1 + IPC_LINE_MAX + 1);                   /* IPC_MSG_LOG_* */
    PASS();
}

/* Régression macOS : un datagramme IPC de taille maximale doit passer entre
 * deux sockets construits par build_udp_local_socket. Sans SO_SNDBUF relevé,
 * macOS limite un datagramme AF_UNIX à net.local.dgram.maxdgram (2048 par
 * défaut) : l'envoi échoue en EMSGSIZE — silencieusement dans run_checker,
 * donc le parent ne reçoit plus AUCUNE statistique des forks (stats/records
 * muets, l'application semble « ne rien faire »). Déclenché dès que
 * sizeof(struct client_statistics) dépasse 2047 (ex. FC_STAT_MAX_K=256 pour
 * accompagner un FORWARD_CHECK_K élevé) — et déjà latent pour les lignes de
 * log proches d'IPC_LINE_MAX (4000 > 2048), même en configuration par défaut. */
TEST build_udp_local_socket_allows_max_ipc_datagram(void)
{
    const char *rx_path = "/tmp/etii_lsock_maxdgram_rx";
    const char *tx_path = "/tmp/etii_lsock_maxdgram_tx";

    struct sockaddr_un *rx_addr = build_sockaddr(rx_path);
    int rx_fd = build_udp_local_socket(rx_addr);
    ASSERT(rx_fd >= 0);

    struct sockaddr_un *tx_addr = build_sockaddr(tx_path);
    int tx_fd = build_udp_local_socket(tx_addr);
    ASSERT(tx_fd >= 0);

    /* recv borné dans le temps : si l'envoi échoue, on ne bloque pas le runner. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(rx_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    size_t max_dgram = ipc_max_datagram();
    char *payload = malloc(max_dgram);
    ASSERT(payload != NULL);
    memset(payload, 0x5a, max_dgram);

    ssize_t sent = sendto(tx_fd, payload, max_dgram, 0,
                          (struct sockaddr *)rx_addr, sizeof(struct sockaddr_un));
    ASSERT_EQ_FMT((long)max_dgram, (long)sent, "%ld");

    char *rxbuf = malloc(max_dgram);
    ASSERT(rxbuf != NULL);
    ssize_t received = recvfrom(rx_fd, rxbuf, max_dgram, 0, NULL, NULL);
    ASSERT_EQ_FMT((long)max_dgram, (long)received, "%ld");
    ASSERT_EQ_FMT(0, memcmp(payload, rxbuf, max_dgram), "%d");

    free(payload);
    free(rxbuf);
    close(rx_fd);
    close(tx_fd);
    unlink(rx_path);
    unlink(tx_path);
    free(rx_addr);
    free(tx_addr);
    PASS();
}

/* ------------------------------------------------------------------------
 * Nettoyage des fichiers socket à la terminaison (cf. local_socket.h).
 * ------------------------------------------------------------------------ */

/* local_socket_cleanup_owned : supprime le fichier du socket créé par CE
 * process. Régression : sans ce nettoyage, chaque exécution laissait une
 * socket `etii_main.<pid>` orpheline dans le répertoire de travail — invisible
 * de `git status` (git ne suit pas les fichiers spéciaux) et fatale au
 * `cp -R` de `make test-docker` (« cannot stat ... : Operation not supported »). */
TEST cleanup_owned_removes_socket_file(void)
{
    const char *path = "/tmp/etii_ls_cleanup_owned";
    struct sockaddr_un *addr = build_sockaddr(path);
    int fd = build_udp_local_socket(addr);
    ASSERT(fd >= 0);

    struct stat st;
    ASSERT_EQ_FMT(0, stat(path, &st), "%d"); /* bind a bien créé le fichier */

    local_socket_cleanup_owned();

    ASSERT_EQ_FMT(-1, stat(path, &st), "%d"); /* le fichier a disparu */
    ASSERT_EQ_FMT((int)ENOENT, errno, "%d");

    close(fd);
    free(addr);
    PASS();
}

/* Idempotence : un second appel (ou un `remove()` explicite déjà fait par le
 * chemin de sortie normal de main.c/fork_orchestrator.c) ne doit rien casser. */
TEST cleanup_owned_is_idempotent(void)
{
    const char *path = "/tmp/etii_ls_cleanup_twice";
    struct sockaddr_un *addr = build_sockaddr(path);
    int fd = build_udp_local_socket(addr);
    ASSERT(fd >= 0);

    local_socket_cleanup_owned();
    local_socket_cleanup_owned(); /* ne doit pas crasher */

    struct stat st;
    ASSERT_EQ_FMT(-1, stat(path, &st), "%d");

    close(fd);
    free(addr);
    PASS();
}

/* Invariant de fork (AGENTS.md) : un FILS ne doit JAMAIS supprimer la socket
 * de son parent. Le fils hérite pourtant de la table d'enregistrement et de la
 * chaîne atexit() du parent : seule la comparaison du pid propriétaire l'en
 * empêche. On forke un fils qui appelle explicitement le nettoyage puis sort ;
 * la socket du parent doit survivre aux deux (appel direct + atexit du fils). */
TEST cleanup_owned_never_removes_parent_socket_from_child(void)
{
    const char *path = "/tmp/etii_ls_cleanup_parent_owned";
    struct sockaddr_un *addr = build_sockaddr(path);
    int fd = build_udp_local_socket(addr);
    ASSERT(fd >= 0);

    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        /* Fils : hérite de l'enregistrement du parent, ne doit rien supprimer. */
        local_socket_cleanup_owned();
        exit(EXIT_SUCCESS); /* exit() et non _exit() : joue aussi la chaîne atexit */
    }
    int status = 0;
    ASSERT(waitpid(child, &status, 0) == child);

    struct stat st;
    ASSERT_EQ_FMT(0, stat(path, &st), "%d"); /* la socket du parent est intacte */

    local_socket_cleanup_owned();
    close(fd);
    free(addr);
    PASS();
}

/* Câblage atexit() : un process qui crée une socket locale puis se termine
 * (ici par `exit()`, comme le fait `exit_interpreter` de la console ou le
 * `exit(0)` de signal_end_handler côté serveur) ne doit RIEN laisser derrière
 * lui, même sans passer par le `remove()` explicite du chemin de sortie
 * nominal de main.c. On l'observe depuis le parent, sur un fils dédié. */
TEST socket_file_removed_at_process_exit(void)
{
    char path[64];
    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        char child_path[64];
        snprintf(child_path, sizeof child_path, "/tmp/etii_ls_atexit.%d", (int)getpid());
        struct sockaddr_un *a = build_sockaddr(child_path);
        int cfd = build_udp_local_socket(a);
        /* Sortie sans remove() explicite : seul atexit() peut nettoyer. */
        exit(cfd >= 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    int status = 0;
    ASSERT(waitpid(child, &status, 0) == child);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ_FMT(EXIT_SUCCESS, WEXITSTATUS(status), "%d");

    snprintf(path, sizeof path, "/tmp/etii_ls_atexit.%d", (int)child);
    struct stat st;
    ASSERT_EQ_FMT(-1, stat(path, &st), "%d"); /* plus de socket orpheline */
    ASSERT_EQ_FMT((int)ENOENT, errno, "%d");
    PASS();
}

/* R8 — send_typed_to_childs transporte une charge utile BINAIRE intacte.
 * `send_command_to_childs` mesurait la charge par strlen() : un
 * possibility_packet, qui contient des octets nuls dès sa deuxième case vide,
 * aurait été tronqué au premier zéro. Ce test envoie un paquet complet et
 * exige l'égalité octet pour octet. */
TEST send_typed_to_childs_carries_binary_payload(void)
{
    const char *child_path = "/tmp/etii_lsock_bin_child";
    const char *main_path  = "/tmp/etii_lsock_bin_main";

    struct sockaddr_un *child_addr = build_sockaddr(child_path);
    int child_fd = build_udp_local_socket(child_addr);
    ASSERT(child_fd >= 0);
    struct sockaddr_un *main_addr = build_sockaddr(main_path);
    int main_fd = build_udp_local_socket(main_addr);
    ASSERT(main_fd >= 0);

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(child_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int      saved_nb = NB_THREADS;
    pid_t    saved_pp = parent_pid;
    char   **saved_fk = forkId;
    int     *saved_ms = main_socket_id;
    NB_THREADS = 1;
    parent_pid = getpid();
    forkId = malloc(sizeof(char *));
    forkId[0] = (char *)child_path;
    main_socket_id = &main_fd;

    /* Motif contenant des octets nuls dès le deuxième octet : strlen() aurait
       annoncé une longueur de 1. */
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    ((unsigned char *)&pkt)[0] = 0x42;
    ((unsigned char *)&pkt)[sizeof pkt - 1] = 0x99;

    int delivered = send_typed_to_childs(IPC_MSG_BEST_BOARD, &pkt, sizeof pkt);
    ASSERT_EQ_FMT(1, delivered, "%d");

    char rxbuf[1 + sizeof(struct possibility_packet)];
    ssize_t n = recvfrom(child_fd, rxbuf, sizeof rxbuf, 0, NULL, NULL);
    ASSERT_EQ_FMT((long)(1 + sizeof pkt), (long)n, "%ld");
    ASSERT_EQ_FMT((int)IPC_MSG_BEST_BOARD, (int)(int8_t)rxbuf[0], "%d");
    ASSERT_EQ_FMT(0, memcmp(rxbuf + 1, &pkt, sizeof pkt), "%d");

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

/* Une charge utile qui déborde ipc_max_datagram() est REFUSÉE, pas tronquée :
 * rien n'est envoyé et l'appel renvoie 0. Une troncature serait pire qu'un
 * refus — le récepteur lirait un message court parfaitement bien formé. */
TEST send_typed_to_childs_refuses_oversized_payload(void)
{
    const char *child_path = "/tmp/etii_lsock_big_child";
    const char *main_path  = "/tmp/etii_lsock_big_main";

    struct sockaddr_un *child_addr = build_sockaddr(child_path);
    int child_fd = build_udp_local_socket(child_addr);
    ASSERT(child_fd >= 0);
    struct sockaddr_un *main_addr = build_sockaddr(main_path);
    int main_fd = build_udp_local_socket(main_addr);
    ASSERT(main_fd >= 0);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(child_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int      saved_nb = NB_THREADS;
    pid_t    saved_pp = parent_pid;
    char   **saved_fk = forkId;
    int     *saved_ms = main_socket_id;
    NB_THREADS = 1;
    parent_pid = getpid();
    forkId = malloc(sizeof(char *));
    forkId[0] = (char *)child_path;
    main_socket_id = &main_fd;

    /* Un octet de trop : 1 (type) + len > ipc_max_datagram(). */
    size_t too_big = ipc_max_datagram();
    char *payload = malloc(too_big);
    ASSERT(payload != NULL);
    memset(payload, 'x', too_big);

    ASSERT_EQ_FMT(0, send_typed_to_childs(IPC_MSG_COMMAND, payload, too_big), "%d");

    /* Rien n'est arrivé : le recvfrom borné expire. */
    char rxbuf[64];
    ASSERT_EQ_FMT(-1, (int)recvfrom(child_fd, rxbuf, sizeof rxbuf, 0, NULL, NULL), "%d");

    /* La borne exacte, elle, passe. */
    ASSERT_EQ_FMT(1, send_typed_to_childs(IPC_MSG_COMMAND, payload, too_big - 1), "%d");

    free(payload);
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

/* ------------------------------------------------------------------------
 * ipc_child_frame_decode — découpage pur du datagramme reçu par un fils.
 * ------------------------------------------------------------------------ */

/* Cas nominal : le type est extrait, la charge utile pointe dans le tampon et
 * est terminée par un octet nul écrit à l'indice numBytes. */
TEST ipc_child_frame_decode_splits_type_and_payload(void)
{
    char buf[16];
    memcpy(buf, "\x08hello", 6); /* IPC_MSG_COMMAND + "hello", sans terminateur */
    int8_t type = 0;
    char *payload = NULL;

    ASSERT_EQ_FMT(1, ipc_child_frame_decode(buf, sizeof buf, 6, &type, &payload), "%d");
    ASSERT_EQ_FMT((int)IPC_MSG_COMMAND, (int)type, "%d");
    ASSERT_STR_EQ("hello", payload);
    ASSERT_EQ_FMT('\0', buf[6], "%d"); /* terminateur écrit à l'indice numBytes */
    PASS();
}

/* RÉGRESSION (R7) : un datagramme qui remplit EXACTEMENT le tampon est refusé
 * plutôt que terminé hors bornes. L'ancien fork_udp lisait 100 octets dans un
 * tampon de 100 puis écrivait value[100] — un débordement d'un octet, silencieux
 * en release et invisible sans ASan. */
TEST ipc_child_frame_decode_refuses_full_buffer(void)
{
    char buf[8];
    memset(buf, 'a', sizeof buf);
    int8_t type = 0;
    char *payload = NULL;

    /* nbytes == bufcap : aucune place pour le terminateur. */
    ASSERT_EQ_FMT(0, ipc_child_frame_decode(buf, sizeof buf, (ssize_t)sizeof buf, &type, &payload), "%d");
    /* Le tampon n'a pas été touché : aucun octet nul n'y a été écrit. */
    for (size_t i = 0; i < sizeof buf; i++) {
        ASSERT_EQ_FMT('a', buf[i], "%c");
    }
    /* Un octet de moins : la place existe, le décodage réussit. */
    ASSERT_EQ_FMT(1, ipc_child_frame_decode(buf, sizeof buf, (ssize_t)sizeof buf - 1, &type, &payload), "%d");
    PASS();
}

/* Datagramme vide (légal en DGRAM) et erreur de recvfrom : rien à décoder. */
TEST ipc_child_frame_decode_rejects_empty_and_error(void)
{
    char buf[8] = { 0 };
    int8_t type = 42;
    char *payload = (char *)0x1;

    ASSERT_EQ_FMT(0, ipc_child_frame_decode(buf, sizeof buf, 0, &type, &payload), "%d");
    ASSERT_EQ_FMT(0, ipc_child_frame_decode(buf, sizeof buf, -1, &type, &payload), "%d");
    ASSERT_EQ_FMT(0, ipc_child_frame_decode(NULL, sizeof buf, 4, &type, &payload), "%d");
    /* Sorties laissées intactes quand le décodage échoue. */
    ASSERT_EQ_FMT(42, (int)type, "%d");
    ASSERT(payload == (char *)0x1);
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
    RUN_TEST(ipc_max_datagram_covers_all_message_types);
    RUN_TEST(build_udp_local_socket_allows_max_ipc_datagram);
    RUN_TEST(send_typed_to_childs_carries_binary_payload);
    RUN_TEST(send_typed_to_childs_refuses_oversized_payload);
    RUN_TEST(ipc_child_frame_decode_splits_type_and_payload);
    RUN_TEST(ipc_child_frame_decode_refuses_full_buffer);
    RUN_TEST(ipc_child_frame_decode_rejects_empty_and_error);
    RUN_TEST(cleanup_owned_removes_socket_file);
    RUN_TEST(cleanup_owned_is_idempotent);
    RUN_TEST(cleanup_owned_never_removes_parent_socket_from_child);
    RUN_TEST(socket_file_removed_at_process_exit);
}
