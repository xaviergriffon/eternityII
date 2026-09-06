/*
 * gen_root — convertit un plateau externe en racine de stock (.back).
 *
 * Sert à injecter dans le stock d'un serveur un plateau qui ne vient pas de la
 * recherche : plateau publié, sauvegarde d'un autre solveur, réparation locale
 * d'un plateau connu (on retire quelques cases et on laisse le moteur rejouer
 * la région). Le fichier produit est au format `.back` — un dump brut de
 * `struct possibility_packet` — et se charge avec la commande console
 * `restore` (qui REMPLACE le stock) ou `import` (qui l'ajoute au stock
 * courant, paquets genèse compris).
 *
 * Toute la conversion vit dans tests/tools/root_from_board.c, testé
 * unitairement ; ce fichier n'est que l'enveloppe d'entrées/sorties.
 *
 * Entrée : ETERN_SIZE² lignes « id top right bottom left », en ordre
 *          LIGNE-MAJEUR (ligne 0 en premier), « 0 0 0 0 0 » pour une case vide.
 *
 * Usage :
 *   make gen-root
 *   tests/tools/gen_root data/pieces.csv plateau.txt racine.back
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/root_from_board.h"

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <pieces.csv> <plateau.txt> <sortie.back>\n", argv[0]);
        fprintf(stderr, "  plateau.txt : %d lignes « id top right bottom left », ordre ligne-majeur,\n",
                ETERN_SIZE * ETERN_SIZE);
        fprintf(stderr, "                « 0 0 0 0 0 » pour une case vide.\n");
        return 2;
    }

    struct array_part *apart = read_parts(argv[1]);
    struct array_part *all = rotate_all_parts(apart);

    root_board_cell *cells = calloc((size_t)ETERN_SIZE * ETERN_SIZE, sizeof *cells);
    if (cells == NULL) {
        fprintf(stderr, "gen_root : allocation du plateau impossible\n");
        return 1;
    }

    FILE *f = fopen(argv[2], "r");
    if (f == NULL) {
        perror(argv[2]);
        return 1;
    }
    for (int i = 0; i < ETERN_SIZE * ETERN_SIZE; i++) {
        if (fscanf(f, "%d %d %d %d %d", &cells[i].id, &cells[i].top,
                   &cells[i].right, &cells[i].bottom, &cells[i].left) != 5) {
            fprintf(stderr, "gen_root : plateau tronqué à la case %d (attendu %d cases)\n",
                    i, ETERN_SIZE * ETERN_SIZE);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    struct possibility_packet packet;
    char err[256] = {0};
    int placed = root_from_board(cells, all, &packet, err, sizeof err);
    free(cells);
    if (placed < 0) {
        fprintf(stderr, "gen_root : %s\n", err);
        return 1;
    }

    FILE *o = fopen(argv[3], "wb");
    if (o == NULL) {
        perror(argv[3]);
        return 1;
    }
    if (fwrite(&packet, sizeof packet, 1, o) != 1) {
        perror("gen_root : écriture");
        fclose(o);
        return 1;
    }
    fclose(o);

    printf("racine écrite : %s — %d pièces posées, %d cases libres, paquet %zu octets\n",
           argv[3], placed, ETERN_SIZE * ETERN_SIZE - placed, sizeof packet);
    return 0;
}
