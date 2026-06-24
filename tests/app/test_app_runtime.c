/*
 * Tests unitaires de app_runtime.c — plomberie extraite de main.c
 * (gestion des signaux + bootstrap runtime).
 *
 * Précautions essentielles :
 *  - Les fonctions de signaux modifient la disposition des signaux DU PROCESSUS
 *    de test. Chaque test SAUVEGARDE puis RESTAURE la disposition concernée
 *    (sigaction / pthread_sigmask) AVANT toute assertion — sinon le runner serait
 *    cassé (ex. un auto-reaper SIGCHLD volerait les waitpid des autres suites qui
 *    forkent : console PTY, run_in_fork, etc.).
 *  - Les globaux touchés (NB_THREADS, counters, request, server, childrens_pid…)
 *    sont sauvegardés/restaurés : l'état est partagé entre suites.
 *  - Le chemin exit(0) de signal_end_handler (mode serveur) est exercé dans un
 *    fils via run_in_fork (greatest est mono-processus).
 */
#include "greatest.h"
#include "fork_assert.h"
#include "app/app_runtime.h"
#include "app/static_variables.h"
#include "app/etii_statistic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>

/* Redirige un FD vers /dev/null le temps d'un appel verbeux (wait_child logue). */
static int g_saved_fd = -1, g_saved_target = -1;
static void mute_fd(int fd)
{
    fflush(NULL);
    g_saved_target = fd;
    g_saved_fd = dup(fd);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, fd); close(dn); }
}
static void unmute_fd(void)
{
    fflush(NULL);
    if (g_saved_fd >= 0) { dup2(g_saved_fd, g_saved_target); close(g_saved_fd); g_saved_fd = -1; }
}

/* ============================ Bootstrap runtime ============================ */

/* init_counters : alloue counters/lastfilesize (NB_THREADS entrées) à zéro. */
TEST init_counters_allocates_zeroed(void)
{
    int saved_nb = NB_THREADS;
    unsigned long long *sc = counters, *sl = lastfilesize;
    NB_THREADS = 4;

    int rc = init_counters();
    int ok = (counters != NULL && lastfilesize != NULL);
    int zeroed = ok;
    if (ok) {
        for (int i = 0; i < 4; i++)
            if (counters[i] != 0 || lastfilesize[i] != 0) zeroed = 0;
    }
    free(counters); free(lastfilesize);
    counters = sc; lastfilesize = sl;   /* restaure les pointeurs d'origine */
    NB_THREADS = saved_nb;

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT(ok);
    ASSERT(zeroed);
    PASS();
}

/* init_childs : alloue/initialise childrens_pid (-1), forkId (""), fork_statistics (0). */
TEST init_childs_initializes_contexts(void)
{
    int saved_nb = NB_THREADS;
    pid_t *spid = childrens_pid;
    char **sfork = forkId;
    struct client_statistics *sstat = fork_statistics;
    NB_THREADS = 4;

    init_childs();
    int ok = (childrens_pid != NULL && forkId != NULL && fork_statistics != NULL);
    int good = ok;
    if (ok) {
        for (int i = 0; i < 4; i++) {
            if (childrens_pid[i] != -1) good = 0;
            if (forkId[i] == NULL || forkId[i][0] != '\0') good = 0;
            if (fork_statistics[i].shots_per_second != 0) good = 0;
        }
        for (int i = 0; i < 4; i++) free(forkId[i]);
    }
    free(childrens_pid); free(forkId); free(fork_statistics);
    childrens_pid = spid; forkId = sfork; fork_statistics = sstat;
    NB_THREADS = saved_nb;

    ASSERT(ok);
    ASSERT(good);
    PASS();
}

/* failed_arg : écrit le message d'usage sur stderr (log_error). */
TEST failed_arg_prints_usage(void)
{
    char path[] = "/tmp/etii_failed_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    fflush(stderr);
    int saved = dup(2);
    dup2(fd, 2);

    failed_arg();

    fflush(stderr);
    dup2(saved, 2); close(saved);
    lseek(fd, 0, SEEK_SET);
    char buf[512] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    (void)n;
    close(fd); unlink(path);

    ASSERT(strstr(buf, "tcpserver") != NULL); /* l'usage liste les modes */
    PASS();
}

/* ================================ Signaux ================================= */

/* signal_ignored : no-op (handler SIGPIPE). */
TEST signal_ignored_is_noop(void)
{
    signal_ignored(SIGPIPE);
    PASS();
}

/* signal_end_handler (client) : positionne request=REQUEST_STOP et parcourt la
   boucle de propagation aux enfants — sans exit (server=0) ni kill() réel : les
   pids du tableau sont tous <= 0, donc la garde `childrens_pid[c] > 0` les saute. */
TEST signal_end_handler_sets_request_stop(void)
{
    int sr = request, ss = server, snb = NB_THREADS;
    pid_t *spid = childrens_pid, sparent = parent_pid;
    pid_t fake[2] = { -1, -1 };       /* <= 0 -> kill() jamais appelé */
    request = REQUEST_CONTINUE;
    server = 0;                        /* pas d'exit */
    NB_THREADS = 2;
    parent_pid = getpid();             /* entre dans la boucle de propagation */
    childrens_pid = fake;

    signal_end_handler(SIGINT);
    int got = request;

    request = sr; server = ss; NB_THREADS = snb;
    childrens_pid = spid; parent_pid = sparent;
    ASSERT_EQ_FMT(REQUEST_STOP, got, "%d");
    PASS();
}

/* Chemin serveur : signal_end_handler appelle exit(0). Exécuté dans un fils
   (server=1 / childrens_pid=NULL positionnés dans la mémoire copiée par fork). */
static void se_handler_server_path(void)
{
    server = 1;
    childrens_pid = NULL;
    signal_end_handler(SIGTERM); /* -> exit(0) */
}
TEST signal_end_handler_server_calls_exit(void)
{
    int code = run_in_fork(se_handler_server_path, NULL);
    ASSERT_EQ_FMT(0, code, "%d");
    PASS();
}

/* sigchld_handler : sans enfant en attente, la boucle waitpid(WNOHANG) sort aussitôt. */
TEST sigchld_handler_without_children_is_noop(void)
{
    sigchld_handler(SIGCHLD);
    PASS();
}

/* wait_child : récolte tous les enfants (boucle wait() jusqu'à ECHILD). */
TEST wait_child_reaps_children(void)
{
    pid_t pid = fork();
    if (pid == 0) { _exit(0); }
    ASSERT(pid > 0);

    mute_fd(1);     /* wait_child logue "start/end wait_child" */
    wait_child();   /* bloque sur wait() jusqu'à ECHILD (récolte l'enfant) */
    unmute_fd();

    /* l'enfant a déjà été récolté : un nouveau waitpid renvoie -1 (ECHILD) */
    ASSERT_EQ_FMT(-1, (int)waitpid(pid, NULL, WNOHANG), "%d");
    PASS();
}

/* init_sigchld_sigaction : installe sigchld_handler sur SIGCHLD. */
TEST init_sigchld_sigaction_installs_handler(void)
{
    struct sigaction old, cur;
    sigaction(SIGCHLD, NULL, &old);   /* sauvegarde */
    init_sigchld_sigaction();
    sigaction(SIGCHLD, NULL, &cur);   /* relit */
    sigaction(SIGCHLD, &old, NULL);   /* RESTAURE avant assertion */
    ASSERT(cur.sa_handler == sigchld_handler);
    PASS();
}

/* init_signals : signal_end_handler sur SIGINT/HUP/QUIT/TERM + signal_ignored sur SIGPIPE. */
TEST init_signals_installs_handlers(void)
{
    struct sigaction o_int, o_hup, o_quit, o_term, o_pipe, cur;
    sigaction(SIGINT,  NULL, &o_int);
    sigaction(SIGHUP,  NULL, &o_hup);
    sigaction(SIGQUIT, NULL, &o_quit);
    sigaction(SIGTERM, NULL, &o_term);
    sigaction(SIGPIPE, NULL, &o_pipe);

    init_signals();

    sigaction(SIGINT,  NULL, &cur);  int int_ok  = (cur.sa_handler == signal_end_handler);
    sigaction(SIGPIPE, NULL, &cur);  int pipe_ok = (cur.sa_handler == signal_ignored);

    sigaction(SIGINT,  &o_int,  NULL);  /* RESTAURE tout avant assertion */
    sigaction(SIGHUP,  &o_hup,  NULL);
    sigaction(SIGQUIT, &o_quit, NULL);
    sigaction(SIGTERM, &o_term, NULL);
    sigaction(SIGPIPE, &o_pipe, NULL);

    ASSERT(int_ok);
    ASSERT(pipe_ok);
    PASS();
}

/* configure_child_signals : débloque SIGINT et y installe signal_end_handler. */
TEST configure_child_signals_installs_sigint(void)
{
    struct sigaction old, cur;
    sigset_t oldmask;
    sigaction(SIGINT, NULL, &old);
    pthread_sigmask(SIG_SETMASK, NULL, &oldmask);

    configure_child_signals();
    sigaction(SIGINT, NULL, &cur);
    int ok = (cur.sa_handler == signal_end_handler);

    sigaction(SIGINT, &old, NULL);                 /* RESTAURE avant assertion */
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    ASSERT(ok);
    PASS();
}

SUITE(app_runtime_suite)
{
    RUN_TEST(init_counters_allocates_zeroed);
    RUN_TEST(init_childs_initializes_contexts);
    RUN_TEST(failed_arg_prints_usage);
    RUN_TEST(signal_ignored_is_noop);
    RUN_TEST(signal_end_handler_sets_request_stop);
    RUN_TEST(signal_end_handler_server_calls_exit);
    RUN_TEST(sigchld_handler_without_children_is_noop);
    RUN_TEST(wait_child_reaps_children);
    RUN_TEST(init_sigchld_sigaction_installs_handler);
    RUN_TEST(init_signals_installs_handlers);
    RUN_TEST(configure_child_signals_installs_sigint);
}
