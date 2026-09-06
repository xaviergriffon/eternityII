/*
 * Tests de tests/tools/border_walk.c — cœur pur du walker de bordure.
 *
 * Voir docs/superpowers/specs/2026-09-06-masse-bordure-design.md pour le
 * raisonnement complet (pourquoi aucune modification à part.c/readdata.c,
 * pourquoi la fermeture du cycle se vérifie gratuitement, etc.).
 */
#include "greatest.h"

#include "tools/border_walk.h"
#include "core/core_static_variables.h"

#include <string.h>

TEST border_ring_order_has_the_right_length_and_starts_at_origin(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    ASSERT_EQ_FMT(0, (int)ring[0][0], "%d");
    ASSERT_EQ_FMT(0, (int)ring[0][1], "%d");
    PASS();
}

TEST border_ring_order_visits_every_perimeter_cell_exactly_once(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    int seen[ETERN_SIZE][ETERN_SIZE];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int x = ring[i][0];
        int y = ring[i][1];
        ASSERT(x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1);
        ASSERT_EQ_FMT(0, seen[x][y], "%d");
        seen[x][y] = 1;
    }

    int total = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1) {
                ASSERT_EQ_FMT(1, seen[x][y], "%d");
                total++;
            }
        }
    }
    ASSERT_EQ_FMT(BORDER_RING_LEN, total, "%d");
    PASS();
}

TEST border_ring_order_is_a_closed_walk_of_adjacent_cells(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int j = (i + 1) % BORDER_RING_LEN;
        int dx = ring[j][0] - ring[i][0];
        int dy = ring[j][1] - ring[i][1];
        int manhattan = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        ASSERT_EQ_FMT(1, manhattan, "%d");
    }
    PASS();
}

SUITE(border_walk_suite)
{
    RUN_TEST(border_ring_order_has_the_right_length_and_starts_at_origin);
    RUN_TEST(border_ring_order_visits_every_perimeter_cell_exactly_once);
    RUN_TEST(border_ring_order_is_a_closed_walk_of_adjacent_cells);
}
