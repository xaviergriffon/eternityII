/*
 * Tests unitaires de logger.c (variante ANSI).
 *
 * Les fonctions de log écrivent sur stdout/stderr (sauf routage IPC vers le
 * parent, inactif ici : parent_pid/fork_checker_socket_id/main_addr restent à
 * leur valeur par défaut). On capture la sortie en redirigeant le descripteur
 * vers un fichier temporaire le temps de l'appel, puis on relit le contenu.
 *
 * Effet de bord : la redirection rend isatty() faux, ce qui fait que
 * clear_console / status_zone_init / status_zone_teardown prennent leur
 * early-return (pas d'écriture de séquences ANSI dans le terminal du runner).
 */
#include "greatest.h"
#include "ui/logger.h"
#include "app/static_variables.h"
#include "net/ipc_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

/* Capture sur FD (1=stdout, FP=stdout / 2=stderr, FP=stderr) la sortie de BODY
   dans le tampon OUT (tableau char). */
#define CAPTURE(FD, FP, BODY, OUT) do {            \
    char _p[] = "/tmp/etii_log_XXXXXX";            \
    int _f = mkstemp(_p);                          \
    fflush(FP);                                    \
    int _s = dup(FD); dup2(_f, FD);                \
    BODY;                                          \
    fflush(FP);                                    \
    dup2(_s, FD); close(_s);                       \
    lseek(_f, 0, SEEK_SET);                        \
    memset(OUT, 0, sizeof(OUT));                   \
    ssize_t _n = read(_f, OUT, sizeof(OUT) - 1);   \
    (void)_n;                                      \
    close(_f); unlink(_p);                         \
} while (0)

TEST log_info_formats_to_stdout(void)
{
    char out[256];
    CAPTURE(1, stdout, log_info("value=%d done", 42), out);
    ASSERT(strstr(out, "value=42 done") != NULL);
    PASS();
}

TEST log_debug_and_console_to_stdout(void)
{
    char out[256];
    CAPTURE(1, stdout, log_debug("dbg-%s", "x"), out);
    ASSERT(strstr(out, "dbg-x") != NULL);

    CAPTURE(1, stdout, log_console("cons-%d", 7), out);
    ASSERT(strstr(out, "cons-7") != NULL);
    PASS();
}

TEST log_error_to_stderr(void)
{
    char out[256];
    CAPTURE(2, stderr, log_error("boom %d", 9), out);
    ASSERT(strstr(out, "boom 9") != NULL);
    PASS();
}

TEST log_errno_appends_strerror(void)
{
    char out[256];
    errno = ENOENT;
    CAPTURE(2, stderr, log_errno("ctx %d => ", 5), out);
    ASSERT(strstr(out, "ctx 5") != NULL);          /* message formaté */
    ASSERT(strstr(out, strerror(ENOENT)) != NULL); /* + texte de l'errno */
    PASS();
}

/* log_status est un no-op en mode ANSI : aucune sortie. */
TEST log_status_is_noop(void)
{
    char out[64];
    CAPTURE(1, stdout, log_status("ignored %d", 1), out);
    ASSERT_EQ_FMT(0, (int)strlen(out), "%d");
    PASS();
}

/* log_event imprime un événement horodaté (zone fixe inactive) et l'ajoute à
   events.log. On vérifie la présence du message et on nettoie le fichier. */
TEST log_event_prints_and_logs(void)
{
    unlink("events.log");
    char out[256];
    CAPTURE(1, stdout, log_event("SOLUTION %d", 256), out);
    ASSERT(strstr(out, "SOLUTION 256") != NULL);
    ASSERT(strstr(out, "[") != NULL); /* horodatage entre crochets */

    /* events.log doit avoir été créé et contenir le message. */
    FILE *f = fopen("events.log", "r");
    ASSERT(f != NULL);
    char line[256] = {0};
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    (void)n;
    ASSERT(strstr(line, "SOLUTION 256") != NULL);
    unlink("events.log");
    PASS();
}

/* Regroupe les helpers à exécuter sous redirection (isatty faux). */
static void run_zone_helpers(void)
{
    flush_info();
    flush_debug();
    flush_console();
    clear_console();        /* non-tty -> return */
    status_zone_init();     /* non-tty -> return */
    status_zone_teardown(); /* zone inactive -> return */
}

/* flush_*, clear_console et status_zone_* : exécution sans effet de bord visible
   (sortie redirigée -> non-tty -> early-return). Couverture des chemins. */
TEST flush_and_zone_helpers_run(void)
{
    char out[64];
    CAPTURE(1, stdout, run_zone_helpers(), out);
    CAPTURE(2, stderr, flush_error(), out);
    PASS();
}

/* log_send_to_parent : dans un enfant forké (parent_pid != getpid(),
 * fork_checker_socket_id ouvert, main_addr renseigné), les logs ne vont plus sur
 * stdout mais sont émis en datagramme UDP vers le parent (1 octet de type + texte).
 * On câble les trois globales sur un socket Unix DGRAM récepteur local — SANS fork :
 * un seul process suffit à exercer log_should_route_to_parent() + log_send_to_parent(). */
TEST log_routes_to_parent_over_udp_socket(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/etii_ipc_%d.sock", (int)getpid());
    unlink(path);

    int rx = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT(rx >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    ASSERT_EQ_FMT(0, bind(rx, (struct sockaddr *)&addr, sizeof addr), "%d");

    /* Garde-fou : timeout de réception pour ne jamais bloquer le runner. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int tx = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT(tx >= 0);

    pid_t saved_parent = parent_pid;
    int   saved_sock   = fork_checker_socket_id;
    struct sockaddr_un *saved_addr = main_addr;

    parent_pid = getpid() + 1;       /* != getpid() et != 0 */
    fork_checker_socket_id = tx;     /* socket émetteur */
    main_addr = &addr;               /* destination = récepteur */

    log_info("ipc-%d", 7);           /* route -> sendto(tx, ..., addr) au lieu de stdout */

    /* Restauration AVANT toute assertion : sinon les logs des tests suivants
       continueraient à router vers un socket fermé / une adresse pendante. */
    parent_pid = saved_parent;
    fork_checker_socket_id = saved_sock;
    main_addr = saved_addr;

    char buf[1 + IPC_LINE_MAX];
    memset(buf, 0, sizeof buf);
    ssize_t n = recvfrom(rx, buf, sizeof(buf) - 1, 0, NULL, NULL);

    close(tx);
    close(rx);
    unlink(path);

    ASSERT(n >= 1);                                          /* datagramme reçu (pas de timeout) */
    ASSERT_EQ_FMT((int)IPC_MSG_LOG_INFO, (int)buf[0], "%d"); /* octet de type */
    ASSERT_STR_EQ("ipc-7", buf + 1);                         /* texte (null-terminé par le memset) */
    PASS();
}

SUITE(logger_suite)
{
    RUN_TEST(log_info_formats_to_stdout);
    RUN_TEST(log_debug_and_console_to_stdout);
    RUN_TEST(log_error_to_stderr);
    RUN_TEST(log_errno_appends_strerror);
    RUN_TEST(log_status_is_noop);
    RUN_TEST(log_event_prints_and_logs);
    RUN_TEST(flush_and_zone_helpers_run);
    RUN_TEST(log_routes_to_parent_over_udp_socket);
}
