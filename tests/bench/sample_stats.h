/**
 * @file sample_stats.h
 * @brief Cœur pur de `tests/bench/bench_sample.c` : tirage uniforme des racines
 *        d'un stock et lecture statistique des tentatives de fermeture.
 *
 * Aucune entrée/sortie, pour être testable (`tests/bench/test_sample_stats.c`).
 * Trois choses s'y jouent qui fausseraient silencieusement une extrapolation :
 *
 *   - **l'uniformité du tirage** : un échantillonnage régulier (1 sur k, celui
 *     de `bench_refutation --kpi`) suit l'ordre des piles du stock, qui n'est
 *     pas aléatoire — l'expansion range les enfants d'un même parent côte à
 *     côte. Le réservoir (algorithme R) donne à chacune des N racines la même
 *     probabilité n/N, en UNE passe et sans connaître N d'avance ;
 *   - **le biais du modulo** : `rand() % s` favorise les petites valeurs dès
 *     que s ne divise pas 2⁶⁴ ; sur 6,7·10⁹ racines l'écart reste minuscule,
 *     mais il est gratuit de l'éviter (méthode de Lemire) ;
 *   - **la censure** : une racine non fermée sous le plafond a coûté AU MOINS
 *     le plafond, pas le plafond. La moyenne des coûts observés n'est alors
 *     qu'une BORNE BASSE du coût d'épuisement, et le résumé le dit
 *     (`censored`), au lieu de présenter une borne comme une estimation.
 */
#ifndef eternityII_sample_stats_h
#define eternityII_sample_stats_h

#include <stddef.h>
#include <stdint.h>

/** @brief Générateur splitmix64 : rapide, sans état partagé, reproductible. */
typedef struct {
    uint64_t state;
} sample_rng_t;

void sample_rng_seed(sample_rng_t *rng, uint64_t seed);
uint64_t sample_rng_next(sample_rng_t *rng);

/**
 * @brief Entier uniforme dans `[0, bound)`, sans biais de modulo (Lemire).
 * @param bound > 0.
 */
uint64_t sample_rng_below(sample_rng_t *rng, uint64_t bound);

/** @brief Réservoir de `capacity` places (algorithme R). */
typedef struct {
    uint64_t capacity;
    uint64_t seen;
    sample_rng_t rng;
} sample_reservoir_t;

void sample_reservoir_init(sample_reservoir_t *r, uint64_t capacity, uint64_t seed);

/**
 * @brief Présente l'élément suivant du flux.
 * @return La place où le RANGER (en écrasant ce qu'elle contient), ou -1 s'il
 *         n'est pas retenu. Les `capacity` premiers vont aux places 0, 1, …
 *         dans l'ordre ; ensuite le k-ième (k > capacity) est retenu avec la
 *         probabilité capacity/k, à une place uniforme.
 */
int64_t sample_reservoir_offer(sample_reservoir_t *r);

/** @brief Nombre d'éléments effectivement retenus (min(seen, capacity)). */
uint64_t sample_reservoir_size(const sample_reservoir_t *r);

/**
 * @brief Intervalle de Wilson à 95 % (z = 1,96) d'une proportion k/n.
 *
 * Wilson plutôt que l'intervalle normal : celui-ci donne [0, 0] pour 0/n, ce
 * qui affirmerait « aucune racine ne se ferme » sur un échantillon fini.
 * n = 0 rend [0, 1].
 */
void sample_wilson95(uint64_t k, uint64_t n, double *lo, double *hi);

/** @brief Issue d'une tentative de fermeture d'une racine. */
typedef enum {
    SAMPLE_CLOSED = 0,    /**< sous-arbre épuisé sans solution : mort, prouvé */
    SAMPLE_BUDGET = 1,    /**< plafond atteint : indéterminé (observation CENSURÉE) */
    SAMPLE_SOLUTION = 2,  /**< une solution a été rencontrée dans le sous-arbre */
    SAMPLE_ERROR = 3,     /**< la mesure n'a pas eu lieu (fils mort, arrêt) */
} sample_outcome_t;

typedef struct {
    sample_outcome_t outcome;
    unsigned long long nodes;
    double seconds;
    /** 1 si la racine a une case vide contrainte sans candidat libre : un
     *  enfant que l'expansion n'insère plus (`search_possiblity_light_pruned`). */
    uint8_t dead_at_insertion;
    /** 1 si le contrôle bon marché du pruner la tue (case morte, au point fixe
     *  du forçage des cases à candidat unique) — sans preuve bornée. */
    uint8_t dead_cheap;
} sample_result_t;

/** @brief Nombre de seuils de la fonction de répartition empirique. */
#define SAMPLE_ECDF_POINTS 12

/** @brief Lecture d'un ensemble de tentatives, toutes au même plafond. */
typedef struct {
    uint64_t n;            /**< tentatives valides (hors SAMPLE_ERROR) */
    uint64_t closed;
    uint64_t budget;
    uint64_t solutions;
    uint64_t errors;
    double closed_lo, closed_hi;  /**< Wilson 95 % de closed / n */
    /** Quantiles du coût (nœuds) des racines FERMÉES ; 0 si aucune. */
    unsigned long long q50, q90, q99, qmax;
    /** Moyenne de min(nœuds, plafond) sur les n tentatives, et son erreur type. */
    double mean_nodes, sem_nodes;
    /** 1 si au moins une tentative est censurée : `mean_nodes` n'est alors
     *  qu'une borne basse du coût moyen d'épuisement d'une racine. */
    int censored;
    double nodes_per_second;  /**< débit agrégé : Σ nœuds / Σ secondes */
    /** Fonction de répartition : part des n racines fermées en au plus
     *  `ecdf_at[i]` nœuds (puissances de 10 de 10¹ à 10¹²). Comme une fermeture
     *  est déterministe, UN run au plafond B donne toute la courbe sous B. */
    unsigned long long ecdf_at[SAMPLE_ECDF_POINTS];
    double ecdf[SAMPLE_ECDF_POINTS];
} sample_summary_t;

/**
 * @brief Résume `count` tentatives.
 * @param scratch Tampon de `count` entrées (tri des coûts), fourni par
 *                l'appelant pour garder ce module sans allocation.
 */
void sample_summarize(const sample_result_t *res, size_t count,
                      unsigned long long *scratch, sample_summary_t *out);

/** @brief Plafonds de la preuve bornée lus par `sample_sweep_tally` ; le
 *  dernier est le défaut du pruner (`PRUNER_DFS_BUDGET_DEFAULT`). */
#define SAMPLE_SWEEP_BUDGETS 3

/**
 * @brief Ce qu'un balayage pruner retirerait encore d'un stock dont
 *        l'expansion écarte déjà les enfants morts.
 *
 * Trois parts disjointes : les racines mortes à l'insertion (l'expansion ne
 * les produit plus), celles que seul le contrôle bon marché du pruner tue, et
 * — parmi les racines qui y survivent — celles que la preuve bornée ferme
 * sous chaque plafond. `residual_at[k]` = la deuxième + la troisième : la
 * valeur du balayage une fois l'expansion élaguée.
 */
typedef struct {
    uint64_t n;                    /**< tentatives valides (hors SAMPLE_ERROR) */
    uint64_t dead_at_insertion;
    uint64_t dead_cheap_only;
    unsigned long long budget_at[SAMPLE_SWEEP_BUDGETS];  /**< 100, 1 000, 10 000 */
    uint64_t dfs_closed_at[SAMPLE_SWEEP_BUDGETS];
    uint64_t residual_at[SAMPLE_SWEEP_BUDGETS];
} sample_sweep_t;

void sample_sweep_tally(const sample_result_t *res, size_t count, sample_sweep_t *out);

#endif /* eternityII_sample_stats_h */
