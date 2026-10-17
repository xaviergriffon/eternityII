/*
 * Cœur pur de bench_sample : cf. sample_stats.h.
 */
#include "bench/sample_stats.h"

#include <math.h>
#include <stdlib.h>

void sample_rng_seed(sample_rng_t *rng, uint64_t seed)
{
    rng->state = seed;
}

uint64_t sample_rng_next(sample_rng_t *rng)
{
    uint64_t z = (rng->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t sample_rng_below(sample_rng_t *rng, uint64_t bound)
{
    /* Lemire : le mot haut de x × bound est uniforme dans [0, bound) dès qu'on
       rejette les x dont le mot bas tombe sous 2⁶⁴ mod bound. */
    __uint128_t m = (__uint128_t)sample_rng_next(rng) * bound;
    uint64_t low = (uint64_t)m;
    if (low < bound) {
        uint64_t threshold = (0 - bound) % bound;
        while (low < threshold) {
            m = (__uint128_t)sample_rng_next(rng) * bound;
            low = (uint64_t)m;
        }
    }
    return (uint64_t)(m >> 64);
}

void sample_reservoir_init(sample_reservoir_t *r, uint64_t capacity, uint64_t seed)
{
    r->capacity = capacity;
    r->seen = 0;
    sample_rng_seed(&r->rng, seed);
}

int64_t sample_reservoir_offer(sample_reservoir_t *r)
{
    r->seen++;
    if (r->seen <= r->capacity) {
        return (int64_t)(r->seen - 1);
    }
    uint64_t j = sample_rng_below(&r->rng, r->seen);
    return (j < r->capacity) ? (int64_t)j : -1;
}

uint64_t sample_reservoir_size(const sample_reservoir_t *r)
{
    return (r->seen < r->capacity) ? r->seen : r->capacity;
}

void sample_wilson95(uint64_t k, uint64_t n, double *lo, double *hi)
{
    if (n == 0) {
        *lo = 0.0;
        *hi = 1.0;
        return;
    }
    const double z = 1.96;
    double nn = (double)n;
    double p = (double)k / nn;
    double denom = 1.0 + z * z / nn;
    double centre = (p + z * z / (2.0 * nn)) / denom;
    double half = z * sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn)) / denom;
    *lo = centre - half;
    *hi = centre + half;
    if (*lo < 0.0) *lo = 0.0;
    if (*hi > 1.0) *hi = 1.0;
    /* Bornes exactes aux extrémités : l'arrondi flottant laisserait 1e-17. */
    if (k == 0) *lo = 0.0;
    if (k == n) *hi = 1.0;
}

static int cmp_ull(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a;
    unsigned long long y = *(const unsigned long long *)b;
    return (x > y) - (x < y);
}

/** Quantile au rang le plus proche (sans interpolation) d'un tableau trié. */
static unsigned long long nearest_rank(const unsigned long long *sorted, size_t m, double p)
{
    size_t rank = (size_t)ceil(p * (double)m);
    if (rank < 1) rank = 1;
    if (rank > m) rank = m;
    return sorted[rank - 1];
}

void sample_summarize(const sample_result_t *res, size_t count,
                      unsigned long long *scratch, sample_summary_t *out)
{
    *out = (sample_summary_t){0};
    size_t m = 0;
    double sum = 0.0, sum2 = 0.0, secs = 0.0, all_nodes = 0.0;

    for (size_t i = 0; i < count; i++) {
        const sample_result_t *r = &res[i];
        if (r->outcome == SAMPLE_ERROR) {
            out->errors++;
            continue;
        }
        out->n++;
        double x = (double)r->nodes;
        sum += x;
        sum2 += x * x;
        secs += r->seconds;
        all_nodes += x;
        switch (r->outcome) {
            case SAMPLE_CLOSED:
                out->closed++;
                scratch[m++] = r->nodes;
                break;
            case SAMPLE_BUDGET:
                out->budget++;
                out->censored = 1;
                break;
            case SAMPLE_SOLUTION:
                /* Un sous-arbre qui contient une solution n'est pas mort : son
                   coût observé n'est pas un coût de fermeture, mais il a bien
                   été dépensé — il compte dans la moyenne, pas dans les
                   quantiles de fermeture. */
                out->solutions++;
                break;
            case SAMPLE_ERROR:
                break;
        }
    }

    sample_wilson95(out->closed, out->n, &out->closed_lo, &out->closed_hi);

    if (m > 0) {
        qsort(scratch, m, sizeof(*scratch), cmp_ull);
        out->q50 = nearest_rank(scratch, m, 0.50);
        out->q90 = nearest_rank(scratch, m, 0.90);
        out->q99 = nearest_rank(scratch, m, 0.99);
        out->qmax = scratch[m - 1];
    }

    if (out->n > 0) {
        double nn = (double)out->n;
        out->mean_nodes = sum / nn;
        if (out->n > 1) {
            double var = (sum2 - sum * sum / nn) / (nn - 1.0);
            out->sem_nodes = (var > 0.0) ? sqrt(var / nn) : 0.0;
        }
    }
    out->nodes_per_second = (secs > 0.0) ? all_nodes / secs : 0.0;

    unsigned long long threshold = 10;
    size_t idx = 0;
    for (int i = 0; i < SAMPLE_ECDF_POINTS; i++) {
        while (idx < m && scratch[idx] <= threshold) {
            idx++;
        }
        out->ecdf_at[i] = threshold;
        out->ecdf[i] = (out->n > 0) ? (double)idx / (double)out->n : 0.0;
        threshold *= 10;
    }
}

void sample_sweep_tally(const sample_result_t *res, size_t count, sample_sweep_t *out)
{
    *out = (sample_sweep_t){0};
    unsigned long long b = 100;
    for (int k = 0; k < SAMPLE_SWEEP_BUDGETS; k++, b *= 10) {
        out->budget_at[k] = b;
    }
    for (size_t i = 0; i < count; i++) {
        const sample_result_t *r = &res[i];
        if (r->outcome == SAMPLE_ERROR) {
            continue;
        }
        out->n++;
        if (r->dead_at_insertion) {
            out->dead_at_insertion++;
            continue;
        }
        if (r->dead_cheap) {
            out->dead_cheap_only++;
            continue;
        }
        for (int k = 0; k < SAMPLE_SWEEP_BUDGETS; k++) {
            out->dfs_closed_at[k] += (r->outcome == SAMPLE_CLOSED && r->nodes <= out->budget_at[k]);
        }
    }
    for (int k = 0; k < SAMPLE_SWEEP_BUDGETS; k++) {
        out->residual_at[k] = out->dead_cheap_only + out->dfs_closed_at[k];
    }
}

double sample_percentile_rank(const double *score, size_t n, size_t target)
{
    if (n <= 1 || target >= n) {
        return 0.0;
    }
    double above = 0.0, ties = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (i == target) continue;
        if (score[i] > score[target]) above += 1.0;
        else if (score[i] == score[target]) ties += 1.0;
    }
    return (above + 0.5 * ties) / (double)(n - 1);
}

double sample_sign_test_p(uint64_t k, uint64_t n)
{
    if (n == 0) {
        return 1.0;
    }
    uint64_t lo = (k < n - k) ? k : n - k;
    /* P(X <= lo) sous Binomiale(n, 1/2), en log pour rester exact aux grands n. */
    double tail = 0.0;
    for (uint64_t i = 0; i <= lo; i++) {
        double lc = lgamma((double)n + 1.0) - lgamma((double)i + 1.0) - lgamma((double)(n - i) + 1.0);
        tail += exp(lc - (double)n * log(2.0));
    }
    double p = 2.0 * tail;
    return p > 1.0 ? 1.0 : p;
}

static const double *g_rank_key;

static int cmp_rank_index(const void *x, const void *y)
{
    double a = g_rank_key[(size_t)*(const double *)x];
    double b = g_rank_key[(size_t)*(const double *)y];
    return (a > b) - (a < b);
}

/* Rangs moyens (1..n) de `v` dans `rank`, `idx` en tampon de n. */
static void average_ranks(const double *v, size_t n, double *idx, double *rank)
{
    for (size_t i = 0; i < n; i++) idx[i] = (double)i;
    g_rank_key = v;
    qsort(idx, n, sizeof(*idx), cmp_rank_index);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j + 1 < n && v[(size_t)idx[j + 1]] == v[(size_t)idx[i]]) j++;
        double r = ((double)i + (double)j) / 2.0 + 1.0;
        for (size_t k = i; k <= j; k++) rank[(size_t)idx[k]] = r;
        i = j + 1;
    }
}

double sample_spearman(const double *a, const double *b, size_t n, double *scratch)
{
    if (n < 2) {
        return 0.0;
    }
    double *idx = scratch, *ra = scratch + n, *rb = scratch + 2 * n;
    average_ranks(a, n, idx, ra);
    average_ranks(b, n, idx, rb);
    double m = ((double)n + 1.0) / 2.0, num = 0.0, da = 0.0, db = 0.0;
    for (size_t i = 0; i < n; i++) {
        num += (ra[i] - m) * (rb[i] - m);
        da += (ra[i] - m) * (ra[i] - m);
        db += (rb[i] - m) * (rb[i] - m);
    }
    return (da > 0.0 && db > 0.0) ? num / sqrt(da * db) : 0.0;
}
