/*
 * Tests unitaires du module stock_rate.c (cumul d'ajouts/consommations du
 * stock sur 1min/1h/1jour — cf. AGENTS.md).
 *
 * stock_rate.c est totalement autonome (aucune dépendance autre que <time.h>
 * et <stdint.h>) : chaque test construit son propre `stock_rate_counter_t`
 * sur la pile, sans dépendre de l'état global de datamanager.c. `now` est
 * toujours une valeur synthétique choisie par le test, jamais `time(NULL)` :
 * c'est ce qui rend les fenêtres heure/jour vérifiables sans attendre ni
 * mocker l'horloge système (cf. stock_rate.h).
 */
#include "greatest.h"
#include "core/stock_rate.h"

/* Une base arbitraire, loin de 0 et alignée sur une frontière de minute,
 * pour que les calculs de fenêtre (now/60, now%60...) restent lisibles dans
 * les tests. */
#define BASE_NOW ((time_t)1000000000)

/* --------------------------------------------------------------------------
 * stock_rate_record / stock_rate_windows — fenêtre 1 minute
 * ------------------------------------------------------------------------ */

TEST record_then_windows_reports_the_cumulative_count(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    stock_rate_record(&c, 60, BASE_NOW);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);

    /* Un seul événement de 60 : le même total cumulé se retrouve dans les
     * trois fenêtres, puisqu'il est plus récent que chacune d'elles. */
    ASSERT_EQ_FMT(60ULL, m, "%llu");
    ASSERT_EQ_FMT(60ULL, h, "%llu");
    ASSERT_EQ_FMT(60ULL, d, "%llu");
    PASS();
}

TEST record_with_zero_count_is_noop(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    stock_rate_record(&c, 0, BASE_NOW);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, m, "%llu");
    ASSERT_EQ_FMT(0ULL, h, "%llu");
    ASSERT_EQ_FMT(0ULL, d, "%llu");
    PASS();
}

TEST event_outside_the_minute_window_is_excluded_from_1m(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Un événement il y a 120s : hors de la fenêtre 1 minute (60s), mais
     * toujours dans les fenêtres 1h/1j. */
    stock_rate_record(&c, 60, BASE_NOW - 120);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, m, "%llu");
    ASSERT_EQ_FMT(60ULL, h, "%llu");
    ASSERT_EQ_FMT(60ULL, d, "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * Fenêtre 1 heure
 * ------------------------------------------------------------------------ */

TEST event_within_the_hour_but_outside_the_minute_counts_only_in_1h_and_1d(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Il y a 30 minutes : dans la fenêtre 1h et 1j, hors de la fenêtre 1min. */
    stock_rate_record(&c, 3600, BASE_NOW - 1800);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, m, "%llu");
    ASSERT_EQ_FMT(3600ULL, h, "%llu");
    ASSERT_EQ_FMT(3600ULL, d, "%llu");
    PASS();
}

TEST event_outside_the_hour_window_is_excluded_from_1h(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Il y a 2 heures : hors de la fenêtre 1h, toujours dans la fenêtre 1j. */
    stock_rate_record(&c, 3600, BASE_NOW - 7200);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, h, "%llu");
    ASSERT_EQ_FMT(3600ULL, d, "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * Fenêtre 1 jour
 * ------------------------------------------------------------------------ */

TEST event_outside_the_day_window_is_excluded_from_1d(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Il y a 2 jours : hors des trois fenêtres. */
    stock_rate_record(&c, 86400, BASE_NOW - 2 * 86400);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, m, "%llu");
    ASSERT_EQ_FMT(0ULL, h, "%llu");
    ASSERT_EQ_FMT(0ULL, d, "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * Rollover du ring buffer : un ancien bucket réutilisé (même index modulo,
 * époque différente) ne doit jamais fuiter dans une fenêtre qui ne le
 * couvre plus.
 * ------------------------------------------------------------------------ */

TEST bucket_rollover_does_not_leak_stale_seconds(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Un événement à BASE_NOW retombe dans le même bucket "seconde" (index
     * modulo STOCK_RATE_SEC_BUCKETS) qu'un événement STOCK_RATE_SEC_BUCKETS
     * secondes plus tard — mais avec une époque différente. */
    stock_rate_record(&c, 42, BASE_NOW);

    time_t later = BASE_NOW + STOCK_RATE_SEC_BUCKETS;
    unsigned long long m, h, d;
    stock_rate_windows(&c, later, &m, &h, &d);
    /* Le vieil événement est hors de la fenêtre 1min à `later` : le bucket
     * recyclé ne doit rapporter aucun événement pour cette seconde. */
    ASSERT_EQ_FMT(0ULL, m, "%llu");
    PASS();
}

TEST bucket_rollover_does_not_leak_stale_minutes(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Même principe sur la granularité minute : deux minutes espacées d'un
     * tour complet du ring buffer (STOCK_RATE_MIN_BUCKETS) partagent le même
     * index mais pas la même époque. */
    stock_rate_record(&c, 100, BASE_NOW);

    time_t later = BASE_NOW + (time_t)STOCK_RATE_MIN_BUCKETS * 60;
    unsigned long long m, h, d;
    stock_rate_windows(&c, later, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, h, "%llu");
    ASSERT_EQ_FMT(0ULL, d, "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * Accumulation de plusieurs appels dans le même bucket, et sur des buckets
 * différents à l'intérieur de la même fenêtre.
 * ------------------------------------------------------------------------ */

TEST multiple_records_in_the_same_second_accumulate(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    stock_rate_record(&c, 10, BASE_NOW);
    stock_rate_record(&c, 20, BASE_NOW);
    stock_rate_record(&c, 30, BASE_NOW);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(60ULL, m, "%llu");
    PASS();
}

TEST records_across_different_seconds_within_the_minute_sum_up(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Trois secondes distinctes, toutes dans la fenêtre 1min à BASE_NOW. */
    stock_rate_record(&c, 1, BASE_NOW - 2);
    stock_rate_record(&c, 2, BASE_NOW - 1);
    stock_rate_record(&c, 3, BASE_NOW);

    unsigned long long m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(6ULL, m, "%llu");
    ASSERT_EQ_FMT(6ULL, h, "%llu");
    ASSERT_EQ_FMT(6ULL, d, "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * Robustesse : paramètres NULL n'écrivent jamais hors de leur propre sortie
 * (pas de crash), stock_rate_record(NULL, ...) est un no-op.
 * ------------------------------------------------------------------------ */

TEST windows_tolerates_null_output_pointers(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);
    stock_rate_record(&c, 5, BASE_NOW);

    /* Ne doit pas planter même si seule une partie des sorties est demandée. */
    unsigned long long m;
    stock_rate_windows(&c, BASE_NOW, &m, NULL, NULL);
    ASSERT_EQ_FMT(5ULL, m, "%llu");
    PASS();
}

TEST record_tolerates_null_counter(void)
{
    /* Ne doit pas planter : no-op documenté. */
    stock_rate_record(NULL, 5, BASE_NOW);
    PASS();
}

TEST windows_tolerates_null_counter(void)
{
    unsigned long long m = 999, h = 999, d = 999;
    stock_rate_windows(NULL, BASE_NOW, &m, &h, &d);
    ASSERT_EQ_FMT(0ULL, m, "%llu");
    ASSERT_EQ_FMT(0ULL, h, "%llu");
    ASSERT_EQ_FMT(0ULL, d, "%llu");
    PASS();
}

SUITE(stock_rate_suite)
{
    RUN_TEST(record_then_windows_reports_the_cumulative_count);
    RUN_TEST(record_with_zero_count_is_noop);
    RUN_TEST(event_outside_the_minute_window_is_excluded_from_1m);
    RUN_TEST(event_within_the_hour_but_outside_the_minute_counts_only_in_1h_and_1d);
    RUN_TEST(event_outside_the_hour_window_is_excluded_from_1h);
    RUN_TEST(event_outside_the_day_window_is_excluded_from_1d);
    RUN_TEST(bucket_rollover_does_not_leak_stale_seconds);
    RUN_TEST(bucket_rollover_does_not_leak_stale_minutes);
    RUN_TEST(multiple_records_in_the_same_second_accumulate);
    RUN_TEST(records_across_different_seconds_within_the_minute_sum_up);
    RUN_TEST(windows_tolerates_null_output_pointers);
    RUN_TEST(record_tolerates_null_counter);
    RUN_TEST(windows_tolerates_null_counter);
}
