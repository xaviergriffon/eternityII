#ifndef eternityII_bench_solve_stats_h
#define eternityII_bench_solve_stats_h

/**
 * @file bench_solve_stats.h
 * @brief Cœur PUR du banc « côté trouver » (tests/bench/bench_solve.c).
 *
 * Aucune E/S, aucun global, aucune allocation : uniquement de l'agrégation sur
 * des tableaux fournis par l'appelant. C'est ce qui rend ces fonctions
 * testables comme n'importe quel module (`tests/bench/test_bench_solve_stats.c`,
 * rattaché à `make test`) alors que le banc lui-même ne l'est pas — même
 * découpage que `tests/tools/root_from_board.c` vis-à-vis de `gen_root`, et
 * même intention que `tests/bench/bench_lib.sh` côté shell.
 *
 * Toutes les fonctions décrivent la même grille de mesure : `n` exécutions,
 * `nodes[i]` nœuds explorés par la i-ème, `solved[i]` non nul si elle a atteint
 * la solution (0 = plafond atteint ou sous-arbre épuisé sans solution).
 *
 * Pourquoi médiane et moyenne GÉOMÉTRIQUE, jamais la moyenne arithmétique : la
 * distribution du temps jusqu'à la solution est à queue lourde (§3.3 de
 * docs/conception/banc_resolution_clones.md). Une seule exécution malchanceuse
 * y domine la moyenne arithmétique, qui ne classe alors plus rien.
 */

/**
 * @brief Tri croissant EN PLACE (tri par insertion).
 *
 * `n` vaut quelques dizaines à quelques centaines (instances × graines) : le
 * tri par insertion évite d'avoir à raisonner sur la stabilité et le
 * comparateur de `qsort` pour un gain nul à cette taille.
 */
void bench_stats_sort(double *v, int n);

/**
 * @brief Médiane d'un tableau DÉJÀ TRIÉ croissant.
 *
 * Exige le tri plutôt que de trier une copie : la fonction reste sans
 * allocation, et l'appelant trie de toute façon une fois pour toutes.
 *
 * @return 0 si `n == 0` ; moyenne des deux valeurs centrales si `n` est pair.
 */
double bench_stats_median_sorted(const double *v, int n);

/**
 * @brief Moyenne géométrique des valeurs STRICTEMENT POSITIVES.
 *
 * Les valeurs nulles ou négatives sont ignorées (le logarithme n'y est pas
 * défini) ; une exécution coûte au minimum un nœud, donc en pratique aucune
 * n'est écartée. @return 0 si aucune valeur exploitable.
 */
double bench_stats_geomean(const double *v, int n);

/**
 * @brief Part des exécutions RÉSOLUES en au plus `threshold` nœuds.
 *
 * C'est un point de la « courbe de survie » : la tracer pour
 * threshold = 10⁴, 10⁵, … jusqu'au plafond dit à quelle vitesse une politique
 * convertit du budget en solutions, là où la médiane seule n'en dit rien dès
 * qu'une moitié des exécutions échoue.
 *
 * @return fraction dans [0,1] ; 0 si `n == 0`.
 */
double bench_stats_survival(const double *nodes, const int *solved, int n, double threshold);

/**
 * @brief Comparaison APPARIÉE de deux politiques sur les mêmes exécutions.
 *
 * Seules les paires où les DEUX politiques ont résolu sont comparables : une
 * paire dont l'une au moins a buté sur le plafond compte comme indécise
 * (`undecided`), jamais comme une victoire — le plafond ne dit pas de combien
 * l'autre aurait perdu. C'est le même principe que la « comparaison appariée
 * sur les racines fermées par TOUS les moteurs » de `bench_refutation`.
 *
 * Moins de nœuds = victoire pour `a`. Les sorties acceptent NULL.
 */
void bench_stats_paired(const double *a_nodes, const int *a_solved,
                        const double *b_nodes, const int *b_solved,
                        int n, int *wins, int *losses, int *ties, int *undecided);

/** @brief Résultat d'une analyse de redémarrage à seuil fixe. */
typedef struct {
    /** P(résolu en moins de `cutoff` nœuds), estimée sur l'échantillon. */
    double p_success;
    /** E[nœuds | résolu avant le seuil]. 0 si aucune exécution n'y parvient. */
    double mean_below;
    /** Nombre d'exécutions résolues sous le seuil. */
    int samples_below;
    /** Coût attendu, en nœuds, d'une stratégie qui relance à `cutoff`.
     *  -1 si `p_success == 0` (le seuil ne mène jamais à une solution : le
     *  redémarrage boucle indéfiniment — surtout pas 0, qui se lirait comme
     *  « gratuit »). */
    double expected_nodes;
} bench_restart_t;

/**
 * @brief Coût attendu d'une stratégie de redémarrage au seuil `cutoff`.
 *
 * Chaque tentative coûte `min(N, cutoff)` ; elle réussit avec probabilité
 * `p = P(N < cutoff)`. Le nombre de tentatives suit une loi géométrique, d'où
 *
 *     E[coût] = (1 − p)/p × cutoff + E[N | N < cutoff]
 *
 * — (1−p)/p tentatives infructueuses en moyenne, à `cutoff` nœuds chacune,
 * puis la tentative gagnante à son coût propre.
 *
 * NOTE : §3.4 du document de conception écrit `(c + E[N | N < c]) / P(...)`.
 * Cette forme-là compte `c` sur la tentative GAGNANTE (qui s'arrête pourtant
 * avant d'atteindre le seuil, par définition) et divise en plus son coût
 * propre par `p`. Les deux corrections vont dans le même sens : elle
 * SURESTIME le coût du redémarrage, donc sous-estime son intérêt — l'exact
 * contraire de la prudence recherchée. La forme implémentée ici est celle de
 * la littérature ; l'écart est consigné dans le document.
 *
 * Ne rend jamais ce redémarrage OBLIGATOIRE : c'est une lecture de la courbe
 * de survie, pas un mécanisme. Aucun code de production n'en dépend.
 */
bench_restart_t bench_stats_restart(const double *nodes, const int *solved, int n, double cutoff);

#endif
