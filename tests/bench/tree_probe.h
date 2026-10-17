/**
 * @file tree_probe.h
 * @brief Estimateur de Knuth sur l'arbre de l'expansion : taille de l'arbre
 *        par niveau, sans le parcourir ni le stocker.
 *
 * Une SONDE descend d'une racine en tirant à chaque nœud un enfant au hasard,
 * uniformément, parmi ceux que produit l'expansion
 * (`search_possiblity_light_pruned` : l'élagage exact, sans enfant mort). Son
 * poids au niveau L est le produit des nombres d'enfants rencontrés jusque-là
 * (Knuth, 1975) : son espérance est EXACTEMENT le nombre de nœuds du niveau L
 * sous la racine, pour tout L. L'arbre est défini par le plateau seul (MRV de
 * l'expansion, `light_choose_cell`), donc chaque sonde est indépendante.
 *
 * Le prix de l'absence de biais est une variance lourde : un nœud rare à fort
 * branchement porte un poids énorme. D'où l'erreur type rapportée à chaque
 * niveau, et l'usage de beaucoup de sondes.
 *
 * Testé par `tree_probe_suite` (tests/bench/test_tree_probe.c), dont un contrôle
 * d'exactitude contre l'énumération complète d'un petit arbre réel.
 */
#ifndef eternityII_tree_probe_h
#define eternityII_tree_probe_h

#include <stdint.h>

#include "bench/sample_stats.h"
#include "core/possibility.h"

/** @brief Niveaux 0..ETERN_PARTS (nombre de pièces posées). */
#define TREE_PROBE_LEVELS (ETERN_PARTS + 1)

/** @brief Accumulateur d'observations indépendantes de poids par niveau. */
typedef struct {
    uint64_t obs;
    double sum[TREE_PROBE_LEVELS];
    double sum2[TREE_PROBE_LEVELS];
    uint64_t reached[TREE_PROBE_LEVELS];  /**< observations de poids > 0 au niveau */
} tree_estimate_t;

void tree_estimate_init(tree_estimate_t *e);

/**
 * @brief Ajoute une observation : `w[L]` est son poids au niveau L (0 si non
 *        atteint). Une sonde, ou la moyenne de K sondes d'une même racine.
 */
void tree_estimate_add(tree_estimate_t *e, const double w[TREE_PROBE_LEVELS]);

void tree_estimate_merge(tree_estimate_t *dst, const tree_estimate_t *src);

/**
 * @brief Moyenne des poids au niveau `level`, multipliée par `scale`, et son
 *        erreur type (0 sous deux observations).
 */
void tree_estimate_level(const tree_estimate_t *e, int level, double scale, double *mean, double *se);

/**
 * @brief Une sonde depuis `root` : AJOUTE son poids à `w[L]` pour chaque niveau
 *        atteint (la racine compte pour 1).
 *
 * Le dernier niveau n'est pas développé par `search_possiblity_light_pruned`
 * (qui termine le processus sur un plateau complet) : les complétions d'une
 * case unique sont COMPTÉES, chacune étant une solution, sans être produites.
 *
 * @param stop_level Niveau où la sonde s'arrête (≤ 0 : jusqu'au bout) — les
 *        niveaux au-delà ne sont pas estimés.
 * @param capture_level Niveau auquel recopier le nœud atteint dans `*captured`
 *        et son poids dans `*captured_w` (≤ 0 : rien), pour mesurer le coût de
 *        son sous-arbre — l'espérance de poids × coût est le coût total du
 *        niveau. `*captured_w` reste à 0 si la sonde meurt avant.
 * @return Le dernier niveau atteint.
 */
int tree_probe(const struct possibility_packet *root, map_big_array *map, struct array_part *rot,
               int16_t id_parts[][4], sample_rng_t *rng, double w[TREE_PROBE_LEVELS], int stop_level,
               int capture_level, struct possibility_packet *captured, double *captured_w);

#endif /* eternityII_tree_probe_h */
