#include "tools/border_walk.h"

#include <string.h>
#include <stdlib.h>

void border_ring_order(int8_t ring[BORDER_RING_LEN][2])
{
    int i = 0;

    for (int x = 0; x < ETERN_SIZE; x++) {
        ring[i][0] = (int8_t)x;
        ring[i][1] = 0;
        i++;
    }
    for (int y = 1; y < ETERN_SIZE; y++) {
        ring[i][0] = (int8_t)(ETERN_SIZE - 1);
        ring[i][1] = (int8_t)y;
        i++;
    }
    for (int x = ETERN_SIZE - 2; x >= 0; x--) {
        ring[i][0] = (int8_t)x;
        ring[i][1] = (int8_t)(ETERN_SIZE - 1);
        i++;
    }
    for (int y = ETERN_SIZE - 2; y >= 1; y--) {
        ring[i][0] = 0;
        ring[i][1] = (int8_t)y;
        i++;
    }
}

static int bw_is_corner_cell(int x, int y)
{
    return (x == 0 || x == ETERN_SIZE - 1) && (y == 0 || y == ETERN_SIZE - 1);
}

void border_corners_first_order(int8_t order[BORDER_RING_LEN][2])
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    int k = 0;
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        if (bw_is_corner_cell(ring[i][0], ring[i][1])) {
            order[k][0] = ring[i][0];
            order[k][1] = ring[i][1];
            k++;
        }
    }
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        if (!bw_is_corner_cell(ring[i][0], ring[i][1])) {
            order[k][0] = ring[i][0];
            order[k][1] = ring[i][1];
            k++;
        }
    }
}

struct bw_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    const int8_t (*order)[2];
    struct possibility_packet state;
    long long count;
    border_ring_found_cb on_found;
    void *user_ctx;
    long long nodes;
    long long progress_since_last;
    const struct border_progress_opts *progress;
};

/* ATTENTION : `border_walk_expand_frontier` (plus bas dans ce fichier)
   réimplémente indépendamment la même logique de candidats à une case (le
   filtre `id <= 0`, le filtre `is_face_used`, le placement via
   `id_for_rotated_part`) — un changement ici doit être répercuté là-bas,
   sinon le total obtenu en parallélisant (frontière + reprise) diverge
   silencieusement du total séquentiel. Les tests
   `border_walk_expand_frontier_then_resume_matches_direct_count` et sa
   variante « coins d'abord » verrouillent que les deux restent en
   lockstep — s'ils échouent après une modification d'un seul des deux
   sites, c'est exactement ce dont il s'agit. */
/* Le décompte de progression (`ctx->nodes`/`ctx->progress`) est signalé en
   POST-ordre, à la toute fin de la fonction — après que la case courante
   (feuille ou nœud interne) a fini tout son travail, fermeture d'anneau
   comprise. Un signal en pré-ordre raterait de peu le nœud qui vient de
   fermer le dernier anneau : `ctx->count` n'y serait pas encore incrémenté
   au moment de l'appel. */
/* Retourne non nul si `on_found` a demandé l'arrêt : l'appelant doit alors
   remonter sans essayer le moindre candidat suivant. L'état du plateau
   (`ctx->state`) reste celui de l'anneau qui a déclenché l'arrêt — on ne
   défait pas les placements en remontant, personne ne le relit après. */
static int bw_dfs(struct bw_ctx *ctx, int i)
{
    int stop = 0;
    if (i == BORDER_RING_LEN) {
        ctx->count++;
        if (ctx->on_found != NULL) {
            stop = (ctx->on_found(&ctx->state, ctx->user_ctx) != 0);
        }
    } else {
        int8_t x = ctx->order[i][0];
        int8_t y = ctx->order[i][1];

        key_part key;
        what_search_in_grid_to_key(ctx->all_rotate_parts, &ctx->state, x, y, &key,
                                    (int8_t)ctx->map->sizearrayM);
        map_bucket bucket = map_bucket_packed(ctx->map, &key);

        for (int s = 0; s < bucket.size; s++) {
            const struct part *cand = &bucket.parts[s];
            if (cand->id <= 0) {
                continue;
            }
            uint16_t face_idx = (uint16_t)(cand->id - 1);
            if (is_face_used(ctx->state.b_faceused, face_idx)) {
                continue;
            }

            ctx->state.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
            set_face_used(ctx->state.b_faceused, face_idx, 1);
            ctx->state.alloc = (uint16_t)(i + 1);

            stop = bw_dfs(ctx, i + 1);

            set_face_used(ctx->state.b_faceused, face_idx, 0);
            ctx->state.grid[x][y] = -2;
            ctx->state.alloc = (uint16_t)i;

            if (stop) {
                break;
            }
        }
    }

    ctx->nodes++;
    if (ctx->progress != NULL) {
        ctx->progress_since_last++;
        if (ctx->progress_since_last == ctx->progress->interval_nodes) {
            ctx->progress_since_last = 0;
            ctx->progress->on_progress(ctx->nodes, ctx->count, ctx->progress->ctx);
        }
    }
    return stop;
}

long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx,
                                     const struct border_progress_opts *progress)
{
    struct bw_ctx bw;
    bw.map = map;
    bw.all_rotate_parts = all_rotate_parts;
    bw.order = order;

    if (start_state != NULL) {
        bw.state = *start_state;
    } else {
        memset(&bw.state, 0, sizeof bw.state);
        for (int x = 0; x < ETERN_SIZE; x++) {
            for (int y = 0; y < ETERN_SIZE; y++) {
                bw.state.grid[x][y] = -2;
            }
        }
        bw.state.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
    }

    bw.count = 0;
    bw.on_found = on_found;
    bw.user_ctx = ctx;
    bw.nodes = 0;
    bw.progress_since_last = 0;
    bw.progress = progress;

    (void)bw_dfs(&bw, start_depth);
    return bw.count;
}

long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    return border_walk_count_ordered(map, all_rotate_parts, ring, 0, NULL, on_found, ctx, NULL);
}

/* ATTENTION : réimplémente indépendamment la même logique de candidats que
   `bw_dfs` (plus haut dans ce fichier) — voir le commentaire sur `bw_dfs`
   pour pourquoi les deux doivent rester en lockstep. */
long long border_walk_expand_frontier(map_big_array *map,
                                       struct array_part *all_rotate_parts,
                                       const int8_t order[BORDER_RING_LEN][2],
                                       int target_partitions,
                                       border_partial_cb on_partial, void *partial_ctx,
                                       border_ring_found_cb on_complete, void *complete_ctx)
{
    struct possibility_packet *level = malloc(sizeof *level);
    int level_size = 1;
    memset(&level[0], 0, sizeof level[0]);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            level[0].grid[x][y] = -2;
        }
    }
    level[0].min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    long long completed = 0;
    int depth = 0;
    int stop = 0;

    while (!stop && level_size < target_partitions && depth < BORDER_RING_LEN) {
        int8_t x = order[depth][0];
        int8_t y = order[depth][1];

        struct possibility_packet *next_level = NULL;
        int next_size = 0;
        int next_cap = 0;

        for (int e = 0; e < level_size && !stop; e++) {
            struct possibility_packet *base = &level[e];
            key_part key;
            what_search_in_grid_to_key(all_rotate_parts, base, x, y, &key, (int8_t)map->sizearrayM);
            map_bucket bucket = map_bucket_packed(map, &key);

            for (int s = 0; s < bucket.size; s++) {
                const struct part *cand = &bucket.parts[s];
                if (cand->id <= 0) {
                    continue;
                }
                uint16_t face_idx = (uint16_t)(cand->id - 1);
                if (is_face_used(base->b_faceused, face_idx)) {
                    continue;
                }

                struct possibility_packet child = *base;
                child.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
                set_face_used(child.b_faceused, face_idx, 1);
                child.alloc = (uint16_t)(depth + 1);

                if (depth + 1 == BORDER_RING_LEN) {
                    completed++;
                    if (on_complete != NULL && on_complete(&child, complete_ctx) != 0) {
                        stop = 1;
                        break;
                    }
                    continue;
                }

                if (next_size == next_cap) {
                    next_cap = (next_cap == 0) ? 16 : next_cap * 2;
                    next_level = realloc(next_level, (size_t)next_cap * sizeof *next_level);
                }
                next_level[next_size++] = child;
            }
        }

        free(level);
        level = next_level;
        level_size = next_size;
        depth++;

        if (level_size == 0) {
            break;
        }
    }

    /* Arrêt demandé : aucun état partiel n'est livré. Les distribuer quand
       même ferait repartir des workers sur un travail dont l'appelant vient
       précisément de dire qu'il n'en voulait plus. */
    if (on_partial != NULL && !stop) {
        for (int e = 0; e < level_size; e++) {
            on_partial(&level[e], depth, partial_ctx);
        }
    }
    free(level);

    return completed;
}
