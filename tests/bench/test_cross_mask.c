/*
 * Suite de la croix séparatrice (tests/bench/cross_mask.c).
 *
 * La croix est définie EN COMPRÉHENSION précisément pour être vérifiable
 * plutôt que recopiée (cf. l'en-tête de cross_mask.h et §3 de
 * docs/conception/croix_separatrice_ordre_variables.md). Cette suite est donc
 * la contrepartie de ce choix : elle ne relit pas la formule, elle contrôle
 * les propriétés dont le document se sert — séparation en quatre régions,
 * symétrie, présence des indices officiels, et la taille mesurée au 16×16.
 *
 * Deux oracles sont volontairement INDÉPENDANTS de l'implémentation :
 * l'appartenance est recalculée par distance de Manhattan à une diagonale
 * (parcours explicite des cases diagonales, pas la formule `|x−y| ≤ 1`), et
 * les régions sont comptées par un remplissage par diffusion.
 */
#include "greatest.h"
#include "bench/cross_mask.h"

#include <string.h>

#include "core/core_static_variables.h"

/* --------------------------------------------------------------------------
 * Oracle 1 — appartenance par distance de Manhattan à une diagonale.
 *
 * Chemin de calcul distinct de celui de `cross_cell_on` : on énumère les
 * cases des deux diagonales et on cherche la plus proche, au lieu d'évaluer
 * `|x−y| ≤ 1`. Les deux doivent coïncider sur tout le plateau.
 * ------------------------------------------------------------------------ */
static int oracle_on_cross(int x, int y)
{
    for (int i = 0; i < ETERN_SIZE; i++) {
        int dx = x - i;
        int dy1 = y - i;                       /* diagonale y = x        */
        int dy2 = y - (ETERN_SIZE - 1 - i);    /* anti-diagonale y = n−1−x */
        if (dx < 0) dx = -dx;
        if (dy1 < 0) dy1 = -dy1;
        if (dy2 < 0) dy2 = -dy2;
        if (dx + dy1 <= 1 || dx + dy2 <= 1) {
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Oracle 2 — composantes connexes (4-voisinage) du complément de la croix.
 * ------------------------------------------------------------------------ */
static int region_sizes(int *sizes, int max_regions)
{
    uint8_t seen[ETERN_PARTS];
    int stack[ETERN_PARTS];
    int nb = 0;

    memset(seen, 0, sizeof(seen));
    for (int x0 = 0; x0 < ETERN_SIZE; x0++) {
        for (int y0 = 0; y0 < ETERN_SIZE; y0++) {
            if (cross_cell_on(x0, y0) || seen[x0 * ETERN_SIZE + y0]) {
                continue;
            }
            if (nb >= max_regions) {
                return -1; /* plus de régions que le test n'en attend */
            }
            int top = 0, count = 0;
            stack[top++] = x0 * ETERN_SIZE + y0;
            seen[x0 * ETERN_SIZE + y0] = 1;
            while (top > 0) {
                int pos = stack[--top];
                int x = pos / ETERN_SIZE, y = pos % ETERN_SIZE;
                count++;
                static const int dx[4] = {1, -1, 0, 0};
                static const int dy[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; d++) {
                    int nx = x + dx[d], ny = y + dy[d];
                    if (nx < 0 || ny < 0 || nx >= ETERN_SIZE || ny >= ETERN_SIZE) {
                        continue;
                    }
                    int np = nx * ETERN_SIZE + ny;
                    if (seen[np] || cross_cell_on(nx, ny)) {
                        continue;
                    }
                    seen[np] = 1;
                    stack[top++] = np;
                }
            }
            sizes[nb++] = count;
        }
    }
    return nb;
}

/* --------------------------------------------------------------------------
 * Géométrie
 * ------------------------------------------------------------------------ */

TEST cross_membership_matches_an_independent_oracle(void)
{
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            ASSERT_EQ_FMT(oracle_on_cross(x, y), cross_cell_on(x, y), "%d");
        }
    }
    PASS();
}

TEST cross_is_symmetric_under_the_board_reflections(void)
{
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            int on = cross_cell_on(x, y);
            ASSERT_EQ_FMT(on, cross_cell_on(ETERN_SIZE - 1 - x, y), "%d");
            ASSERT_EQ_FMT(on, cross_cell_on(x, ETERN_SIZE - 1 - y), "%d");
            ASSERT_EQ_FMT(on, cross_cell_on(y, x), "%d");
        }
    }
    PASS();
}

TEST cross_rejects_coordinates_outside_the_board(void)
{
    ASSERT_EQ_FMT(0, cross_cell_on(-1, 0), "%d");
    ASSERT_EQ_FMT(0, cross_cell_on(0, -1), "%d");
    ASSERT_EQ_FMT(0, cross_cell_on(ETERN_SIZE, 0), "%d");
    ASSERT_EQ_FMT(0, cross_cell_on(0, ETERN_SIZE), "%d");
    PASS();
}

/* La propriété qui porte toute la proposition : la croix est un SÉPARATEUR.
   Le 4×4 est le cas dégénéré — la croix y couvre le plateau entier, donc il
   n'y a aucune région et aucun mécanisme fondé sur elle n'y fait quoi que ce
   soit. Ce n'est pas une exemption du test : c'est l'assertion inverse, et
   c'est ce qui interdit d'aller mesurer ces variantes en build 16. */
TEST cross_separates_the_board_into_four_equal_regions(void)
{
    int sizes[ETERN_PARTS];
    int nb = region_sizes(sizes, ETERN_PARTS);

    if (ETERN_SIZE < 6) {
        ASSERT_EQ_FMT(ETERN_PARTS, cross_size(), "%d");
        ASSERT_EQ_FMT(0, nb, "%d");
        PASS();
    }
    ASSERT_EQ_FMT(4, nb, "%d");
    for (int i = 1; i < nb; i++) {
        ASSERT_EQ_FMT(sizes[0], sizes[i], "%d");
    }
    /* Et rien ne s'est perdu : croix + régions = plateau. */
    ASSERT_EQ_FMT(ETERN_PARTS, cross_size() + 4 * sizes[0], "%d");
    PASS();
}

/* Les indices sont une propriété de l'INSTANCE (cf. first_possibility), mais
   leurs POSITIONS sont géométriques et reproduites à toute taille par
   tools/gen_clone.py : (2,2) depuis chaque coin, plus l'indice géométrique au
   centre. C'est cette géométrie-là que la croix doit porter — la vérifier sans
   lire data/indices.csv garde la suite indépendante du répertoire courant. */
TEST cross_carries_the_official_hint_positions(void)
{
    const int n = ETERN_SIZE;
    ASSERT(cross_cell_on(2, 2));
    ASSERT(cross_cell_on(n - 3, 2));
    ASSERT(cross_cell_on(2, n - 3));
    ASSERT(cross_cell_on(n - 3, n - 3));
    /* Indice géométrique : (n/2 − 1, n/2), de somme n−1, donc sur l'anti-diagonale. */
    ASSERT(cross_cell_on(n / 2 - 1, n / 2));
    PASS();
}

#if ETERN_PARTS == 256
/* Les valeurs mesurées du §3/§4 du document de conception, verrouillées telles
   quelles : si la formule change, c'est ici que le document devient faux. */
TEST cross_has_eighty_eight_cells_and_four_regions_of_forty_two(void)
{
    int sizes[ETERN_PARTS];
    ASSERT_EQ_FMT(88, cross_size(), "%d");
    ASSERT_EQ_FMT(4, region_sizes(sizes, ETERN_PARTS), "%d");
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_FMT(42, sizes[i], "%d");
    }
    PASS();
}

/* Les cinq indices officiels de data/indices.csv, aux positions littérales. */
TEST cross_carries_the_five_official_hints_of_the_real_puzzle(void)
{
    static const int hints[5][2] = {{7, 8}, {2, 2}, {13, 2}, {2, 13}, {13, 13}};
    for (int i = 0; i < 5; i++) {
        ASSERT(cross_cell_on(hints[i][0], hints[i][1]));
    }
    PASS();
}
#endif // ETERN_PARTS == 256

/* --------------------------------------------------------------------------
 * Masques
 * ------------------------------------------------------------------------ */

TEST fill_writes_one_bit_per_cell_at_the_engine_index(void)
{
    uint8_t mask[ETERN_PARTS];
    int n = cross_fill(mask);

    ASSERT_EQ_FMT(cross_size(), n, "%d");
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            /* L'index est celui de BT_CELL_POS : x * ETERN_SIZE + y. */
            ASSERT_EQ_FMT(cross_cell_on(x, y), (int)mask[x * ETERN_SIZE + y], "%d");
        }
    }
    PASS();
}

TEST complement_is_the_exact_complement_of_the_cross(void)
{
    uint8_t on[ETERN_PARTS], off[ETERN_PARTS];
    int a = cross_fill(on);
    int b = cross_fill_complement(off);

    ASSERT_EQ_FMT(ETERN_PARTS, a + b, "%d");
    for (int i = 0; i < ETERN_PARTS; i++) {
        ASSERT_EQ_FMT(1, (int)(on[i] + off[i]), "%d");
    }
    PASS();
}

/* Densité de contrôle : celle de la croix elle-même (§6.2 — le contrôle
   aléatoire n'a de sens qu'à MÊME densité). Elle dépend donc de la taille
   compilée, et aucun compte littéral n'a sa place dans ces tests. */
TEST random_mask_draws_exactly_the_requested_count(void)
{
    uint8_t mask[ETERN_PARTS];
    const int want = cross_size();
    int n = cross_fill_random(mask, want, 12345);
    int levels = 0;

    ASSERT_EQ_FMT(want, n, "%d");
    for (int i = 0; i < ETERN_PARTS; i++) {
        ASSERT(mask[i] == 0 || mask[i] == 1);
        levels += mask[i];
    }
    ASSERT_EQ_FMT(want, levels, "%d");
    PASS();
}

TEST random_mask_is_reproducible_for_a_given_seed(void)
{
    uint8_t a[ETERN_PARTS], b[ETERN_PARTS], c[ETERN_PARTS];
    /* La moitié du plateau : le tirage le plus discriminant quelle que soit la
       taille compilée (en 4×4, cross_size() vaut ETERN_PARTS et tout tirage
       rendrait le même masque plein — le contre-contrôle ne dirait rien). */
    const int want = ETERN_PARTS / 2;

    cross_fill_random(a, want, 777);
    cross_fill_random(b, want, 777);
    ASSERT_EQ_FMT(0, memcmp(a, b, sizeof(a)), "%d");

    /* Contre-contrôle : une graine différente doit donner un autre tirage,
       faute de quoi le « contrôle aléatoire » du §6.2 ne contrôlerait rien.
       Plusieurs graines, parce qu'une collision isolée reste possible sur un
       petit plateau et ne serait pas un défaut du générateur. */
    int differs = 0;
    for (uint64_t seed = 778; seed < 788 && !differs; seed++) {
        cross_fill_random(c, want, seed);
        differs = (memcmp(a, c, sizeof(a)) != 0);
    }
    ASSERT(differs);
    PASS();
}

TEST random_mask_clamps_its_count_and_accepts_the_degenerate_cases(void)
{
    uint8_t mask[ETERN_PARTS];

    ASSERT_EQ_FMT(ETERN_PARTS, cross_fill_random(mask, ETERN_PARTS + 100, 1), "%d");
    ASSERT_EQ_FMT(0, cross_fill_random(mask, 0, 1), "%d");
    for (int i = 0; i < ETERN_PARTS; i++) {
        ASSERT_EQ_FMT(0, (int)mask[i], "%d");
    }
    ASSERT_EQ_FMT(0, cross_fill_random(mask, -3, 1), "%d");
    /* Graine 0 : état absorbant du xorshift, remplacée en interne — le tirage
       doit rester un tirage, pas un masque vide ou constant. */
    const int want = ETERN_PARTS / 2;
    ASSERT_EQ_FMT(want, cross_fill_random(mask, want, 0), "%d");
    int levels = 0;
    for (int i = 0; i < ETERN_PARTS; i++) {
        levels += mask[i];
    }
    ASSERT_EQ_FMT(want, levels, "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * Halo : le bras minimal (§8 du document de conception)
 * ------------------------------------------------------------------------ */

/** @brief Plateau vide sauf les cases données — la genèse, en miniature. */
static void board_with(int16_t grid[ETERN_SIZE][ETERN_SIZE],
                       const int cells[][2], int nb)
{
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            grid[x][y] = -2;
        }
    }
    for (int i = 0; i < nb; i++) {
        grid[cells[i][0]][cells[i][1]] = 1; /* une pièce quelconque : seule la présence compte */
    }
}

TEST halo_of_one_interior_piece_is_its_four_neighbours(void)
{
    int16_t grid[ETERN_SIZE][ETERN_SIZE];
    uint8_t mask[ETERN_PARTS];
    const int one[][2] = {{2, 2}};

    board_with(grid, one, 1);
    ASSERT_EQ_FMT(4, cross_fill_halo(mask, grid), "%d");
    ASSERT_EQ_FMT(1, (int)mask[1 * ETERN_SIZE + 2], "%d");
    ASSERT_EQ_FMT(1, (int)mask[3 * ETERN_SIZE + 2], "%d");
    ASSERT_EQ_FMT(1, (int)mask[2 * ETERN_SIZE + 1], "%d");
    ASSERT_EQ_FMT(1, (int)mask[2 * ETERN_SIZE + 3], "%d");
    /* La case posée elle-même n'est PAS dans son halo. */
    ASSERT_EQ_FMT(0, (int)mask[2 * ETERN_SIZE + 2], "%d");
    PASS();
}

/* Un indice collé au bord : le bord de plateau contraint la case, mais il
   n'est pas une pièce — le halo est plus petit, il ne déborde pas. */
TEST halo_of_a_corner_piece_has_only_two_cells(void)
{
    int16_t grid[ETERN_SIZE][ETERN_SIZE];
    uint8_t mask[ETERN_PARTS];
    const int corner[][2] = {{0, 0}};

    board_with(grid, corner, 1);
    ASSERT_EQ_FMT(2, cross_fill_halo(mask, grid), "%d");
    PASS();
}

#if ETERN_PARTS == 256
/* La propriété qui fonde le bras « halo » : sur la genèse du vrai puzzle, il
   fait 20 cases, et elles sont TOUTES sur la croix — compléter la croix
   consomme donc bien toutes les contraintes d'indice, mais en 88 cases au lieu
   de 20. */
TEST halo_of_the_official_hints_is_twenty_cells_all_on_the_cross(void)
{
    int16_t grid[ETERN_SIZE][ETERN_SIZE];
    uint8_t mask[ETERN_PARTS];
    const int hints[][2] = {{7, 8}, {2, 2}, {13, 2}, {2, 13}, {13, 13}};

    board_with(grid, hints, 5);
    ASSERT_EQ_FMT(20, cross_fill_halo(mask, grid), "%d");
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (mask[x * ETERN_SIZE + y]) {
                ASSERT(cross_cell_on(x, y));
            }
        }
    }
    PASS();
}
#endif // ETERN_PARTS == 256

TEST halo_of_an_empty_board_is_empty(void)
{
    int16_t grid[ETERN_SIZE][ETERN_SIZE];
    uint8_t mask[ETERN_PARTS];

    board_with(grid, NULL, 0);
    ASSERT_EQ_FMT(0, cross_fill_halo(mask, grid), "%d");
    PASS();
}

SUITE(cross_mask_suite)
{
    RUN_TEST(cross_membership_matches_an_independent_oracle);
    RUN_TEST(cross_is_symmetric_under_the_board_reflections);
    RUN_TEST(cross_rejects_coordinates_outside_the_board);
    RUN_TEST(cross_separates_the_board_into_four_equal_regions);
    RUN_TEST(cross_carries_the_official_hint_positions);
#if ETERN_PARTS == 256
    RUN_TEST(cross_has_eighty_eight_cells_and_four_regions_of_forty_two);
    RUN_TEST(cross_carries_the_five_official_hints_of_the_real_puzzle);
#endif
    RUN_TEST(fill_writes_one_bit_per_cell_at_the_engine_index);
    RUN_TEST(complement_is_the_exact_complement_of_the_cross);
    RUN_TEST(random_mask_draws_exactly_the_requested_count);
    RUN_TEST(random_mask_is_reproducible_for_a_given_seed);
    RUN_TEST(random_mask_clamps_its_count_and_accepts_the_degenerate_cases);
    RUN_TEST(halo_of_one_interior_piece_is_its_four_neighbours);
    RUN_TEST(halo_of_a_corner_piece_has_only_two_cells);
#if ETERN_PARTS == 256
    RUN_TEST(halo_of_the_official_hints_is_twenty_cells_all_on_the_cross);
#endif
    RUN_TEST(halo_of_an_empty_board_is_empty);
}
