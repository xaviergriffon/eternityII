/**
 * @file etii_search.h
 * @brief Méthodes pour la recherche de possibilités.
 */
#ifndef etii_search_h
#define etii_search_h

/**
 * @brief Recherche des possibilités
 * 
 * @param userdata contexte du thread client (le type attendu est client_possibility_t mais utilisation de void pour les thread)
 * @return void* null. Retourne un pointeur afin de respecter le format d'une méthode de thread.
 */
void *autosearch (void *userdata);

/**
 * @brief Thread de vérification d'un client pruner (mode `pruner`).
 *
 * @param userdata contexte du thread client (type attendu : client_possibility_t)
 * @return void* null.
 */
void *autoprune (void *userdata);

#ifdef WITH_CUDA
/**
 * @brief Variante GPU de `autoprune` (option `--gpu` du mode `pruner`, build CUDA uniquement).
 *
 * Même flux que `autoprune`, mais la boucle « pour chaque paquet :
 * possibility_all_has_a_next » est remplacée par un unique
 * `gpu_pruner_check_batch` sur tout le lot. Les vivants/morts sont renvoyés
 * comme dans `autoprune`. `gpu_pruner_init` doit avoir été appelé au préalable.
 *
 * @param userdata contexte du thread client (type attendu : client_possibility_t)
 * @return void* null.
 */
void *autoprune_gpu (void *userdata);
#endif // WITH_CUDA

#include "core/possibility.h"

/// Plafond de nœuds de la preuve courte d'expansion. Mesuré sur les enfants de
/// 20 000 parents tirés uniformément du stock du niveau 26 (48 915 enfants
/// gardés par l'élagage exact) : 4 nœuds ferment 11,05 % des enfants pour
/// 7,1 µs par enfant, 8 → 12,27 % pour 8,9 µs, 16 → 12,34 % pour 12,5 µs,
/// 32 → 12,36 % pour 19,7 µs. Au-delà de 8, on paie sans rien fermer.
/// Redéfinissable à la compilation pour la mesure (`-DEXPAND_PROOF_BUDGET=…`)
/// (docs/conception/etude_arbre_expansion_profonde.md).
#ifndef EXPAND_PROOF_BUDGET
#define EXPAND_PROOF_BUDGET 8
#endif

/**
 * @brief Preuve courte d'un enfant d'expansion (`datamanager_expand_proof_fn`) :
 *        la preuve bornée du pruner (`search_packet_backtracking_mrv`,
 *        délégation coupée), plafonnée à `EXPAND_PROOF_BUDGET` nœuds.
 *
 * Réentrante : tout l'état de la recherche est sur la pile, l'enfant n'est
 * pas modifié. Seules écritures partagées : les statistiques `counters[0]` et
 * `max_result`, comme le reste de l'expansion — sans effet sur un verdict.
 * `counters` doit être alloué (`init_counters`). Un plafond atteint ou un
 * arrêt demandé rend 0 : seul un sous-arbre épuisé est prouvé mort.
 *
 * @return 1 si le sous-arbre de `child` est prouvé sans solution, 0 sinon.
 */
int etii_expand_short_proof(const struct possibility_packet *child, map_big_array *map,
                            struct array_part *all_rotate_part, int16_t id_parts[][4],
                            unsigned long long *nodes);
#endif /* etii_search_h */
