#include "bench_solve_stats.h"

#include <math.h>
#include <stddef.h>

void bench_stats_sort(double *v, int n)
{
    for (int i = 1; i < n; i++) {
        double key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

double bench_stats_median_sorted(const double *v, int n)
{
    if (n <= 0) {
        return 0.0;
    }
    if (n % 2 == 1) {
        return v[n / 2];
    }
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double bench_stats_geomean(const double *v, int n)
{
    double sum_log = 0.0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (v[i] > 0.0) {
            sum_log += log(v[i]);
            used++;
        }
    }
    if (used == 0) {
        return 0.0;
    }
    return exp(sum_log / (double)used);
}

double bench_stats_survival(const double *nodes, const int *solved, int n, double threshold)
{
    if (n <= 0) {
        return 0.0;
    }
    int hit = 0;
    for (int i = 0; i < n; i++) {
        if (solved[i] && nodes[i] <= threshold) {
            hit++;
        }
    }
    return (double)hit / (double)n;
}

void bench_stats_paired(const double *a_nodes, const int *a_solved,
                        const double *b_nodes, const int *b_solved,
                        int n, int *wins, int *losses, int *ties, int *undecided)
{
    int w = 0, l = 0, t = 0, u = 0;
    for (int i = 0; i < n; i++) {
        if (!a_solved[i] || !b_solved[i]) {
            u++;
        } else if (a_nodes[i] < b_nodes[i]) {
            w++;
        } else if (a_nodes[i] > b_nodes[i]) {
            l++;
        } else {
            t++;
        }
    }
    if (wins != NULL)      *wins = w;
    if (losses != NULL)    *losses = l;
    if (ties != NULL)      *ties = t;
    if (undecided != NULL) *undecided = u;
}

bench_restart_t bench_stats_restart(const double *nodes, const int *solved, int n, double cutoff)
{
    bench_restart_t out;
    out.p_success = 0.0;
    out.mean_below = 0.0;
    out.samples_below = 0;
    out.expected_nodes = -1.0;

    if (n <= 0 || cutoff <= 0.0) {
        return out;
    }

    double sum_below = 0.0;
    for (int i = 0; i < n; i++) {
        if (solved[i] && nodes[i] < cutoff) {
            sum_below += nodes[i];
            out.samples_below++;
        }
    }
    out.p_success = (double)out.samples_below / (double)n;
    if (out.samples_below == 0) {
        // Aucune exécution ne franchit ce seuil : le redémarrage ne termine
        // jamais. -1 (et non 0, ni l'infini silencieux d'une division par 0).
        return out;
    }
    out.mean_below = sum_below / (double)out.samples_below;
    out.expected_nodes = (1.0 - out.p_success) / out.p_success * cutoff + out.mean_below;
    return out;
}
