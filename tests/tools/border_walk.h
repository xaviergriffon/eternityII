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

/**
 * @brief Appelé pour chaque anneau de bordure fermé trouvé par
 * `border_walk_count`. `ring_state` n'est valide que pendant l'appel (le
 * DFS continue son backtracking juste après) — le copier si on veut le
 * garder.
 */
typedef void (*border_ring_found_cb)(const struct possibility_packet *ring_state, void *ctx);

/**
 * @brief Variante de `border_walk_count` acceptant un ordre de parcours
 * explicite et un état de départ optionnel — permet de reprendre depuis un
 * état partiel produit par `border_walk_expand_frontier` (parallélisation
 * par forks) ou de rejouer le même DFS dans un ordre différent (coins
 * d'abord, `border_corners_first_order`).
 *
 * `border_walk_count` (ci-dessous) est un simple appel à cette fonction avec
 * `border_ring_order()` et un état vide — comportement inchangé, aucun test
 * existant à retoucher.
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`).
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param order            Ordre de parcours des BORDER_RING_LEN cases
 *                         (`border_ring_order` ou `border_corners_first_order`).
 * @param start_depth      Index dans `order` à partir duquel poser des
 *                         pièces (0 pour repartir d'un plateau vide).
 * @param start_state      Si non NULL, état du plateau déjà posé jusqu'à
 *                         `start_depth` (copié — jamais modifié). Si NULL,
 *                         part d'un plateau vide (équivalent à
 *                         `start_depth = 0`).
 * @param on_found         Appelé pour chaque anneau trouvé (peut être NULL).
 * @param ctx              Passé tel quel à `on_found`.
 * @return                 Nombre d'anneaux trouvés en complétant depuis
 *                         `start_state`/`start_depth`.
 */
long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx);

/**
 * @brief Énumère par recherche exhaustive tous les anneaux de bordure
 * valides, ancrés au coin `(0,0)`, dans l'ordre `border_ring_order`.
 *
 * Ne pose jamais de case intérieure : `what_search_in_grid_to_key` traite
 * alors le côté intérieur d'une pièce de bord comme joker, exactement le
 * comportement voulu. La fermeture du cycle (dernière case posée, `(0,1)`,
 * adjacente à `(0,0)` déjà posé) est vérifiée par ce même mécanisme, sans
 * code dédié.
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`).
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param on_found         Appelé pour chaque anneau trouvé (peut être NULL).
 * @param ctx              Passé tel quel à `on_found`.
 * @return                 N — la masse totale directement. Aucun voisin
 *                         n'est encore posé à la toute première case : le DFS
 *                         explore donc déjà les 4 coins possibles comme point
 *                         d'ouverture en (0,0), retrouvant chaque anneau
 *                         abstrait une fois par coin. Pas de ×4 à appliquer
 *                         en aval.
 */
long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx);

#endif /* eternityII_border_walk_h */
