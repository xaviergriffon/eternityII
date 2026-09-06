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
#include "core/readdata.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

/* Bases choisies pour rester à l'intérieur de int8_t (les faces de `struct
   part` sont des int8_t, cf. src/core/part.h) tout en restant juste après la
   plage des leurres ([1,10]) : maxFace = BW_EDGE_BASE + BORDER_RING_LEN - 1
   ≈ 71 sur le jeu 256 pièces (contre 22 pour data/pieces.csv), gardant la
   table de lookup de la fixture proche de l'échelle de production. */
#define BW_EDGE_BASE 12
#define BW_INTERIOR_PLACEHOLDER 11

/* Couleur requise sur la face de `ring[i]` tournée vers `(nx,ny)` : 0 hors
   grille, une couleur unique par arête de l'anneau si `(nx,ny)` est le
   voisin précédent/suivant dans l'anneau, un joker constant sinon (case
   intérieure — jamais vérifiée par le DFS, la valeur exacte importe peu). */
static int bw_required_face(const int8_t ring[BORDER_RING_LEN][2], int i, int nx, int ny)
{
    if (nx < 0 || nx >= ETERN_SIZE || ny < 0 || ny >= ETERN_SIZE) {
        return 0;
    }
    int prev = (i - 1 + BORDER_RING_LEN) % BORDER_RING_LEN;
    int next = (i + 1) % BORDER_RING_LEN;
    if (ring[prev][0] == nx && ring[prev][1] == ny) {
        return BW_EDGE_BASE + prev;
    }
    if (ring[next][0] == nx && ring[next][1] == ny) {
        return BW_EDGE_BASE + i;
    }
    return BW_INTERIOR_PLACEHOLDER;
}

/* Écrit un jeu de ETERN_PARTS pièces (fichier temporaire, format read_parts)
   et retourne rotate_all_parts() dessus. Si `with_unique_ring` est vrai, les
   BORDER_RING_LEN premières pièces (id 1..BORDER_RING_LEN) forment l'UNIQUE
   anneau de bordure valide possible : chaque arête de l'anneau porte une
   couleur qui n'apparaît que sur les deux pièces qui la partagent, donc
   aucune pièce ne peut se substituer à une autre position. Toutes les autres
   pièces (et la totalité si `with_unique_ring` est faux) sont des leurres
   sans face à 0 — jamais candidats sur le bord, qui exige toujours au moins
   un 0. */
static struct array_part *bw_make_rotate_parts(int with_unique_ring)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    char path[] = "/tmp/etii_bw_pieces_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fprintf(fp, "ntiles: %d\n", ETERN_PARTS);

    for (int id = 1; id <= ETERN_PARTS; id++) {
        int top, right, bottom, left;
        if (with_unique_ring && id <= BORDER_RING_LEN) {
            int i = id - 1;
            int x = ring[i][0];
            int y = ring[i][1];
            top = bw_required_face(ring, i, x, y - 1);
            right = bw_required_face(ring, i, x + 1, y);
            bottom = bw_required_face(ring, i, x, y + 1);
            left = bw_required_face(ring, i, x - 1, y);
        } else {
            int base = id % 7;
            top = base + 1;
            left = base + 2;
            bottom = base + 3;
            right = base + 4;
        }
        /* read_parts attend : id top left bottom right */
        fprintf(fp, "%d %d %d %d %d\n", id, top, left, bottom, right);
    }
    fclose(fp);

    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

struct bw_found_record {
    int calls;
    struct possibility_packet last;
};

static void bw_on_found(const struct possibility_packet *ring_state, void *ctx)
{
    struct bw_found_record *rec = (struct bw_found_record *)ctx;
    rec->calls++;
    rec->last = *ring_state;
}

TEST border_walk_count_returns_zero_without_any_border_shaped_piece(void)
{
    struct array_part *all = bw_make_rotate_parts(0);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long n = border_walk_count(map, all, NULL, NULL);
    ASSERT_EQ_FMT(0LL, n, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Une pièce crée un anneau valide UNIQUE (en tant qu'objet cyclique abstrait)
   quand `with_unique_ring` est vrai — et pourtant border_walk_count doit
   retourner 4, pas 1 : le DFS ne fixe aucune pièce à l'ouverture en (0,0), et
   à cette toute première étape aucun voisin n'est encore posé, donc n'importe
   lequel des 4 coins de l'anneau peut servir de point d'ouverture. Chacun
   produit un placement complet différent sur la grille absolue (le même
   anneau, mais tourné) — c'est exactement le mécanisme dont dépend la
   masse totale (`N` EST la masse, pas `4×N`, cf. la spec). Vérifié
   empiriquement (12 et 60 cases) avant d'écrire cette assertion — ne pas la
   « corriger » vers 1 sans revérifier. */
TEST border_walk_count_finds_the_crafted_ring_once_per_opening_corner(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    struct bw_found_record rec;
    memset(&rec, 0, sizeof rec);
    long long n = border_walk_count(map, all, bw_on_found, &rec);

    ASSERT_EQ_FMT(4LL, n, "%lld");
    ASSERT_EQ_FMT(4, rec.calls, "%d");
    ASSERT_EQ_FMT(BORDER_RING_LEN, possibility_placed_count(&rec.last), "%d");

    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        ASSERT(rec.last.grid[ring[i][0]][ring[i][1]] != -2);
    }
    /* Contre-épreuve : le centre du plateau (jamais un bord) doit être resté
       vide — le walker ne pose jamais l'intérieur. */
    ASSERT_EQ_FMT((int16_t)-2, rec.last.grid[ETERN_SIZE / 2][ETERN_SIZE / 2], "%d");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

SUITE(border_walk_suite)
{
    RUN_TEST(border_ring_order_has_the_right_length_and_starts_at_origin);
    RUN_TEST(border_ring_order_visits_every_perimeter_cell_exactly_once);
    RUN_TEST(border_ring_order_is_a_closed_walk_of_adjacent_cells);
    RUN_TEST(border_walk_count_returns_zero_without_any_border_shaped_piece);
    RUN_TEST(border_walk_count_finds_the_crafted_ring_once_per_opening_corner);
}
