/* Estimateur de Knuth sur l'arbre de l'expansion — cf. tree_probe.h. */
#include "bench/tree_probe.h"

#include <math.h>
#include <string.h>

#include "core/lifo.h"
#include "core/part.h"

void tree_estimate_init(tree_estimate_t *e)
{
    memset(e, 0, sizeof(*e));
}

void tree_estimate_add(tree_estimate_t *e, const double w[TREE_PROBE_LEVELS])
{
    e->obs++;
    for (int l = 0; l < TREE_PROBE_LEVELS; l++) {
        if (w[l] > 0.0) {
            e->sum[l] += w[l];
            e->sum2[l] += w[l] * w[l];
            e->reached[l]++;
        }
    }
}

void tree_estimate_merge(tree_estimate_t *dst, const tree_estimate_t *src)
{
    dst->obs += src->obs;
    for (int l = 0; l < TREE_PROBE_LEVELS; l++) {
        dst->sum[l] += src->sum[l];
        dst->sum2[l] += src->sum2[l];
        dst->reached[l] += src->reached[l];
    }
}

void tree_estimate_level(const tree_estimate_t *e, int level, double scale, double *mean, double *se)
{
    *mean = 0.0;
    *se = 0.0;
    if (e->obs == 0 || level < 0 || level >= TREE_PROBE_LEVELS) {
        return;
    }
    double n = (double)e->obs;
    double m = e->sum[level] / n;
    *mean = m * scale;
    if (e->obs > 1) {
        double var = (e->sum2[level] - n * m * m) / (n - 1.0);
        *se = (var > 0.0) ? sqrt(var / n) * scale : 0.0;
    }
}

/* Complétions de la dernière case vide de `p` : une par entrée (pièce,
   rotation) libre de son compartiment — chacune une solution. */
static int count_completions(struct possibility_packet *p, map_big_array *map, struct array_part *rot)
{
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (p->grid[x][y] != -2) {
                continue;
            }
            key_part key;
            what_search_in_grid_to_key(rot, p, (int8_t)x, (int8_t)y, &key, (int8_t)map->sizearrayM);
            struct array_part *b = get_parts_bigarray_with_key(map, &key);
            int n = 0;
            for (int s = 0; s < b->size; s++) {
                int16_t id = b->parts[s].id;
                n += (id != 0 && !is_face_used(p->b_faceused, (uint16_t)(id - 1)));
            }
            return n;
        }
    }
    return 0;
}

int tree_probe(const struct possibility_packet *root, map_big_array *map, struct array_part *rot,
               int16_t id_parts[][4], sample_rng_t *rng, double w[TREE_PROBE_LEVELS], int stop_level,
               int capture_level, struct possibility_packet *captured, double *captured_w)
{
    struct possibility_packet cur;
    memcpy(&cur, root, sizeof(cur));
    int level = possibility_placed_count(&cur);
    double weight = 1.0;
    if (captured_w != NULL) {
        *captured_w = 0.0;
    }
    File kids;
    init_file(&kids, sizeof(struct possibility_packet));
    for (;;) {
        w[level] += weight;
        if (level == capture_level && captured != NULL && captured_w != NULL) {
            memcpy(captured, &cur, sizeof(cur));
            *captured_w = weight;
        }
        if (level >= ETERN_PARTS || (stop_level > 0 && level >= stop_level)) {
            return level;
        }
        if (level == ETERN_PARTS - 1) {
            int n = count_completions(&cur, map, rot);
            if (n > 0) {
                w[ETERN_PARTS] += weight * (double)n;
                return ETERN_PARTS;
            }
            return level;
        }
        unsigned long long pruned = 0;
        search_possiblity_light_pruned(&kids, &cur, map, rot, id_parts, &pruned);
        unsigned long long b = kids.size;
        if (b == 0) {
            return level;
        }
        uint64_t pick = sample_rng_below(rng, b);
        struct possibility_packet child;
        for (uint64_t i = 0; scroll(&kids, &child); i++) {
            if (i == pick) {
                memcpy(&cur, &child, sizeof(cur));
            }
        }
        weight *= (double)b;
        level = possibility_placed_count(&cur);
    }
}
