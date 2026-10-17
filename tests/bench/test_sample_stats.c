/*
 * Suite du cœur pur de bench_sample (tests/bench/sample_stats.c).
 *
 * Ce qui est verrouillé ici est ce qui fausserait une extrapolation SANS
 * qu'aucun chiffre n'ait l'air faux : un réservoir non uniforme (chaque racine
 * doit avoir la même chance d'être tirée), un intervalle qui affirmerait 0 %
 * sur un échantillon fini, et une moyenne de coûts censurés présentée comme
 * une estimation alors qu'elle n'est qu'une borne basse.
 */
#include "greatest.h"
#include "bench/sample_stats.h"

#include <math.h>
#include <string.h>

#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

TEST rng_below_stays_in_range_and_is_reproducible(void)
{
    sample_rng_t a, b;
    sample_rng_seed(&a, 42);
    sample_rng_seed(&b, 42);
    for (int i = 0; i < 10000; i++) {
        uint64_t bound = (uint64_t)i + 1;
        uint64_t x = sample_rng_below(&a, bound);
        ASSERT(x < bound);
        ASSERT_EQ(x, sample_rng_below(&b, bound));
    }
    ASSERT_EQ(0, sample_rng_below(&a, 1));
    PASS();
}

TEST rng_below_is_uniform_on_a_bound_that_does_not_divide_two_to_the_64(void)
{
    sample_rng_t rng;
    sample_rng_seed(&rng, 7);
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 30000; i++) {
        counts[sample_rng_below(&rng, 3)]++;
    }
    for (int k = 0; k < 3; k++) {
        /* Attendu 10 000, écart type ~82 : ±400 est à ~5 écarts types. */
        ASSERT_IN_RANGE(10000, counts[k], 400);
    }
    PASS();
}

TEST reservoir_fills_its_first_places_in_order(void)
{
    sample_reservoir_t r;
    sample_reservoir_init(&r, 4, 1);
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(i, sample_reservoir_offer(&r));
    }
    ASSERT_EQ(4, sample_reservoir_size(&r));
    PASS();
}

TEST reservoir_smaller_stream_keeps_everything(void)
{
    sample_reservoir_t r;
    sample_reservoir_init(&r, 10, 1);
    for (int i = 0; i < 3; i++) {
        ASSERT(sample_reservoir_offer(&r) >= 0);
    }
    ASSERT_EQ(3, sample_reservoir_size(&r));
    PASS();
}

/* Le cœur de l'outil : chaque élément du flux doit finir dans l'échantillon
 * avec la même probabilité capacity/N, qu'il arrive au début ou à la fin. Un
 * réservoir qui garderait les premiers (ou les derniers) échouerait ici —
 * c'est exactement le biais d'un stock lu dans l'ordre de ses piles. */
TEST reservoir_gives_every_element_the_same_chance(void)
{
    enum { N = 10, CAP = 3, TRIALS = 30000 };
    int kept[N];
    memset(kept, 0, sizeof(kept));
    for (int t = 0; t < TRIALS; t++) {
        sample_reservoir_t r;
        sample_reservoir_init(&r, CAP, (uint64_t)t * 2654435761u + 1);
        int slot[CAP];
        for (int i = 0; i < N; i++) {
            int64_t s = sample_reservoir_offer(&r);
            if (s >= 0) {
                slot[s] = i;
            }
        }
        for (int s = 0; s < CAP; s++) {
            kept[slot[s]]++;
        }
    }
    for (int i = 0; i < N; i++) {
        /* Attendu 9 000 (= 30 000 × 3/10), écart type ~79. */
        ASSERT_IN_RANGE(9000, kept[i], 400);
    }
    PASS();
}

TEST wilson_never_claims_zero_on_a_finite_sample(void)
{
    double lo, hi;
    sample_wilson95(0, 10, &lo, &hi);
    ASSERT_EQ(0.0, lo);
    ASSERT(NEAR(hi, 0.2775, 1e-3));

    sample_wilson95(5, 10, &lo, &hi);
    ASSERT(NEAR(lo, 0.2366, 1e-3));
    ASSERT(NEAR(hi, 0.7634, 1e-3));

    sample_wilson95(10, 10, &lo, &hi);
    ASSERT(NEAR(lo, 0.7225, 1e-3));
    ASSERT_EQ(1.0, hi);

    sample_wilson95(0, 0, &lo, &hi);
    ASSERT_EQ(0.0, lo);
    ASSERT_EQ(1.0, hi);
    PASS();
}

TEST summary_without_censoring_is_an_estimate(void)
{
    sample_result_t res[4] = {
        { SAMPLE_CLOSED, 10, 1.0 },
        { SAMPLE_CLOSED, 20, 1.0 },
        { SAMPLE_CLOSED, 30, 1.0 },
        { SAMPLE_CLOSED, 1000, 1.0 },
    };
    unsigned long long scratch[4];
    sample_summary_t s;
    sample_summarize(res, 4, scratch, &s);
    ASSERT_EQ(4, s.n);
    ASSERT_EQ(4, s.closed);
    ASSERT_EQ(0, s.censored);
    ASSERT_EQ(20, s.q50);
    ASSERT_EQ(1000, s.q90);
    ASSERT_EQ(1000, s.qmax);
    ASSERT(NEAR(s.mean_nodes, 265.0, 1e-9));
    ASSERT(NEAR(s.nodes_per_second, 265.0, 1e-9));
    /* ≤ 10 : une racine sur quatre ; ≤ 100 : trois ; ≤ 1000 : toutes. */
    ASSERT_EQ(10, s.ecdf_at[0]);
    ASSERT(NEAR(s.ecdf[0], 0.25, 1e-12));
    ASSERT(NEAR(s.ecdf[1], 0.75, 1e-12));
    ASSERT(NEAR(s.ecdf[2], 1.0, 1e-12));
    ASSERT(NEAR(s.ecdf[SAMPLE_ECDF_POINTS - 1], 1.0, 1e-12));
    PASS();
}

TEST summary_flags_a_censored_mean_and_keeps_open_roots_out_of_the_quantiles(void)
{
    sample_result_t res[5] = {
        { SAMPLE_CLOSED, 100, 1.0 },
        { SAMPLE_BUDGET, 5000, 1.0 },
        { SAMPLE_BUDGET, 5000, 1.0 },
        { SAMPLE_SOLUTION, 300, 1.0 },
        { SAMPLE_ERROR, 999999, 1.0 },
    };
    unsigned long long scratch[5];
    sample_summary_t s;
    sample_summarize(res, 5, scratch, &s);
    ASSERT_EQ(4, s.n);
    ASSERT_EQ(1, s.errors);
    ASSERT_EQ(1, s.closed);
    ASSERT_EQ(2, s.budget);
    ASSERT_EQ(1, s.solutions);
    ASSERT_EQ(1, s.censored);
    /* Seule la racine FERMÉE alimente les quantiles de fermeture. */
    ASSERT_EQ(100, s.q50);
    ASSERT_EQ(100, s.qmax);
    /* La moyenne prend toutes les tentatives valides, l'erreur exclue. */
    ASSERT(NEAR(s.mean_nodes, (100.0 + 5000 + 5000 + 300) / 4.0, 1e-9));
    ASSERT(NEAR(s.ecdf[1], 0.25, 1e-12));
    ASSERT(NEAR(s.ecdf[SAMPLE_ECDF_POINTS - 1], 0.25, 1e-12));
    PASS();
}

TEST summary_of_nothing_is_empty(void)
{
    sample_summary_t s;
    sample_summarize(NULL, 0, NULL, &s);
    ASSERT_EQ(0, s.n);
    ASSERT_EQ(0, s.closed);
    ASSERT_EQ(0.0, s.mean_nodes);
    ASSERT_EQ(0.0, s.closed_lo);
    ASSERT_EQ(1.0, s.closed_hi);
    PASS();
}

/* Les trois parts sont disjointes : une racine morte à l'insertion n'est pas
 * recomptée au contrôle bon marché, ni une racine que ce contrôle tue parmi
 * les fermetures de la preuve bornée ; une fermeture compte sous tout plafond
 * qui la couvre, une racine censurée sous aucun. */
TEST sweep_tally_keeps_its_three_parts_disjoint(void)
{
    sample_result_t r[8];
    memset(r, 0, sizeof r);
    r[0] = (sample_result_t){ SAMPLE_CLOSED, 1, 0.0, 1, 1 };     /* morte à l'insertion */
    r[1] = (sample_result_t){ SAMPLE_CLOSED, 3, 0.0, 0, 1 };     /* contrôle bon marché seul */
    r[2] = (sample_result_t){ SAMPLE_CLOSED, 50, 0.0, 0, 0 };    /* fermée sous 100 */
    r[3] = (sample_result_t){ SAMPLE_CLOSED, 100, 0.0, 0, 0 };   /* fermée sous 100 (borne incluse) */
    r[4] = (sample_result_t){ SAMPLE_CLOSED, 5000, 0.0, 0, 0 };  /* fermée sous 10 000 seulement */
    r[5] = (sample_result_t){ SAMPLE_CLOSED, 20000, 0.0, 0, 0 }; /* au-delà des trois plafonds */
    r[6] = (sample_result_t){ SAMPLE_BUDGET, 50, 0.0, 0, 0 };    /* censurée : jamais fermée */
    r[7] = (sample_result_t){ SAMPLE_ERROR, 0, 0.0, 1, 1 };      /* exclue */
    sample_sweep_t t;
    sample_sweep_tally(r, 8, &t);
    ASSERT_EQ_FMT(7ULL, (unsigned long long)t.n, "%llu");
    ASSERT_EQ_FMT(1ULL, (unsigned long long)t.dead_at_insertion, "%llu");
    ASSERT_EQ_FMT(1ULL, (unsigned long long)t.dead_cheap_only, "%llu");
    ASSERT_EQ_FMT(100ULL, t.budget_at[0], "%llu");
    ASSERT_EQ_FMT(10000ULL, t.budget_at[2], "%llu");
    ASSERT_EQ_FMT(2ULL, (unsigned long long)t.dfs_closed_at[0], "%llu");
    ASSERT_EQ_FMT(2ULL, (unsigned long long)t.dfs_closed_at[1], "%llu");
    ASSERT_EQ_FMT(3ULL, (unsigned long long)t.dfs_closed_at[2], "%llu");
    ASSERT_EQ_FMT(4ULL, (unsigned long long)t.residual_at[2], "%llu");
    PASS();
}

/* Rang centile : en tête 0, en queue 1, ex aequo comptés pour moitié. */
TEST percentile_rank_puts_the_best_first_and_halves_ties(void)
{
    const double sc[5] = { 5.0, 9.0, 1.0, 5.0, 3.0 };
    ASSERT(NEAR(sample_percentile_rank(sc, 5, 1), 0.0, 1e-12));          /* le plus fort */
    ASSERT(NEAR(sample_percentile_rank(sc, 5, 2), 1.0, 1e-12));          /* le plus faible */
    ASSERT(NEAR(sample_percentile_rank(sc, 5, 0), (1.0 + 0.5) / 4.0, 1e-12)); /* 9 au-dessus, 5 ex aequo */
    const double flat[3] = { 2.0, 2.0, 2.0 };
    ASSERT(NEAR(sample_percentile_rank(flat, 3, 0), 0.5, 1e-12));        /* tout ex aequo : le hasard */
    PASS();
}

/* Test des signes exact : valeurs de référence de la binomiale (n, 1/2). */
TEST sign_test_matches_the_binomial(void)
{
    ASSERT(NEAR(sample_sign_test_p(15, 30), 1.0, 1e-9));
    ASSERT(NEAR(sample_sign_test_p(0, 10), 2.0 / 1024.0, 1e-12));
    ASSERT(NEAR(sample_sign_test_p(10, 10), 2.0 / 1024.0, 1e-12));
    ASSERT(NEAR(sample_sign_test_p(8, 10), 2.0 * 56.0 / 1024.0, 1e-12)); /* P(X<=2) = 56/1024 */
    ASSERT(sample_sign_test_p(23, 30) < 0.01);  /* 23/30 : p = 0,0052 */
    ASSERT(sample_sign_test_p(21, 30) > 0.01);  /* 21/30 : p = 0,043 */
    PASS();
}

/* Spearman : 1 sur un ordre identique (même non linéaire), −1 inversé,
 * ex aequo pris en rangs moyens, 0 sur une série constante. */
TEST spearman_reads_ranks_not_values(void)
{
    double scratch[4 * 5];
    const double a[5] = { 1, 2, 3, 4, 5 };
    const double sq[5] = { 1, 4, 9, 16, 1000 };
    const double rev[5] = { 5, 4, 3, 2, 1 };
    const double flat[5] = { 7, 7, 7, 7, 7 };
    ASSERT(NEAR(sample_spearman(a, sq, 5, scratch), 1.0, 1e-12));
    ASSERT(NEAR(sample_spearman(a, rev, 5, scratch), -1.0, 1e-12));
    ASSERT(NEAR(sample_spearman(a, flat, 5, scratch), 0.0, 1e-12));
    const double t1[4] = { 1, 2, 2, 3 };
    const double t2[4] = { 1, 2, 3, 4 };
    /* rangs de t1 : 1, 2.5, 2.5, 4 → ρ = 4.5 / sqrt(4.5 · 5) */
    double sc4[16];
    ASSERT(NEAR(sample_spearman(t1, t2, 4, sc4), 4.5 / sqrt(4.5 * 5.0), 1e-12));
    PASS();
}

SUITE(sample_stats_suite)
{
    RUN_TEST(rng_below_stays_in_range_and_is_reproducible);
    RUN_TEST(rng_below_is_uniform_on_a_bound_that_does_not_divide_two_to_the_64);
    RUN_TEST(reservoir_fills_its_first_places_in_order);
    RUN_TEST(reservoir_smaller_stream_keeps_everything);
    RUN_TEST(reservoir_gives_every_element_the_same_chance);
    RUN_TEST(wilson_never_claims_zero_on_a_finite_sample);
    RUN_TEST(summary_without_censoring_is_an_estimate);
    RUN_TEST(summary_flags_a_censored_mean_and_keeps_open_roots_out_of_the_quantiles);
    RUN_TEST(summary_of_nothing_is_empty);
    RUN_TEST(sweep_tally_keeps_its_three_parts_disjoint);
    RUN_TEST(percentile_rank_puts_the_best_first_and_halves_ties);
    RUN_TEST(sign_test_matches_the_binomial);
    RUN_TEST(spearman_reads_ranks_not_values);
}
