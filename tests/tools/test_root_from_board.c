/*
 * Tests de tests/tools/root_from_board.c — le cœur pur de l'outil gen_root.
 *
 * Ce qui est verrouillé ici, ce sont les deux conventions qu'un outil externe
 * a toutes les chances d'inverser sans que rien ne proteste :
 *   1. l'orientation `grid[colonne][ligne]` (et non l'inverse) ;
 *   2. l'indice de rotation, qui n'est jamais calculé mais RETROUVÉ dans la
 *      table de rotate_all_parts par correspondance des 4 faces.
 * Chaque test porte sa contre-épreuve : une convention inversée doit faire
 * échouer l'assertion, pas produire un plateau plausible.
 *
 * Le jeu de pièces est SYNTHÉTISÉ avec exactement ETERN_PARTS pièces (et non
 * lu depuis data/) : rotate_all_parts indexe en `id + ETERN_PARTS * r`, donc
 * une fixture plus courte déborderait son allocation — cf. tests/README.md.
 */
#include "greatest.h"

#include "tools/root_from_board.h"
#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/core_static_variables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Jeu de ETERN_PARTS pièces sans symétrie de rotation (4 faces distinctes),
   pour que « la rotation retrouvée » soit une information non ambiguë. */
static struct array_part *rfb_make_rotate_parts(void)
{
    char path[] = "/tmp/etii_rfb_pieces_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fprintf(fp, "ntiles: %d\n", ETERN_PARTS);
    for (int i = 1; i <= ETERN_PARTS; i++) {
        int base = i % 7;
        /* read_parts attend : id top left bottom right */
        fprintf(fp, "%d %d %d %d %d\n", i, base + 1, base + 2, base + 3, base + 4);
    }
    fclose(fp);
    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

/* Remplit `cells` de cases vides, puis y place la pièce `id` dans la rotation
   `rot` (faces lues dans la table) à la ligne/colonne demandée. */
static void rfb_put(root_board_cell *cells, const struct array_part *all,
                    int id, int rot, int row, int col)
{
    const struct part *q = &all->parts[id + ETERN_PARTS * rot];
    root_board_cell *c = &cells[row * ETERN_SIZE + col];
    c->id = id;
    c->top = q->top;
    c->right = q->right;
    c->bottom = q->bottom;
    c->left = q->left;
}

TEST root_from_board_writes_grid_column_major(void)
{
    struct array_part *all = rfb_make_rotate_parts();
    ASSERT(all != NULL);
    root_board_cell *cells = calloc((size_t)ETERN_SIZE * ETERN_SIZE, sizeof *cells);
    ASSERT(cells != NULL);

    /* Une seule pièce, en LIGNE 0 / COLONNE 1 : la case dissymétrique qui
       distingue grid[colonne][ligne] de grid[ligne][colonne]. */
    rfb_put(cells, all, 1, 0, 0, 1);

    struct possibility_packet p;
    char err[128] = {0};
    int placed = root_from_board(cells, all, &p, err, sizeof err);
    ASSERT_EQ_FMT(1, placed, "%d");
    ASSERT_EQ_FMT((uint16_t)1, p.alloc, "%u");
    /* grid[colonne][ligne] */
    ASSERT_EQ_FMT((int16_t)(1 + ETERN_PARTS * 0), p.grid[1][0], "%d");
    /* Contre-épreuve : la case transposée doit être restée VIDE. Sans elle, le
       test passerait aussi avec l'orientation inverse. */
    ASSERT_EQ_FMT((int16_t)-2, p.grid[0][1], "%d");

    free(cells);
    free_array_part(all);
    PASS();
}

TEST root_from_board_recovers_the_rotation_from_the_faces(void)
{
    struct array_part *all = rfb_make_rotate_parts();
    ASSERT(all != NULL);
    root_board_cell *cells = calloc((size_t)ETERN_SIZE * ETERN_SIZE, sizeof *cells);
    ASSERT(cells != NULL);

    /* La MÊME pièce, dans les 4 rotations, sur 4 cases distinctes : la valeur
       écrite doit porter l'indice de rotation d'origine à chaque fois. */
    for (int rot = 0; rot < 4; rot++) {
        memset(cells, 0, (size_t)ETERN_SIZE * ETERN_SIZE * sizeof *cells);
        rfb_put(cells, all, 2, rot, 1, 2);
        struct possibility_packet p;
        char err[128] = {0};
        ASSERT_EQ_FMT(1, root_from_board(cells, all, &p, err, sizeof err), "%d");
        ASSERT_EQ_FMT((int16_t)(2 + ETERN_PARTS * rot), p.grid[2][1], "%d");
    }

    /* Contre-épreuve : des faces qu'aucune rotation ne produit sont un ÉCHEC
       explicite, jamais un placement approximatif. */
    memset(cells, 0, (size_t)ETERN_SIZE * ETERN_SIZE * sizeof *cells);
    rfb_put(cells, all, 2, 0, 1, 2);
    cells[1 * ETERN_SIZE + 2].top = 99;
    struct possibility_packet p;
    char err[128] = {0};
    ASSERT_EQ_FMT(-1, root_from_board(cells, all, &p, err, sizeof err), "%d");
    ASSERT(strstr(err, "aucune rotation") != NULL);

    free(cells);
    free_array_part(all);
    PASS();
}

TEST root_from_board_tracks_used_pieces_and_empty_cells(void)
{
    struct array_part *all = rfb_make_rotate_parts();
    ASSERT(all != NULL);
    root_board_cell *cells = calloc((size_t)ETERN_SIZE * ETERN_SIZE, sizeof *cells);
    ASSERT(cells != NULL);

    rfb_put(cells, all, 1, 0, 0, 0);
    rfb_put(cells, all, 3, 1, 2, 1);

    struct possibility_packet p;
    char err[128] = {0};
    ASSERT_EQ_FMT(2, root_from_board(cells, all, &p, err, sizeof err), "%d");
    ASSERT_EQ_FMT((uint16_t)2, p.alloc, "%u");
    /* b_faceused est indexé en base 0 (id - 1). */
    ASSERT(is_face_used(p.b_faceused, 0));
    ASSERT(is_face_used(p.b_faceused, 2));
    ASSERT(!is_face_used(p.b_faceused, 1));
    /* Toutes les autres cases restent à -2, et checked à 0 (pool non vérifié). */
    int empty = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (p.grid[x][y] == -2) empty++;
        }
    }
    ASSERT_EQ_FMT(ETERN_SIZE * ETERN_SIZE - 2, empty, "%d");
    ASSERT_EQ_FMT((uint8_t)0, p.checked, "%u");
    ASSERT_EQ_FMT((int16_t)POSSIBILITY_MIN_CANDIDATS_UNKNOWN, p.min_candidats, "%d");

    /* Contre-épreuve : la même pièce deux fois est refusée — sans ce contrôle,
       b_faceused mentirait sur le stock de pièces encore libres. */
    rfb_put(cells, all, 1, 2, 3, 3);
    ASSERT_EQ_FMT(-1, root_from_board(cells, all, &p, err, sizeof err), "%d");
    ASSERT(strstr(err, "deux fois") != NULL);

    free(cells);
    free_array_part(all);
    PASS();
}

TEST root_from_board_rejects_out_of_range_ids(void)
{
    struct array_part *all = rfb_make_rotate_parts();
    ASSERT(all != NULL);
    root_board_cell *cells = calloc((size_t)ETERN_SIZE * ETERN_SIZE, sizeof *cells);
    ASSERT(cells != NULL);

    cells[0].id = ETERN_PARTS + 1;
    struct possibility_packet p;
    char err[128] = {0};
    ASSERT_EQ_FMT(-1, root_from_board(cells, all, &p, err, sizeof err), "%d");
    ASSERT(strstr(err, "hors bornes") != NULL);

    free(cells);
    free_array_part(all);
    PASS();
}

SUITE(root_from_board_suite)
{
    RUN_TEST(root_from_board_writes_grid_column_major);
    RUN_TEST(root_from_board_recovers_the_rotation_from_the_faces);
    RUN_TEST(root_from_board_tracks_used_pieces_and_empty_cells);
    RUN_TEST(root_from_board_rejects_out_of_range_ids);
}
