/*
 * border_mass — mesure la masse totale des anneaux de bordure valides.
 *
 * Énumère par recherche exhaustive tous les anneaux de bordure valides
 * (BORDER_RING_LEN cases du pourtour du plateau), ancrés au coin (0,0), et
 * rapporte N — la masse totale directement. Aucun voisin n'est encore posé à
 * la toute première case : le DFS explore donc déjà les 4 coins possibles
 * comme point d'ouverture, retrouvant chaque anneau abstrait une fois par
 * coin — pas de ×4 supplémentaire à appliquer (constaté empiriquement
 * pendant l'implémentation, cf. docs/superpowers/specs/2026-09-06-masse-bordure-design.md
 * pour le raisonnement complet). Cela suppose qu'aucun indice officiel ne
 * touche le bord (vérifié ci-dessous) — sinon un seul coin serait valide,
 * pas 4.
 *
 * Toute la logique d'énumération vit dans tests/tools/border_walk.c, testée
 * unitairement ; ce fichier n'est que l'enveloppe d'entrées/sorties.
 *
 * Usage :
 *   make border-mass
 *   tests/tools/border_mass data/pieces.csv data/indices.csv
 */
#include <stdio.h>

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"

static int border_mass_check_indices_not_on_border(const struct array_index *indices)
{
    for (int i = 0; i < indices->size; i++) {
        int x = indices->indices[i].x;
        int y = indices->indices[i].y;
        if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1 ||
            x >= ETERN_SIZE || y >= ETERN_SIZE) {
            fprintf(stderr,
                    "border_mass : l'indice officiel id=%d est en (%d,%d), sur le bord — "
                    "un seul coin pourrait alors ouvrir la recherche, ce chiffre ne serait "
                    "plus la masse totale, refus de continuer\n",
                    indices->indices[i].id, x, y);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <pieces.csv> <indices.csv>\n", argv[0]);
        return 2;
    }

    struct array_index *indices = read_indices(argv[2]);
    int indices_ok = (border_mass_check_indices_not_on_border(indices) == 0);
    free_array_index(indices);
    if (!indices_ok) {
        return 1;
    }

    struct array_part *apart = read_parts(argv[1]);
    struct array_part *all = rotate_all_parts(apart);
    map_big_array *map = prepare_map_part(all);
    if (map == NULL) {
        fprintf(stderr, "border_mass : construction de la map de lookup impossible\n");
        return 1;
    }

    long long n = border_walk_count(map, all, NULL, NULL);
    printf("masse totale des anneaux de bordure valides : %lld\n", n);

    return 0;
}
