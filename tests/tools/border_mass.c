/*
 * border_mass — mesure la masse totale des anneaux de bordure valides.
 *
 * Énumère par recherche exhaustive tous les anneaux de bordure valides
 * (BORDER_RING_LEN cases du pourtour du plateau), ancrés au coin (0,0), et
 * rapporte N — la masse totale directement. Aucun voisin n'est encore posé à
 * la toute première case : le DFS explore donc déjà les 4 coins possibles
 * comme point d'ouverture, retrouvant chaque anneau abstrait une fois par
 * coin — pas de ×4 supplémentaire à appliquer. Cela suppose qu'aucun indice
 * officiel ne touche le bord (vérifié ci-dessous).
 *
 * `--forks N` (défaut : nombre de cœurs détecté, borné à 1024) parallélise :
 * le plateau est posé dans l'ordre « coins d'abord » (border_corners_first_order
 * — très peu de pièces ont 2 faces nulles adjacentes, donc le facteur de
 * branchement des 4 premières étapes est minuscule), étendu en largeur
 * jusqu'à N*8 états partiels (border_walk_expand_frontier), puis distribué
 * en round-robin à N process forkés — chacun termine sa part avec
 * border_walk_count_ordered et renvoie son sous-total au parent par un
 * pipe dédié. La map de lookup est construite UNE SEULE FOIS avant tout
 * fork (héritée en COW par les enfants), comme le fait déjà main.c pour le
 * serveur/client réel.
 *
 * Aucune coordination façon fork_gate.c : border_mass est mono-thread avant
 * de forker ses workers, donc le problème que fork_gate.c résout (un thread
 * du parent qui tourne encore pendant le fork()) ne se pose pas ici — voir
 * docs/superpowers/specs/2026-09-06-border-mass-parallel-design.md pour la
 * parallélisation, et docs/superpowers/specs/2026-09-06-masse-bordure-design.md
 * pour le raisonnement complet sur « N est déjà la masse ».
 *
 * Aucun gestionnaire de signal : Ctrl-C envoie SIGINT à tout le groupe de
 * process (parent + enfants forkés, aucun setpgid/setsid n'est appelé) —
 * comportement par défaut du terminal, suffisant, volontairement pas
 * réimplémenté.
 *
 * Si un worker échoue (tué, code de sortie non nul, ou pipe vide), le total
 * n'est PAS affiché comme définitif : border_mass échoue bruyamment (code de
 * sortie 1) plutôt que d'imprimer un nombre plausible mais sous-évalué en
 * silence — ce nombre est la seule sortie observable de l'outil.
 *
 * Toute la logique d'énumération vit dans tests/tools/border_walk.c, testée
 * unitairement ; ce fichier n'est que l'enveloppe d'entrées/sorties et
 * l'orchestration fork/pipe/wait, non testée unitairement (comme
 * gen_root.c) — vérifiée par smoke test manuel (--forks 1 vs --forks 4 sur
 * le jeu 16 pièces, même total).
 *
 * Chaque worker journalise sur stderr, en plus de la ligne « partition X/Y
 * terminee » (qui n'arrive qu'une fois tout le sous-arbre d'une partition
 * épuisé — potentiellement très long, cf. docs/tests_et_ci.md), une ligne de
 * progression tous les BM_PROGRESS_INTERVAL_NODES nœuds DFS visités (voir
 * `struct border_progress_opts`, tests/tools/border_walk.h) : nœuds explorés,
 * anneaux trouvés jusqu'ici, et un débit nœuds/s calculé depuis la dernière
 * ligne — un signal de vie et de vitesse indépendant du bouclage d'une
 * partition entière.
 *
 * `--dp` bascule sur un algorithme radicalement différent et EXACT :
 * `border_ring_count_dp` (tests/tools/border_ring_dp.c) regroupe les
 * pièces de bord interchangeables (même paire ORDONNÉE de couleurs "anneau",
 * la face intérieure n'étant jamais vérifiée par ce comptage) en classes, et
 * calcule niveau par niveau (une position de l'anneau à la fois, jamais les
 * 59 mémoïsées ensemble) le nombre de façons d'atteindre chaque état — bien
 * plus petit qu'un masque de bits par pièce réelle, et bien plus sobre en
 * mémoire qu'une mémoïsation globale — là où le DFS aveugle (par défaut,
 * sans `--dp`) explose (des dizaines de milliards de nœuds pour UNE SEULE
 * partition sur 270 sur `data/pieces.csv`, cf. docs/tests_et_ci.md). Combine
 * `--dp` avec `--forks N` : la transition d'un niveau assez gros est
 * répartie sur N process forkés (chacun traite une plage d'états déjà
 * calculés, en lecture seule, et écrit sa part du niveau suivant dans un
 * fichier temporaire fusionné par le parent) — sans `--forks`, `--dp` prend
 * le nombre de cœurs détecté par défaut, comme le DFS. Au-delà d'une taille
 * de niveau généreuse (mesurée insuffisante même sur une machine à 48 Go de
 * RAM sans ce mécanisme), un niveau bascule en fragments sur disque
 * (partitionnement externe par hachage) au lieu d'une table unique en
 * mémoire — voir border_ring_dp.h pour le raisonnement complet. `--spill-dir
 * DIR` redirige ces fragments (et les fichiers temporaires de `--forks`) vers
 * DIR au lieu de `/tmp`, souvent une petite partition ou un tmpfs plafonné
 * bien en-deçà de la RAM de la machine — observé en pratique, `/tmp` saturé
 * par plusieurs dizaines de Go de fragments même sur une machine bien dotée
 * (2x10 cœurs/48 Go), cf. docs/tests_et_ci.md.
 *
 * Usage :
 *   make border-mass
 *   tests/tools/border_mass [--dp] [--forks N] [--spill-dir DIR] data/pieces.csv data/indices.csv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"
#include "tools/border_ring_dp.h"

#define BM_MAX_FORKS 1024

/* Nombre de nœuds DFS entre deux lignes de progression par worker — choisi
   empiriquement (~23M nœuds/s constatés sur data/pieces.csv, 256 pièces,
   avec ce réglage : une ligne toutes les 4-5 secondes) pour ni un flot
   illisible, ni un silence de plusieurs minutes entre deux signes de vie. */
#define BM_PROGRESS_INTERVAL_NODES 100000000LL

struct bm_progress_ctx {
    int worker_id;
    int partition_index;  /* 1-based, position du worker dans SA liste */
    int partition_total;  /* nombre de partitions assignées à ce worker */
    struct timespec last_time;
    long long last_nodes;
};

static double bm_elapsed_seconds(const struct timespec *from, const struct timespec *to)
{
    return (double)(to->tv_sec - from->tv_sec) + (double)(to->tv_nsec - from->tv_nsec) / 1e9;
}

/* Callback `border_progress_cb` : appelé par border_walk_count_ordered tous
   les BM_PROGRESS_INTERVAL_NODES nœuds. border_walk.c ne lit aucune horloge
   (cœur pur, cf. son commentaire) — tout le calcul de vitesse vit ici. */
static void bm_report_progress(long long nodes_visited, long long rings_found, void *ctx_)
{
    struct bm_progress_ctx *ctx = (struct bm_progress_ctx *)ctx_;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double elapsed_s = bm_elapsed_seconds(&ctx->last_time, &now);
    long long delta_nodes = nodes_visited - ctx->last_nodes;
    double rate = (elapsed_s > 0.0) ? (double)delta_nodes / elapsed_s : 0.0;

    fprintf(stderr,
            "border_mass[worker %d] : partition %d/%d en cours, %lld noeuds explores, "
            "%lld anneaux trouves (dans cette partition), %.0f noeuds/s\n",
            ctx->worker_id, ctx->partition_index, ctx->partition_total,
            nodes_visited, rings_found, rate);

    ctx->last_time = now;
    ctx->last_nodes = nodes_visited;
}

static int border_mass_check_indices_not_on_border(const struct array_index *indices)
{
    for (int i = 0; i < indices->size; i++) {
        int x = indices->indices[i].x;
        int y = indices->indices[i].y;
        if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1 ||
            x >= ETERN_SIZE || y >= ETERN_SIZE) {
            fprintf(stderr,
                    "border_mass : l'indice officiel id=%d est en (%d,%d), sur le bord — "
                    "un seul coin pourrait alors ouvrir la recherche, ce chiffre ne serait "
                    "plus la masse totale, refus de continuer\n",
                    indices->indices[i].id, x, y);
            return -1;
        }
    }
    return 0;
}

struct bm_partition {
    struct possibility_packet state;
    int depth;
};

struct bm_collect_ctx {
    struct bm_partition *partitions;
    int count;
    int cap;
};

static void bm_collect_partial(const struct possibility_packet *partial_state, int depth, void *ctx_)
{
    struct bm_collect_ctx *ctx = (struct bm_collect_ctx *)ctx_;
    if (ctx->count == ctx->cap) {
        ctx->cap = (ctx->cap == 0) ? 16 : ctx->cap * 2;
        ctx->partitions = realloc(ctx->partitions, (size_t)ctx->cap * sizeof *ctx->partitions);
    }
    ctx->partitions[ctx->count].state = *partial_state;
    ctx->partitions[ctx->count].depth = depth;
    ctx->count++;
}

static int bm_default_forks(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        n = 1;
    }
    if (n > BM_MAX_FORKS) {
        n = BM_MAX_FORKS;
    }
    return (int)n;
}

/* Traite les partitions worker_id, worker_id+nb_workers, worker_id+2*nb_workers,
   ... — round-robin plutôt qu'un bloc contigu, pour ne pas concentrer un
   déséquilibre de charge entre sous-arbres voisins sur un seul worker. */
static long long bm_run_worker(int worker_id, int nb_workers,
                                map_big_array *map, struct array_part *all,
                                const int8_t order[BORDER_RING_LEN][2],
                                const struct bm_partition *partitions, int nb_partitions)
{
    long long total = 0;
    int done = 0;
    int assigned = 0;
    for (int p = worker_id; p < nb_partitions; p += nb_workers) {
        assigned++;
    }

    struct bm_progress_ctx progress_ctx;
    progress_ctx.worker_id = worker_id;
    progress_ctx.partition_total = assigned;
    struct border_progress_opts progress = { BM_PROGRESS_INTERVAL_NODES, bm_report_progress, &progress_ctx };

    for (int p = worker_id; p < nb_partitions; p += nb_workers) {
        progress_ctx.partition_index = done + 1;
        progress_ctx.last_nodes = 0;
        clock_gettime(CLOCK_MONOTONIC, &progress_ctx.last_time);

        long long sub = border_walk_count_ordered(map, all, order, partitions[p].depth,
                                                    &partitions[p].state, NULL, NULL, &progress);
        total += sub;
        done++;
        fprintf(stderr, "border_mass[worker %d] : partition %d/%d terminee, sous-total %lld\n",
                worker_id, done, assigned, total);
    }
    return total;
}

/* Tue et récolte les `count` premiers workers déjà forkés (0..count-1) — utilisé
   quand pipe()/fork() échoue en cours de boucle : sans cela, les workers déjà
   lancés continueraient de tourner sans personne pour les attendre, parfois
   pendant des heures sur un vrai run. */
static void bm_abort_workers(int (*pipes)[2], const pid_t *pids, int count)
{
    for (int i = 0; i < count; i++) {
        kill(pids[i], SIGTERM);
    }
    for (int i = 0; i < count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        close(pipes[i][0]);
    }
}

int main(int argc, char **argv)
{
    int forks = bm_default_forks();
    int use_dp = 0;
    const char *spill_dir = NULL;
    int argi = 1;
    /* `--dp`, `--forks N` et `--spill-dir DIR` sont des options indépendantes,
       combinables dans n'importe quel ordre — `--dp --forks N` choisit
       l'algorithme ET le nombre de process forkés pour sa parallélisation par
       niveau (cf. border_ring_dp.h) ; `--forks N` seul garde son sens
       historique (DFS par forks) ; `--dp` seul reprend le nombre de cœurs
       détecté par défaut, comme `--forks` seul. `--spill-dir` n'a d'effet
       qu'avec `--dp` (fragments du mode disque, cf. `border_ring_dp_set_spill_dir`) —
       `/tmp` est souvent une petite partition ou un tmpfs plafonné bien
       en-deçà de la RAM de la machine, qui peut saturer même sur une machine
       par ailleurs bien dotée (observé en pratique, cf. docs/tests_et_ci.md). */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--dp") == 0) {
            use_dp = 1;
            argi++;
        } else if (strcmp(argv[argi], "--forks") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr,
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] <pieces.csv> <indices.csv>\n",
                        argv[0]);
                return 2;
            }
            char *endptr = NULL;
            long forks_long = strtol(argv[argi + 1], &endptr, 10);
            if (endptr == argv[argi + 1] || *endptr != '\0' || forks_long < 1) {
                forks = 1;
            } else if (forks_long > BM_MAX_FORKS) {
                forks = BM_MAX_FORKS;
            } else {
                forks = (int)forks_long;
            }
            argi += 2;
        } else if (strcmp(argv[argi], "--spill-dir") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr,
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] <pieces.csv> <indices.csv>\n",
                        argv[0]);
                return 2;
            }
            spill_dir = argv[argi + 1];
            argi += 2;
        } else {
            break;
        }
    }
    if (argc - argi != 2) {
        fprintf(stderr, "usage: %s [--dp] [--forks N] [--spill-dir DIR] <pieces.csv> <indices.csv>\n",
                argv[0]);
        return 2;
    }
    const char *pieces_path = argv[argi];
    const char *indices_path = argv[argi + 1];

    struct array_index *indices = read_indices(indices_path);
    int indices_ok = (border_mass_check_indices_not_on_border(indices) == 0);
    free_array_index(indices);
    if (!indices_ok) {
        return 1;
    }

    struct array_part *apart = read_parts(pieces_path);
    struct array_part *all = rotate_all_parts(apart);
    map_big_array *map = prepare_map_part(all);
    if (map == NULL) {
        fprintf(stderr, "border_mass : construction de la map de lookup impossible\n");
        return 1;
    }

    if (use_dp) {
        if (spill_dir != NULL) {
            border_ring_dp_set_spill_dir(spill_dir);
        }
        long long total = border_ring_count_dp(map, all, forks);
        printf("masse totale des anneaux de bordure valides : %lld\n", total);
        return 0;
    }

    int8_t order[BORDER_RING_LEN][2];
    border_corners_first_order(order);

    struct bm_collect_ctx collect;
    memset(&collect, 0, sizeof collect);
    long long completed_during_expansion =
        border_walk_expand_frontier(map, all, order, forks * 8,
                                     bm_collect_partial, &collect, NULL, NULL);

    fprintf(stderr, "border_mass : %d partitions, %d worker(s)\n", collect.count, forks);

    if (collect.count == 0) {
        printf("masse totale des anneaux de bordure valides : %lld\n", completed_during_expansion);
        free(collect.partitions);
        return 0;
    }

    /* Vider les tampons stdio AVANT fork() : sinon chaque enfant hérite d'une
       copie du contenu déjà écrit (mais pas encore vidé) par read_parts/read_indices
       plus haut, et le revide indépendamment à son propre exit() — dupliquant les
       lignes de log une fois par worker. */
    fflush(stdout);
    fflush(stderr);

    int (*pipes)[2] = malloc((size_t)forks * sizeof *pipes);
    pid_t *pids = malloc((size_t)forks * sizeof *pids);

    for (int w = 0; w < forks; w++) {
        if (pipe(pipes[w]) != 0) {
            fprintf(stderr, "border_mass : pipe() a echoue pour le worker %d\n", w);
            bm_abort_workers(pipes, pids, w);
            free(pipes);
            free(pids);
            free(collect.partitions);
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_mass : fork() a echoue pour le worker %d\n", w);
            close(pipes[w][0]);
            close(pipes[w][1]);
            bm_abort_workers(pipes, pids, w);
            free(pipes);
            free(pids);
            free(collect.partitions);
            return 1;
        }
        if (pid == 0) {
            /* Un enfant n'a besoin que de l'extrémité écriture de SON pipe —
               il hérite aussi des extrémités lecture de tous les pipes des
               workers déjà forkés avant lui (pipes[0..w-1][0]), jamais
               utilisées ici : les fermer explicitement. */
            for (int i = 0; i < w; i++) {
                close(pipes[i][0]);
            }
            close(pipes[w][0]);
            long long sub = bm_run_worker(w, forks, map, all, order, collect.partitions, collect.count);
            dprintf(pipes[w][1], "%lld\n", sub);
            close(pipes[w][1]);
            exit(0);
        }
        close(pipes[w][1]);
        pids[w] = pid;
    }

    long long total = completed_during_expansion;
    int any_worker_failed = 0;
    for (int w = 0; w < forks; w++) {
        char buf[64];
        size_t got = 0;
        ssize_t r;
        while (got < sizeof buf - 1 && (r = read(pipes[w][0], buf + got, sizeof buf - 1 - got)) > 0) {
            got += (size_t)r;
        }
        close(pipes[w][0]);

        int status;
        waitpid(pids[w], &status, 0);

        if (got == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_mass : le worker %d a echoue (status=%d, %zu octets lus)\n",
                    w, status, got);
            any_worker_failed = 1;
            continue;
        }

        buf[got] = '\0';
        total += strtoll(buf, NULL, 10);
    }

    if (any_worker_failed) {
        fprintf(stderr,
                "border_mass : au moins un worker a echoue, total incomplet non affiche comme definitif\n");
        free(pipes);
        free(pids);
        free(collect.partitions);
        return 1;
    }

    printf("masse totale des anneaux de bordure valides : %lld\n", total);

    free(pipes);
    free(pids);
    free(collect.partitions);
    return 0;
}
