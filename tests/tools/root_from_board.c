#include "tools/root_from_board.h"

#include <stdio.h>
#include <string.h>

#include "core/core_static_variables.h"

int root_from_board(const root_board_cell *cells,
                    const struct array_part *all_rotate,
                    struct possibility_packet *out,
                    char *err, size_t errsz)
{
    if (cells == NULL || all_rotate == NULL || out == NULL) {
        if (err != NULL && errsz > 0) {
            snprintf(err, errsz, "root_from_board : argument NULL");
        }
        return -1;
    }

    memset(out, 0, sizeof(*out));
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            out->grid[x][y] = -2;
        }
    }
    // x/y sont sans objet depuis VERSION 13 (cf. possibility.h) : laissés à 0,
    // comme first_possibility les laisse à directions[0].
    out->x = 0;
    out->y = 0;
    out->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
    // Une racine construite hors du moteur n'a été vérifiée par aucun pruner :
    // elle part dans le pool NON vérifié, comme tout produit d'expansion.
    out->checked = 0;

    int placed = 0;
    for (int row = 0; row < ETERN_SIZE; row++) {
        for (int col = 0; col < ETERN_SIZE; col++) {
            const root_board_cell *c = &cells[row * ETERN_SIZE + col];
            if (c->id == 0) {
                continue;
            }
            if (c->id < 0 || c->id > ETERN_PARTS) {
                if (err != NULL && errsz > 0) {
                    snprintf(err, errsz, "pièce %d hors bornes en (l%d,c%d)", c->id, row, col);
                }
                return -1;
            }
            int found = -1;
            for (int rot = 0; rot < 4; rot++) {
                int pos = c->id + ETERN_PARTS * rot;
                if (pos >= all_rotate->size) {
                    break;
                }
                const struct part *q = &all_rotate->parts[pos];
                if (q->id == c->id && q->top == c->top && q->right == c->right
                    && q->bottom == c->bottom && q->left == c->left) {
                    found = pos;
                    break;
                }
            }
            if (found < 0) {
                if (err != NULL && errsz > 0) {
                    snprintf(err, errsz,
                             "pièce %d : aucune rotation ne donne (%d %d %d %d) en (l%d,c%d)",
                             c->id, c->top, c->right, c->bottom, c->left, row, col);
                }
                return -1;
            }
            if (is_face_used(out->b_faceused, (uint16_t)(c->id - 1))) {
                if (err != NULL && errsz > 0) {
                    snprintf(err, errsz, "pièce %d posée deux fois (l%d,c%d)", c->id, row, col);
                }
                return -1;
            }
            // grid[x][y] : x = colonne, y = ligne.
            out->grid[col][row] = (int16_t)found;
            set_face_used(out->b_faceused, (uint16_t)(c->id - 1), 1);
            placed++;
        }
    }
    // `alloc` porte le NOMBRE de pièces posées (canonique depuis VERSION 13).
    out->alloc = (uint16_t)placed;
    return placed;
}
