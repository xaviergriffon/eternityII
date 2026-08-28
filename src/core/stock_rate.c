#include "core/stock_rate.h"

/**
 * @brief Incrémente `bucket` de `n`, en le remettant d'abord à zéro s'il
 *        portait l'époque d'un tour précédent du ring buffer.
 *
 * Aucun verrou : deux threads peuvent tous les deux constater `epoch`
 * périmée et remettre le compteur à zéro avant d'incrémenter — la statistique
 * résultante sous-compte alors très légèrement cet instant, jamais plus,
 * acceptable pour une valeur d'affichage (cf. stock_rate.h).
 */
static void bucket_add(rate_bucket_t *bucket, uint64_t epoch, unsigned int n)
{
    uint64_t current_epoch = __atomic_load_n(&bucket->epoch, __ATOMIC_RELAXED);
    if (current_epoch != epoch)
    {
        __atomic_store_n(&bucket->count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&bucket->epoch, epoch, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&bucket->count, n, __ATOMIC_RELAXED);
}

/**
 * @brief Lit `bucket` s'il correspond à `epoch`, 0 sinon (bucket vide ou
 *        périmé — tour précédent du ring buffer jamais réécrit depuis).
 */
static unsigned int bucket_read(const rate_bucket_t *bucket, uint64_t epoch)
{
    uint64_t current_epoch = __atomic_load_n(&bucket->epoch, __ATOMIC_RELAXED);
    if (current_epoch != epoch)
    {
        return 0;
    }
    return __atomic_load_n(&bucket->count, __ATOMIC_RELAXED);
}

void stock_rate_record(stock_rate_counter_t *c, unsigned int n, time_t now)
{
    if (c == NULL || n == 0)
    {
        return;
    }
    uint64_t sec_epoch = (uint64_t)now;
    uint64_t min_epoch = (uint64_t)now / 60;
    bucket_add(&c->sec[sec_epoch % STOCK_RATE_SEC_BUCKETS], sec_epoch, n);
    bucket_add(&c->min[min_epoch % STOCK_RATE_MIN_BUCKETS], min_epoch, n);
}

/**
 * @brief Somme les buckets à granularité seconde couvrant les
 *        `window_seconds` dernières secondes avant `now` inclus.
 */
static unsigned long long sum_sec_window(const stock_rate_counter_t *c, uint64_t now_sec, int window_seconds)
{
    unsigned long long total = 0;
    for (int i = 0; i < window_seconds; i++)
    {
        uint64_t epoch = now_sec - (uint64_t)i;
        total += bucket_read(&c->sec[epoch % STOCK_RATE_SEC_BUCKETS], epoch);
    }
    return total;
}

/**
 * @brief Somme les buckets à granularité minute couvrant les
 *        `window_minutes` dernières minutes avant `now_min` inclus.
 */
static unsigned long long sum_min_window(const stock_rate_counter_t *c, uint64_t now_min, int window_minutes)
{
    unsigned long long total = 0;
    for (int i = 0; i < window_minutes; i++)
    {
        uint64_t epoch = now_min - (uint64_t)i;
        total += bucket_read(&c->min[epoch % STOCK_RATE_MIN_BUCKETS], epoch);
    }
    return total;
}

void stock_rate_windows(const stock_rate_counter_t *c, time_t now,
                         unsigned long long *last_1m, unsigned long long *last_1h,
                         unsigned long long *last_1d)
{
    if (c == NULL)
    {
        if (last_1m != NULL) *last_1m = 0;
        if (last_1h != NULL) *last_1h = 0;
        if (last_1d != NULL) *last_1d = 0;
        return;
    }

    uint64_t now_sec = (uint64_t)now;
    uint64_t now_min = (uint64_t)now / 60;

    if (last_1m != NULL)
    {
        *last_1m = sum_sec_window(c, now_sec, STOCK_RATE_SEC_BUCKETS);
    }
    if (last_1h != NULL)
    {
        *last_1h = sum_min_window(c, now_min, 60);
    }
    if (last_1d != NULL)
    {
        *last_1d = sum_min_window(c, now_min, STOCK_RATE_MIN_BUCKETS);
    }
}

void stock_rate_reset_for_tests(stock_rate_counter_t *c)
{
    if (c == NULL)
    {
        return;
    }
    for (int i = 0; i < STOCK_RATE_SEC_BUCKETS; i++)
    {
        c->sec[i].epoch = 0;
        c->sec[i].count = 0;
    }
    for (int i = 0; i < STOCK_RATE_MIN_BUCKETS; i++)
    {
        c->min[i].epoch = 0;
        c->min[i].count = 0;
    }
}
