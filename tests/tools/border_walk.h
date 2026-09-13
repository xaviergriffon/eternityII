/**
 * @file border_walk.h
 * @brief Énumération exhaustive des anneaux de bordure valides du plateau.
 *
 * Cœur pur de l'outil `tests/tools/border_mass.c` (voir
 * docs/conception/border_mass.md) : aucune
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
 * @brief Remplit `order` avec les BORDER_RING_LEN cases du pourtour, les 4
 * coins en premier (dans leur ordre `border_ring_order`), puis les cases de
 * bord dans leur ordre `border_ring_order` habituel.
 *
 * Ne change aucune propriété de comptage de `border_walk_count_ordered` : le
 * raisonnement « N est déjà la masse » (aucun voisin posé à la toute
 * première case, donc n'importe lequel des 4 coins peut ouvrir la
 * recherche) tient toujours — les 4 coins sont simplement tous posés tôt au
 * lieu qu'un seul le soit et que les 3 autres se déduisent en refermant le
 * cycle. Sert à réduire drastiquement le facteur de branchement initial :
 * très peu de pièces ont 2 faces nulles adjacentes (4 sur le jeu 256
 * pièces réel), contre des dizaines de candidats pour une case de bord
 * arbitraire.
 *
 * @param order Tableau de sortie, `BORDER_RING_LEN` couples `(x,y)`.
 */
void border_corners_first_order(int8_t order[BORDER_RING_LEN][2]);

/**
 * @brief Appelé pour chaque anneau de bordure fermé trouvé par
 * `border_walk_count`. `ring_state` n'est valide que pendant l'appel (le
 * DFS continue son backtracking juste après) — le copier si on veut le
 * garder.
 *
 * @return 0 pour continuer l'énumération, non nul pour l'ARRÊTER net.
 *
 * L'arrêt est ce qui rend l'échantillonnage possible : la population
 * d'anneaux du jeu réel (~10³⁷) ne s'énumère jamais entièrement, mais le
 * walker en ferme ~15 M/s, et un appelant qui n'en veut que N doit
 * pouvoir rendre la main après le N-ième au lieu de laisser tourner un DFS
 * qui ne se terminera pas. L'arrêt remonte toute la récursion : le compte
 * retourné par `border_walk_count`/`_ordered` est alors partiel (les
 * anneaux livrés jusque-là), jamais une masse totale.
 */
typedef int (*border_ring_found_cb)(const struct possibility_packet *ring_state, void *ctx);

/**
 * @brief Suivi de progression pour `border_walk_count_ordered` — appelé tous
 * les `interval_nodes` nœuds DFS entièrement traités (une case candidate
 * essayée, qu'elle mène à un anneau ou à un échec, backtrack compris ;
 * signalé en post-ordre, une fois le sous-arbre du nœud épuisé, pour que
 * `rings_found` soit toujours à jour au moment de l'appel — y compris pour
 * le nœud qui vient de fermer le dernier anneau). Aucune horloge lue ici :
 * `border_walk.c` reste un cœur pur sans I/O, c'est l'appelant qui décide
 * quoi faire du signal (mesurer une vitesse, journaliser une ligne…).
 * `interval_nodes` doit être > 0 pour être actif ; passer `progress = NULL`
 * à `border_walk_count_ordered` désactive tout suivi (coût nul).
 */
struct border_progress_opts {
    long long interval_nodes;
    void (*on_progress)(long long nodes_visited, long long rings_found, void *ctx);
    void *ctx;
};

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
 * @param progress         Suivi de progression optionnel (peut être NULL).
 * @return                 Nombre d'anneaux trouvés en complétant depuis
 *                         `start_state`/`start_depth`.
 */
long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx,
                                     const struct border_progress_opts *progress);

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

/**
 * @brief Appelé pour chaque état partiel de la frontière finale produite par
 * `border_walk_expand_frontier`. `partial_state` n'est valide que pendant
 * l'appel — le copier si on veut le garder (c'est un besoin réel : ces
 * états sont ensuite distribués à des workers forkés).
 */
typedef void (*border_partial_cb)(const struct possibility_packet *partial_state,
                                   int depth, void *ctx);

/**
 * @brief Étend le plateau en largeur (BFS, un niveau entier à la fois)
 * jusqu'à ce que le nombre d'états partiels atteigne `target_partitions`,
 * ou que `BORDER_RING_LEN` soit atteint (jeu de pièces trop petit pour
 * produire assez de partitions).
 *
 * Développe toujours un niveau ENTIER avant de tester la cible — jamais
 * coupé en cours de route — pour ne pas biaiser la frontière vers les
 * premières branches explorées dans `order`.
 *
 * Analogue en miniature de `expand_datas_to_level` (`core/datamanager.c`) :
 * même idée (peupler un ensemble d'états à répartir), sans stock ni
 * persistance — tout tient en mémoire, le temps de l'appel.
 *
 * @param map                Table de lookup pré-calculée.
 * @param all_rotate_parts   Tableau de toutes les rotations.
 * @param order              Ordre de parcours (`border_ring_order` ou
 *                           `border_corners_first_order`).
 * @param target_partitions  Nombre d'états partiels visés (peut être
 *                           dépassé : un niveau entier est toujours
 *                           développé en une fois).
 * @param on_partial         Appelé pour chaque état partiel de la frontière
 *                           finale, avec sa profondeur (l'index dans
 *                           `order` à partir duquel `border_walk_count_ordered`
 *                           doit reprendre). Peut être NULL.
 * @param partial_ctx        Passé tel quel à `on_partial`.
 * @param on_complete        Appelé pour chaque anneau complet trouvé
 *                           PENDANT l'expansion (jeu de pièces trop petit
 *                           pour atteindre `target_partitions` sans épuiser
 *                           l'arbre) — ne jamais compter ces anneaux une
 *                           deuxième fois côté appelant. Peut être NULL.
 *                           S'il demande l'arrêt (retour non nul),
 *                           l'expansion s'interrompt immédiatement et
 *                           `on_partial` n'est appelé pour AUCUN état : il
 *                           n'y a plus de travail à distribuer.
 * @param complete_ctx       Passé tel quel à `on_complete`.
 * @return                   Nombre d'anneaux comptés via `on_complete`
 *                           pendant l'expansion elle-même (0 dans le cas
 *                           courant où la frontière est atteinte avant
 *                           d'épuiser l'arbre).
 */
long long border_walk_expand_frontier(map_big_array *map,
                                       struct array_part *all_rotate_parts,
                                       const int8_t order[BORDER_RING_LEN][2],
                                       int target_partitions,
                                       border_partial_cb on_partial, void *partial_ctx,
                                       border_ring_found_cb on_complete, void *complete_ctx);

#endif /* eternityII_border_walk_h */
