#include "tools/border_walk.h"

#include <string.h>

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

struct bw_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    int8_t ring[BORDER_RING_LEN][2];
    struct possibility_packet state;
    long long count;
    border_ring_found_cb on_found;
    void *user_ctx;
};

static void bw_dfs(struct bw_ctx *ctx, int i)
{
    if (i == BORDER_RING_LEN) {
        ctx->count++;
        if (ctx->on_found != NULL) {
            ctx->on_found(&ctx->state, ctx->user_ctx);
        }
        return;
    }

    int8_t x = ctx->ring[i][0];
    int8_t y = ctx->ring[i][1];

    key_part key;
    what_search_in_grid_to_key(ctx->all_rotate_parts, &ctx->state, x, y, &key,
                                (int8_t)ctx->map->sizearrayM);
    map_bucket bucket = map_bucket_packed(ctx->map, &key);

    for (int s = 0; s < bucket.size; s++) {
        const struct part *cand = &bucket.parts[s];
        uint16_t face_idx = (uint16_t)(cand->id - 1);
        if (is_face_used(ctx->state.b_faceused, face_idx)) {
            continue;
        }

        ctx->state.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
        set_face_used(ctx->state.b_faceused, face_idx, 1);

        bw_dfs(ctx, i + 1);

        set_face_used(ctx->state.b_faceused, face_idx, 0);
        ctx->state.grid[x][y] = -2;
    }
}

long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx)
{
    struct bw_ctx bw;
    bw.map = map;
    bw.all_rotate_parts = all_rotate_parts;
    border_ring_order(bw.ring);

    memset(&bw.state, 0, sizeof bw.state);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            bw.state.grid[x][y] = -2;
        }
    }
    bw.state.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    bw.count = 0;
    bw.on_found = on_found;
    bw.user_ctx = ctx;

    bw_dfs(&bw, 0);
    return bw.count;
}
