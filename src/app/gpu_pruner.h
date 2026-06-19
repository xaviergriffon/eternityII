/**
 * @file gpu_pruner.h
 * @brief Interface C du pruner GPU (mode `gpupruner`, build CUDA uniquement).
 *
 * Tout le code CUDA vit dans `gpu_pruner.cu`. Cet en-tête expose une interface
 * minimale, en `extern "C"`, consommée par la boucle pruner GPU (`autoprune_gpu`
 * dans etii_search.c). Le contrôle réalisé est strictement équivalent à
 * `possibility_all_has_a_next` (possibility.c) mais appliqué par lots sur le GPU.
 *
 * Cible : SoC à mémoire unifiée (NVIDIA Jetson Orin Nano). Le GPU et le CPU
 * partagent la même DRAM physique : les buffers sont alloués en mémoire managed
 * (`cudaMallocManaged`), ce qui constitue le chemin « zéro-copie » sur ces
 * plateformes intégrées (aucun transfert PCIe).
 */
#ifndef gpu_pruner_h
#define gpu_pruner_h

#include <stdint.h>

#include "core/part.h"
#include "core/possibility.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Construit le miroir GPU résident de la map (une seule fois par processus).
 *
 * Doit être appelé APRÈS la construction de la map, et DANS le processus qui
 * exécutera le contrôle (les contextes CUDA ne sont pas hérités par `fork()`).
 *
 * Copie en mémoire managed :
 *  - `arena` (listes de candidats compactées) → `arena_dev`,
 *  - les rotations (`all_rotate_part->parts`) → `part_dev`,
 *  - pour chaque clé du tableau 4D plat : offset (dans l'arène) et taille.
 *
 * @param map             Table de lookup 4D hôte (pointeurs `flat[].parts` dans `arena`).
 * @param all_rotate_part Tableau de toutes les rotations (indexé par grid value).
 * @return                0 si OK, valeur négative en cas d'erreur (pas de GPU, alloc…).
 */
int gpu_pruner_init(const map_big_array *map, const struct array_part *all_rotate_part);

/**
 * @brief Contrôle un lot de possibilités sur le GPU.
 *
 * Pour chaque `packets[i]` :
 *  - `alive_out[i] = 1` si vivant (au moins une pièce candidate par case vide),
 *    `0` si mort.
 *  - Court-circuit : si `packets[i].checked == 1`, vivant sans recalcul.
 *  - Si vivant et non court-circuité : `packets[i]` est éventuellement MUTÉ
 *    (pièces forcées des cases à candidat unique placées dans `grid` +
 *    `b_faceused`), exactement comme `possibility_all_has_a_next` sur sa copie,
 *    et `checked` est mis à 1. Si le plateau se complète, `alloc` passe à
 *    `ETERN_PARTS` (l'appelant détecte alors la solution).
 *
 * @param packets   Tableau de paquets (entrée/sortie, muté en place).
 * @param n         Nombre de paquets.
 * @param alive_out Tableau de sortie (au moins `n` octets) : 1 = vivant, 0 = mort.
 */
void gpu_pruner_check_batch(struct possibility_packet *packets, int n, uint8_t *alive_out);

/** @brief Libère le miroir GPU et les buffers de travail. */
void gpu_pruner_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* gpu_pruner_h */
