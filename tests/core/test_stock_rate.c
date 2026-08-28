/*
 * Tests unitaires du module stock_rate.c (débit d'ajouts/consommations du
 * stock, moyenné sur 1min/1h/1jour — cf. AGENTS.md).
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

TEST record_then_windows_reports_events_within_the_minute(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    stock_rate_record(&c, 60, BASE_NOW);

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);

    /* 60 événements concentrés dans UNE seconde => 1 événement/s en moyenne
     * sur la fenêtre minute (60 événements / 60s). Les fenêtres heure/jour
     * moyennent le même total sur une durée bien plus longue (3600s/86400s),
     * donc un débit bien plus faible — ce n'est pas la même quantité que "m",
     * juste le même total réparti sur un dénominateur différent. */
    ASSERT_IN_RANGE(1.0, m, 0.001);
    ASSERT_IN_RANGE(60.0 / 3600.0, h, 0.001);
    ASSERT_IN_RANGE(60.0 / 86400.0, d, 0.0001);
    PASS();
}

TEST record_with_zero_count_is_noop(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    stock_rate_record(&c, 0, BASE_NOW);

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, m, 0.001);
    ASSERT_IN_RANGE(0.0, h, 0.001);
    ASSERT_IN_RANGE(0.0, d, 0.001);
    PASS();
}

TEST event_outside_the_minute_window_is_excluded_from_1m(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Un événement il y a 120s : hors de la fenêtre 1 minute (60s). */
    stock_rate_record(&c, 60, BASE_NOW - 120);

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, m, 0.001);
    PASS();
}

/* --------------------------------------------------------------------------
 * Fenêtre 1 heure
 * ------------------------------------------------------------------------ */

TEST event_within_the_hour_but_outside_the_minute_counts_only_in_1h_and_1d(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Il y a 30 minutes : dans la fenêtre 1h (3600s) et 1j, hors de la
     * fenêtre 1min. */
    stock_rate_record(&c, 3600, BASE_NOW - 1800);

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, m, 0.001);
    /* 3600 événements / 3600s de fenêtre heure => 1 événement/s. */
    ASSERT_IN_RANGE(1.0, h, 0.001);
    /* Même total (3600) mais dénominateur jour (86400s) => débit plus faible. */
    ASSERT_IN_RANGE(3600.0 / 86400.0, d, 0.001);
    PASS();
}

TEST event_outside_the_hour_window_is_excluded_from_1h(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    /* Il y a 2 heures : hors de la fenêtre 1h, toujours dans la fenêtre 1j. */
    stock_rate_record(&c, 3600, BASE_NOW - 7200);

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, h, 0.001);
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

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, m, 0.001);
    ASSERT_IN_RANGE(0.0, h, 0.001);
    ASSERT_IN_RANGE(0.0, d, 0.001);
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
    double m, h, d;
    stock_rate_windows(&c, later, &m, &h, &d);
    /* Le vieil événement est hors de la fenêtre 1min à `later` : le bucket
     * recyclé ne doit rapporter aucun événement pour cette seconde. */
    ASSERT_IN_RANGE(0.0, m, 0.001);
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
    double m, h, d;
    stock_rate_windows(&c, later, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, h, 0.001);
    ASSERT_IN_RANGE(0.0, d, 0.001);
    PASS();
}

/* --------------------------------------------------------------------------
 * Accumulation de plusieurs appels dans le même bucket.
 * ------------------------------------------------------------------------ */

TEST multiple_records_in_the_same_second_accumulate(void)
{
    stock_rate_counter_t c;
    stock_rate_reset_for_tests(&c);

    stock_rate_record(&c, 10, BASE_NOW);
    stock_rate_record(&c, 20, BASE_NOW);
    stock_rate_record(&c, 30, BASE_NOW);

    double m, h, d;
    stock_rate_windows(&c, BASE_NOW, &m, &h, &d);
    /* 60 événements / 60s de fenêtre => 1 événement/s. */
    ASSERT_IN_RANGE(1.0, m, 0.001);
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
    double m;
    stock_rate_windows(&c, BASE_NOW, &m, NULL, NULL);
    ASSERT_IN_RANGE(5.0 / 60.0, m, 0.001);
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
    double m = -1.0, h = -1.0, d = -1.0;
    stock_rate_windows(NULL, BASE_NOW, &m, &h, &d);
    ASSERT_IN_RANGE(0.0, m, 0.001);
    ASSERT_IN_RANGE(0.0, h, 0.001);
    ASSERT_IN_RANGE(0.0, d, 0.001);
    PASS();
}

SUITE(stock_rate_suite)
{
    RUN_TEST(record_then_windows_reports_events_within_the_minute);
    RUN_TEST(record_with_zero_count_is_noop);
    RUN_TEST(event_outside_the_minute_window_is_excluded_from_1m);
    RUN_TEST(event_within_the_hour_but_outside_the_minute_counts_only_in_1h_and_1d);
    RUN_TEST(event_outside_the_hour_window_is_excluded_from_1h);
    RUN_TEST(event_outside_the_day_window_is_excluded_from_1d);
    RUN_TEST(bucket_rollover_does_not_leak_stale_seconds);
    RUN_TEST(bucket_rollover_does_not_leak_stale_minutes);
    RUN_TEST(multiple_records_in_the_same_second_accumulate);
    RUN_TEST(windows_tolerates_null_output_pointers);
    RUN_TEST(record_tolerates_null_counter);
    RUN_TEST(windows_tolerates_null_counter);
}
