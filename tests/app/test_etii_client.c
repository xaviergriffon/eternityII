/*
 * Tests unitaires des fonctions pures extraites de etii_client.c.
 *
 * Fonctions couvertes :
 *   - next_no_work_sleep       : calcul du back-off adaptatif
 *   - count_created_forks      : décompte des process enfants créés
 *   - find_fork_index          : recherche d'un socket fork par son chemin
 *   - build_thread_queues_table: tableau de stats par fork (corps extrait de la
 *                                boucle check_client_threads)
 */
#include "greatest.h"
#include "app/etii_client.h"
#include "app/app_static_variables.h"
#include "app/fork_orchestrator.h"
#include "app/etii_statistic.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "net/etii_protocol.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

/* Helpers internes de etii_client.c non exposés dans etii_client.h. */
void *control_thread(void *param);
pthread_t build_control_thread(client_possibility_t *thread_params);
void *feed_thread_aposs(void *param);
pthread_t build_feed_thread(client_possibility_t *thread_params);
void check_client_threads_step(int *last_record);

/* Vide le stock local du datamanager (état global partagé entre suites). */
static void dm_drain_local(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
        free_array_possibility_packet(r);
    }
}

/* ---------- next_no_work_sleep ------------------------------------------- */

TEST sleep_zero_returns_start(void)
{
    ASSERT_EQ((useconds_t)NO_WORK_SLEEP_START, next_no_work_sleep(0));
    PASS();
}

TEST sleep_doubles_below_max(void)
{
    useconds_t v = next_no_work_sleep(NO_WORK_SLEEP_START);
    ASSERT_EQ((useconds_t)(NO_WORK_SLEEP_START * 2), v);
    PASS();
}

TEST sleep_caps_at_max(void)
{
    ASSERT_EQ((useconds_t)NO_WORK_SLEEP_MAX, next_no_work_sleep(NO_WORK_SLEEP_MAX));
    ASSERT_EQ((useconds_t)NO_WORK_SLEEP_MAX, next_no_work_sleep(NO_WORK_SLEEP_MAX / 2 + 1));
    PASS();
}

TEST sleep_progression_reaches_max(void)
{
    useconds_t v = 0;
    for (int i = 0; i < 64; i++) {
        v = next_no_work_sleep(v);
        if (v == (useconds_t)NO_WORK_SLEEP_MAX) break;
    }
    ASSERT_EQ((useconds_t)NO_WORK_SLEEP_MAX, v);
    PASS();
}

/* ---------- count_created_forks ------------------------------------------ */

TEST count_all_negative_returns_zero(void)
{
    pid_t pids[] = { -1, -1, -1 };
    ASSERT_EQ(0, count_created_forks(pids, 3));
    PASS();
}

TEST count_all_positive(void)
{
    pid_t pids[] = { 100, 200, 300 };
    ASSERT_EQ(3, count_created_forks(pids, 3));
    PASS();
}

TEST count_mixed(void)
{
    pid_t pids[] = { 100, -1, 300, -1, 500 };
    ASSERT_EQ(3, count_created_forks(pids, 5));
    PASS();
}

TEST count_zero_is_not_counted(void)
{
    pid_t pids[] = { 0, 100, 0 };
    ASSERT_EQ(1, count_created_forks(pids, 3));
    PASS();
}

TEST count_empty_array(void)
{
    ASSERT_EQ(0, count_created_forks(NULL, 0));
    PASS();
}

/* ---------- find_fork_index ---------------------------------------------- */

TEST find_returns_minus_one_when_empty(void)
{
    char *ids[] = { "", "", "" };
    ASSERT_EQ(-1, find_fork_index("etii_fork.999", ids, 3));
    PASS();
}

TEST find_returns_correct_index(void)
{
    char *ids[] = { "etii_fork.10", "etii_fork.20", "etii_fork.30" };
    ASSERT_EQ(1, find_fork_index("etii_fork.20", ids, 3));
    PASS();
}

TEST find_returns_first_match(void)
{
    char *ids[] = { "etii_fork.99", "etii_fork.99", "etii_fork.99" };
    ASSERT_EQ(0, find_fork_index("etii_fork.99", ids, 3));
    PASS();
}

TEST find_no_match_returns_minus_one(void)
{
    char *ids[] = { "etii_fork.1", "etii_fork.2" };
    ASSERT_EQ(-1, find_fork_index("etii_fork.42", ids, 2));
    PASS();
}

/* ---------- build_thread_queues_table ------------------------------------ */

/* Agrège les statistiques par fork : totaux stock/analysed/coups-s + mise à jour
   de max_result (meilleur des forks). NB_THREADS / fork_statistics / max_result
   sont sauvegardés et restaurés (état global partagé entre suites). */
TEST thread_queues_table_aggregates_forks(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved = fork_statistics;

    struct client_statistics fs[3];
    memset(fs, 0, sizeof fs);
    fs[0].possibilities_in_stock = 10; fs[0].analyses_in_stock = 1; fs[0].shots_per_second = 100; fs[0].max_result = 50;
    fs[1].possibilities_in_stock = 20; fs[1].analyses_in_stock = 2; fs[1].shots_per_second = 200; fs[1].max_result = 80;
    fs[2].possibilities_in_stock = 30; fs[2].analyses_in_stock = 3; fs[2].shots_per_second = 300; fs[2].max_result = 40;
    NB_THREADS = 3;
    fork_statistics = fs;
    max_result = 0;

    unsigned long long stock = 0, analysed = 0, bys = 0;
    char *t = build_thread_queues_table(&stock, &analysed, &bys);
    int header = (strstr(t, "Thread queues") != NULL);
    int total  = (strstr(t, "Total|") != NULL);
    uint16_t mr = max_result;
    free(t);

    fork_statistics = saved; NB_THREADS = saved_nb; max_result = saved_mr;

    ASSERT(header);
    ASSERT(total);
    ASSERT_EQ_FMT(60ULL, stock, "%llu");     /* 10+20+30 */
    ASSERT_EQ_FMT(6ULL, analysed, "%llu");   /* 1+2+3 */
    ASSERT_EQ_FMT(600ULL, bys, "%llu");      /* 100+200+300 */
    ASSERT_EQ_FMT(80, (int)mr, "%d");        /* max des max_result par fork */
    PASS();
}

/* Colonne Type (dosage recherche/contrôle par fork, --pruner-forks) : le rôle
 * affiché doit refléter current_fork_role(f), pas une valeur figée -- les
 * rangs les plus hauts sont les pruners (fork_role_for), comme
 * spawn_child_body les affecte réellement. */
TEST thread_queues_table_shows_per_fork_role(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved = fork_statistics;
    int saved_pfr = pruner_forks_requested;
    int saved_pm = pruner_mode;

    struct client_statistics fs[4];
    memset(fs, 0, sizeof fs);
    NB_THREADS = 4;
    fork_statistics = fs;
    pruner_mode = 0;
    pruner_forks_requested = 2; /* forks 2 et 3 : pruner ; forks 0 et 1 : recherche */

    unsigned long long stock = 0, analysed = 0, bys = 0;
    char *t = build_thread_queues_table(&stock, &analysed, &bys);

    int row0_search = (strstr(t, "   0 | search") != NULL);
    int row1_search = (strstr(t, "   1 | search") != NULL);
    int row2_prune  = (strstr(t, "   2 | prune") != NULL);
    int row3_prune  = (strstr(t, "   3 | prune") != NULL);
    free(t);

    fork_statistics = saved; NB_THREADS = saved_nb; max_result = saved_mr;
    pruner_forks_requested = saved_pfr; pruner_mode = saved_pm;

    ASSERT(row0_search);
    ASSERT(row1_search);
    ASSERT(row2_prune);
    ASSERT(row3_prune);
    PASS();
}

/* Régression du débordement de tas : sur un NB_THREADS élevé, le buffer est
   dimensionné dynamiquement (256 + NB_THREADS*80). ASan ferait échouer le test
   en cas de corruption. */
TEST thread_queues_table_large_nb_threads_no_overflow(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved = fork_statistics;

    struct client_statistics *fs = calloc(100, sizeof *fs);
    for (int i = 0; i < 100; i++) fs[i].possibilities_in_stock = 1;
    NB_THREADS = 100;
    fork_statistics = fs;

    unsigned long long stock = 0, analysed = 0, bys = 0;
    char *t = build_thread_queues_table(&stock, &analysed, &bys);
    int ok = (t != NULL && strstr(t, "Total|") != NULL);
    free(t);

    fork_statistics = saved; NB_THREADS = saved_nb; max_result = saved_mr;
    free(fs);

    ASSERT(ok);
    ASSERT_EQ_FMT(100ULL, stock, "%llu");   /* 100 forks * 1 */
    PASS();
}

/* ---------- control_step ------------------------------------------------- */
/*
 * control_step régule le débit via la globale `request`. Les tests sauvegardent
 * et restaurent toutes les globales touchées (request, max_search_by_sec,
 * NB_THREADS, counters) pour ne pas polluer les autres suites.
 */

/* En mode illimité (max_search_by_sec == 0), `request` n'est jamais modifié ;
   seul le compteur de fenêtre avance. */
TEST control_step_unlimited_leaves_request(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;

    request = REQUEST_CONTINUE;
    max_search_by_sec = 0;
    unsigned long long oneSecond = 0;
    int nbCheck = 0;

    control_step(NULL, NULL, &oneSecond, &nbCheck);

    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");
    ASSERT_EQ_FMT(1, nbCheck, "%d");      /* fenêtre avancée */

    request = saved_req;
    max_search_by_sec = saved_max;
    PASS();
}

/* Débit estimé au-dessus de la limite : CONTINUE -> PAUSE. */
TEST control_step_high_rate_pauses(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;

    NB_THREADS = 1;
    max_search_by_sec = 1;
    unsigned long long my_counters[1] = { 1000000ULL };
    counters = my_counters;

    array_possibility_packet dummy = { .size = 1, .possibilities = NULL };
    client_possibility_t tp[1];
    memset(tp, 0, sizeof tp);
    tp[0].works = 1;
    tp[0].aposs = &dummy;            /* thread actif */

    unsigned long long lastCheck[1] = { 0ULL };
    unsigned long long oneSecond = 0;
    int nbCheck = 1;                 /* > 0 : le calcul de débit s'exécute */
    request = REQUEST_CONTINUE;

    control_step(tp, lastCheck, &oneSecond, &nbCheck);

    ASSERT_EQ_FMT(REQUEST_PAUSE, request, "%d");
    ASSERT_EQ_FMT(1000000ULL, lastCheck[0], "%llu");  /* compteur mémorisé */

    request = saved_req;
    max_search_by_sec = saved_max;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    PASS();
}

/* counters[t] < lastCheck[t] : le compteur (non signé) a rebouclé — la branche
 * de rattrapage calcule le delta modulo 2^64 au lieu d'un delta négatif. */
TEST control_step_counter_wraparound(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;

    NB_THREADS = 1;
    max_search_by_sec = 1000000000ULL;
    unsigned long long my_counters[1] = { 4ULL };    /* reparti de ~0 après le tour */
    counters = my_counters;

    array_possibility_packet dummy = { .size = 1, .possibilities = NULL };
    client_possibility_t tp[1];
    memset(tp, 0, sizeof tp);
    tp[0].works = 1;
    tp[0].aposs = &dummy;

    unsigned long long lastCheck[1] = { ~0ULL - 5 }; /* proche du max avant rebouclage */
    unsigned long long oneSecond = 0;
    int nbCheck = 1;
    request = REQUEST_CONTINUE;

    control_step(tp, lastCheck, &oneSecond, &nbCheck);

    ASSERT_EQ_FMT(4ULL, lastCheck[0], "%llu");       /* compteur re-mémorisé */
    /* Formule de rattrapage : (~0ULL - lastCheck) + counter = 5 + 4. */
    ASSERT_EQ_FMT(9ULL, oneSecond, "%llu");

    request = saved_req;
    max_search_by_sec = saved_max;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    PASS();
}

/* Débit estimé sous la limite : PAUSE -> CONTINUE. */
TEST control_step_low_rate_resumes(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;

    NB_THREADS = 1;
    max_search_by_sec = 1000000000ULL;
    unsigned long long my_counters[1] = { 1ULL };
    counters = my_counters;

    array_possibility_packet dummy = { .size = 1, .possibilities = NULL };
    client_possibility_t tp[1];
    memset(tp, 0, sizeof tp);
    tp[0].works = 1;
    tp[0].aposs = &dummy;

    unsigned long long lastCheck[1] = { 0ULL };
    unsigned long long oneSecond = 0;
    int nbCheck = 1;
    request = REQUEST_PAUSE;

    control_step(tp, lastCheck, &oneSecond, &nbCheck);

    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    max_search_by_sec = saved_max;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    PASS();
}

/* Thread inactif pendant une pause : la fenêtre le réveille (PAUSE -> CONTINUE). */
TEST control_step_idle_thread_resumes(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;

    NB_THREADS = 1;
    max_search_by_sec = 1;
    unsigned long long my_counters[1] = { 0ULL };
    counters = my_counters;

    client_possibility_t tp[1];
    memset(tp, 0, sizeof tp);
    tp[0].works = 0;                 /* inactif */
    tp[0].aposs = NULL;

    unsigned long long lastCheck[1] = { 0ULL };
    unsigned long long oneSecond = 0;
    int nbCheck = 0;                 /* 0 : pas de calcul de débit ce tour */
    request = REQUEST_PAUSE;

    control_step(tp, lastCheck, &oneSecond, &nbCheck);

    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    max_search_by_sec = saved_max;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    PASS();
}

/* Au bout de 1000 tours, la fenêtre de mesure est réinitialisée et une pause
   éventuelle est levée. */
TEST control_step_window_resets_after_1000(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;

    max_search_by_sec = 0;           /* on isole le bloc de reset de fenêtre */
    request = REQUEST_PAUSE;
    unsigned long long oneSecond = 5;
    int nbCheck = 1001;

    control_step(NULL, NULL, &oneSecond, &nbCheck);

    ASSERT_EQ_FMT(0, nbCheck, "%d");
    ASSERT_EQ_FMT(0ULL, oneSecond, "%llu");
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    max_search_by_sec = saved_max;
    PASS();
}

/* Régression : REQUEST_ADMIN_PAUSE est une pause OPÉRATEUR (commande console
 * `pause`/`resume`), distincte de REQUEST_PAUSE (régulation de débit). Le
 * régulateur control_step ne doit JAMAIS la lever automatiquement — ni via la
 * branche "thread inactif" (qui lève REQUEST_PAUSE -> REQUEST_CONTINUE), ni via
 * la branche "débit sous la limite", ni via le reset de fenêtre des 1000 tours.
 * Sans cette garantie, la pause admin serait annulée dès le prochain tour de
 * control_thread, la rendant inutilisable. */
TEST control_step_does_not_touch_admin_pause(void)
{
    int saved_req = request;
    unsigned long long saved_max = max_search_by_sec;
    int saved_nb = NB_THREADS;
    unsigned long long *saved_counters = counters;

    NB_THREADS = 1;
    max_search_by_sec = 1;
    unsigned long long my_counters[1] = { 0ULL };
    counters = my_counters;

    client_possibility_t tp[1];
    memset(tp, 0, sizeof tp);
    tp[0].works = 0;                 /* thread inactif : chemin le plus agressif */
    tp[0].aposs = NULL;

    unsigned long long lastCheck[1] = { 0ULL };
    unsigned long long oneSecond = 0;
    int nbCheck = 0;
    request = REQUEST_ADMIN_PAUSE;

    control_step(tp, lastCheck, &oneSecond, &nbCheck);
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, request, "%d");

    /* Idem sur le reset de fenêtre (nbCheck > 1000) : max_search_by_sec remis à
       0 pour isoler ce bloc, comme control_step_window_resets_after_1000 —
       sinon le bloc de régulation ci-dessus déréférencerait thread_params=NULL. */
    request = REQUEST_ADMIN_PAUSE;
    max_search_by_sec = 0;
    oneSecond = 5;
    nbCheck = 1001;
    control_step(NULL, NULL, &oneSecond, &nbCheck);
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, request, "%d");

    request = saved_req;
    max_search_by_sec = saved_max;
    NB_THREADS = saved_nb;
    counters = saved_counters;
    PASS();
}

/* ---------- feed_one_thread ---------------------------------------------- */
/*
 * feed_one_thread alimente un thread en mode local (server_ip == NULL par
 * défaut) : get_last_possibility/send_possibility_analysed passent par le
 * datamanager. On câble un client_possibility_t à la main (mutex initialisés).
 */

static void init_test_client(client_possibility_t *c, int works)
{
    memset(c, 0, sizeof *c);
    c->id = 0;
    c->compteur = 0;
    c->socket_id = -1;
    c->works = works;
    c->aposs = NULL;
    pthread_mutex_init(&c->works_mutex, NULL);
    pthread_mutex_init(&c->socket_mutex, NULL);
}

static void destroy_test_client(client_possibility_t *c)
{
    pthread_mutex_destroy(&c->works_mutex);
    pthread_mutex_destroy(&c->socket_mutex);
}

/* request == REQUEST_STOP : la fonction ne touche à rien. */
TEST feed_one_thread_not_continue_is_noop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    client_possibility_t client[1];
    init_test_client(&client[0], 0);

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(0, needed, "%d");
    ASSERT_EQ_FMT(0, got, "%d");

    destroy_test_client(&client[0]);
    request = saved_req;
    PASS();
}

/* En pause (admin ou régulation), un thread sans travail (works == 0) ne doit
 * PAS réclamer de nouvelle possibilité au serveur — mais s'il a un socket
 * ouvert, le keepalive doit quand même tourner : sinon une pause plus longue
 * que tcp_timeout laisse le serveur fermer le socket sans que le client ne le
 * sache, d'où l'erreur observée à la reprise ("Error on need work poll",
 * poll_server_hunger sur un socket déjà mort côté serveur). */
TEST feed_one_thread_admin_pause_keeps_socket_alive(void)
{
    int saved_req = request;
    request = REQUEST_ADMIN_PAUSE;
    __atomic_store_n(&server_hunger, 0, __ATOMIC_RELAXED);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair");

    client_possibility_t client[1];
    init_test_client(&client[0], 0);   /* works = 0 : sans travail */
    client[0].socket_id = sv[0];
    client[0].last_socket_activity = 0; /* intervalle de sonde forcément écoulé */

    int32_t hunger = 3;
    if (send_all(sv[1], &hunger, sizeof(hunger)) != (long)sizeof(hunger)) FAILm("send_all");

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(0, needed, "%d");                  /* pas de demande de travail */
    ASSERT_EQ_FMT(0, got, "%d");
    ASSERT_EQ_FMT(sv[0], client[0].socket_id, "%d"); /* socket conservé (keepalive) */
    ASSERT_EQ_FMT((int)INST_NEED_WORK, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]);
    close(sv[1]);
    destroy_test_client(&client[0]);
    __atomic_store_n(&server_hunger, 0, __ATOMIC_RELAXED);
    request = saved_req;
    PASS();
}

/* Thread sans travail, une possibilité dispo : il la reçoit et passe works=1. */
TEST feed_one_thread_gets_work(void)
{
    int saved_req = request, saved_pm = pruner_mode;
    dm_drain_local();
    request = REQUEST_CONTINUE;
    pruner_mode = 0;

    struct possibility_packet *p = malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->alloc = 2;
    array_possibility_packet *ap = malloc(sizeof *ap);
    ap->size = 1;
    ap->possibilities = p;
    add_possibility(NULL, ap);
    free_array_possibility_packet(ap);

    client_possibility_t client[1];
    init_test_client(&client[0], 0);

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(1, needed, "%d");
    ASSERT_EQ_FMT(1, got, "%d");
    ASSERT_EQ_FMT(1, (int)client[0].works, "%d");
    ASSERT(client[0].aposs != NULL);
    ASSERT_EQ_FMT(1, client[0].aposs->size, "%d");

    /* nettoyage : aposs reçue + vidage de l'« en analyse » du thread 0 */
    free_array_possibility_packet(client[0].aposs);
    send_possibility_analysed(&client[0]);
    destroy_test_client(&client[0]);
    request = saved_req;
    pruner_mode = saved_pm;
    PASS();
}

/* Un lot servi depuis le pool LOCAL (server_ip != NULL, mais
 * get_last_possibility trouve tout via scroll_from_local avant de solliciter
 * le réseau — cf. son repli put_to_local, typiquement un ADD refusé par le
 * serveur sous contention) ne doit PAS être suivi dans
 * file_possibility_analysed : le serveur n'a rien (ré-)enregistré comme « en
 * analyse » pour ce lot cette fois-ci. Avant le correctif, feed_one_thread
 * appelait add_possibility_analysed inconditionnellement, quelle que soit la
 * source — un lot recyclé localement finissait par être acquitté une seconde
 * fois (send_possibility_analysed), le serveur ne retrouvant alors plus rien
 * à retirer (« absence confirmée »), reproduit en conditions réelles
 * (--expand-level 9, pruner 4 forks, cf. AGENTS.md) mais invisible ici sans
 * ce test : aucun test existant ne pousse jamais le stock local du client au
 * point de faire échouer un ADD serveur. */
TEST feed_one_thread_local_recycle_skips_analysed_tracking(void)
{
    int saved_req = request, saved_pm = pruner_mode;
    dm_drain_local();
    request = REQUEST_CONTINUE;
    pruner_mode = 0;

    struct possibility_packet *p = malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->alloc = 3;
    array_possibility_packet *ap = malloc(sizeof *ap);
    ap->size = 1;
    ap->possibilities = p;
    add_possibility(NULL, ap); /* put_to_local : simule le repli d'un ADD refusé */
    free_array_possibility_packet(ap);

    client_possibility_t client[1];
    init_test_client(&client[0], 0);
    set_server_ip("127.0.0.1"); /* scénario client réel : scroll_from_local doit primer */

    ASSERT_EQ_FMT(0ULL, file_analysed_size(0), "%llu");

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(1, needed, "%d");
    ASSERT_EQ_FMT(1, got, "%d");
    ASSERT(client[0].aposs != NULL);
    ASSERT_EQ_FMT(1, client[0].aposs->size, "%d");
    /* Le cœur du correctif : rien à acquitter côté serveur pour ce lot. */
    ASSERT_EQ_FMT(0ULL, file_analysed_size(0), "%llu");

    free_array_possibility_packet(client[0].aposs);
    set_server_ip(NULL);
    destroy_test_client(&client[0]);
    request = saved_req;
    pruner_mode = saved_pm;
    PASS();
}

/* Thread sans travail mais stock vide : il a réclamé mais rien reçu. */
TEST feed_one_thread_no_work_available(void)
{
    int saved_req = request, saved_pm = pruner_mode;
    dm_drain_local();
    request = REQUEST_CONTINUE;
    pruner_mode = 0;

    client_possibility_t client[1];
    init_test_client(&client[0], 0);

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(1, needed, "%d");
    ASSERT_EQ_FMT(0, got, "%d");
    ASSERT_EQ_FMT(0, (int)client[0].works, "%d");

    destroy_test_client(&client[0]);
    request = saved_req;
    pruner_mode = saved_pm;
    PASS();
}

/* Thread occupé sans socket ouvert : ni demande ni keepalive. */
TEST feed_one_thread_busy_no_socket_noop(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;

    client_possibility_t client[1];
    init_test_client(&client[0], 1);   /* works = 1 : occupé */

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(0, needed, "%d");
    ASSERT_EQ_FMT(0, got, "%d");

    destroy_test_client(&client[0]);
    request = saved_req;
    PASS();
}

/* Thread occupé (works=1) AVEC un socket ouvert : au-delà de l'intervalle
 * d'inactivité, feed_one_thread sonde la faim du serveur (poll_server_hunger,
 * INST_NEED_WORK — la sonde tient lieu de keepalive). Socket vivant ->
 * l'horodatage d'activité est rafraîchi, le socket conservé, et la faim reçue
 * publiée dans `server_hunger`. On simule le serveur via un socketpair : sv[1]
 * pré-charge la réponse int32 que poll_server_hunger(sv[0]) relira. */
TEST feed_one_thread_keepalive_refreshes_alive_socket(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;
    __atomic_store_n(&server_hunger, 0, __ATOMIC_RELAXED);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair");

    client_possibility_t client[1];
    init_test_client(&client[0], 1);        /* works = 1 : occupé */
    client[0].socket_id = sv[0];
    client[0].last_socket_activity = 0;     /* intervalle de sonde forcément écoulé */

    /* Réponse du serveur pré-chargée : faim = 5. */
    int32_t hunger = 5;
    if (send_all(sv[1], &hunger, sizeof(hunger)) != (long)sizeof(hunger)) FAILm("send_all");

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(0, needed, "%d");                  /* pas de demande de travail    */
    ASSERT_EQ_FMT(0, got, "%d");
    ASSERT_EQ_FMT(sv[0], client[0].socket_id, "%d"); /* socket conservé (vivant)     */
    ASSERT(client[0].last_socket_activity != 0);     /* activité rafraîchie à `now`  */
    /* Faim publiée pour la délégation anticipée des threads de recherche. */
    ASSERT_EQ_FMT(5, __atomic_load_n(&server_hunger, __ATOMIC_RELAXED), "%d");
    /* Le pair a bien reçu l'instruction de sonde. */
    ASSERT_EQ_FMT((int)INST_NEED_WORK, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]);
    close(sv[1]);
    destroy_test_client(&client[0]);
    __atomic_store_n(&server_hunger, 0, __ATOMIC_RELAXED);
    request = saved_req;
    PASS();
}

/* Même situation, mais le socket est mort (pair fermé) : la sonde échoue,
 * feed_one_thread oublie le socket (socket_id = -1) — il sera rouvert au
 * prochain besoin de travail — et remet la faim à zéro (info morte).
 * poll_server_hunger ferme sv[0] lui-même. */
TEST feed_one_thread_keepalive_drops_dead_socket(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;
    signal(SIGPIPE, SIG_IGN); /* send sur pair fermé -> EPIPE, pas de signal fatal */
    __atomic_store_n(&server_hunger, 9, __ATOMIC_RELAXED); /* faim périmée à effacer */

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair");
    close(sv[1]);             /* pair fermé -> la sonde échoue et ferme sv[0] */

    client_possibility_t client[1];
    init_test_client(&client[0], 1);
    client[0].socket_id = sv[0];
    client[0].last_socket_activity = 0;

    int needed = 0, got = 0;
    feed_one_thread(client, 0, &needed, &got);

    ASSERT_EQ_FMT(0, needed, "%d");
    ASSERT_EQ_FMT(0, got, "%d");
    ASSERT_EQ_FMT(-1, client[0].socket_id, "%d"); /* socket oublié */
    /* Faim remise à zéro : pas de délégation sur une connexion morte. */
    ASSERT_EQ_FMT(0, __atomic_load_n(&server_hunger, __ATOMIC_RELAXED), "%d");

    /* sv[0] fermé par poll_server_hunger, sv[1] déjà fermé : rien à fermer ici. */
    destroy_test_client(&client[0]);
    request = saved_req;
    PASS();
}

/* ---------- init_client_possibility --------------------------------------- */

TEST init_client_possibility_sets_fields(void)
{
    client_possibility_t p;
    memset(&p, 0xAA, sizeof p); /* bourrage non nul pour détecter les oublis */

    struct array_part rp; memset(&rp, 0, sizeof rp);
    map_big_array map; memset(&map, 0, sizeof map);

    init_client_possibility(&p, &rp, &map, 3, 7, 4242, 5);

    ASSERT_EQ_FMT(0, p.works, "%d");
    ASSERT_EQ(NULL, p.aposs);
    ASSERT_EQ(&rp, p.all_rotate_part);
    ASSERT_EQ(&map, p.map_part);
    ASSERT_EQ(NULL, p.tid);
    ASSERT_EQ_FMT(3, p.id, "%d");
    ASSERT_EQ_FMT(5, p.fork_seq, "%d");
    ASSERT_EQ_FMT(4242, (int)p.pid, "%d");
    ASSERT_EQ_FMT(7, p.compteur, "%d");
    ASSERT_EQ_FMT(-1, p.max_shots_per_second, "%d");
    ASSERT_EQ_FMT(-1, p.socket_id, "%d");
    ASSERT_EQ(NULL, p.delegate_buf);
    ASSERT_EQ_FMT(0, p.delegate_buf_capacity, "%d");

    pthread_mutex_destroy(&p.works_mutex);
    pthread_mutex_destroy(&p.socket_mutex);
    PASS();
}

/* ---------- control_thread / build_control_thread ------------------------- */

/* NB_THREADS <= 0 : retour immédiat, sans même allouer lastCheck/oneSecond. */
TEST control_thread_nb_threads_zero_returns_null(void)
{
    int saved_nb = NB_THREADS;
    NB_THREADS = 0;
    void *ret = control_thread(NULL);
    ASSERT_EQ(NULL, ret);
    NB_THREADS = saved_nb;
    PASS();
}

/* REQUEST_STOP dès l'entrée : la boucle ne s'exécute jamais, mais l'allocation/
 * libération de lastCheck/oneSecond est bien exercée (appel direct, sûr : pas
 * d'usleep atteint). */
TEST control_thread_stop_returns_null_without_looping(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    request = REQUEST_STOP;
    NB_THREADS = 1;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    void *ret = control_thread(&cp);
    ASSERT_EQ(NULL, ret);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    PASS();
}

/* build_control_thread : création réelle du thread pthread, joint après un
 * retour immédiat (REQUEST_STOP déjà positionné). */
TEST build_control_thread_runs_and_joins(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    request = REQUEST_STOP;
    NB_THREADS = 1;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    pthread_t tid = build_control_thread(&cp);
    pthread_join(tid, NULL);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    PASS();
}

/* ---------- feed_thread_aposs / build_feed_thread -------------------------- */

/* REQUEST_STOP dès l'entrée : retour immédiat, aucune itération. */
TEST feed_thread_aposs_stop_returns_null_without_looping(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    request = REQUEST_STOP;
    NB_THREADS = 1;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    void *ret = feed_thread_aposs(&cp);
    ASSERT_EQ(NULL, ret);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    PASS();
}

/* Pool local vide + REQUEST_CONTINUE : le thread réel réclame du travail, n'en
 * reçoit pas, et exerce la pause adaptative (next_no_work_sleep) découpée en
 * tranches — jusqu'à ce que le test positionne REQUEST_STOP, qui l'interrompt
 * sans attendre la totalité du back-off. */
TEST feed_thread_aposs_backs_off_when_no_work_available(void)
{
    dm_drain_local();
    int saved_req = request;
    int saved_nb = NB_THREADS;
    request = REQUEST_CONTINUE;
    NB_THREADS = 1;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    pthread_t tid = build_feed_thread(&cp);
    usleep(20000); /* laisse le thread réclamer du travail et entrer en back-off */
    request = REQUEST_STOP;
    pthread_join(tid, NULL);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    dm_drain_local();
    PASS();
}

/* ---------- check_client_threads_step -------------------------------------- */

/* Lit lastcheck sous son mutex documenté (contrat de app_static_variables.h), comme
 * le fait check_interpreter côté production. */
static char *read_lastcheck_copy(void)
{
    pthread_mutex_lock(&lastcheck_mutex);
    char *copy = lastcheck != NULL ? strdup(lastcheck) : NULL;
    pthread_mutex_unlock(&lastcheck_mutex);
    return copy;
}

/* Rapport minimal : ni nouveau record, ni forward-check, ni pruner (tous les
 * compteurs à 0) — seules les lignes de base (files, actif/s, limites) sont
 * exercées. */
TEST check_client_threads_step_basic_report(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved_fs = fork_statistics;
    unsigned long long saved_msbs = max_search_by_sec;

    struct client_statistics fs[1];
    memset(fs, 0, sizeof fs);
    fs[0].pruner_cells_per_second = 7; /* débit prunage remonté par le fork */
    NB_THREADS = 1;
    fork_statistics = fs;
    max_result = 10;
    max_search_by_sec = 0;

    int last_record = (int)max_result;
    check_client_threads_step(&last_record);

    char *report = read_lastcheck_copy();
    ASSERT(report != NULL);
    ASSERT(strstr(report, "Thread queues") != NULL);
    ASSERT(strstr(report, "active thread/s") != NULL);
    /* Indice cumulé = coups (0 ici) + prunage (7), part prunage isolée */
    ASSERT(strstr(report, "études/s (recherche+prunage) :7") != NULL);
    ASSERT(strstr(report, "dont prunage/s :7") != NULL);
    ASSERT_EQ_FMT(10, last_record, "%d"); /* pas de nouveau record */
    free(report);

    fork_statistics = saved_fs;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    max_search_by_sec = saved_msbs;
    PASS();
}

/* max_result > *last_record : détection de record (log_event -> events.log,
 * nettoyé après coup comme dans test_logger.c). */
TEST check_client_threads_step_detects_new_record(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved_fs = fork_statistics;

    struct client_statistics fs[1];
    memset(fs, 0, sizeof fs);
    NB_THREADS = 1;
    fork_statistics = fs;
    max_result = 42;

    unlink("events.log");
    int last_record = 10;
    check_client_threads_step(&last_record);
    ASSERT_EQ_FMT(42, last_record, "%d");
    unlink("events.log");

    fork_statistics = saved_fs;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    PASS();
}

#if FORWARD_CHECK_K > 0
/* fca > 0 : la section forward-check est ajoutée au rapport ; prc+prr > 0 :
 * la section pruner aussi. */
TEST check_client_threads_step_reports_forward_check_and_pruner(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved_fs = fork_statistics;
    unsigned long long saved_fca = fc_attempts;
    unsigned long long saved_fcp = fc_pruned;
    unsigned long long saved_prc = pruner_checked;
    unsigned long long saved_prr = pruner_removed;

    struct client_statistics fs[1];
    memset(fs, 0, sizeof fs);
    NB_THREADS = 1;
    fork_statistics = fs;
    max_result = 5;
    fc_attempts = 100;
    fc_pruned = 10;
    pruner_checked = 3;
    pruner_removed = 2;

    int last_record = (int)max_result;
    check_client_threads_step(&last_record);

    char *report = read_lastcheck_copy();
    ASSERT(report != NULL);
    ASSERT(strstr(report, "forward-check") != NULL);
    ASSERT(strstr(report, "pruner :") != NULL);
    free(report);

    fork_statistics = saved_fs;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    fc_attempts = saved_fca;
    fc_pruned = saved_fcp;
    pruner_checked = saved_prc;
    pruner_removed = saved_prr;
    PASS();
}
#endif /* FORWARD_CHECK_K > 0 */

/* Bandeau log_status avec limite active : max_search_by_sec > 0 emprunte la
 * branche qui formate la limite en nombre (l'autre branche « - » est déjà
 * couverte par check_client_threads_step_basic_report). */
TEST check_client_threads_step_shows_numeric_limit(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved_fs = fork_statistics;
    unsigned long long saved_msbs = max_search_by_sec;

    struct client_statistics fs[1];
    memset(fs, 0, sizeof fs);
    NB_THREADS = 1;
    fork_statistics = fs;
    max_result = 10;
    max_search_by_sec = 500;

    int last_record = (int)max_result;
    check_client_threads_step(&last_record);
    ASSERT_EQ_FMT(10, last_record, "%d");

    fork_statistics = saved_fs;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    max_search_by_sec = saved_msbs;
    PASS();
}

/* ---------- control_thread / feed_thread_aposs : un tour réel de boucle ---- */

static void *ec_stop_after_delay(void *arg)
{
    (void)arg;
    usleep(20000);
    request = REQUEST_STOP;
    return NULL;
}

/* REQUEST_CONTINUE puis STOP posé par un thread auxiliaire : le corps du while
 * (control_step + usleep) est pris au moins une fois — complète le test
 * « arrêt immédiat » qui ne l'exerçait jamais. max_search_by_sec = 0 : le tour
 * se réduit à la fenêtre nbCheck, sans lire counters. */
TEST control_thread_loops_once_then_stops(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    unsigned long long saved_msbs = max_search_by_sec;
    NB_THREADS = 1;
    max_search_by_sec = 0;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    request = REQUEST_CONTINUE;
    pthread_t stopper;
    pthread_create(&stopper, NULL, ec_stop_after_delay, NULL);
    void *ret = control_thread(&cp);
    pthread_join(stopper, NULL);

    ASSERT_EQ(NULL, ret);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    max_search_by_sec = saved_msbs;
    PASS();
}

/* REQUEST_ADMIN_PAUSE dès l'entrée : control_thread doit continuer à tourner
 * (et donc réappliquer `limit` dès le resume) au lieu de sortir immédiatement.
 * Avant le correctif, la condition de boucle `request == REQUEST_CONTINUE ||
 * request == REQUEST_PAUSE` ne couvrait pas REQUEST_ADMIN_PAUSE : une pause
 * distante (canal de contrôle serveur -> client) terminait le thread pour de
 * bon, et le resume qui suivait ne relançait rien — la limite de débit restait
 * durablement sans effet. On vérifie ici que le corps de boucle est bien
 * exécuté (via le thread auxiliaire qui pose REQUEST_STOP après un délai),
 * ce qui n'aurait jamais lieu avec l'ancienne condition. */
TEST control_thread_survives_admin_pause(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    unsigned long long saved_msbs = max_search_by_sec;
    NB_THREADS = 1;
    max_search_by_sec = 0;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    request = REQUEST_ADMIN_PAUSE;
    pthread_t stopper;
    pthread_create(&stopper, NULL, ec_stop_after_delay, NULL);
    void *ret = control_thread(&cp);
    pthread_join(stopper, NULL);

    ASSERT_EQ(NULL, ret);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    max_search_by_sec = saved_msbs;
    PASS();
}

/* En pause admin, control_thread doit ralentir sa cadence de tick (500 ms,
 * ADMIN_PAUSE_POLL_SLEEP_US) au lieu des 1 ms habituels : rien à réguler
 * pendant une pause administrative (aucune recherche en cours), donc pas
 * besoin de la précision de control_step. On le vérifie en mesurant le temps
 * écoulé avant que le thread auxiliaire (délai de 20 ms) ne pose REQUEST_STOP
 * et que la boucle s'arrête : avec la cadence rapide (1 ms) ce serait ~20 ms
 * (cf. control_thread_loops_once_then_stops), avec la cadence lente c'est le
 * temps d'un seul tick usleep (jusqu'à 500 ms) qui domine. */
TEST control_thread_admin_pause_uses_slow_cadence(void)
{
    int saved_req = request;
    int saved_nb = NB_THREADS;
    unsigned long long saved_msbs = max_search_by_sec;
    NB_THREADS = 1;
    max_search_by_sec = 0;

    client_possibility_t cp;
    init_test_client(&cp, 0);

    request = REQUEST_ADMIN_PAUSE;
    pthread_t stopper;
    pthread_create(&stopper, NULL, ec_stop_after_delay, NULL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    void *ret = control_thread(&cp);
    clock_gettime(CLOCK_MONOTONIC, &end);
    pthread_join(stopper, NULL);

    ASSERT_EQ(NULL, ret);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000
                     + (end.tv_nsec - start.tv_nsec) / 1000000;
    /* Bien au-delà des ~20 ms de la cadence rapide : preuve que le tick lent a
       été utilisé (borne haute large pour tolérer un CI chargé). */
    ASSERT(elapsed_ms >= 100);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    max_search_by_sec = saved_msbs;
    PASS();
}

/* Thread occupé (works=1, pas de socket) : personne ne réclame de travail, la
 * boucle prend la branche « cadence normale » (remise à zéro du back-off) au
 * lieu de la pause adaptative. */
TEST feed_thread_aposs_normal_cadence_when_threads_busy(void)
{
    dm_drain_local();
    int saved_req = request;
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;

    client_possibility_t cp;
    init_test_client(&cp, 1);      /* works=1 : n'a pas besoin de travail */

    request = REQUEST_CONTINUE;
    pthread_t stopper;
    pthread_create(&stopper, NULL, ec_stop_after_delay, NULL);
    void *ret = feed_thread_aposs(&cp);
    pthread_join(stopper, NULL);

    ASSERT_EQ(NULL, ret);

    destroy_test_client(&cp);
    request = saved_req;
    NB_THREADS = saved_nb;
    dm_drain_local();
    PASS();
}

/* Enveloppe de thread check_client_threads : REQUEST_STOP prépositionné, la
 * boucle (while(request != REQUEST_STOP), refactor P8) ne s'exécute jamais —
 * appel direct sûr, le corps est couvert via check_client_threads_step. */
TEST check_client_threads_stops_immediately_on_request_stop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    void *ret = check_client_threads(NULL);
    ASSERT_EQ(NULL, ret);

    request = saved_req;
    PASS();
}

/* ---------- run_mono_client (smoke, mode local) ---------------------------- */
/*
 * REQUEST_STOP prépositionné : autosearch/autoprune reviennent au premier tour,
 * les threads d'alimentation et de contrôle se terminent immédiatement, les
 * joins aboutissent. Exerce toute l'orchestration (read_parts sur le fichier de
 * pièces par défaut du build courant, construction map/rotations, joins, bloc
 * de fermeture socket). La map construite localement EST libérée en fin de
 * run_mono_client (cf. acquire_search_parts) ; le contexte client lui-même ne
 * l'est pas (le process de production sort juste après) : toléré ici, comme en
 * CI ASan (detect_leaks=0).
 */

/* counters/lastfilesize sont lus/écrits par la pile de recherche : on les
 * alloue une fois pour toutes, assez grands pour tous les NB_THREADS des tests. */
static void ensure_counters(void)
{
    if (counters == NULL)     counters     = calloc(64, sizeof(*counters));
    if (lastfilesize == NULL) lastfilesize = calloc(64, sizeof(*lastfilesize));
}

TEST run_mono_client_search_stops_immediately(void)
{
    dm_drain_local();
    ensure_counters();
    int saved_req = request;
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    request = REQUEST_STOP;

    run_mono_client(parts_files, 0);

    request = saved_req;
    NB_THREADS = saved_nb;
    dm_drain_local();
    PASS();
}

TEST run_mono_client_pruner_stops_immediately(void)
{
    dm_drain_local();
    ensure_counters();
    int saved_req = request;
    int saved_nb = NB_THREADS;
    int saved_pruner = pruner_mode;
    NB_THREADS = 1;
    request = REQUEST_STOP;
    pruner_mode = 1;

    run_mono_client(parts_files, 0);

    pruner_mode = saved_pruner;
    request = saved_req;
    NB_THREADS = saved_nb;
    dm_drain_local();
    PASS();
}

/* ---------- pièces de recherche partagées (map héritée du parent) ---------- */
/*
 * La map de lookup est construite UNE fois par le process parent avant sa
 * boucle de fork() : les process de recherche l'héritent en copy-on-write au
 * lieu d'en construire chacun une copie privée. Le point délicat est la
 * PROPRIÉTÉ — un fork qui libérerait la map de son parent la retirerait à ses
 * frères — d'où ces tests sur le contrat d'acquire_search_parts.
 */

/* Le fichier de pièces du build courant est-il lisible depuis le CWD ? */
static int parts_file_available(void)
{
    FILE *f = fopen(parts_files, "r");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

TEST acquire_search_parts_builds_and_owns_when_nothing_published(void)
{
    if (!parts_file_available()) {
        SKIPm("fichier de pièces absent du répertoire courant");
    }
    set_inherited_search_parts(NULL);

    search_parts_t parts;
    int owned = acquire_search_parts(&parts, parts_files);

    ASSERT_EQ(1, owned); /* construites ici -> à libérer ici */
    ASSERT(parts.map != NULL);
    ASSERT(parts.rotate_parts != NULL);
    /* rotate_all_parts : 4 rotations par pièce + le bouchon d'indice 0. */
    ASSERT_EQ(ETERN_PARTS * 4 + 1, parts.rotate_parts->size);

    free_search_parts(&parts);
    ASSERT_EQ(NULL, parts.map);
    ASSERT_EQ(NULL, parts.rotate_parts);
    PASS();
}

TEST acquire_search_parts_reuses_published_parts(void)
{
    /* Pointeurs factices : acquire_search_parts ne doit RIEN déréférencer,
       seulement transmettre. */
    search_parts_t published;
    published.rotate_parts = (struct array_part *)0x1;
    published.map = (map_big_array *)0x2;
    set_inherited_search_parts(&published);

    search_parts_t parts;
    int owned = acquire_search_parts(&parts, "/inexistant.csv");

    ASSERT_EQ(0, owned); /* hérité -> l'appelant ne libère pas */
    ASSERT_EQ(published.rotate_parts, parts.rotate_parts);
    ASSERT_EQ(published.map, parts.map);

    set_inherited_search_parts(NULL);
    PASS();
}

TEST acquire_search_parts_builds_again_after_publication_cleared(void)
{
    if (!parts_file_available()) {
        SKIPm("fichier de pièces absent du répertoire courant");
    }
    search_parts_t published;
    published.rotate_parts = (struct array_part *)0x1;
    published.map = (map_big_array *)0x2;
    set_inherited_search_parts(&published);
    set_inherited_search_parts(NULL);

    search_parts_t parts;
    ASSERT_EQ(1, acquire_search_parts(&parts, parts_files));
    ASSERT(parts.map != NULL);
    free_search_parts(&parts);
    PASS();
}

TEST free_search_parts_is_null_safe_and_idempotent(void)
{
    free_search_parts(NULL); /* ne doit pas planter */

    search_parts_t parts;
    parts.rotate_parts = NULL;
    parts.map = NULL;
    free_search_parts(&parts);
    free_search_parts(&parts); /* pas de double libération */
    ASSERT_EQ(NULL, parts.map);
    PASS();
}

/*
 * Régression : un process de recherche ne doit JAMAIS libérer la map publiée
 * par son parent (elle survit au fork et est partagée par tous ses frères).
 * On publie une vraie map, on exécute run_mono_client, puis on relit la map :
 * sous ASan, une libération fautive se manifeste ici en use-after-free.
 */
TEST run_mono_client_does_not_free_published_parts(void)
{
    if (!parts_file_available()) {
        SKIPm("fichier de pièces absent du répertoire courant");
    }
    dm_drain_local();
    ensure_counters();
    int saved_req = request;
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    request = REQUEST_STOP;

    search_parts_t published;
    build_search_parts(&published, parts_files);
    int expected_size = published.rotate_parts->size;
    int expected_sizearray = published.map->sizearray;
    set_inherited_search_parts(&published);

    run_mono_client(parts_files, 0);

    /* Toujours intactes et exploitables après le passage du « fork ». */
    ASSERT_EQ(expected_size, published.rotate_parts->size);
    ASSERT_EQ(expected_sizearray, published.map->sizearray);
    int8_t key[4] = {0, 0, 0, 0};
    struct array_part *bucket = get_parts_bigarray(published.map, key);
    ASSERT(bucket != NULL);

    set_inherited_search_parts(NULL);
    free_search_parts(&published);
    request = saved_req;
    NB_THREADS = saved_nb;
    dm_drain_local();
    PASS();
}

/* ---------- suite --------------------------------------------------------- */

SUITE(etii_client_suite)
{
    RUN_TEST(sleep_zero_returns_start);
    RUN_TEST(sleep_doubles_below_max);
    RUN_TEST(sleep_caps_at_max);
    RUN_TEST(sleep_progression_reaches_max);

    RUN_TEST(count_all_negative_returns_zero);
    RUN_TEST(count_all_positive);
    RUN_TEST(count_mixed);
    RUN_TEST(count_zero_is_not_counted);
    RUN_TEST(count_empty_array);

    RUN_TEST(find_returns_minus_one_when_empty);
    RUN_TEST(find_returns_correct_index);
    RUN_TEST(find_returns_first_match);
    RUN_TEST(find_no_match_returns_minus_one);

    RUN_TEST(thread_queues_table_aggregates_forks);
    RUN_TEST(thread_queues_table_shows_per_fork_role);
    RUN_TEST(thread_queues_table_large_nb_threads_no_overflow);

    RUN_TEST(control_step_unlimited_leaves_request);
    RUN_TEST(control_step_high_rate_pauses);
    RUN_TEST(control_step_counter_wraparound);
    RUN_TEST(control_step_low_rate_resumes);
    RUN_TEST(control_step_idle_thread_resumes);
    RUN_TEST(control_step_window_resets_after_1000);
    RUN_TEST(control_step_does_not_touch_admin_pause);

    RUN_TEST(feed_one_thread_not_continue_is_noop);
    RUN_TEST(feed_one_thread_admin_pause_keeps_socket_alive);
    RUN_TEST(feed_one_thread_gets_work);
    RUN_TEST(feed_one_thread_local_recycle_skips_analysed_tracking);
    RUN_TEST(feed_one_thread_no_work_available);
    RUN_TEST(feed_one_thread_busy_no_socket_noop);
    RUN_TEST(feed_one_thread_keepalive_refreshes_alive_socket);
    RUN_TEST(feed_one_thread_keepalive_drops_dead_socket);

    RUN_TEST(init_client_possibility_sets_fields);

    RUN_TEST(control_thread_nb_threads_zero_returns_null);
    RUN_TEST(control_thread_stop_returns_null_without_looping);
    RUN_TEST(build_control_thread_runs_and_joins);
    RUN_TEST(control_thread_loops_once_then_stops);
    RUN_TEST(control_thread_survives_admin_pause);
    RUN_TEST(control_thread_admin_pause_uses_slow_cadence);

    RUN_TEST(feed_thread_aposs_stop_returns_null_without_looping);
    RUN_TEST(feed_thread_aposs_backs_off_when_no_work_available);
    RUN_TEST(feed_thread_aposs_normal_cadence_when_threads_busy);

    RUN_TEST(check_client_threads_step_basic_report);
    RUN_TEST(check_client_threads_step_detects_new_record);
#if FORWARD_CHECK_K > 0
    RUN_TEST(check_client_threads_step_reports_forward_check_and_pruner);
#endif
    RUN_TEST(check_client_threads_step_shows_numeric_limit);
    RUN_TEST(check_client_threads_stops_immediately_on_request_stop);

    RUN_TEST(acquire_search_parts_builds_and_owns_when_nothing_published);
    RUN_TEST(acquire_search_parts_reuses_published_parts);
    RUN_TEST(acquire_search_parts_builds_again_after_publication_cleared);
    RUN_TEST(free_search_parts_is_null_safe_and_idempotent);

    RUN_TEST(run_mono_client_search_stops_immediately);
    RUN_TEST(run_mono_client_pruner_stops_immediately);
    RUN_TEST(run_mono_client_does_not_free_published_parts);
}
