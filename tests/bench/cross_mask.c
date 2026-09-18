#include "cross_mask.h"

#include <string.h>

#include "core/core_static_variables.h"

int cross_cell_on(int x, int y)
{
    if (x < 0 || y < 0 || x >= ETERN_SIZE || y >= ETERN_SIZE) {
        return 0;
    }
    int d1 = x - y;
    int d2 = x + y - (ETERN_SIZE - 1);
    if (d1 < 0) d1 = -d1;
    if (d2 < 0) d2 = -d2;
    return (d1 <= 1 || d2 <= 1) ? 1 : 0;
}

int cross_size(void)
{
    int n = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            n += cross_cell_on(x, y);
        }
    }
    return n;
}

int cross_fill(uint8_t *out)
{
    int n = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            uint8_t on = (uint8_t)cross_cell_on(x, y);
            out[x * ETERN_SIZE + y] = on;
            n += on;
        }
    }
    return n;
}

int cross_fill_complement(uint8_t *out)
{
    int n = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            uint8_t on = (uint8_t)(cross_cell_on(x, y) ? 0 : 1);
            out[x * ETERN_SIZE + y] = on;
            n += on;
        }
    }
    return n;
}

int cross_fill_halo(uint8_t *out, const int16_t grid[][ETERN_SIZE])
{
    static const int dx[4] = {1, -1, 0, 0};
    static const int dy[4] = {0, 0, 1, -1};
    int n = 0;

    memset(out, 0, (size_t)ETERN_PARTS);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (grid[x][y] != -2) {
                continue; /* case posée : ce n'est pas elle qu'on veut ouvrir */
            }
            for (int d = 0; d < 4; d++) {
                int nx = x + dx[d], ny = y + dy[d];
                if (nx < 0 || ny < 0 || nx >= ETERN_SIZE || ny >= ETERN_SIZE) {
                    continue; /* le bord de plateau contraint, mais n'est pas une pièce */
                }
                if (grid[nx][ny] != -2) {
                    out[x * ETERN_SIZE + y] = 1;
                    n++;
                    break;
                }
            }
        }
    }
    return n;
}

/** @brief xorshift64* — reproductible et indépendant de la libc. */
static uint64_t cross_rand(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

int cross_fill_random(uint8_t *out, int count, uint64_t seed)
{
    uint16_t perm[ETERN_PARTS];
    uint64_t state = (seed == 0) ? 1 : seed;

    memset(out, 0, (size_t)ETERN_PARTS);
    if (count <= 0) {
        return 0;
    }
    if (count > ETERN_PARTS) {
        count = ETERN_PARTS;
    }
    for (int i = 0; i < ETERN_PARTS; i++) {
        perm[i] = (uint16_t)i;
    }
    // Fisher-Yates : chaque sous-ensemble de `count` cases est équiprobable.
    // Le biais du modulo est sans portée ici (ETERN_PARTS ≤ 256 contre un tirage
    // sur 64 bits) mais le rejet ne coûte rien à cette échelle — le banc doit
    // pouvoir affirmer « tirage uniforme » sans réserve.
    for (int i = ETERN_PARTS - 1; i > 0; i--) {
        uint64_t bound = (uint64_t)i + 1;
        uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
        uint64_t r;
        do {
            r = cross_rand(&state);
        } while (r >= limit);
        int j = (int)(r % bound);
        uint16_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }
    for (int i = 0; i < count; i++) {
        out[perm[i]] = 1;
    }
    return count;
}
