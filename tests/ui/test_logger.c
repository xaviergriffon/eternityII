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
 * early-return. Le cycle de vie réel de la zone fixe ANSI (vrai terminal) est
 * couvert à part via un pseudo-terminal — voir status_zone_lifecycle_over_pty.
 */
/* _GNU_SOURCE doit précéder tout include : sur la glibc, posix_openpt / grantpt /
   unlockpt / ptsname ne sont déclarés qu'avec _XOPEN_SOURCE>=600 (cf. test_console.c). */
#define _GNU_SOURCE 1
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
#include <sys/ioctl.h>
#include <sys/wait.h>

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

/* fatal_error : journalise le message sur stderr PUIS termine le process avec
 * exit(EXIT_FAILURE). On l'exécute dans un fils dont stderr est redirigé vers un
 * fichier temporaire (pas /dev/null comme run_in_fork) afin d'asserter À LA FOIS
 * le code de sortie ET le message — ce funnel est le point de couture sur lequel
 * s'appuient les tests fork des chemins fataux (cf. first_possibility). */
TEST fatal_error_logs_then_exits_failure(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/etii_fatal_%d.txt", (int)getpid());
    unlink(path);

    pid_t pid = fork();
    if (pid == 0) {
        int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) { dup2(f, 2); if (f > 2) close(f); }
        fatal_error("fatal-%d boom", 42);
        _exit(0); /* inatteignable : fatal_error est noreturn */
    }
    ASSERT(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);

    ASSERT(WIFEXITED(status));
    ASSERT_EQ_FMT(EXIT_FAILURE, WEXITSTATUS(status), "%d");

    char buf[256] = {0};
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    (void)n;
    unlink(path);
    ASSERT(strstr(buf, "fatal-42 boom") != NULL);
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

/* log_send_to_parent : complète le test INFO ci-dessus en vérifiant l'octet de
 * TYPE pour chaque autre niveau routé vers le parent (log_error/log_errno ->
 * ERROR, log_debug -> DEBUG, log_console -> CONSOLE, log_event -> EVENT). Couvre
 * les sites log_send_to_parent de logger.c restés morts (un seul, INFO, l'était).
 * On collecte tous les types AVANT de restaurer les globales : un ASSERT en
 * échec ferait un return et laisserait le routage actif pour les tests suivants. */
TEST log_routes_each_type_to_parent(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/etii_ipc_types_%d.sock", (int)getpid());
    unlink(path);

    int rx = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT(rx >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    ASSERT_EQ_FMT(0, bind(rx, (struct sockaddr *)&addr, sizeof addr), "%d");

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int tx = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT(tx >= 0);

    pid_t saved_parent = parent_pid;
    int   saved_sock   = fork_checker_socket_id;
    struct sockaddr_un *saved_addr = main_addr;

    parent_pid = getpid() + 1;
    fork_checker_socket_id = tx;
    main_addr = &addr;

    unsigned char buf[1 + IPC_LINE_MAX];
    int t_error, t_errno, t_debug, t_console, t_event;

    log_error("e1");
    t_error   = (recvfrom(rx, buf, sizeof buf, 0, NULL, NULL) >= 1) ? buf[0] : -1;
    errno = 0;
    log_errno("e2");
    t_errno   = (recvfrom(rx, buf, sizeof buf, 0, NULL, NULL) >= 1) ? buf[0] : -1;
    log_debug("d1");
    t_debug   = (recvfrom(rx, buf, sizeof buf, 0, NULL, NULL) >= 1) ? buf[0] : -1;
    log_console("c1");
    t_console = (recvfrom(rx, buf, sizeof buf, 0, NULL, NULL) >= 1) ? buf[0] : -1;
    log_event("v1");
    t_event   = (recvfrom(rx, buf, sizeof buf, 0, NULL, NULL) >= 1) ? buf[0] : -1;

    parent_pid = saved_parent;
    fork_checker_socket_id = saved_sock;
    main_addr = saved_addr;

    close(tx);
    close(rx);
    unlink(path);

    ASSERT_EQ_FMT((int)IPC_MSG_LOG_ERROR,   t_error,   "%d");
    ASSERT_EQ_FMT((int)IPC_MSG_LOG_ERROR,   t_errno,   "%d");
    ASSERT_EQ_FMT((int)IPC_MSG_LOG_DEBUG,   t_debug,   "%d");
    ASSERT_EQ_FMT((int)IPC_MSG_LOG_CONSOLE, t_console, "%d");
    ASSERT_EQ_FMT((int)IPC_MSG_EVENT,       t_event,   "%d");
    PASS();
}

/*
 * Ligne de saisie protégée (console_input_render / console_input_end) :
 * - render publie et affiche la ligne (efface la ligne courante puis réécrit
 *   prompt + saisie) ;
 * - tant que la saisie est active, un log terminé par '\n' efface la ligne,
 *   s'écrit, puis la redessine en dessous ;
 * - un log SANS '\n' final (bloc partiel) ne redessine pas la ligne ;
 * - end imprime le saut de ligne et lève la protection (les logs suivants
 *   redeviennent bruts, sans séquence d'effacement).
 */
TEST console_input_line_survives_async_logs(void)
{
    char out[512];

    CAPTURE(1, stdout, console_input_render("commande :", "che"), out);
    ASSERT(strstr(out, "\r\033[K") != NULL);
    ASSERT(strstr(out, "commande :che") != NULL);

    /* Log complet (avec '\n') : effacement, log, puis redessin de la saisie. */
    CAPTURE(1, stdout, log_info("stat: %d coups/s\n", 42), out);
    char *erase = strstr(out, "\r\033[K");
    ASSERT(erase != NULL);
    char *text = strstr(out, "stat: 42 coups/s\n");
    ASSERT(text != NULL);
    char *redraw = strstr(out, "commande :che");
    ASSERT(redraw != NULL);
    ASSERT(erase < text);            /* effacement AVANT le log            */
    ASSERT(text < redraw);           /* redessin APRÈS le log              */

    /* Bloc partiel (sans '\n') : pas de redessin de la saisie. */
    CAPTURE(1, stdout, log_info("partiel"), out);
    ASSERT(strstr(out, "partiel") != NULL);
    ASSERT(strstr(out, "commande :che") == NULL);

    /* Fin de saisie : saut de ligne, puis plus aucune protection. */
    CAPTURE(1, stdout, console_input_end(), out);
    ASSERT(strstr(out, "\n") != NULL);
    CAPTURE(1, stdout, log_info("apres\n"), out);
    ASSERT(strstr(out, "apres\n") != NULL);
    ASSERT(strstr(out, "\r\033[K") == NULL);
    ASSERT(strstr(out, "commande :") == NULL);
    PASS();
}

/* console_pager_begin hors terminal (stdin/stdout redirigés) : early-return,
   aucune pagination — les logs restent bruts et console_pager_end est sûr. */
TEST console_pager_noop_without_tty(void)
{
    char out[256];
    /* Sous CAPTURE, stdout est redirigé -> isatty faux -> begin early-return. */
    CAPTURE(1, stdout, (console_pager_begin(), log_info("libre-%d\n", 8)), out);
    console_pager_end();
    ASSERT(strstr(out, "libre-8") != NULL);
    ASSERT(strstr(out, "--Suite--") == NULL);
    PASS();
}

/* console_input_end est idempotent : sans saisie active, aucune sortie. */
TEST console_input_end_without_render_is_noop(void)
{
    char out[64];
    CAPTURE(1, stdout, console_input_end(), out);
    ASSERT_EQ_FMT(0, (int)strlen(out), "%d");
    PASS();
}

/* log_error pendant une saisie active : le message part bien sur stderr
   (l'effacement et le redessin, eux, vont sur stdout). */
TEST log_error_during_input_goes_to_stderr(void)
{
    char out[256];
    char devnull[256];
    /* Active la saisie en jetant l'affichage stdout de render. */
    CAPTURE(1, stdout, console_input_render("commande :", "x"), devnull);
    CAPTURE(2, stderr, log_error("err-%d\n", 3), out);
    /* Lève la protection avant les assertions (état global partagé). */
    CAPTURE(1, stdout, console_input_end(), devnull);
    ASSERT(strstr(out, "err-3") != NULL);
    PASS();
}

/*
 * Cycle de vie de la zone fixe ANSI sur un VRAI terminal (pseudo-terminal).
 * Les fonctions de zone (status_zone_init/teardown, clear_console, et le thread
 * event_zone_loop + query_terminal_rows / redraw_event_zone_locked) sont gardées
 * par isatty(STDOUT) et ioctl(TIOCGWINSZ) : seul un PTY les rend atteignables.
 * On les exécute dans un fils dont stdout est l'esclave d'un PTY (taille fixée
 * via TIOCSWINSZ) pour isoler du runner le thread détaché, l'atexit et les
 * séquences ANSI. Le fils sort par exit(0) — et non _exit — afin que gcov écrive
 * sa couverture ; alarm() est un garde-fou anti-blocage.
 */
TEST status_zone_lifecycle_over_pty(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT(master >= 0);
    ASSERT_EQ_FMT(0, grantpt(master), "%d");
    ASSERT_EQ_FMT(0, unlockpt(master), "%d");
    char *slave_name = ptsname(master);
    ASSERT(slave_name != NULL);
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    ASSERT(slave >= 0);

    /* Fenêtre assez haute pour réserver la zone (rows > ZONE_RESERVED+1 == 8). */
    struct winsize ws = { .ws_row = 40, .ws_col = 120, .ws_xpixel = 0, .ws_ypixel = 0 };
    ioctl(slave, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid == 0) {
        /* Enfant : stdout = esclave PTY (vrai terminal -> isatty + TIOCGWINSZ OK). */
        close(master);
        dup2(slave, STDOUT_FILENO);
        if (slave > 2) close(slave);
        int dn = open("/dev/null", O_WRONLY);   /* stderr muet */
        if (dn >= 0) { dup2(dn, 2); if (dn > 2) close(dn); }
        alarm(5);                       /* garde-fou : write PTY / thread détaché */
        status_zone_init();             /* isatty✓, query_terminal_rows✓, zone_active=1, thread */
        usleep(50000);                  /* laisse event_zone_loop faire une itération (puis sleep) */
        log_event("ZONE EVENT %d", 7);  /* déclenche redraw_event_zone_locked */
        clear_console();                /* branche zone_active (efface la région) */
        status_zone_teardown();         /* zone_active=0 -> event_zone_loop sortira */
        exit(0);                        /* flush gcov + atexit teardown (no-op) ; thread en sleep */
    }

    ASSERT(pid > 0);
    close(slave);
    /* Draine l'affichage ANSI jusqu'à ce que l'enfant ferme l'esclave (sortie) :
       évite tout blocage d'écriture côté enfant, se termine sur EOF/EIO du maître. */
    char drainbuf[4096];
    while (read(master, drainbuf, sizeof drainbuf) > 0) { /* jeter */ }
    int status = 0;
    waitpid(pid, &status, 0);
    close(master);
    unlink("events.log");               /* log_event a écrit dans le CWD */

    ASSERT(WIFEXITED(status));           /* sortie propre (pas tué par SIGALRM) */
    ASSERT_EQ_FMT(0, WEXITSTATUS(status), "%d");
    PASS();
}

SUITE(logger_suite)
{
    RUN_TEST(log_info_formats_to_stdout);
    RUN_TEST(log_debug_and_console_to_stdout);
    RUN_TEST(log_error_to_stderr);
    RUN_TEST(fatal_error_logs_then_exits_failure);
    RUN_TEST(log_errno_appends_strerror);
    RUN_TEST(log_status_is_noop);
    RUN_TEST(log_event_prints_and_logs);
    RUN_TEST(flush_and_zone_helpers_run);
    RUN_TEST(log_routes_to_parent_over_udp_socket);
    RUN_TEST(log_routes_each_type_to_parent);
    RUN_TEST(console_input_line_survives_async_logs);
    RUN_TEST(console_pager_noop_without_tty);
    RUN_TEST(console_input_end_without_render_is_noop);
    RUN_TEST(log_error_during_input_goes_to_stderr);
    RUN_TEST(status_zone_lifecycle_over_pty);
}
