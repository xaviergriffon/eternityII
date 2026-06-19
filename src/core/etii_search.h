/**
 * @file etii_search.h
 * @brief Méthodes pour la recherche de possibilités.
 */
#ifndef etii_search_h
#define etii_search_h

#include <stdio.h>

/**
 * @brief Recherche des possibilités
 * 
 * @param userdata contexte du thread client (le type attendu est client_possibility_t mais utilisation de void pour les thread)
 * @return void* null. Retourne un pointeur afin de respecter le format d'une méthode de thread.
 */
void *autosearch (void *userdata);

/**
 * @brief Thread de vérification d'un client pruner (mode `tcppruner`).
 *
 * @param userdata contexte du thread client (type attendu : client_possibility_t)
 * @return void* null.
 */
void *autoprune (void *userdata);

#ifdef WITH_CUDA
/**
 * @brief Variante GPU de `autoprune` (mode `gpupruner`, build CUDA uniquement).
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
#endif /* etii_search_h */
