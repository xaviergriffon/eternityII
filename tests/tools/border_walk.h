/**
 * @file border_walk.h
 * @brief Énumération exhaustive des anneaux de bordure valides du plateau.
 *
 * Cœur pur de l'outil `tests/tools/border_mass.c` (voir
 * docs/superpowers/specs/2026-09-06-masse-bordure-design.md) : aucune
 * entrée/sortie, testable en isolation. Réutilise tel quel l'infrastructure
 * de lookup de `core/part.h`/`core/possibility.h` — aucune fonction nouvelle
 * n'y est nécessaire.
 */
#ifndef eternityII_border_walk_h
#define eternityII_border_walk_h

#include "core/part.h"
#include "core/possibility.h"

/** @brief Nombre de cases du pourtour du plateau (4 coins + bords). */
#define BORDER_RING_LEN (4 * (ETERN_SIZE - 1))

/**
 * @brief Remplit `ring` avec les BORDER_RING_LEN cases du pourtour, dans le
 * sens horaire, en partant de `(0,0)`.
 *
 * Ordre : ligne du haut (`(0,0)` → `(ETERN_SIZE-1,0)`), colonne droite
 * (`(ETERN_SIZE-1,1)` → `(ETERN_SIZE-1,ETERN_SIZE-1)`), ligne du bas
 * (`(ETERN_SIZE-2,ETERN_SIZE-1)` → `(0,ETERN_SIZE-1)`), colonne gauche
 * (`(0,ETERN_SIZE-2)` → `(0,1)`). La dernière case, `(0,1)`, est donc
 * adjacente à `(0,0)` — la fermeture du cycle.
 *
 * @param ring Tableau de sortie, `BORDER_RING_LEN` couples `(x,y)`.
 */
void border_ring_order(int8_t ring[BORDER_RING_LEN][2]);

#endif /* eternityII_border_walk_h */
