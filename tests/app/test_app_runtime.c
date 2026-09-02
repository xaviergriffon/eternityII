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
#include "app/app_static_variables.h"
#include "app/etii_statistic.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/best_board.h"
#include "net/local_socket.h"
#include "net/ipc_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

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

/* init_counters : alloue counters/lastfilesize (NB_THREADS entrées) à zéro.
 *
 * NE PAS restaurer les pointeurs `counters`/`lastfilesize` pré-existants
 * après cet appel : depuis que `init_counters()` libère elle-même son ancien
 * contenu avant de réallouer (cf. sa doc, nécessaire pour un redémarrage à
 * chaud répété via `configApply` -> `orchestrator_apply_restart_config`), le
 * pointeur sauvegardé AVANT l'appel est déjà libéré par l'appel lui-même —
 * le restaurer ensuite réintroduit dans la globale un pointeur pendouillant,
 * qui provoque un double-free (détecté par ASan ET par l'allocateur macOS
 * par défaut) au prochain `init_counters()` réel exécuté ailleurs dans le
 * process de test (reproduit précisément par
 * `apply_restart_config_quiesces_concurrent_array_readers`,
 * tests/app/test_fork_orchestrator.c, avant ce correctif). On libère donc
 * simplement le résultat de CET appel et on laisse les globales à NULL —
 * état sûr pour n'importe quel appelant suivant (`free(NULL)` est un
 * no-op, y compris dans `init_counters()` elle-même). */
TEST init_counters_allocates_zeroed(void)
{
    int saved_nb = NB_THREADS;
    NB_THREADS = 4;

    int rc = init_counters();
    int ok = (counters != NULL && lastfilesize != NULL);
    int zeroed = ok;
    if (ok) {
        for (int i = 0; i < 4; i++)
            if (counters[i] != 0 || lastfilesize[i] != 0) zeroed = 0;
    }
    free(counters); free(lastfilesize);
    counters = NULL; lastfilesize = NULL;
    NB_THREADS = saved_nb;

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT(ok);
    ASSERT(zeroed);
    PASS();
}

/* Régression : `init_counters()` doit être sûre à appeler PLUSIEURS FOIS de
 * suite sans double-free — c'est exactement ce qu'un redémarrage à chaud
 * répété (`configApply` changeant `nb_forks` plusieurs fois de suite) fait
 * en production via `orchestrator_apply_restart_config`. Avant le correctif
 * ci-dessus (et le `free()` initial ajouté dans `init_counters()`), un second
 * appel fuyait le premier tampon ; après le `free()` initial, il fallait
 * s'assurer qu'aucun appelant du process ne restaure un pointeur déjà libéré
 * entre-temps (cf. commentaire ci-dessus) — ce test couvre le cas simple
 * (deux appels consécutifs, aucune restauration intercalée). */
TEST init_counters_is_safe_to_call_twice_in_a_row(void)
{
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;
    unsigned long long *saved_lastfilesize = lastfilesize;
    counters = NULL;
    lastfilesize = NULL;

    NB_THREADS = 3;
    ASSERT_EQ_FMT(0, init_counters(), "%d");
    ASSERT(counters != NULL);
    ASSERT(lastfilesize != NULL);

    NB_THREADS = 6; /* simule un nb_forks agrandi entre deux redémarrages */
    ASSERT_EQ_FMT(0, init_counters(), "%d");
    int ok = (counters != NULL && lastfilesize != NULL);
    int zeroed = ok;
    if (ok) {
        for (int i = 0; i < 6; i++) {
            if (counters[i] != 0 || lastfilesize[i] != 0) zeroed = 0;
        }
    }

    free(counters); free(lastfilesize);
    counters = saved_counters;
    lastfilesize = saved_lastfilesize;
    NB_THREADS = saved_nb;

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

/* ensure_childs_capacity : agrandit childrens_pid/forkId/fork_statistics
   au-delà de la capacité allouée par init_childs, en préservant les slots
   existants et en initialisant les nouveaux comme le ferait init_childs.
   Reproduit le scénario réel qui segfaultait : init_childs(NB_THREADS=1) puis
   "config nb_forks 6" + "start" écrivaient hors bornes dans ces tableaux. */
TEST ensure_childs_capacity_grows_and_preserves_existing_slots(void)
{
    int saved_nb = NB_THREADS;
    pid_t *spid = childrens_pid;
    char **sfork = forkId;
    struct client_statistics *sstat = fork_statistics;

    NB_THREADS = 1;
    init_childs();
    childrens_pid[0] = 4242; /* simule un fork déjà en vie sur le slot 0 */
    strcpy(forkId[0], "etii_fork.4242");
    fork_statistics[0].shots_per_second = 99;

    ensure_childs_capacity(6);

    int ok = (childrens_pid != NULL && forkId != NULL && fork_statistics != NULL);
    int preserved = ok && childrens_pid[0] == 4242
        && strcmp(forkId[0], "etii_fork.4242") == 0
        && fork_statistics[0].shots_per_second == 99;
    int new_slots_ok = ok;
    if (ok) {
        for (int i = 1; i < 6; i++) {
            if (childrens_pid[i] != -1) new_slots_ok = 0;
            if (forkId[i] == NULL || forkId[i][0] != '\0') new_slots_ok = 0;
            if (fork_statistics[i].shots_per_second != 0) new_slots_ok = 0;
        }
        for (int i = 0; i < 6; i++) free(forkId[i]);
    }
    free(childrens_pid); free(forkId); free(fork_statistics);
    childrens_pid = spid; forkId = sfork; fork_statistics = sstat;
    NB_THREADS = saved_nb;

    ASSERT(ok);
    ASSERT(preserved);
    ASSERT(new_slots_ok);
    PASS();
}

/* Un appel avec needed <= capacité déjà allouée ne fait rien (pas de
   rétrécissement, pas de perte des slots existants). */
TEST ensure_childs_capacity_is_a_no_op_when_already_covered(void)
{
    int saved_nb = NB_THREADS;
    pid_t *spid = childrens_pid;
    char **sfork = forkId;
    struct client_statistics *sstat = fork_statistics;

    NB_THREADS = 4;
    init_childs();
    childrens_pid[3] = 777;

    ensure_childs_capacity(2); /* inférieur à la capacité existante (4) */

    int preserved = (childrens_pid[3] == 777);

    for (int i = 0; i < 4; i++) free(forkId[i]);
    free(childrens_pid); free(forkId); free(fork_statistics);
    childrens_pid = spid; forkId = sfork; fork_statistics = sstat;
    NB_THREADS = saved_nb;

    ASSERT(preserved);
    PASS();
}

/* free_childs : libère les trois tableaux et remet la capacité suivie
   à 0 — symétrique d'init_childs(). Réutilisée par ORCH_APPLYING
   (fork_orchestrator.c) quand nb_forks change, pour un shrink/regrow propre
   plutôt qu'un simple ensure_childs_capacity (qui ne fait QUE grandir). */
TEST free_childs_then_init_childs_resizes_cleanly(void)
{
    int saved_nb = NB_THREADS;
    pid_t *spid = childrens_pid;
    char **sfork = forkId;
    struct client_statistics *sstat = fork_statistics;

    NB_THREADS = 4;
    init_childs();
    childrens_pid[2] = 555;
    strcpy(forkId[2], "etii_fork.555");

    free_childs();
    int freed_ok = (childrens_pid == NULL && forkId == NULL && fork_statistics == NULL);

    NB_THREADS = 2; /* rétrécissement */
    init_childs();
    int ok = (childrens_pid != NULL && forkId != NULL && fork_statistics != NULL);
    int fresh = ok;
    if (ok) {
        for (int i = 0; i < 2; i++) {
            if (childrens_pid[i] != -1) fresh = 0;
            if (forkId[i] == NULL || forkId[i][0] != '\0') fresh = 0;
        }
        /* ensure_childs_capacity doit repartir de la NOUVELLE capacité (2),
           pas de l'ancienne (4) : sans la remise à zéro dans free_childs, un
           needed <= 4 serait à tort traité comme déjà couvert. */
        ensure_childs_capacity(4);
        for (int i = 2; i < 4; i++) {
            if (childrens_pid[i] != -1) fresh = 0;
        }
        for (int i = 0; i < 4; i++) free(forkId[i]);
    }
    free(childrens_pid); free(forkId); free(fork_statistics);
    childrens_pid = spid; forkId = sfork; fork_statistics = sstat;
    NB_THREADS = saved_nb;

    ASSERT(freed_ok);
    ASSERT(ok);
    ASSERT(fresh);
    PASS();
}

/* free_childs sur un état déjà libéré (NULL) est un no-op sûr — idempotente,
   comme client_config_free. */
TEST free_childs_tolerates_already_freed_state(void)
{
    pid_t *spid = childrens_pid;
    char **sfork = forkId;
    struct client_statistics *sstat = fork_statistics;

    childrens_pid = NULL;
    forkId = NULL;
    fork_statistics = NULL;

    free_childs(); /* ne doit pas planter */

    childrens_pid = spid; forkId = sfork; fork_statistics = sstat;
    PASS();
}

/* ---------- pid_is_alive / reap_dead_child_slots -------------------------- */

/* pid_is_alive : vrai pour le process courant, faux pour un pid déjà récolté
 * (fork()+waitpid(), même patron que fork_exit_client_with_children_array
 * dans tests/ui/test_command_lines.c). */
TEST pid_is_alive_detects_self_and_reaped_child(void)
{
    ASSERT_EQ_FMT(1, pid_is_alive(getpid()), "%d");

    pid_t child = fork();
    if (child == 0) {
        _exit(0);
    }
    ASSERT(child > 0);
    waitpid(child, NULL, 0); /* récolte -> pid maintenant disparu */
    ASSERT_EQ_FMT(0, pid_is_alive(child), "%d");
    PASS();
}

/* pid_is_alive(0) / pid_is_alive(-1) : jamais un pid de fils valide, traité
 * comme mort sans même appeler kill(). */
TEST pid_is_alive_rejects_non_positive_pid(void)
{
    ASSERT_EQ_FMT(0, pid_is_alive(0), "%d");
    ASSERT_EQ_FMT(0, pid_is_alive(-1), "%d");
    PASS();
}

static int reap_test_alive_only_100(pid_t pid)
{
    return pid == 100;
}

/* reap_dead_child_slots : nettoie uniquement les slots dont le pid n'est pas
 * "vivant" au sens du prédicat injecté ; les slots déjà vides (-1) et les
 * slots vivants sont laissés intacts. */
TEST reap_dead_child_slots_clears_only_dead_slots(void)
{
    pid_t pids[3] = { 100, 200, -1 };
    char buf0[300] = "etii_fork.100";
    char buf1[300] = "etii_fork.200";
    char buf2[300] = "";
    char *forkids[3] = { buf0, buf1, buf2 };
    struct client_statistics stats[3];
    memset(stats, 0, sizeof stats);
    stats[1].shots_per_second = 4242; /* doit être remis à zéro (slot mort) */

    int cleaned = reap_dead_child_slots(pids, forkids, stats, 3, reap_test_alive_only_100);

    ASSERT_EQ_FMT(1, cleaned, "%d");
    /* pid 100 : vivant selon le prédicat, intact. */
    ASSERT_EQ_FMT(100, (int)pids[0], "%d");
    ASSERT_STR_EQ("etii_fork.100", forkids[0]);
    /* pid 200 : mort selon le prédicat, nettoyé. */
    ASSERT_EQ_FMT(-1, (int)pids[1], "%d");
    ASSERT_STR_EQ("", forkids[1]);
    ASSERT_EQ_FMT(0ULL, stats[1].shots_per_second, "%llu");
    /* slot déjà vide : intact (jamais compté dans `cleaned`). */
    ASSERT_EQ_FMT(-1, (int)pids[2], "%d");
    PASS();
}

/* alive == NULL : repli sur pid_is_alive (comportement de production). */
TEST reap_dead_child_slots_defaults_to_pid_is_alive(void)
{
    pid_t child = fork();
    if (child == 0) {
        _exit(0);
    }
    ASSERT(child > 0);
    waitpid(child, NULL, 0);

    pid_t pids[1] = { child };
    char buf[300] = "etii_fork.dead";
    char *forkids[1] = { buf };
    struct client_statistics stats[1];
    memset(stats, 0, sizeof stats);

    int cleaned = reap_dead_child_slots(pids, forkids, stats, 1, NULL);

    ASSERT_EQ_FMT(1, cleaned, "%d");
    ASSERT_EQ_FMT(-1, (int)pids[0], "%d");
    ASSERT_STR_EQ("", forkids[0]);
    PASS();
}

/* Tableaux NULL ou nb <= 0 : no-op sûr (jamais de déréférencement). */
TEST reap_dead_child_slots_tolerates_null_arrays(void)
{
    ASSERT_EQ_FMT(0, reap_dead_child_slots(NULL, NULL, NULL, 0, NULL), "%d");

    pid_t pids[1] = { 100 };
    char buf[300] = "";
    char *forkids[1] = { buf };
    struct client_statistics stats[1];
    memset(stats, 0, sizeof stats);
    ASSERT_EQ_FMT(0, reap_dead_child_slots(pids, forkids, stats, 0, reap_test_alive_only_100), "%d");
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

    ASSERT(strstr(buf, "server") != NULL); /* l'usage liste les modes */
    PASS();
}

/* ============================== Aide CLI ================================== */

/* format_cli_help : l'aide générale liste TOUS les sujets de la table (chaque
   mode ET chaque option), dans un tampon assez grand pour ne rien tronquer. */
TEST cli_help_general_lists_every_topic(void)
{
    char buf[6144]; /* même taille que CLI_HELP_BUF_SIZE (src/app/app_runtime.c) */
    int len = format_cli_help(buf, sizeof buf);
    ASSERT(len > 0);
    ASSERT(len < (int)sizeof buf - 1); /* non tronquée */

    int count = 0;
    const cli_help_topic_t *topics = cli_help_topics(&count);
    ASSERT(count > 0);
    for (int i = 0; i < count; i++) {
        ASSERT(strstr(buf, topics[i].usage) != NULL);
        ASSERT(strstr(buf, topics[i].summary) != NULL);
    }
    PASS();
}

/* cli_help_find_topic : insensible à la casse et aux tirets de tête ; NULL et
   sujet inconnu renvoient NULL. */
TEST cli_help_find_topic_matches_flexibly(void)
{
    const cli_help_topic_t *t = cli_help_find_topic("server");
    ASSERT(t != NULL);
    ASSERT_STR_EQ("server", t->name);

    ASSERT_EQ(t, cli_help_find_topic("SerVer"));         /* casse ignorée */
    t = cli_help_find_topic("http-port");                 /* tirets de tête facultatifs */
    ASSERT(t != NULL);
    ASSERT_STR_EQ("--http-port", t->name);
    ASSERT_EQ(t, cli_help_find_topic("--http-port"));

    ASSERT_EQ(NULL, cli_help_find_topic("zorglub"));
    ASSERT_EQ(NULL, cli_help_find_topic(NULL));
    PASS();
}

/* format_cli_help_topic : usage + résumé + détails pour un sujet connu ;
   -1 pour un sujet inconnu (tampon non modifié). */
TEST cli_help_topic_formats_known_and_rejects_unknown(void)
{
    char buf[4096] = "sentinelle";
    int len = format_cli_help_topic("pruner", buf, sizeof buf);
    ASSERT(len > 0);
    ASSERT(strstr(buf, "pruner [serveur]") != NULL);
    ASSERT(strstr(buf, "PRUNER_BATCH_MAX") != NULL); /* les détails sont inclus */

    strcpy(buf, "sentinelle");
    ASSERT_EQ_FMT(-1, format_cli_help_topic("zorglub", buf, sizeof buf), "%d");
    ASSERT_STR_EQ("sentinelle", buf); /* inconnu : buf non modifié */
    PASS();
}

/* Troncature : un tampon trop petit est rempli sans débordement et reste
   terminé par un nul. */
TEST cli_help_truncates_safely_in_small_buffer(void)
{
    char buf[32];
    memset(buf, 'X', sizeof buf);
    int len = format_cli_help(buf, sizeof buf);
    ASSERT(len <= (int)sizeof buf - 1);
    ASSERT_EQ_FMT((int)strlen(buf), len, "%d");
    PASS();
}

/* print_cli_help_topic : 0 et affichage pour un sujet connu, -1 sinon. */
TEST print_cli_help_topic_returns_status(void)
{
    mute_fd(1); /* log_console écrit sur stdout */
    int ok = print_cli_help_topic("test");
    int ko = print_cli_help_topic("zorglub");
    unmute_fd();
    ASSERT_EQ_FMT(0, ok, "%d");
    ASSERT_EQ_FMT(-1, ko, "%d");
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

/* ---------- child_death_record / child_death_drain / child_death_format_reason -- */

/* Vide le ring d'un éventuel résidu laissé par un test précédent (globale de
   module partagée entre tous les tests de ce binaire) et remet à 0 le
   compteur de pertes — mêmes précautions que sur les autres globaux de ce
   fichier (cf. en-tête). */
static void drain_all_child_deaths(void)
{
    child_death_record_t buf[CHILD_DEATH_RING_CAPACITY];
    while (child_death_drain(buf, CHILD_DEATH_RING_CAPACITY) > 0) { }
    child_death_dropped_count();
}

/* child_death_record + child_death_drain : round-trip en ordre FIFO. */
TEST child_death_ring_records_and_drains_in_order(void)
{
    drain_all_child_deaths();

    child_death_record(4242, 0x1234);
    child_death_record(5252, 0x5678);

    child_death_record_t out[4];
    int n = child_death_drain(out, 4);
    ASSERT_EQ_FMT(2, n, "%d");
    ASSERT_EQ_FMT(4242, (int)out[0].pid, "%d");
    ASSERT_EQ_FMT(0x1234, out[0].status, "%d");
    ASSERT_EQ_FMT(5252, (int)out[1].pid, "%d");
    ASSERT_EQ_FMT(0x5678, out[1].status, "%d");

    /* Un second drain sans nouvel évènement ne renvoie rien. */
    ASSERT_EQ_FMT(0, child_death_drain(out, 4), "%d");
    PASS();
}

/* Dépassement de capacité entre deux drains : les entrées les plus anciennes
   sont écrasées, comptées par child_death_dropped_count(), et le drain
   suivant reprend au plus vieux slot encore valide (jamais une entrée
   potentiellement réécrite). */
TEST child_death_ring_reports_dropped_on_overflow(void)
{
    drain_all_child_deaths();

    for (int i = 0; i < CHILD_DEATH_RING_CAPACITY + 5; i++) {
        child_death_record(1000 + i, i);
    }

    child_death_record_t out[CHILD_DEATH_RING_CAPACITY];
    int n = child_death_drain(out, CHILD_DEATH_RING_CAPACITY);
    ASSERT_EQ_FMT(CHILD_DEATH_RING_CAPACITY, n, "%d");
    /* Les 5 plus anciennes (pid 1000..1004) ont été écrasées : le premier
       élément restant est le 6e inséré. */
    ASSERT_EQ_FMT(1005, (int)out[0].pid, "%d");
    ASSERT_EQ_FMT(5, child_death_dropped_count(), "%d");
    /* Le compteur de pertes est un delta, remis à 0 à chaque lecture. */
    ASSERT_EQ_FMT(0, child_death_dropped_count(), "%d");
    PASS();
}

/* child_death_format_reason : décodage d'un statut de sortie normale. */
TEST child_death_format_reason_decodes_normal_exit(void)
{
    pid_t pid = fork();
    if (pid == 0) { _exit(3); }
    ASSERT(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT(WIFEXITED(status));

    char buf[64];
    child_death_format_reason(status, buf, sizeof(buf));
    ASSERT(strstr(buf, "sortie normale") != NULL);
    ASSERT(strstr(buf, "3") != NULL);
    PASS();
}

/* child_death_format_reason : décodage d'un statut "tué par signal". */
TEST child_death_format_reason_decodes_signal_death(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        raise(SIGKILL);
        _exit(0); /* jamais atteint */
    }
    ASSERT(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT(WIFSIGNALED(status));

    char buf[64];
    char needle[16];
    snprintf(needle, sizeof(needle), "(%d)", SIGKILL);
    child_death_format_reason(status, buf, sizeof(buf));
    ASSERT(strstr(buf, "tué par le signal") != NULL);
    ASSERT(strstr(buf, needle) != NULL);
    PASS();
}

/* child_death_is_clean_exit : vrai UNIQUEMENT pour une sortie normale de
   code 0 — un fork qui exhauste tout son espace de recherche local (petit
   puzzle) s'arrête ainsi, sans le moindre rapport avec stopForks/configApply
   ni un crash ; ni un code de sortie non nul ni une terminaison par signal
   ne doivent jamais être classés "propres". */
TEST child_death_is_clean_exit_only_for_exit_code_zero(void)
{
    pid_t pid_ok = fork();
    if (pid_ok == 0) { _exit(0); }
    ASSERT(pid_ok > 0);
    int status_ok = 0;
    waitpid(pid_ok, &status_ok, 0);
    ASSERT_EQ_FMT(1, child_death_is_clean_exit(status_ok), "%d");

    pid_t pid_err = fork();
    if (pid_err == 0) { _exit(1); }
    ASSERT(pid_err > 0);
    int status_err = 0;
    waitpid(pid_err, &status_err, 0);
    ASSERT_EQ_FMT(0, child_death_is_clean_exit(status_err), "%d");

    pid_t pid_sig = fork();
    if (pid_sig == 0) { raise(SIGKILL); _exit(0); }
    ASSERT(pid_sig > 0);
    int status_sig = 0;
    waitpid(pid_sig, &status_sig, 0);
    ASSERT_EQ_FMT(0, child_death_is_clean_exit(status_sig), "%d");
    PASS();
}

/* NULL/0 : jamais de déréférencement, no-op silencieux. */
TEST child_death_format_reason_tolerates_null_output(void)
{
    child_death_format_reason(0, NULL, 0);
    char buf[8];
    child_death_format_reason(0, buf, 0);
    PASS();
}

/* sigchld_handler enregistre bien pid+statut dans le ring pour un enfant
   réellement mort — le chemin de production complet (signal handler ->
   child_death_record), pas seulement la fonction de bas niveau isolée. */
TEST sigchld_handler_records_child_death(void)
{
    drain_all_child_deaths();

    pid_t pid = fork();
    if (pid == 0) { _exit(7); }
    ASSERT(pid > 0);

    /* _exit() est quasi instantané, mais on ne bloque jamais dans le test sur
       une hypothèse de timing : on rappelle le handler jusqu'à ce qu'il ait
       effectivement récolté l'enfant (waitpid(WNOHANG) interne), borné pour
       ne jamais accrocher le runner en cas de régression. */
    child_death_record_t out[4];
    int n = 0;
    for (int tries = 0; tries < 500 && n == 0; tries++) {
        usleep(1000);
        sigchld_handler(SIGCHLD);
        n = child_death_drain(out, 4);
    }

    ASSERT_EQ_FMT(1, n, "%d");
    ASSERT_EQ_FMT((int)pid, (int)out[0].pid, "%d");
    ASSERT(WIFEXITED(out[0].status));
    ASSERT_EQ_FMT(7, WEXITSTATUS(out[0].status), "%d");
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

/* configure_child_signals : débloque SIGINT et y installe signal_end_handler,
 * SANS SA_RESTART — même rationale qu'init_signals (cf. son commentaire) :
 * un appel bloquant interrompu par SIGINT doit renvoyer EINTR, jamais être
 * relancé silencieusement, pour que la boucle appelante puisse constater
 * request==REQUEST_STOP. Un fork resté sourd à SIGINT/SIGTERM (nécessitant
 * l'escalade jusqu'à SIGKILL) a été reproduit deux fois en conditions
 * réelles avec SA_RESTART actif ici par erreur — verrouillé pour ne jamais
 * régresser (cf. docs/echanges_client_serveur.md). */
TEST configure_child_signals_installs_sigint(void)
{
    struct sigaction old, cur;
    sigset_t oldmask;
    sigaction(SIGINT, NULL, &old);
    pthread_sigmask(SIG_SETMASK, NULL, &oldmask);

    configure_child_signals();
    sigaction(SIGINT, NULL, &cur);
    int ok = (cur.sa_handler == signal_end_handler);
    int no_restart = (cur.sa_flags & SA_RESTART) == 0;

    sigaction(SIGINT, &old, NULL);                 /* RESTAURE avant assertion */
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    ASSERT(ok);
    ASSERT(no_restart);
    PASS();
}

/* ==================== Parsing d'arguments CLI (main.c) ===================== */

/* ---------- parse_positive_int_or_default -------------------------------- */

TEST parse_positive_int_absent_arg_uses_fallback(void)
{
    int invalid = -1;
    ASSERT_EQ_FMT(7, parse_positive_int_or_default(NULL, 7, &invalid), "%d");
    ASSERT_EQ_FMT(0, invalid, "%d"); /* absent != invalide */
    PASS();
}

TEST parse_positive_int_valid_arg_returns_parsed_value(void)
{
    int invalid = -1;
    ASSERT_EQ_FMT(42, parse_positive_int_or_default("42", 7, &invalid), "%d");
    ASSERT_EQ_FMT(0, invalid, "%d");
    PASS();
}

TEST parse_positive_int_zero_or_negative_is_invalid(void)
{
    int invalid = 0;
    ASSERT_EQ_FMT(7, parse_positive_int_or_default("0", 7, &invalid), "%d");
    ASSERT_EQ_FMT(1, invalid, "%d");

    invalid = 0;
    ASSERT_EQ_FMT(7, parse_positive_int_or_default("-5", 7, &invalid), "%d");
    ASSERT_EQ_FMT(1, invalid, "%d");
    PASS();
}

TEST parse_positive_int_non_numeric_is_invalid(void)
{
    int invalid = 0;
    ASSERT_EQ_FMT(7, parse_positive_int_or_default("abc", 7, &invalid), "%d");
    ASSERT_EQ_FMT(1, invalid, "%d");
    PASS();
}

/* out_was_invalid == NULL accepté (appelant qui ne veut pas distinguer). */
TEST parse_positive_int_null_out_param_is_safe(void)
{
    ASSERT_EQ_FMT(7, parse_positive_int_or_default("bad", 7, NULL), "%d");
    ASSERT_EQ_FMT(9, parse_positive_int_or_default("9", 7, NULL), "%d");
    PASS();
}

/* ---------- parse_server_thread_arg ------------------------------------ */

TEST server_arg_absent_keeps_default(void)
{
    int nb = -1;
    int r = parse_server_thread_arg(NULL, 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_AS_COUNT, r, "%d");
    ASSERT_EQ_FMT(80, nb, "%d");
    PASS();
}

TEST server_arg_valid_number_sets_count(void)
{
    int nb = -1;
    int r = parse_server_thread_arg("40", 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_AS_COUNT, r, "%d");
    ASSERT_EQ_FMT(40, nb, "%d");
    PASS();
}

TEST server_arg_zero_is_invalid_count_keeps_default(void)
{
    int nb = -1;
    int r = parse_server_thread_arg("0", 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_INVALID_COUNT, r, "%d");
    ASSERT_EQ_FMT(80, nb, "%d");
    PASS();
}

TEST server_arg_negative_is_invalid_count_keeps_default(void)
{
    int nb = -1;
    int r = parse_server_thread_arg("-3", 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_INVALID_COUNT, r, "%d");
    ASSERT_EQ_FMT(80, nb, "%d");
    PASS();
}

/* Ne ressemble pas à un nombre (ne commence ni par un chiffre, ni +/-) :
 * traité comme le fichier de pièces, nombre de threads inchangé. */
TEST server_arg_filename_is_detected(void)
{
    int nb = -1;
    int r = parse_server_thread_arg("data/pieces16.csv", 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_AS_FILENAME, r, "%d");
    ASSERT_EQ_FMT(80, nb, "%d");
    PASS();
}

/* Chaîne vide : premier caractère '\0', ne ressemble à rien de numérique ->
 * traitée comme un fichier (cas limite du même mécanisme). */
TEST server_arg_empty_string_is_filename(void)
{
    int nb = -1;
    int r = parse_server_thread_arg("", 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_AS_FILENAME, r, "%d");
    ASSERT_EQ_FMT(80, nb, "%d");
    PASS();
}

TEST server_arg_plus_prefixed_number_is_count(void)
{
    int nb = -1;
    int r = parse_server_thread_arg("+12", 80, &nb);
    ASSERT_EQ_FMT(SERVER_ARG_AS_COUNT, r, "%d");
    ASSERT_EQ_FMT(12, nb, "%d");
    PASS();
}

/* ---------- parse_client_args ------------------------------------------ */
/*
 * Le sens des arguments positionnels dépend de pruner_mode (lu, jamais écrit) :
 *   client [serveur] [nb_threads] [max_stock] [pieces.csv]
 *   pruner [serveur] [nb_threads] [pieces.csv] [batch]
 * Chaque test sauvegarde/restaure les globales positionnées (NB_THREADS,
 * max_stock_by_thread, pruner_batch_size, parts_files) et pruner_mode.
 */

static int g_saved_nb_threads;
static int g_saved_max_stock;
static int g_saved_batch;
static char *g_saved_parts_files;
static int g_saved_pruner_mode;

static void save_client_args_globals(void)
{
    g_saved_nb_threads  = NB_THREADS;
    g_saved_max_stock   = max_stock_by_thread;
    g_saved_batch       = pruner_batch_size;
    g_saved_parts_files = parts_files;
    g_saved_pruner_mode = pruner_mode;
}

static void restore_client_args_globals(void)
{
    NB_THREADS          = g_saved_nb_threads;
    max_stock_by_thread = g_saved_max_stock;
    pruner_batch_size   = g_saved_batch;
    parts_files         = g_saved_parts_files;
    pruner_mode         = g_saved_pruner_mode;
}

/* Aucun argument optionnel : localhost, 1 thread, rien d'autre touché. */
TEST client_args_defaults(void)
{
    save_client_args_globals();
    pruner_mode = 0;
    const char *argv[] = { "prog", "client" };

    const char *ip = parse_client_args(2, argv);

    ASSERT_STR_EQ("localhost", ip);
    ASSERT_EQ_FMT(1, NB_THREADS, "%d");
    ASSERT_EQ_FMT(g_saved_max_stock, max_stock_by_thread, "%d");
    ASSERT_EQ(g_saved_parts_files, parts_files);

    restore_client_args_globals();
    PASS();
}

/* Serveur + nombre de threads valide. */
TEST client_args_server_and_threads(void)
{
    save_client_args_globals();
    pruner_mode = 0;
    const char *argv[] = { "prog", "client", "monserveur", "4" };

    const char *ip = parse_client_args(4, argv);

    ASSERT_STR_EQ("monserveur", ip);
    ASSERT_EQ_FMT(4, NB_THREADS, "%d");

    restore_client_args_globals();
    PASS();
}

/* Nombre de threads invalide : repli sur 1 (log_error, silencié). */
TEST client_args_invalid_threads_falls_back(void)
{
    save_client_args_globals();
    pruner_mode = 0;
    const char *argv[] = { "prog", "client", "srv", "zero" };

    mute_fd(2);
    const char *ip = parse_client_args(4, argv);
    unmute_fd();

    ASSERT_STR_EQ("srv", ip);
    ASSERT_EQ_FMT(1, NB_THREADS, "%d");

    restore_client_args_globals();
    PASS();
}

/* Client de recherche : argv[4] = stock max par thread, argv[5] = fichier. */
TEST client_args_search_client_stock_and_file(void)
{
    save_client_args_globals();
    pruner_mode = 0;
    const char *argv[] = { "prog", "client", "srv", "2", "500", "./mes_pieces.csv" };

    parse_client_args(6, argv);

    ASSERT_EQ_FMT(2, NB_THREADS, "%d");
    ASSERT_EQ_FMT(500, max_stock_by_thread, "%d");
    ASSERT_STR_EQ("./mes_pieces.csv", parts_files);
    ASSERT_EQ_FMT(g_saved_batch, pruner_batch_size, "%d");   /* pas un pruner */

    restore_client_args_globals();
    PASS();
}

/* Pruner : argv[4] = fichier de pièces, argv[5] = taille de lot. */
TEST client_args_pruner_file_and_batch(void)
{
    save_client_args_globals();
    pruner_mode = 1;
    const char *argv[] = { "prog", "pruner", "srv", "3", "./mes_pieces.csv", "100" };

    parse_client_args(6, argv);

    ASSERT_EQ_FMT(3, NB_THREADS, "%d");
    ASSERT_STR_EQ("./mes_pieces.csv", parts_files);
    ASSERT_EQ_FMT(100, pruner_batch_size, "%d");
    ASSERT_EQ_FMT(g_saved_max_stock, max_stock_by_thread, "%d");   /* pas de stock local */

    restore_client_args_globals();
    PASS();
}

/* Taille de lot pruner bornée des deux côtés : [1, PRUNER_BATCH_MAX]. */
TEST client_args_pruner_batch_is_clamped(void)
{
    save_client_args_globals();
    pruner_mode = 1;

    const char *argv_low[] = { "prog", "pruner", "srv", "1", "./p.csv", "0" };
    parse_client_args(6, argv_low);
    ASSERT_EQ_FMT(1, pruner_batch_size, "%d");

    const char *argv_high[] = { "prog", "pruner", "srv", "1", "./p.csv", "99999999" };
    parse_client_args(6, argv_high);
    ASSERT_EQ_FMT(PRUNER_BATCH_MAX, pruner_batch_size, "%d");

    restore_client_args_globals();
    PASS();
}

/* ---------- gpu_pruner_forks_conflict -------------------------------------- */

/* GPU inactif : jamais de conflit, quel que soit le dosage demandé. */
TEST gpu_pruner_forks_conflict_never_when_gpu_inactive(void)
{
    ASSERT_EQ_FMT(0, gpu_pruner_forks_conflict(0, 2, 8), "%d");
    ASSERT_EQ_FMT(0, gpu_pruner_forks_conflict(0, -1, 8), "%d");
    PASS();
}

/* GPU actif, aucun dosage demandé (-1, sentinel) : jamais de conflit — le
   défaut (pruner_forks == nb_forks en mode pruner) s'applique sans friction. */
TEST gpu_pruner_forks_conflict_never_when_not_requested(void)
{
    ASSERT_EQ_FMT(0, gpu_pruner_forks_conflict(1, -1, 8), "%d");
    PASS();
}

/* GPU actif, dosage demandé ÉGAL à nb_forks : pas de conflit (cas dégénéré
   explicite, équivalent au défaut). */
TEST gpu_pruner_forks_conflict_never_when_requested_equals_nb_forks(void)
{
    ASSERT_EQ_FMT(0, gpu_pruner_forks_conflict(1, 8, 8), "%d");
    PASS();
}

/* GPU actif, dosage mixte demandé (différent de nb_forks) : conflit — un
   fork "recherche" tournerait quand même sur autoprune_gpu (cf. la doc de
   gpu_pruner_forks_conflict, app_runtime.h). */
TEST gpu_pruner_forks_conflict_detected_for_mixed_dosage(void)
{
    ASSERT_EQ_FMT(1, gpu_pruner_forks_conflict(1, 2, 8), "%d");
    ASSERT_EQ_FMT(1, gpu_pruner_forks_conflict(1, 0, 8), "%d");
    PASS();
}

/* ---------- backup_failed_exit -------------------------------------------- */

/* Files vides : aucun fichier de secours créé. */
TEST backup_failed_exit_empty_is_noop(void)
{
    char path[80], path_an[96];
    snprintf(path, sizeof path, "./failed_exit_eternityII_%i.back", (int)getpid());
    snprintf(path_an, sizeof path_an, "./failed_exit_eternityII-in_analyse_%i.back", (int)getpid());
    unlink(path);
    unlink(path_an);

    while (datas_size() > 0) {   /* état partagé entre suites */
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        free_array_possibility_packet(r);
    }

    backup_failed_exit();

    ASSERT(access(path, F_OK) != 0);
    ASSERT(access(path_an, F_OK) != 0);
    PASS();
}

/* Du stock résiduel (anomalie en mode client) : les deux fichiers de secours
 * nommés d'après le pid sont écrits. */
TEST backup_failed_exit_saves_leftover_stock(void)
{
    char path[80], path_an[96];
    snprintf(path, sizeof path, "./failed_exit_eternityII_%i.back", (int)getpid());
    snprintf(path_an, sizeof path_an, "./failed_exit_eternityII-in_analyse_%i.back", (int)getpid());
    unlink(path);
    unlink(path_an);

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 5;
    array_possibility_packet arr = { .size = 1, .possibilities = &pkt };
    add_possibility(NULL, &arr);

    backup_failed_exit();

    ASSERT_EQ_FMT(0, access(path, F_OK), "%d");
    ASSERT_EQ_FMT(0, access(path_an, F_OK), "%d");

    unlink(path);
    unlink(path_an);
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        free_array_possibility_packet(r);
    }
    PASS();
}

/* ---------- ensure_stock_files_cover_forks (PR4) --------------------------- */
/*
 * nb_file_possibility est un état GLOBAL qui persiste entre tests : chaque
 * test le restaure à NB_FILE_POSSIBILITY_DEFAULT avant PASS().
 */

TEST ensure_stock_files_cover_forks_noop_when_already_sufficient(void)
{
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, nb_file_possibility, "%d");
    ASSERT_EQ_FMT(0, ensure_stock_files_cover_forks(NB_FILE_POSSIBILITY_DEFAULT), "%d");
    ASSERT_EQ_FMT(0, ensure_stock_files_cover_forks(NB_FILE_POSSIBILITY_DEFAULT - 2), "%d");
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, nb_file_possibility, "%d"); /* jamais réduit */
    PASS();
}

TEST ensure_stock_files_cover_forks_grows_when_forks_exceed_files(void)
{
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, nb_file_possibility, "%d");
    ASSERT_EQ_FMT(1, ensure_stock_files_cover_forks(25), "%d");
    ASSERT_EQ_FMT(25, nb_file_possibility, "%d");

    datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT);
    PASS();
}

/* ---------- run_checker ---------------------------------------------------- */
/*
 * REQUEST_STOP prépositionné : le thread détaché (check_server ou
 * check_client_threads, refactorés en while(request != REQUEST_STOP)) sort dès
 * son premier test de boucle. Un thread détaché ne se joint pas : on laisse un
 * délai large avant de restaurer request, pour qu'il ait évalué sa condition.
 */

TEST run_checker_client_starts_and_stops(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    ASSERT_EQ_FMT(0, run_checker(0), "%d");
    usleep(200000);                 /* laisse le thread détaché sortir */

    request = saved_req;
    PASS();
}

TEST run_checker_server_starts_and_stops(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    ASSERT_EQ_FMT(0, run_checker(1), "%d");
    usleep(200000);

    request = saved_req;
    PASS();
}

/* ==================== IPC parent<->enfants (sockets Unix UDP) ==================
 *
 * fork_checker / server_tcp / fork_udp bouclent sur recvfrom()/sleep() jusqu'à
 * REQUEST_STOP. Comme pour communicate_with_client (etii_server.c) en P2, on
 * les lance dans un vrai pthread (pas de fork() : aucun souci de flush de
 * couverture), avec un socket Unix DGRAM réel construit via build_sockaddr +
 * build_udp_local_socket (même pattern que tests/net/test_local_socket.c).
 * SO_RCVTIMEO borne toute attente réseau : un blocage inattendu fait échouer
 * le test au lieu de suspendre le runner.
 */

static void set_recv_timeout_ar(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* ---------- server_tcp ----------------------------------------------------- */

/* Le serveur reçoit un IPC_MSG_STATS depuis un « enfant » identifié par son
 * sun_path (forkId[0]) : fork_statistics[0] est mis à jour par copie exacte.
 * Puis un IPC_MSG_EVENT (vérifié via events.log, comme test_logger.c) et un
 * type inconnu (branche « default », débloque aussi la sortie sur
 * REQUEST_STOP). */
TEST server_tcp_updates_stats_and_handles_event_and_unknown_type(void)
{
    char server_path[] = "/tmp/etii_ar_srv_XXXXXX";
    char child_path[]  = "/tmp/etii_ar_chd_XXXXXX";
    int sfd = mkstemp(server_path); ASSERT(sfd >= 0); close(sfd); unlink(server_path);
    int cfd = mkstemp(child_path);  ASSERT(cfd >= 0); close(cfd); unlink(child_path);

    struct sockaddr_un *server_addr = build_sockaddr(server_path);
    int server_fd = build_udp_local_socket(server_addr);
    ASSERT(server_fd >= 0);
    struct sockaddr_un *child_addr = build_sockaddr(child_path);
    int child_fd = build_udp_local_socket(child_addr);
    ASSERT(child_fd >= 0);
    set_recv_timeout_ar(child_fd, 2); /* non utilisé en réception ici, par précaution */

    int saved_req = request;
    int saved_nb = NB_THREADS;
    char **saved_forkid = forkId;
    struct client_statistics *saved_fs = fork_statistics;

    request = REQUEST_CONTINUE;
    NB_THREADS = 1;
    forkId = malloc(sizeof(char *));
    forkId[0] = (char *)child_path;
    struct client_statistics fs[1];
    memset(fs, 0, sizeof fs);
    fork_statistics = fs;

    pthread_t tid;
    ASSERT_EQ_FMT(0, pthread_create(&tid, NULL, server_tcp, &server_fd), "%d");

    /* IPC_MSG_STATS : payload connu, vérifié après réception. */
    struct { int8_t type; struct client_statistics stat; } __attribute__((packed)) msg;
    memset(&msg, 0, sizeof msg);
    msg.type = IPC_MSG_STATS;
    msg.stat.shots_per_second = 4242;
    msg.stat.max_result = 99;
    ASSERT_EQ_FMT((int)sizeof msg,
                  (int)sendto(child_fd, &msg, sizeof msg, 0,
                              (struct sockaddr *)server_addr, sizeof(struct sockaddr_un)),
                  "%d");

    /* Attente active bornée : fork_statistics[0] mis à jour de façon asynchrone. */
    for (int i = 0; i < 100 && fork_statistics[0].shots_per_second == 0; i++) usleep(10000);
    ASSERT_EQ_FMT(4242ULL, fork_statistics[0].shots_per_second, "%llu");
    ASSERT_EQ_FMT(99, (int)fork_statistics[0].max_result, "%d");

    /* IPC_MSG_EVENT : vérifié via events.log (comme test_logger.c). */
    unlink("events.log");
    char evbuf[16];
    evbuf[0] = IPC_MSG_EVENT;
    memcpy(evbuf + 1, "hi", 2);
    ASSERT_EQ_FMT(3, (int)sendto(child_fd, evbuf, 3, 0,
                                  (struct sockaddr *)server_addr, sizeof(struct sockaddr_un)), "%d");
    int seen_event = 0;
    for (int i = 0; i < 100 && !seen_event; i++) {
        usleep(10000);
        FILE *f = fopen("events.log", "r");
        if (f != NULL) {
            char line[64] = {0};
            size_t n = fread(line, 1, sizeof line - 1, f);
            fclose(f);
            (void)n;
            seen_event = (strstr(line, "hi") != NULL);
        }
    }
    ASSERT(seen_event);
    unlink("events.log");

    /* Types IPC_MSG_LOG_* (relais des logs des enfants vers le parent) et
     * datagramme VIDE (numBytes == 0 -> ignoré) : verbeux, on coupe
     * stdout+stderr le temps de l'envoi. */
    fflush(NULL);
    int s1 = dup(1), s2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }
    {
        char logbuf[8];
        const int8_t log_types[] = { IPC_MSG_LOG_INFO, IPC_MSG_LOG_ERROR,
                                     IPC_MSG_LOG_DEBUG, IPC_MSG_LOG_CONSOLE };
        for (size_t lt = 0; lt < sizeof log_types; lt++) {
            logbuf[0] = (char)log_types[lt];
            memcpy(logbuf + 1, "x\n", 2);
            sendto(child_fd, logbuf, 3, 0,
                   (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
        }
        sendto(child_fd, logbuf, 0, 0,   /* datagramme vide */
               (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
    }
    /* La boucle a survécu à tout le lot : un second STATS est encore traité
     * (les datagrammes UDP locaux sont servis dans l'ordre). */
    msg.stat.shots_per_second = 777;
    sendto(child_fd, &msg, sizeof msg, 0,
           (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
    for (int i = 0; i < 100 && fork_statistics[0].shots_per_second != 777; i++) usleep(10000);
    fflush(NULL);
    dup2(s1, 1); dup2(s2, 2);
    close(s1); close(s2);
    ASSERT_EQ_FMT(777ULL, fork_statistics[0].shots_per_second, "%llu");

    /* Type inconnu (branche default) : ne doit pas planter. */
    char unk[2] = { (char)0x7f, 0 };
    sendto(child_fd, unk, 1, 0, (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
    usleep(20000);

    /* Débloque et arrête la boucle : REQUEST_STOP puis un dernier datagramme
     * pour sortir du recvfrom() bloquant. */
    request = REQUEST_STOP;
    sendto(child_fd, unk, 1, 0, (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
    pthread_join(tid, NULL);

    request = saved_req;
    NB_THREADS = saved_nb;
    free(forkId);
    forkId = saved_forkid;
    fork_statistics = saved_fs;
    close(server_fd);
    close(child_fd);
    unlink(server_path);
    unlink(child_path);
    free(server_addr);
    free(child_addr);
    PASS();
}

/* ---------- fork_udp --------------------------------------------------------- */

/* Reçoit une commande, la délègue à do_command_line (ici « help », inoffensive),
 * puis sort sur REQUEST_STOP après un dernier datagramme de déblocage. */
TEST fork_udp_delegates_command_then_stops(void)
{
    char sock_path[] = "/tmp/etii_ar_udp_XXXXXX";
    int tfd = mkstemp(sock_path); ASSERT(tfd >= 0); close(tfd); unlink(sock_path);

    struct sockaddr_un *addr = build_sockaddr(sock_path);
    int fd = build_udp_local_socket(addr);
    ASSERT(fd >= 0);
    /* Timeout court : recvfrom expirera au moins une fois avant la commande,
     * exerçant la branche d'erreur (numBytes == -1 hors REQUEST_STOP). */
    struct timeval short_tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof short_tv);

    /* Émetteur : un second socket DGRAM anonyme suffit pour sendto(). */
    int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT(sender >= 0);

    int saved_req = request;
    request = REQUEST_CONTINUE;

    pthread_t tid;
    ASSERT_EQ_FMT(0, pthread_create(&tid, NULL, fork_udp, &fd), "%d");

    /* do_command_line("help") et log_errno (timeout, 1er au plus tôt à +100ms)
     * sont verbeux : coupe stdout+stderr le temps du thread (mute_fd/unmute_fd
     * ne gèrent qu'un seul fd à la fois). */
    fflush(NULL);
    int s1 = dup(1), s2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); close(dn); }

    usleep(300000); /* >= 1 timeout de réception -> branche d'erreur prise */

    char cmd[] = "help";
    sendto(sender, cmd, strlen(cmd), 0, (struct sockaddr *)addr, sizeof(struct sockaddr_un));
    usleep(30000); /* laisse fork_udp traiter la commande et reboucler sur recvfrom */

    request = REQUEST_STOP;
    sendto(sender, cmd, strlen(cmd), 0, (struct sockaddr *)addr, sizeof(struct sockaddr_un));
    pthread_join(tid, NULL);

    fflush(NULL);
    dup2(s1, 1); dup2(s2, 2);
    close(s1); close(s2);

    request = saved_req;
    close(sender);
    close(fd);
    unlink(sock_path);
    free(addr);
    PASS();
}

/* ---------- fork_checker ----------------------------------------------------- */

/* fork_checker construit son propre socket (etii_fork.<pid>, chemin relatif au
 * CWD comme en production), démarre fork_udp via run_fork_thread, puis envoie
 * périodiquement ses statistiques au socket "parent" fourni. On vérifie la
 * réception d'au moins un datagramme IPC_MSG_STATS, puis on arrête proprement
 * les DEUX threads (fork_checker lui-même + le fork_udp qu'il a démarré). */
TEST fork_checker_sends_stats_to_parent(void)
{
    char parent_path[] = "/tmp/etii_ar_parent_XXXXXX";
    int pfd = mkstemp(parent_path); ASSERT(pfd >= 0); close(pfd); unlink(parent_path);

    struct sockaddr_un *parent_addr = build_sockaddr(parent_path);
    int parent_fd = build_udp_local_socket(parent_addr);
    ASSERT(parent_fd >= 0);
    set_recv_timeout_ar(parent_fd, 3);

    char child_sock_path[64];
    snprintf(child_sock_path, sizeof child_sock_path, "etii_fork.%d", (int)getpid());
    unlink(child_sock_path); /* résidu éventuel d'un run précédent */

    int saved_req = request;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;
    unsigned long long *saved_lastfilesize = lastfilesize;
    request = REQUEST_CONTINUE;
    NB_THREADS = 1;
    unsigned long long ctr_buf[1] = {0}, lfs_buf[1] = {0};
    counters = ctr_buf;
    lastfilesize = lfs_buf;
    // g_search_best_board est un global process-wide (cf. core/best_board.h) :
    // une suite précédente (recherche/genèse) a pu déjà y écrire un record, ce
    // qui ferait émettre un second datagramme IPC_MSG_BEST_BOARD (taille
    // différente) que ce test ne connaît pas — réinitialisé pour garder les
    // lectures ci-dessous strictement sur IPC_MSG_STATS.
    best_board_init(&g_search_best_board);

    pthread_t tid;
    ASSERT_EQ_FMT(0, pthread_create(&tid, NULL, fork_checker, parent_addr), "%d");

    /* fork_checker envoie sa 1re statistique dès le démarrage (avant le 1er
     * sleep(1)) : borne large (3s) pour absorber la charge du runner. */
    char buf[1 + sizeof(struct client_statistics)];
    ssize_t n = recvfrom(parent_fd, buf, sizeof buf, 0, NULL, NULL);
    ASSERT(n > 0);
    ASSERT_EQ_FMT((int)IPC_MSG_STATS, (int)buf[0], "%d");

    /* 2e itération : de l'activité (compteur + cases étudiées) -> sps/pps > 0,
     * la moyenne glissante sur 5s emprunte ses branches m > 0 / mp > 0. */
    unsigned long long saved_prcells = pruner_cells_studied;
    ctr_buf[0] = 1000;
    pruner_cells_studied = 1000;
    struct client_statistics got;
    memset(&got, 0, sizeof got);
    for (int tour = 0; tour < 5 && got.shots_per_second == 0; tour++) {
        n = recvfrom(parent_fd, buf, sizeof buf, 0, NULL, NULL);
        ASSERT(n == (ssize_t)sizeof buf);
        memcpy(&got, buf + 1, sizeof got);
    }
    ASSERT(got.shots_per_second > 0);
    ASSERT(got.pruner_cells_per_second > 0);

    /* 3e itération : compteurs qui RECULENT (< dernier relevé) -> branches de
     * rattrapage « le compteur a fait un tour » (sps et pps). Les valeurs
     * calculées sont énormes (delta modulo 2^64) : on ne vérifie que
     * l'exécution, pas la moyenne. */
    ctr_buf[0] = 1;
    pruner_cells_studied = 1;
    /* 2 lectures : la 1re peut être un datagramme déjà en tampon, calculé
     * avant l'écriture ci-dessus ; la 2e a forcément échantillonné après. */
    for (int tour = 0; tour < 2; tour++) {
        n = recvfrom(parent_fd, buf, sizeof buf, 0, NULL, NULL);
        ASSERT(n == (ssize_t)sizeof buf);
    }
    pruner_cells_studied = saved_prcells;

    /* Arrêt propre : REQUEST_STOP fait sortir fork_checker au prochain tour
     * (borné par son sleep(1) interne) ; on débloque en plus le fork_udp
     * qu'il a démarré (etii_fork.<pid>), sinon il resterait bloqué sur
     * recvfrom indéfiniment. */
    request = REQUEST_STOP;
    int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sender >= 0) {
        struct sockaddr_un *child_addr = build_sockaddr(child_sock_path);
        char wake[] = "help";
        sendto(sender, wake, strlen(wake), 0, (struct sockaddr *)child_addr, sizeof(struct sockaddr_un));
        free(child_addr);
        close(sender);
    }

    pthread_join(tid, NULL);
    usleep(50000); /* laisse le fork_udp détaché terminer avant le nettoyage du fichier socket */

    request = saved_req;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    lastfilesize = saved_lastfilesize;
    close(parent_fd);
    unlink(parent_path);
    unlink(child_sock_path);
    free(parent_addr);
    PASS();
}

/* ---------- run_server_thread / run_fork_checker (wrappers détachés) -------- */
/*
 * Les threads sont DÉTACHÉS : pas de pthread_join possible. On prouve leur
 * démarrage par un effet observable (stats mises à jour / datagramme reçu),
 * on arrête par REQUEST_STOP + datagramme de réveil, puis on attend assez
 * longtemps pour que le thread ait constaté l'arrêt AVANT de restaurer les
 * globales qu'il lit (les tampons câblés sont file-static, jamais libérés :
 * aucun accès à de la mémoire morte même si l'attente était trop courte).
 */

static unsigned long long g_det_ctr[1];
static unsigned long long g_det_lfs[1];
static struct client_statistics g_det_fs[1];
static char *g_det_forkid[1];

/* run_server_thread : le server_tcp détaché reçoit un IPC_MSG_STATS et met à
 * jour fork_statistics[0]. */
TEST run_server_thread_receives_stats(void)
{
    char server_path[] = "/tmp/etii_ar_rst_XXXXXX";
    char child_path[]  = "/tmp/etii_ar_rstc_XXXXXX";
    int sfd = mkstemp(server_path); ASSERT(sfd >= 0); close(sfd); unlink(server_path);
    int cfd = mkstemp(child_path);  ASSERT(cfd >= 0); close(cfd); unlink(child_path);

    struct sockaddr_un *server_addr = build_sockaddr(server_path);
    static int server_fd;                     /* lu par le thread détaché */
    server_fd = build_udp_local_socket(server_addr);
    ASSERT(server_fd >= 0);
    struct sockaddr_un *child_addr = build_sockaddr(child_path);
    int child_fd = build_udp_local_socket(child_addr);
    ASSERT(child_fd >= 0);

    int saved_req = request;
    int saved_nb = NB_THREADS;
    char **saved_forkid = forkId;
    struct client_statistics *saved_fs = fork_statistics;

    request = REQUEST_CONTINUE;
    NB_THREADS = 1;
    g_det_forkid[0] = (char *)child_path;
    forkId = g_det_forkid;
    memset(g_det_fs, 0, sizeof g_det_fs);
    fork_statistics = g_det_fs;

    mute_fd(1);                               /* run_server_thread logue le socket */
    run_server_thread(&server_fd);
    unmute_fd();

    struct { int8_t type; struct client_statistics stat; } __attribute__((packed)) msg;
    memset(&msg, 0, sizeof msg);
    msg.type = IPC_MSG_STATS;
    msg.stat.shots_per_second = 31337;
    sendto(child_fd, &msg, sizeof msg, 0,
           (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
    for (int i = 0; i < 100 && g_det_fs[0].shots_per_second == 0; i++) usleep(10000);
    ASSERT_EQ_FMT(31337ULL, g_det_fs[0].shots_per_second, "%llu");

    /* Arrêt : STOP + datagramme de réveil, puis délai avant restauration. */
    request = REQUEST_STOP;
    char unk[1] = { 0x7f };
    sendto(child_fd, unk, 1, 0, (struct sockaddr *)server_addr, sizeof(struct sockaddr_un));
    usleep(200000);

    request = saved_req;
    NB_THREADS = saved_nb;
    forkId = saved_forkid;
    fork_statistics = saved_fs;
    close(server_fd);
    close(child_fd);
    unlink(server_path);
    unlink(child_path);
    free(server_addr);
    free(child_addr);
    PASS();
}

/* run_fork_checker : le fork_checker détaché construit son socket
 * etii_fork.<pid>, démarre son fork_udp interne, et envoie ses stats au
 * parent. Même chorégraphie d'arrêt que fork_checker_sends_stats_to_parent
 * (réveil du fork_udp interne), avec en plus un délai final couvrant le
 * sleep(1) du fork_checker détaché. */
TEST run_fork_checker_sends_stats(void)
{
    char parent_path[] = "/tmp/etii_ar_rfc_XXXXXX";
    int pfd = mkstemp(parent_path); ASSERT(pfd >= 0); close(pfd); unlink(parent_path);

    static struct sockaddr_un *parent_addr;   /* lu par le thread détaché */
    parent_addr = build_sockaddr(parent_path);
    int parent_fd = build_udp_local_socket(parent_addr);
    ASSERT(parent_fd >= 0);
    set_recv_timeout_ar(parent_fd, 3);

    char child_sock_path[64];
    snprintf(child_sock_path, sizeof child_sock_path, "etii_fork.%d", (int)getpid());
    unlink(child_sock_path);

    int saved_req = request;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;
    unsigned long long *saved_lastfilesize = lastfilesize;
    request = REQUEST_CONTINUE;
    NB_THREADS = 1;
    g_det_ctr[0] = 0;
    g_det_lfs[0] = 0;
    counters = g_det_ctr;
    lastfilesize = g_det_lfs;

    mute_fd(1);                               /* fork_checker logue son socket */
    ASSERT_EQ_FMT(0, run_fork_checker(parent_addr), "%d");

    char buf[1 + sizeof(struct client_statistics)];
    ssize_t n = recvfrom(parent_fd, buf, sizeof buf, 0, NULL, NULL);
    unmute_fd();
    ASSERT(n > 0);
    ASSERT_EQ_FMT((int)IPC_MSG_STATS, (int)buf[0], "%d");

    /* Arrêt : STOP, réveil du fork_udp interne, puis délai > sleep(1) pour que
     * le fork_checker détaché constate l'arrêt avant la restauration. */
    request = REQUEST_STOP;
    int sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sender >= 0) {
        struct sockaddr_un *child_addr = build_sockaddr(child_sock_path);
        char wake[] = "help";
        sendto(sender, wake, strlen(wake), 0, (struct sockaddr *)child_addr, sizeof(struct sockaddr_un));
        free(child_addr);
        close(sender);
    }
    sleep(2);

    request = saved_req;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    lastfilesize = saved_lastfilesize;
    close(parent_fd);
    unlink(parent_path);
    unlink(child_sock_path);
    free(parent_addr);
    PASS();
}

/* ---------- signal_end_handler : propagation aux enfants -------------------- */

/* Le parent (parent_pid == getpid(), mode client) propage le signal d'arrêt à
 * chaque enfant enregistré dans childrens_pid : un vrai fils dormeur doit être
 * terminé par SIGTERM. */
TEST signal_end_handler_kills_registered_children(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    int saved_server = server;
    int saved_parent = parent_pid;
    pid_t *saved_children = childrens_pid;

    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        for (;;) { sleep(10); }               /* tué par le SIGTERM propagé */
    }

    pid_t kids[1] = { child };
    childrens_pid = kids;
    NB_THREADS = 1;
    server = 0;                                /* pas de exit(0) */
    parent_pid = getpid();
    request = REQUEST_CONTINUE;

    signal_end_handler(SIGTERM);

    ASSERT_EQ_FMT(REQUEST_STOP, request, "%d");
    int status = 0;
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT(WIFSIGNALED(status));
    ASSERT_EQ_FMT(SIGTERM, WTERMSIG(status), "%d");

    childrens_pid = saved_children;
    NB_THREADS = saved_nb;
    server = saved_server;
    parent_pid = saved_parent;
    request = saved_req;
    PASS();
}

/* ---------- wait_child : reprise sur EINTR ---------------------------------- */

static void alarm_noop_handler(int sig) { (void)sig; }

/* Un signal sans SA_RESTART (ex. SIGWINCH de ncurses en production) interrompt
 * wait() avec EINTR : wait_child doit RETENTER, pas sortir — sinon le parent
 * abandonnerait des enfants vivants. Le fils dort plus longtemps que l'alarme,
 * garantissant l'ordre EINTR puis récolte puis ECHILD. */
TEST wait_child_retries_on_eintr(void)
{
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = alarm_noop_handler;
    sa.sa_flags = 0;                           /* PAS de SA_RESTART : wait -> EINTR */
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGALRM, &sa, &old_sa));

    pid_t child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        usleep(400000);
        exit(0);                               /* exit() : flush gcov du fils */
    }

    struct itimerval it;
    memset(&it, 0, sizeof it);
    it.it_value.tv_usec = 100000;              /* alarme unique à +100ms */
    ASSERT_EQ(0, setitimer(ITIMER_REAL, &it, NULL));

    mute_fd(1);                                /* wait_child logue début/fin */
    wait_child();                              /* EINTR -> retente -> récolte -> ECHILD */
    unmute_fd();

    sigaction(SIGALRM, &old_sa, NULL);
    /* Le fils a bien été récolté par wait_child : plus rien à attendre. */
    ASSERT_EQ(-1, waitpid(child, NULL, 0));
    PASS();
}

/* --------------------------------------------------------------------------
 * count_alive_forks
 * ------------------------------------------------------------------------ */

/* Prédicat de vivacité déterministe : « vivant » = pid pair. */
static int fake_alive_even(pid_t pid) { return (pid % 2) == 0; }

/* Ne compte que les slots RÉELLEMENT occupés (`pid > 0`) ET vivants. Un slot
 * libre vaut -1 (init_childs) et ne doit jamais être sondé comme un pid. */
TEST count_alive_forks_counts_only_occupied_and_alive_slots(void)
{
    pid_t pids[5] = { 10, 11, -1, 12, 0 };
    /* 10 et 12 pairs -> vivants ; 11 impair -> mort ; -1 et 0 -> slots libres */
    ASSERT_EQ_FMT(2, count_alive_forks(pids, 5, fake_alive_even), "%d");
    PASS();
}

TEST count_alive_forks_handles_empty_and_null(void)
{
    pid_t pids[1] = { 10 };
    ASSERT_EQ_FMT(0, count_alive_forks(NULL, 3, fake_alive_even), "%d");
    ASSERT_EQ_FMT(0, count_alive_forks(pids, 0, fake_alive_even), "%d");
    ASSERT_EQ_FMT(0, count_alive_forks(pids, -1, fake_alive_even), "%d");
    PASS();
}

/* Prédicat NULL : repli sur pid_is_alive, comme reap_dead_child_slots. Le
 * process de test lui-même est nécessairement vivant. */
TEST count_alive_forks_null_predicate_falls_back_to_pid_is_alive(void)
{
    pid_t pids[1] = { getpid() };
    ASSERT_EQ_FMT(1, count_alive_forks(pids, 1, NULL), "%d");
    PASS();
}


SUITE(app_runtime_suite)
{
    RUN_TEST(count_alive_forks_counts_only_occupied_and_alive_slots);
    RUN_TEST(count_alive_forks_handles_empty_and_null);
    RUN_TEST(count_alive_forks_null_predicate_falls_back_to_pid_is_alive);
    RUN_TEST(init_counters_allocates_zeroed);
    RUN_TEST(init_counters_is_safe_to_call_twice_in_a_row);
    RUN_TEST(init_childs_initializes_contexts);
    RUN_TEST(ensure_childs_capacity_grows_and_preserves_existing_slots);
    RUN_TEST(ensure_childs_capacity_is_a_no_op_when_already_covered);
    RUN_TEST(free_childs_then_init_childs_resizes_cleanly);
    RUN_TEST(free_childs_tolerates_already_freed_state);
    RUN_TEST(pid_is_alive_detects_self_and_reaped_child);
    RUN_TEST(pid_is_alive_rejects_non_positive_pid);
    RUN_TEST(reap_dead_child_slots_clears_only_dead_slots);
    RUN_TEST(reap_dead_child_slots_defaults_to_pid_is_alive);
    RUN_TEST(reap_dead_child_slots_tolerates_null_arrays);
    RUN_TEST(failed_arg_prints_usage);
    RUN_TEST(cli_help_general_lists_every_topic);
    RUN_TEST(cli_help_find_topic_matches_flexibly);
    RUN_TEST(cli_help_topic_formats_known_and_rejects_unknown);
    RUN_TEST(cli_help_truncates_safely_in_small_buffer);
    RUN_TEST(print_cli_help_topic_returns_status);
    RUN_TEST(signal_ignored_is_noop);
    RUN_TEST(signal_end_handler_sets_request_stop);
    RUN_TEST(signal_end_handler_server_calls_exit);
    RUN_TEST(sigchld_handler_without_children_is_noop);
    RUN_TEST(child_death_ring_records_and_drains_in_order);
    RUN_TEST(child_death_ring_reports_dropped_on_overflow);
    RUN_TEST(child_death_format_reason_decodes_normal_exit);
    RUN_TEST(child_death_format_reason_decodes_signal_death);
    RUN_TEST(child_death_is_clean_exit_only_for_exit_code_zero);
    RUN_TEST(child_death_format_reason_tolerates_null_output);
    RUN_TEST(sigchld_handler_records_child_death);
    RUN_TEST(wait_child_reaps_children);
    RUN_TEST(init_sigchld_sigaction_installs_handler);
    RUN_TEST(init_signals_installs_handlers);
    RUN_TEST(configure_child_signals_installs_sigint);

    RUN_TEST(parse_positive_int_absent_arg_uses_fallback);
    RUN_TEST(parse_positive_int_valid_arg_returns_parsed_value);
    RUN_TEST(parse_positive_int_zero_or_negative_is_invalid);
    RUN_TEST(parse_positive_int_non_numeric_is_invalid);
    RUN_TEST(parse_positive_int_null_out_param_is_safe);

    RUN_TEST(server_arg_absent_keeps_default);
    RUN_TEST(server_arg_valid_number_sets_count);
    RUN_TEST(server_arg_zero_is_invalid_count_keeps_default);
    RUN_TEST(server_arg_negative_is_invalid_count_keeps_default);
    RUN_TEST(server_arg_filename_is_detected);
    RUN_TEST(server_arg_empty_string_is_filename);
    RUN_TEST(server_arg_plus_prefixed_number_is_count);

    RUN_TEST(client_args_defaults);
    RUN_TEST(client_args_server_and_threads);
    RUN_TEST(client_args_invalid_threads_falls_back);
    RUN_TEST(client_args_search_client_stock_and_file);
    RUN_TEST(client_args_pruner_file_and_batch);
    RUN_TEST(client_args_pruner_batch_is_clamped);

    RUN_TEST(gpu_pruner_forks_conflict_never_when_gpu_inactive);
    RUN_TEST(gpu_pruner_forks_conflict_never_when_not_requested);
    RUN_TEST(gpu_pruner_forks_conflict_never_when_requested_equals_nb_forks);
    RUN_TEST(gpu_pruner_forks_conflict_detected_for_mixed_dosage);

    RUN_TEST(backup_failed_exit_empty_is_noop);
    RUN_TEST(backup_failed_exit_saves_leftover_stock);
    RUN_TEST(ensure_stock_files_cover_forks_noop_when_already_sufficient);
    RUN_TEST(ensure_stock_files_cover_forks_grows_when_forks_exceed_files);

    RUN_TEST(run_checker_client_starts_and_stops);
    RUN_TEST(run_checker_server_starts_and_stops);

    RUN_TEST(server_tcp_updates_stats_and_handles_event_and_unknown_type);
    RUN_TEST(fork_udp_delegates_command_then_stops);
    RUN_TEST(fork_checker_sends_stats_to_parent);

    RUN_TEST(run_server_thread_receives_stats);
    RUN_TEST(run_fork_checker_sends_stats);
    RUN_TEST(signal_end_handler_kills_registered_children);
    RUN_TEST(wait_child_retries_on_eintr);
}
