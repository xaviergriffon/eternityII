/*
 * Suite du cœur PUR du banc « côté trouver » (tests/bench/bench_solve_stats.c).
 *
 * Le banc lui-même (bench_solve.c) n'est pas rattaché à `make test` — c'est un
 * banc, il fork, il cherche pendant des minutes. Son agrégation, elle, est de
 * l'arithmétique sans E/S : c'est elle qui décide si une politique est adoptée
 * (§3.5 du document de conception), donc elle est testée comme n'importe quel
 * module. Même découpage que gen_root / root_from_board.c.
 */
#include "greatest.h"
#include "bench/bench_solve_stats.h"

#include <math.h>

#define CLOSE(a, b) (fabs((a) - (b)) < 1e-9)

/* --------------------------------------------------------------------------
 * bench_stats_sort / bench_stats_median_sorted
 * ------------------------------------------------------------------------ */

TEST sort_orders_ascending_and_is_stable_on_equal_values(void)
{
    double v[] = {5.0, 1.0, 3.0, 3.0, 2.0};
    bench_stats_sort(v, 5);
    ASSERT(CLOSE(1.0, v[0]));
    ASSERT(CLOSE(2.0, v[1]));
    ASSERT(CLOSE(3.0, v[2]));
    ASSERT(CLOSE(3.0, v[3]));
    ASSERT(CLOSE(5.0, v[4]));
    PASS();
}

TEST median_of_odd_sample_is_the_middle_value(void)
{
    double v[] = {1.0, 7.0, 100.0};
    ASSERT(CLOSE(7.0, bench_stats_median_sorted(v, 3)));
    PASS();
}

TEST median_of_even_sample_averages_the_two_middles(void)
{
    double v[] = {1.0, 7.0, 9.0, 100.0};
    ASSERT(CLOSE(8.0, bench_stats_median_sorted(v, 4)));
    PASS();
}

TEST median_of_empty_sample_is_zero(void)
{
    double v[] = {1.0};
    ASSERT(CLOSE(0.0, bench_stats_median_sorted(v, 0)));
    PASS();
}

/* La raison d'être de la médiane ici : une seule exécution malchanceuse ne doit
 * pas déplacer le résumé, là où la moyenne arithmétique en serait dominée.
 * Le test l'énonce comme une propriété, pas comme un chiffre. */
TEST median_is_immune_to_one_heavy_tail_run(void)
{
    double base[]  = {10.0, 20.0, 30.0, 40.0, 50.0};
    double heavy[] = {10.0, 20.0, 30.0, 40.0, 5000000.0};
    ASSERT(CLOSE(bench_stats_median_sorted(base, 5), bench_stats_median_sorted(heavy, 5)));

    double mean_base = 0.0, mean_heavy = 0.0;
    for (int i = 0; i < 5; i++) { mean_base += base[i]; mean_heavy += heavy[i]; }
    ASSERT(mean_heavy / 5.0 > 100.0 * (mean_base / 5.0));
    PASS();
}

/* --------------------------------------------------------------------------
 * bench_stats_geomean
 * ------------------------------------------------------------------------ */

TEST geomean_of_powers_of_ten_is_the_middle_power(void)
{
    double v[] = {1.0, 10.0, 100.0};
    ASSERT(fabs(10.0 - bench_stats_geomean(v, 3)) < 1e-9);
    PASS();
}

TEST geomean_ignores_non_positive_values(void)
{
    double v[] = {0.0, 4.0, -3.0, 9.0};
    /* Seuls 4 et 9 comptent : sqrt(36) = 6. */
    ASSERT(fabs(6.0 - bench_stats_geomean(v, 4)) < 1e-9);
    PASS();
}

TEST geomean_without_any_usable_value_is_zero(void)
{
    double v[] = {0.0, -1.0};
    ASSERT(CLOSE(0.0, bench_stats_geomean(v, 2)));
    ASSERT(CLOSE(0.0, bench_stats_geomean(v, 0)));
    PASS();
}

/* --------------------------------------------------------------------------
 * bench_stats_survival
 * ------------------------------------------------------------------------ */

TEST survival_counts_only_solved_runs_under_the_threshold(void)
{
    double nodes[]  = {100.0, 5000.0, 200.0, 90.0};
    int    solved[] = {1,     1,      0,     1};
    /* Sous 1000 nœuds : 100 et 90 (la 3e a 200 nœuds mais n'a PAS résolu). */
    ASSERT(CLOSE(0.5, bench_stats_survival(nodes, solved, 4, 1000.0)));
    ASSERT(CLOSE(0.75, bench_stats_survival(nodes, solved, 4, 10000.0)));
    ASSERT(CLOSE(0.0, bench_stats_survival(nodes, solved, 4, 50.0)));
    PASS();
}

TEST survival_threshold_is_inclusive(void)
{
    double nodes[]  = {1000.0};
    int    solved[] = {1};
    ASSERT(CLOSE(1.0, bench_stats_survival(nodes, solved, 1, 1000.0)));
    PASS();
}

TEST survival_of_empty_sample_is_zero(void)
{
    ASSERT(CLOSE(0.0, bench_stats_survival(NULL, NULL, 0, 1000.0)));
    PASS();
}

/* --------------------------------------------------------------------------
 * bench_stats_paired
 * ------------------------------------------------------------------------ */

TEST paired_counts_wins_losses_and_ties(void)
{
    double a[] = {10.0, 50.0, 30.0};
    double b[] = {20.0, 10.0, 30.0};
    int ok[]   = {1, 1, 1};
    int w = -1, l = -1, t = -1, u = -1;
    bench_stats_paired(a, ok, b, ok, 3, &w, &l, &t, &u);
    ASSERT_EQ_FMT(1, w, "%d");
    ASSERT_EQ_FMT(1, l, "%d");
    ASSERT_EQ_FMT(1, t, "%d");
    ASSERT_EQ_FMT(0, u, "%d");
    PASS();
}

/* Le point le plus facile à rater : une paire dont l'une des deux a buté sur le
 * plafond est INDÉCISE, jamais une victoire de l'autre — le plafond ne dit pas
 * de combien elle aurait perdu. Même principe que la comparaison appariée « sur
 * les racines fermées par TOUS les moteurs » de bench_refutation. */
TEST paired_treats_an_unsolved_side_as_undecided_not_as_a_win(void)
{
    double a[] = {10.0, 999999.0};
    double b[] = {20.0, 20.0};
    int a_ok[] = {1, 0};   /* la 2e exécution de `a` a buté sur le plafond */
    int b_ok[] = {1, 1};
    int w = -1, l = -1, t = -1, u = -1;
    bench_stats_paired(a, a_ok, b, b_ok, 2, &w, &l, &t, &u);
    ASSERT_EQ_FMT(1, w, "%d");
    ASSERT_EQ_FMT(0, l, "%d");
    ASSERT_EQ_FMT(0, t, "%d");
    ASSERT_EQ_FMT(1, u, "%d");
    PASS();
}

TEST paired_accepts_null_outputs(void)
{
    double a[] = {10.0};
    double b[] = {20.0};
    int ok[]   = {1};
    bench_stats_paired(a, ok, b, ok, 1, NULL, NULL, NULL, NULL);
    PASS();
}

/* --------------------------------------------------------------------------
 * bench_stats_restart
 * ------------------------------------------------------------------------ */

/* Toutes les exécutions passent sous le seuil : aucun redémarrage n'a lieu, le
 * coût attendu est exactement la moyenne des coûts observés. */
TEST restart_cost_with_certain_success_is_the_plain_mean(void)
{
    double nodes[]  = {100.0, 200.0, 300.0};
    int    solved[] = {1, 1, 1};
    bench_restart_t r = bench_stats_restart(nodes, solved, 3, 1000.0);
    ASSERT_EQ_FMT(3, r.samples_below, "%d");
    ASSERT(CLOSE(1.0, r.p_success));
    ASSERT(CLOSE(200.0, r.mean_below));
    ASSERT(CLOSE(200.0, r.expected_nodes));
    PASS();
}

/* Une chance sur deux de réussir sous 1000 nœuds, la réussite coûtant 100 :
 * (1-0,5)/0,5 x 1000 + 100 = 1100. C'est la forme de la littérature, pas celle
 * écrite en §3.4 du document de conception — voir la doc de la fonction. */
TEST restart_cost_charges_the_cutoff_only_on_failed_attempts(void)
{
    double nodes[]  = {100.0, 5000.0};
    int    solved[] = {1, 1};
    bench_restart_t r = bench_stats_restart(nodes, solved, 2, 1000.0);
    ASSERT_EQ_FMT(1, r.samples_below, "%d");
    ASSERT(CLOSE(0.5, r.p_success));
    ASSERT(CLOSE(100.0, r.mean_below));
    ASSERT(CLOSE(1100.0, r.expected_nodes));
    PASS();
}

/* Le seuil est STRICT : une exécution qui coûte exactement `cutoff` aurait été
 * coupée juste avant d'aboutir, elle ne compte pas comme un succès. */
TEST restart_cutoff_is_strict(void)
{
    double nodes[]  = {1000.0};
    int    solved[] = {1};
    bench_restart_t r = bench_stats_restart(nodes, solved, 1, 1000.0);
    ASSERT_EQ_FMT(0, r.samples_below, "%d");
    ASSERT(r.expected_nodes < 0.0);
    PASS();
}

/* Aucune exécution ne franchit le seuil : le redémarrage ne termine jamais.
 * -1 et non 0, qui se lirait comme « gratuit ». */
TEST restart_cost_is_minus_one_when_the_cutoff_never_succeeds(void)
{
    double nodes[]  = {5000.0, 6000.0};
    int    solved[] = {1, 1};
    bench_restart_t r = bench_stats_restart(nodes, solved, 2, 1000.0);
    ASSERT(CLOSE(0.0, r.p_success));
    ASSERT(CLOSE(0.0, r.mean_below));
    ASSERT(r.expected_nodes < 0.0);
    PASS();
}

TEST restart_rejects_empty_sample_and_non_positive_cutoff(void)
{
    double nodes[]  = {100.0};
    int    solved[] = {1};
    ASSERT(bench_stats_restart(nodes, solved, 0, 1000.0).expected_nodes < 0.0);
    ASSERT(bench_stats_restart(nodes, solved, 1, 0.0).expected_nodes < 0.0);
    ASSERT(bench_stats_restart(nodes, solved, 1, -5.0).expected_nodes < 0.0);
    PASS();
}

/* Une exécution non résolue sous le seuil compte dans le DÉNOMINATEUR de
 * p_success : c'est bien une tentative, elle a coûté `cutoff` et échoué. */
TEST restart_counts_unsolved_runs_in_the_failure_probability(void)
{
    double nodes[]  = {100.0, 100.0};
    int    solved[] = {1, 0};
    bench_restart_t r = bench_stats_restart(nodes, solved, 2, 1000.0);
    ASSERT(CLOSE(0.5, r.p_success));
    ASSERT(CLOSE(1100.0, r.expected_nodes));
    PASS();
}

SUITE(bench_solve_stats_suite)
{
    RUN_TEST(sort_orders_ascending_and_is_stable_on_equal_values);
    RUN_TEST(median_of_odd_sample_is_the_middle_value);
    RUN_TEST(median_of_even_sample_averages_the_two_middles);
    RUN_TEST(median_of_empty_sample_is_zero);
    RUN_TEST(median_is_immune_to_one_heavy_tail_run);
    RUN_TEST(geomean_of_powers_of_ten_is_the_middle_power);
    RUN_TEST(geomean_ignores_non_positive_values);
    RUN_TEST(geomean_without_any_usable_value_is_zero);
    RUN_TEST(survival_counts_only_solved_runs_under_the_threshold);
    RUN_TEST(survival_threshold_is_inclusive);
    RUN_TEST(survival_of_empty_sample_is_zero);
    RUN_TEST(paired_counts_wins_losses_and_ties);
    RUN_TEST(paired_treats_an_unsolved_side_as_undecided_not_as_a_win);
    RUN_TEST(paired_accepts_null_outputs);
    RUN_TEST(restart_cost_with_certain_success_is_the_plain_mean);
    RUN_TEST(restart_cost_charges_the_cutoff_only_on_failed_attempts);
    RUN_TEST(restart_cutoff_is_strict);
    RUN_TEST(restart_cost_is_minus_one_when_the_cutoff_never_succeeds);
    RUN_TEST(restart_rejects_empty_sample_and_non_positive_cutoff);
    RUN_TEST(restart_counts_unsolved_runs_in_the_failure_probability);
}
