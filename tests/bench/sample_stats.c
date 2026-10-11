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
