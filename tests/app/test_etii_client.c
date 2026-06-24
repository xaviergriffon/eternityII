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
#include "app/static_variables.h"
#include "app/etii_statistic.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

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
    RUN_TEST(thread_queues_table_large_nb_threads_no_overflow);
}
