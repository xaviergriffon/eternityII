/**
 * @file root_from_board.h
 * @brief Conversion d'un plateau externe en `possibility_packet` (racine de stock).
 *
 * Cœur pur de l'outil `tests/tools/gen_root.c` : aucune entrée/sortie, pour
 * être testable — c'est ici que se jouent les deux conventions faciles à
 * inverser (l'orientation `grid[colonne][ligne]` et l'indice de rotation), et
 * elles sont verrouillées par `tests/tools/test_root_from_board.c`.
 */
#ifndef eternityII_root_from_board_h
#define eternityII_root_from_board_h

#include "core/part.h"
#include "core/possibility.h"

/** @brief Une case du plateau source, dans la numérotation de `data/pieces.csv`. */
typedef struct {
    /** Identifiant de la pièce, ou 0 pour une case vide. */
    int id;
    /** Les 4 faces attendues, dans l'orientation voulue sur le plateau. */
    int top;
    int right;
    int bottom;
    int left;
} root_board_cell;

/**
 * @brief Construit une racine de stock depuis un plateau décrit case par case.
 *
 * `cells` est lu en ordre LIGNE-MAJEUR (`cells[ligne * ETERN_SIZE + colonne]`),
 * et écrit dans `out->grid[colonne][ligne]` — l'orientation de
 * `first_possibility` et de `bt_frontier_init`, où `x` est la colonne et `y` la
 * ligne.
 *
 * La rotation n'est jamais calculée : elle est RETROUVÉE dans @p all_rotate en
 * cherchant l'entrée `id + ETERN_PARTS * r` dont les 4 faces correspondent.
 * Aucune convention de rotation n'est donc supposée ; une case dont aucune
 * rotation ne convient est un échec explicite, jamais un placement approximatif.
 *
 * @param cells      `ETERN_SIZE * ETERN_SIZE` cases, ordre ligne-majeur.
 * @param all_rotate Sortie de `rotate_all_parts()`.
 * @param out        Paquet construit (entièrement réinitialisé par l'appel).
 * @param err        Tampon de message d'erreur (peut être NULL).
 * @param errsz      Taille de @p err.
 * @return           Nombre de pièces posées (≥ 0), ou -1 en cas d'échec.
 */
int root_from_board(const root_board_cell *cells,
                    const struct array_part *all_rotate,
                    struct possibility_packet *out,
                    char *err, size_t errsz);

#endif /* eternityII_root_from_board_h */
