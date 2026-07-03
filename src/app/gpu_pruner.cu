/**
 * @file gpu_pruner.cu
 * @brief Pruner GPU (CUDA) — contrôle par lots, équivalent de
 *        `possibility_all_has_a_next` (possibility.c) exécuté sur le GPU.
 *
 * Cible : SoC à mémoire unifiée (NVIDIA Jetson Orin Nano, `integrated == 1`).
 * Sur ces plateformes le GPU et le CPU partagent la même DRAM : tous les buffers
 * sont alloués en mémoire managed (`cudaMallocManaged`), qui ne subit ni copie ni
 * migration PCIe (chemin zéro-copie). Pour de meilleures performances, exécuter
 * sous le profil énergie maximal :  `sudo nvpmodel -m 0 && sudo jetson_clocks`.
 *
 * PIÈGE map (cf. CLAUDE/spec) : `map->flat[idx].parts` sont des pointeurs HÔTE
 * pointant dans `map->arena` — invalides sur le device. On construit donc un
 * miroir résident : `arena_dev` (copie contiguë de l'arène) + deux tableaux
 * `flat_off`/`flat_size` indexés comme le tableau 4D plat. Le kernel recalcule
 * l'indice avec la même formule que `get_parts_bigarray_with_key`.
 *
 * PARITÉ : la logique `__device__` ci-dessous reproduit À L'IDENTIQUE
 * `possibility_all_has_a_next`, `what_search_in_grid_to_key`, `is/set_face_used`
 * et `id_for_rotated_part`. Toute divergence est détectable via le mode de
 * vérification croisée (`-DGPU_PRUNER_VERIFY`) implémenté côté C dans
 * `autoprune_gpu` (etii_search.c).
 */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/gpu_pruner.h"
#include "core/part.h"
#include "core/possibility.h"
#include "app/static_variables.h"

extern "C" {
#include "ui/logger.h"
}

/* dirx/diry sont des globaux C (static_variables.c) ; déclarés extern dans
   static_variables.h, ils se résolvent au symbole non décoré côté C++. */

/** @brief Miroir GPU résident de la map, passé par valeur au kernel. */
struct GpuMap {
    int m;                   /**< sizearray (base d'indexation 4D). */
    int all_face;            /**< sizearrayM (= « toute face »). */
    int nrot;                /**< Nombre d'entrées dans part_dev. */
    long long total;         /**< Nombre de pièces dans arena_dev. */
    struct part *part_dev;   /**< Copie de all_rotate_part->parts [nrot]. */
    struct part *arena_dev;  /**< Copie contiguë de map->arena [total]. */
    int *flat_off;           /**< Offset dans l'arène par clé [m^4]. */
    int *flat_size;          /**< Taille de bucket par clé [m^4]. */
};

/* Ordre de parcours des cases : recopié en mémoire constante (cache rapide). */
__constant__ uint8_t c_dirx[ETERN_PARTS];
__constant__ uint8_t c_diry[ETERN_PARTS];

/* État résident du processus. */
static GpuMap g_map;
static int    g_inited = 0;
static struct possibility_packet *g_packets = NULL; /* buffer de travail (managed) */
static uint8_t                   *g_alive   = NULL; /* sorties (managed) */
static uint32_t                  *g_cells   = NULL; /* cases examinées par paquet (managed) */
/* Capacité d'un lancement kernel, fixée à l'init selon `pruner_batch_size`
   (bornée par PRUNER_BATCH_MAX). Un lot plus grand est traité par tranches de
   g_cap ; on dimensionne donc g_cap à la taille de lot configurée pour qu'un
   lot tienne en un seul lancement (occupation GPU maximale). */
static int                        g_cap = PRUNER_BATCH_SIZE;

/* ----------------------------------------------------------------------------
 * Réimplémentations __device__ (parité stricte avec possibility.c / possibility.h)
 * ------------------------------------------------------------------------- */

/** @brief Équivalent device de `is_face_used` (possibility.h:67). */
__device__ static inline uint8_t dev_is_face_used(const uint16_t *faceused, uint16_t part)
{
    uint16_t groupe = part >> 4;
    return (faceused[groupe] >> (part - (groupe << 4))) & 1;
}

/** @brief Équivalent device de `set_face_used` (possibility.h:50). */
__device__ static inline void dev_set_face_used(uint16_t *faceused, uint16_t part, uint8_t boolean)
{
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    int8_t n = part - (groupe << 4);
    number = (number & ~(1 << n)) | (boolean << n);
    faceused[groupe] = number;
}

/** @brief Équivalent device de `what_search_in_grid_to_key` (possibility.c:154). */
__device__ static void dev_what_search_in_grid_to_key(const struct part *parts,
                                                      const struct possibility_packet *p,
                                                      int8_t x, int8_t y,
                                                      key_part *key, int8_t all_face)
{
    // TOP
    if (y - 1 < 0) {
        key->k1 = 0;
    } else {
        int16_t g = p->grid[x][y - 1];
        key->k1 = (g < 0) ? all_face : parts[g].bottom;
    }
    // RIGHT
    if (x + 1 >= ETERN_SIZE) {
        key->k2 = 0;
    } else {
        int16_t g = p->grid[x + 1][y];
        key->k2 = (g < 0) ? all_face : parts[g].left;
    }
    // BOTTOM
    if (y + 1 >= ETERN_SIZE) {
        key->k3 = 0;
    } else {
        int16_t g = p->grid[x][y + 1];
        key->k3 = (g < 0) ? all_face : parts[g].top;
    }
    // LEFT
    if (x - 1 < 0) {
        key->k4 = 0;
    } else {
        int16_t g = p->grid[x - 1][y];
        key->k4 = (g < 0) ? all_face : parts[g].right;
    }
}

/**
 * @brief Kernel v1 : un thread par paquet.
 *
 * Reproduit `possibility_all_has_a_next` : parcourt les cases de `alloc` à
 * ETERN_PARTS suivant `dirx/diry`, contrôle qu'au moins une pièce candidate
 * reste posable, place les pièces forcées (bucket de taille 1), honore les
 * mêmes sorties anticipées (case « toute libre » → vivant ; bucket vide →
 * mort). En cas de complétion (`alloc == ETERN_PARTS`), met à jour `alloc` :
 * l'hôte détectera la solution (le device ne peut pas `exit()`).
 * `cells[i]` reçoit le nombre de cases examinées (statistique de débit,
 * même unité qu'un coup de la recherche ; 0 si court-circuit `checked`).
 */
__global__ void prune_kernel(struct possibility_packet *pk, uint8_t *alive, uint32_t *cells, int n, GpuMap map)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) {
        return;
    }
    struct possibility_packet *p = &pk[i];
    cells[i] = 0;

    // Court-circuit : déjà vérifié → vivant sans recalcul ni mutation.
    if (p->checked) {
        alive[i] = 1;
        return;
    }

    const int m = map.m;
    const int all_face = map.all_face;

    int result = 1;
    int alloc = p->alloc;
    key_part wsearch;

    for (int c = p->alloc; c < ETERN_PARTS && result == 1; c++) {
        result = 0;
        cells[i]++;
        int8_t x = (int8_t)c_dirx[c];
        int8_t y = (int8_t)c_diry[c];
        if (p->grid[x][y] == -2) {
            dev_what_search_in_grid_to_key(map.part_dev, p, x, y, &wsearch, (int8_t)all_face);
            if (wsearch.k1 < all_face || wsearch.k2 < all_face || wsearch.k3 < all_face || wsearch.k4 < all_face) {
                int idx = (((int)wsearch.k1 * m + wsearch.k2) * m + wsearch.k3) * m + wsearch.k4;
                int size = map.flat_size[idx];
                if (size > 0) {
                    int off = map.flat_off[idx];
                    for (int s = 0; s < size && result == 0; s++) {
                        struct part cand = map.arena_dev[off + s];
                        if (cand.id != 0 && dev_is_face_used(p->b_faceused, cand.id - 1) == 0) {
                            if (size == 1) {
                                dev_set_face_used(p->b_faceused, cand.id - 1, 1);
                                p->grid[x][y] = (int16_t)(cand.id + ETERN_PARTS * cand.rotation);
                                alloc++;
                            }
                            result = 1;
                        }
                    }
                } else {
                    // Rien trouvé : pas de suite (case morte).
                    break;
                }
            } else {
                result = 1;
                break;
            }
        } else {
            result = 1;
        }
    }

    if (alloc == ETERN_PARTS) {
        // Plateau complet : l'hôte traitera la solution (checkIfResultFound).
        p->alloc = alloc;
    }

    alive[i] = (uint8_t)result;
    if (result) {
        p->checked = 1;
    }
}

/* ----------------------------------------------------------------------------
 * Interface C
 * ------------------------------------------------------------------------- */

/** @brief Log d'erreur CUDA homogène. */
static int gpu_fail(const char *what, cudaError_t err)
{
    log_error("gpu_pruner: %s a échoué : %s\n", what, cudaGetErrorString(err));
    return -1;
}

extern "C" int gpu_pruner_init(const map_big_array *map, const struct array_part *all_rotate_part)
{
    if (g_inited) {
        return 0;
    }
    if (map == NULL || all_rotate_part == NULL) {
        log_error("gpu_pruner_init: arguments NULL\n");
        return -1;
    }

    int dev_count = 0;
    cudaError_t err = cudaGetDeviceCount(&dev_count);
    if (err != cudaSuccess || dev_count <= 0) {
        log_error("gpu_pruner_init: aucun GPU CUDA détecté (%s)\n",
                  err == cudaSuccess ? "device count 0" : cudaGetErrorString(err));
        return -1;
    }
    if ((err = cudaSetDevice(0)) != cudaSuccess) {
        return gpu_fail("cudaSetDevice", err);
    }

    cudaDeviceProp prop;
    if ((err = cudaGetDeviceProperties(&prop, 0)) == cudaSuccess) {
        log_info("gpu_pruner: GPU '%s' (integrated=%d, SM %d.%d)\n",
                 prop.name, prop.integrated, prop.major, prop.minor);
        if (!prop.integrated) {
            log_info("gpu_pruner: GPU discret — la mémoire managed sera migrée à la demande "
                     "(la cible est un SoC à mémoire unifiée type Jetson).\n");
        }
    }
    // Autorise le mappage hôte (zéro-copie) ; sans effet néfaste sur discret.
    cudaSetDeviceFlags(cudaDeviceMapHost);

    const int m = map->sizearray;
    const long long nkeys = (long long)m * m * m * m;

    // Nombre total de pièces dans l'arène (somme des tailles de bucket).
    long long total = 0;
    for (long long idx = 0; idx < nkeys; idx++) {
        total += map->flat[idx].size;
    }
    if (total > (long long)0x7fffffff) {
        log_error("gpu_pruner_init: arène trop grande pour un offset 32 bits (%lld)\n", total);
        return -1;
    }

    const int nrot = all_rotate_part->size;

    memset(&g_map, 0, sizeof(g_map));
    g_map.m = m;
    g_map.all_face = map->sizearrayM;
    g_map.nrot = nrot;
    g_map.total = total;

    if ((err = cudaMallocManaged(&g_map.flat_off, nkeys * sizeof(int))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(flat_off)", err);
    }
    if ((err = cudaMallocManaged(&g_map.flat_size, nkeys * sizeof(int))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(flat_size)", err);
    }
    if ((err = cudaMallocManaged(&g_map.arena_dev,
                                 (total > 0 ? total : 1) * sizeof(struct part))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(arena_dev)", err);
    }
    if ((err = cudaMallocManaged(&g_map.part_dev, (long long)nrot * sizeof(struct part))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(part_dev)", err);
    }

    // Remplissage offset/size (l'arène est copiée en bloc juste après).
    for (long long idx = 0; idx < nkeys; idx++) {
        int sz = map->flat[idx].size;
        g_map.flat_size[idx] = sz;
        if (sz > 0) {
            g_map.flat_off[idx] = (int)(map->flat[idx].parts - map->arena);
        } else {
            g_map.flat_off[idx] = 0;
        }
    }
    if (total > 0) {
        memcpy(g_map.arena_dev, map->arena, (size_t)total * sizeof(struct part));
    }
    memcpy(g_map.part_dev, all_rotate_part->parts, (size_t)nrot * sizeof(struct part));

    // Ordre de parcours → mémoire constante.
    if ((err = cudaMemcpyToSymbol(c_dirx, dirx, ETERN_PARTS * sizeof(uint8_t))) != cudaSuccess) {
        return gpu_fail("cudaMemcpyToSymbol(dirx)", err);
    }
    if ((err = cudaMemcpyToSymbol(c_diry, diry, ETERN_PARTS * sizeof(uint8_t))) != cudaSuccess) {
        return gpu_fail("cudaMemcpyToSymbol(diry)", err);
    }

    // Capacité d'un lancement = taille de lot configurée (bornée). Permet de
    // traiter tout un lot pruner en un seul kernel (plusieurs blocs sur tous les
    // SM) plutôt qu'en tranches de PRUNER_BATCH_SIZE.
    g_cap = pruner_batch_size;
    if (g_cap < 1) {
        g_cap = 1;
    }
    if (g_cap > PRUNER_BATCH_MAX) {
        g_cap = PRUNER_BATCH_MAX;
    }

    // Buffers de travail (managed) dimensionnés pour un lot.
    if ((err = cudaMallocManaged(&g_packets,
                                 (size_t)g_cap * sizeof(struct possibility_packet))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(g_packets)", err);
    }
    if ((err = cudaMallocManaged(&g_alive, (size_t)g_cap * sizeof(uint8_t))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(g_alive)", err);
    }
    if ((err = cudaMallocManaged(&g_cells, (size_t)g_cap * sizeof(uint32_t))) != cudaSuccess) {
        return gpu_fail("cudaMallocManaged(g_cells)", err);
    }

    g_inited = 1;
    log_info("gpu_pruner: miroir map prêt (m=%d, clés=%lld, arène=%lld pièces, rotations=%d)\n",
             m, nkeys, total, nrot);
    return 0;
}

extern "C" void gpu_pruner_check_batch(struct possibility_packet *packets, int n, uint8_t *alive_out, uint32_t *cells_out)
{
    if (!g_inited || n <= 0) {
        return;
    }

    const int threads = 128;
    for (int base = 0; base < n; base += g_cap) {
        int cnt = n - base;
        if (cnt > g_cap) {
            cnt = g_cap;
        }

        memcpy(g_packets, packets + base, (size_t)cnt * sizeof(struct possibility_packet));

        int blocks = (cnt + threads - 1) / threads;
        prune_kernel<<<blocks, threads>>>(g_packets, g_alive, g_cells, cnt, g_map);

        cudaError_t err = cudaGetLastError();
        if (err != cudaSuccess) {
            log_error("gpu_pruner: lancement kernel échoué : %s\n", cudaGetErrorString(err));
            // Conservateur : on marque tout vivant pour ne perdre aucune possibilité.
            for (int i = 0; i < cnt; i++) {
                alive_out[base + i] = 1;
                if (cells_out != NULL) {
                    cells_out[base + i] = 0;
                }
            }
            continue;
        }
        if ((err = cudaDeviceSynchronize()) != cudaSuccess) {
            log_error("gpu_pruner: synchronisation échouée : %s\n", cudaGetErrorString(err));
            for (int i = 0; i < cnt; i++) {
                alive_out[base + i] = 1;
                if (cells_out != NULL) {
                    cells_out[base + i] = 0;
                }
            }
            continue;
        }

        // Recopie des paquets mutés + drapeaux vivant/mort + cases examinées.
        memcpy(packets + base, g_packets, (size_t)cnt * sizeof(struct possibility_packet));
        memcpy(alive_out + base, g_alive, (size_t)cnt * sizeof(uint8_t));
        if (cells_out != NULL) {
            memcpy(cells_out + base, g_cells, (size_t)cnt * sizeof(uint32_t));
        }
    }
}

extern "C" void gpu_pruner_shutdown(void)
{
    if (!g_inited) {
        return;
    }
    cudaFree(g_map.flat_off);
    cudaFree(g_map.flat_size);
    cudaFree(g_map.arena_dev);
    cudaFree(g_map.part_dev);
    cudaFree(g_packets);
    cudaFree(g_alive);
    cudaFree(g_cells);
    memset(&g_map, 0, sizeof(g_map));
    g_packets = NULL;
    g_alive = NULL;
    g_cells = NULL;
    g_inited = 0;
}
