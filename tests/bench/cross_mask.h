#ifndef eternityII_cross_mask_h
#define eternityII_cross_mask_h

#include <stdint.h>

#include "core/core_static_variables.h"

/**
 * @file cross_mask.h
 * @brief La croix séparatrice : cœur PUR, partagé par les bancs et par `make test`.
 *
 * Géométrie proposée par
 * `docs/conception/croix_separatrice_ordre_variables.md` §3 : les deux
 * diagonales du plateau, épaissies à deux cases.
 *
 * ```
 * croix(n) = { (x,y) : |x − y| ≤ 1  ou  |x + y − (n−1)| ≤ 1 }
 * ```
 *
 * Définie **en compréhension** et non par une table, pour trois raisons :
 * elle vaut alors pour toute taille compilée (donc pour les clones de
 * `bench_solve`), elle est symétrique par construction, et elle se **vérifie**
 * (`tests/bench/test_cross_mask.c`) au lieu d'être recopiée.
 *
 * Deux propriétés mesurées au 16×16, verrouillées par ce test : la croix fait
 * **88 cases** et découpe le reste du plateau en **quatre régions de 42 cases
 * sans aucun passage orthogonal** ; elle porte les **cinq indices officiels**
 * (les quatre indices de coin sont à la position relative (2,2) de leur coin,
 * donc sur `y = x` ou `y = n−1−x`, et l'indice géométrique (7,8) vérifie
 * `7 + 8 = 15`).
 *
 * **Cas dégénéré à connaître** : en 4×4 la croix est le plateau ENTIER (16
 * cases sur 16, aucune région restante). Tout mécanisme qui s'appuie sur elle
 * y est donc un no-op — un build `ETERN_PARTS=16` ne peut rien mesurer de ces
 * variantes, et ne prouve rien à leur sujet.
 *
 * Ce fichier vit sous `tests/` et **n'est pas lié dans `./eternityII`** : même
 * découpage que `tests/bench/bench_solve_stats.c` et `tests/tools/root_from_board.c`
 * — un cœur pur, testé par `make test`, utilisé par un banc qui ne l'est pas.
 */

/**
 * @brief La case (x,y) appartient-elle à la croix séparatrice ?
 *
 * Coordonnées hors plateau : retourne 0 (aucun appelant n'en produit, la
 * garde évite d'avoir à le supposer).
 */
int cross_cell_on(int x, int y);

/** @brief Nombre de cases de la croix pour la taille compilée. */
int cross_size(void);

/**
 * @brief Remplit `out[x * ETERN_SIZE + y]` avec 0 ou 1 pour chaque case.
 *
 * L'index est celui de `BT_CELL_POS` (`src/core/etii_search.c`), de sorte que
 * le tableau s'injecte tel quel dans le balayage MRV du moteur.
 *
 * @param out Tableau d'au moins `ETERN_PARTS` octets.
 * @return    Nombre de cases de la croix (identique à `cross_size`).
 */
int cross_fill(uint8_t *out);

/**
 * @brief Remplit `out` avec une croix **inversée** : 1 hors croix, 0 dessus.
 *
 * Le contrôle « anti-croix » du §6.2 du document de conception : si préférer
 * les cases HORS croix gagne aussi, ce qui est mesuré n'est pas le séparateur.
 *
 * @return Nombre de cases hors croix.
 */
int cross_fill_complement(uint8_t *out);

/**
 * @brief Remplit `out` avec `count` cases tirées au sort, sans remise.
 *
 * Le contrôle « aléatoire » du §6.2 : le §4.14 de
 * `docs/conception/elagage_recherche.md` a mesuré qu'un départage **aléatoire**
 * bat déjà l'ordre positionnel de 37 à 56 %. Un bit inséré au-dessus du champ
 * de position perturbe cet ordre par construction — sans ce contrôle, à
 * **même densité**, un gain ne distingue pas « la croix est un bon a priori »
 * de « n'importe quoi vaut mieux que l'ordre des bits ».
 *
 * Tirage par mélange de Fisher-Yates sur un `xorshift64*` ensemencé par
 * `seed` : reproductible, et indépendant de la libc (même discipline que
 * `bench_rand`, `tests/bench/bench_solve.c`).
 *
 * @param out   Tableau d'au moins `ETERN_PARTS` octets.
 * @param count Nombre de cases à lever (borné à `ETERN_PARTS`).
 * @param seed  Graine ; 0 est remplacé par 1 (état absorbant du générateur).
 * @return      Nombre de cases levées.
 */
int cross_fill_random(uint8_t *out, int count, uint64_t seed);

/**
 * @brief Remplit `out` avec le HALO d'un plateau : les cases VIDES ayant au
 *        moins une voisine orthogonale posée.
 *
 * Appliqué au plateau de GENÈSE — vide hormis les indices de l'instance — il
 * donne exactement les cases sur lesquelles une contrainte d'indice porte : 20
 * cases pour les cinq indices du 16×16 officiel, toutes sur la croix.
 *
 * C'est le bras minimal de l'idée de croix. Si l'objectif est de consommer les
 * contraintes d'indice tôt, le halo fait en 20 cases ce que la croix fait en
 * 88 — et il est défini par l'INSTANCE (ses indices), pas par la géométrie du
 * plateau, donc il se transpose sans distorsion d'une taille à l'autre, ce que
 * la croix ne fait pas (cf. §4.4 du document de conception).
 *
 * Dérivé du plateau plutôt que des coordonnées des indices : aucune
 * duplication de la liste d'indices, et un indice posé en bord de plateau (que
 * `tools/gen_clone.py` ne produit pas, mais rien ne l'interdit) donne
 * naturellement un halo plus petit au lieu de déborder.
 *
 * @param out  Tableau d'au moins `ETERN_PARTS` octets, indexé `x * ETERN_SIZE + y`.
 * @param grid Grille du plateau ; `-2` marque une case vide (sentinelle du projet).
 * @return     Nombre de cases du halo.
 */
int cross_fill_halo(uint8_t *out, const int16_t grid[][ETERN_SIZE]);

#endif // eternityII_cross_mask_h
