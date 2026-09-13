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
 * docs/conception/border_mass.md pour la parallélisation
 * et le raisonnement complet sur « N est déjà la masse ».
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
 * partition sur 270 sur `data/pieces.csv`, cf. docs/tests_et_ci.md). Une
 * seule pièce-coin d'ouverture est traitée en entier (les autres candidats
 * lui sont rigoureusement égaux par symétrie de rotation à 90° du plateau,
 * cf. docs/conception/border_mass.md) — le
 * résultat est multiplié par le nombre de candidats plutôt que rejoué une
 * fois par candidat. Combine `--dp` avec `--forks N` : la transition d'un
 * niveau assez gros est répartie sur N process forkés (chacun traite une
 * plage d'états déjà calculés, en lecture seule, et écrit sa part du niveau
 * suivant dans un fichier temporaire fusionné par le parent) — sans
 * `--forks`, `--dp` prend le nombre de cœurs détecté par défaut, comme le
 * DFS. `--dp-max-ram-mo MO` est OBLIGATOIRE avec `--dp` : taille du tampon
 * de tri (cf. `border_ring_dp_set_max_ram_mo`) — au-delà, le tampon déverse
 * un run trié sur disque et les runs sont fusionnés en fin de position, ce
 * qui laisse le niveau ENTIER et intégralement fusionné quelle que soit sa
 * taille ; dépasser le budget ne change plus la forme du calcul, seulement
 * son nombre d'E/S — voir border_ring_dp.h pour le raisonnement
 * complet. `--spill-dir DIR`
 * redirige ces fragments (et les fichiers temporaires de `--forks`) vers DIR
 * au lieu de `/tmp`, souvent une petite partition ou un tmpfs plafonné bien
 * en-deçà de la RAM de la machine — observé en pratique, `/tmp` saturé par
 * plusieurs dizaines de Go de fragments même sur une machine bien dotée
 * (2x10 cœurs/48 Go), cf. docs/tests_et_ci.md.
 *
 * `--save-rings FILE --max-rings N` écrit des anneaux de bordure RÉELS
 * (pièces réelles, pas seulement le total) dans FILE au format `.back` (flux
 * headerless de possibility_packet, même format que `gen_root` — utilisable
 * comme racines de stock pour la recherche réelle), selon DEUX modes.
 *
 * `--max-rings` se lit sur 128 bits (bd_ring_count_parse) — la masse réelle
 * vaut ~10^37, et strtoll y saturait en silence.
 *
 * SANS `--dp` : échantillonnage par le walker. Mode par défaut à privilégier
 * — ~1 M anneaux/s ECRITS (~500 Mo/s, le disque est le facteur limitant :
 * le walker en FERME ~15 M/s), premier anneau écrit en ~1 s, aucun fichier
 * temporaire, et `--max-rings` borne le TRAVAIL autant que la sortie (le
 * quota est réparti entre workers, chacun s'arrêtant sur le sien). Chaque
 * anneau produit a une frontière intérieure unique (vérifié sur 10^6), donc
 * autant de sous-problèmes distincts pour la recherche intérieure.
 *
 * AVEC `--dp` : reconstruction EXHAUSTIVE, à réserver aux jeux de pièces dont
 * on veut tous les anneaux — sur data/pieces.csv (~10^37 anneaux) elle
 * demande ~31 h et ~537 Go avant de livrer le premier.
 * `border_ring_reconstruct_dp` (tests/tools/border_ring_dp.c)
 * calcule, pour chaque niveau déjà persisté de la passe `--dp`, une table de
 * complétion (nombre de façons de finir l'anneau à partir de cet état) qui
 * sert d'oracle d'élagage à un DFS guidé sur les CLASSES (~15-18 sur le jeu
 * réel, pas les pièces individuellement) — chaque suite de classes complète
 * est ensuite développée en toutes ses assignations de pièces réelles
 * possibles. `--max-rings N` est un plafond de SÉCURITÉ, obligatoire (aucune
 * valeur par défaut) : la reconstruction échoue bruyamment si le total
 * reconstruit diverge du total exact sans que ce plafond en soit la cause —
 * jamais un fichier `.back` silencieusement incomplet.
 *
 * Usage :
 *   make border-mass
 *   tests/tools/border_mass [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] \
 *       [--save-rings FILE --max-rings N] data/pieces.csv data/indices.csv
 *
 * Echantillonner 10^6 anneaux comme racines de stock :
 *   tests/tools/border_mass --forks 8 --save-rings rings.back --max-rings 1000000 \
 *       data/pieces.csv data/indices.csv
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

/* Marge confortable au-dessus de PATH_MAX pour `<chemin>.w<N>`. */
#define BM_RINGS_PATH_MAX 5120

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

/* Contexte de `bm_on_ring_found` — `--save-rings FILE`. `budget` borne le
   nombre d'anneaux que CE contexte accepte d'écrire : c'est lui qui traduit
   `--max-rings` en demande d'arrêt du producteur. Il est en 128 bits parce
   que `--max-rings` l'est (la masse réelle vaut ~10^37) ; `bounded` porte le
   « pas de borne », qu'un type non signé ne peut pas coder par -1.
   `written`, lui, reste un `long long` : il compte des enregistrements
   réellement écrits sur disque, jamais plus de 2^63. */
struct bm_rings_ctx {
    FILE *file;
    long long written;
    bd_ring_count_t budget;
    int bounded;
};

/* Écrit un anneau réel (reconstruit par border_ring_reconstruct_dp, ou
   produit directement par le walker) dans le fichier `.back` ouvert — même
   format headerless qu'un `.back` produit par le serveur
   (`docs/utilisation.md`) et que `gen_root.c:76-86` : un `fwrite(&packet,
   sizeof packet, 1, f)` brut, `checked` forcé à 0 (pool non vérifié —
   `import()` le sanitize et recalcule `alloc`/`min_candidats` au chargement
   de toute façon, cf. src/core/datamanager.c). Toute écriture ratée est
   fatale immédiatement — jamais un fichier tronqué qui passe pour un succès
   (même principe que `bd_write_or_die`, border_ring_dp.c).

   Le `fflush` par anneau est délibéré : ces runs se coupent au `kill` une
   fois qu'on en a assez, et un tampon stdio perdu ferait finir le fichier
   sur un enregistrement partiel — `import()` lit des blocs de taille fixe et
   ne le détecterait pas. Le coût est invisible devant le prix d'un anneau. */
static int bm_on_ring_found(const struct possibility_packet *ring, void *ctx_)
{
    struct bm_rings_ctx *ctx = (struct bm_rings_ctx *)ctx_;
    struct possibility_packet packet = *ring;
    packet.checked = 0;
    if (fwrite(&packet, sizeof packet, 1, ctx->file) != 1) {
        fprintf(stderr, "border_mass : ecriture d'un anneau reconstruit impossible (disque plein ?) — arret\n");
        exit(1);
    }
    if (fflush(ctx->file) != 0) {
        fprintf(stderr, "border_mass : vidage du fichier d'anneaux impossible (disque plein ?) — arret\n");
        exit(1);
    }
    ctx->written++;
    return (ctx->bounded && (bd_ring_count_t)ctx->written >= ctx->budget);
}

/* `<base>.<tag>` — un fichier d'anneaux par producteur. Les workers sont des
   forks : un unique `FILE*` partagé à travers `fork()` leur donnerait des
   offsets et des tampons indépendants sur la MÊME description de fichier, et
   leurs enregistrements s'entrelaceraient. Chacun écrit donc le sien, que le
   parent concatène une fois tout le monde sorti. */
static int bm_rings_part_path(char *dst, size_t dstlen, const char *base, const char *tag)
{
    int n = snprintf(dst, dstlen, "%s.%s", base, tag);
    if (n < 0 || (size_t)n >= dstlen) {
        fprintf(stderr, "border_mass : nom de fichier d'anneaux trop long pour '%s'\n", base);
        return -1;
    }
    return 0;
}

/* Concatène `<base>.expand` puis `<base>.w0..w<nb-1>` dans `base`, et efface
   les morceaux. Retourne le nombre d'anneaux (enregistrements de taille fixe)
   écrits, ou -1 en cas d'échec. Toute erreur est fatale côté appelant : un
   `.back` tronqué se relit sans broncher, `import()` lisant des blocs de
   taille fixe. */
static long long bm_rings_concat(const char *base, const char *expand_path, const char *worker_base,
                                  int nb_workers)
{
    FILE *out = fopen(base, "wb");
    if (out == NULL) {
        fprintf(stderr, "border_mass : ouverture de '%s' impossible\n", base);
        return -1;
    }
    long long records = 0;
    for (int i = -1; i < nb_workers; i++) {
        char path[BM_RINGS_PATH_MAX];
        if (i < 0) {
            snprintf(path, sizeof path, "%s", expand_path);
        } else {
            char tag[32];
            snprintf(tag, sizeof tag, "w%d", i);
            if (bm_rings_part_path(path, sizeof path, worker_base, tag) != 0) {
                fclose(out);
                return -1;
            }
        }
        FILE *in = fopen(path, "rb");
        if (in == NULL) {
            continue; /* un worker sans quota n'a rien créé */
        }
        struct possibility_packet packet;
        while (fread(&packet, sizeof packet, 1, in) == 1) {
            if (fwrite(&packet, sizeof packet, 1, out) != 1) {
                fprintf(stderr, "border_mass : ecriture dans '%s' impossible (disque plein ?)\n", base);
                fclose(in);
                fclose(out);
                return -1;
            }
            records++;
        }
        fclose(in);
        remove(path);
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "border_mass : cloture de '%s' impossible (disque plein ?)\n", base);
        return -1;
    }
    return records;
}

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
                                const struct bm_partition *partitions, int nb_partitions,
                                struct bm_rings_ctx *rings)
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
                                                    &partitions[p].state,
                                                    rings != NULL ? bm_on_ring_found : NULL, rings,
                                                    &progress);
        total += sub;
        done++;
        fprintf(stderr, "border_mass[worker %d] : partition %d/%d terminee, sous-total %lld\n",
                worker_id, done, assigned, total);

        /* Quota épuisé : les partitions restantes de ce worker ne servent
           plus à rien. Sans ce test, le DFS repartirait sur la suivante et
           `bm_on_ring_found` la stopperait à son premier anneau — en
           l'écrivant, donc en dépassant le quota d'une unité par partition
           restante. */
        if (rings != NULL && rings->bounded && (bd_ring_count_t)rings->written >= rings->budget) {
            break;
        }
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
    long dp_max_ram_mo = -1;
    const char *save_rings_path = NULL;
    bd_ring_count_t max_rings = 0;
    int max_rings_set = 0;
    int argi = 1;
    /* `--dp`, `--forks N`, `--spill-dir DIR` et `--dp-max-ram-mo MO` sont des
       options indépendantes, combinables dans n'importe quel ordre — `--dp
       --forks N` choisit l'algorithme ET le nombre de process forkés pour sa
       parallélisation par niveau (cf. border_ring_dp.h) ; `--forks N` seul
       garde son sens historique (DFS par forks) ; `--dp` seul reprend le
       nombre de cœurs détecté par défaut, comme `--forks` seul. `--spill-dir`
       n'a d'effet qu'avec `--dp` (fragments du mode disque, cf.
       `border_ring_dp_set_spill_dir`) — `/tmp` est souvent une petite
       partition ou un tmpfs plafonné bien en-deçà de la RAM de la machine,
       qui peut saturer même sur une machine par ailleurs bien dotée (observé
       en pratique, cf. docs/tests_et_ci.md). `--dp-max-ram-mo MO` est
       OBLIGATOIRE avec `--dp` (vérifié plus bas) — taille du tampon de tri
       de la DP, en Mo (cf. `border_ring_dp_set_max_ram_mo`) ; aucune
       valeur par défaut n'est choisie à la place de l'utilisateur, une
       machine différente de celle qui a motivé ce mécanisme rendrait
       n'importe quel défaut faux dans un sens ou dans l'autre.

       `--save-rings FILE --max-rings N` reconstruit chaque anneau réel
       (pièces réelles, pas seulement le compte) et les écrit dans FILE au
       format `.back` (flux headerless de `struct possibility_packet`, même
       format que produit `gen_root`) — cf. `border_ring_reconstruct_dp`,
       tests/tools/border_ring_dp.h. Utilisable UNIQUEMENT avec `--dp` : le
       DFS brut (sans `--dp`) n'a aucune heuristique guidée par les classes et
       reste, mesuré empiriquement, bien trop lent pour cet usage sur le jeu
       réel (cf. docs/tests_et_ci.md) — rejeté explicitement plus bas. Les
       deux options sont obligatoires ensemble : `--max-rings N` est un
       plafond de sécurité (aucune valeur par défaut choisie à la place de
       l'utilisateur, même principe que `--dp-max-ram-mo`), pas une
       estimation de la masse réelle — border_ring_reconstruct_dp échoue
       bruyamment si le total reconstruit diverge du total exact SANS que ce
       plafond en soit la cause. */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--dp") == 0) {
            use_dp = 1;
            argi++;
        } else if (strcmp(argv[argi], "--forks") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr,
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] [--save-rings FILE --max-rings N] "
                        "<pieces.csv> <indices.csv>\n",
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
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] [--save-rings FILE --max-rings N] "
                        "<pieces.csv> <indices.csv>\n",
                        argv[0]);
                return 2;
            }
            spill_dir = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--dp-max-ram-mo") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr,
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] [--save-rings FILE --max-rings N] "
                        "<pieces.csv> <indices.csv>\n",
                        argv[0]);
                return 2;
            }
            char *endptr = NULL;
            dp_max_ram_mo = strtol(argv[argi + 1], &endptr, 10);
            if (endptr == argv[argi + 1] || *endptr != '\0' || dp_max_ram_mo < 1) {
                fprintf(stderr, "border_mass : --dp-max-ram-mo attend un entier positif (Mo)\n");
                return 2;
            }
            argi += 2;
        } else if (strcmp(argv[argi], "--save-rings") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr,
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] [--save-rings FILE --max-rings N] "
                        "<pieces.csv> <indices.csv>\n",
                        argv[0]);
                return 2;
            }
            save_rings_path = argv[argi + 1];
            argi += 2;
        } else if (strcmp(argv[argi], "--max-rings") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr,
                        "usage: %s [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] [--save-rings FILE --max-rings N] "
                        "<pieces.csv> <indices.csv>\n",
                        argv[0]);
                return 2;
            }
            /* 128 bits : la masse réelle des anneaux de bordure est estimée
               à ~3,8x10^37, soit 19 ordres de grandeur au-dessus de ce qu'un
               `long long` peut porter. `strtoll` aurait saturé ici sans un
               mot, ramenant un plafond volontairement énorme à ~9,2x10^18. */
            if (bd_ring_count_parse(argv[argi + 1], &max_rings) != 0 || max_rings == 0) {
                fprintf(stderr,
                        "border_mass : --max-rings attend un entier decimal positif, "
                        "jusqu'a 2^128-1 (340282366920938463463374607431768211455)\n");
                return 2;
            }
            max_rings_set = 1;
            argi += 2;
        } else {
            break;
        }
    }
    if (argc - argi != 2) {
        fprintf(stderr,
                "usage: %s [--dp] [--forks N] [--spill-dir DIR] [--dp-max-ram-mo MO] [--save-rings FILE --max-rings N] "
                "<pieces.csv> <indices.csv>\n",
                argv[0]);
        return 2;
    }
    if (use_dp && dp_max_ram_mo < 0) {
        fprintf(stderr,
                "border_mass : --dp necessite --dp-max-ram-mo MO (budget memoire dedie a un "
                "niveau de la DP, en Mo)\n");
        return 2;
    }
    if ((save_rings_path != NULL) != (max_rings_set != 0)) {
        fprintf(stderr, "border_mass : --save-rings et --max-rings sont obligatoires ensemble\n");
        return 2;
    }
    /* `--save-rings` SANS `--dp` est le mode d'ÉCHANTILLONNAGE, et c'est le
       plus rapide des deux. Mesuré sur data/pieces.csv : le walker ferme un
       anneau tous les ~10 nœuds (~15 M/s), mais le débit utile est celui des
       anneaux ECRITS — ~1 M/s, soit ~500 Mo/s, borné par le disque et non par
       la recherche ; premier anneau écrit en ~1 s. La voie `--dp` doit construire la passe avant ENTIÈRE
       puis toutes ses tables de complétion (~31 h et ~537 Go mesurés) avant
       de livrer son premier anneau : elle ne se justifie que pour la masse
       EXACTE, jamais pour un échantillon.
       Les deux produisent des racines également exploitables : chaque anneau
       a une frontière intérieure UNIQUE (vérifié sur 10^6 anneaux — la face
       tournée vers l'intérieur n'entre pas dans la classe de la pièce, donc
       deux anneaux de même motif de classes restent deux sous-problèmes
       distincts pour la recherche intérieure). */
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
        border_ring_dp_set_max_ram_mo(dp_max_ram_mo);

        if (save_rings_path != NULL) {
            FILE *rings_file = fopen(save_rings_path, "wb");
            if (rings_file == NULL) {
                fprintf(stderr, "border_mass : ouverture de '%s' impossible\n", save_rings_path);
                return 1;
            }
            struct bm_rings_ctx rings_ctx = { rings_file, 0, max_rings, 1 };
            long long delivered =
                border_ring_reconstruct_dp(map, all, forks, max_rings, bm_on_ring_found, &rings_ctx);
            if (fclose(rings_file) != 0) {
                fprintf(stderr, "border_mass : cloture de '%s' impossible (disque plein ?)\n", save_rings_path);
                return 1;
            }
            printf("%lld anneau(x) reconstruit(s) et sauvegarde(s) dans %s\n", delivered, save_rings_path);
            return 0;
        }

        bd_ring_count_t total = border_ring_count_dp(map, all, forks);
        char total_str[BD_RING_COUNT_STRLEN];
        bd_ring_count_format(total, total_str, sizeof total_str);
        printf("masse totale des anneaux de bordure valides : %s\n", total_str);
        return 0;
    }

    int8_t order[BORDER_RING_LEN][2];
    border_corners_first_order(order);

    /* Échantillonnage (`--save-rings` sans `--dp`) : le parent écrit lui-même
       les anneaux que l'expansion de frontière referme au passage — ils sont
       trouvés AVANT le fork, donc aucun worker ne les verra. Fichier à part
       (`<path>.expand`), concaténé avec ceux des workers à la fin : un seul
       `FILE*` partagé à travers `fork()` entrelacerait les écritures des
       workers dans le même fichier. */
    struct bm_rings_ctx expand_rings = { NULL, 0, max_rings, save_rings_path != NULL };
    char expand_path[BM_RINGS_PATH_MAX];
    if (save_rings_path != NULL) {
        if (bm_rings_part_path(expand_path, sizeof expand_path, save_rings_path, "expand") != 0) {
            return 1;
        }
        expand_rings.file = fopen(expand_path, "wb");
        if (expand_rings.file == NULL) {
            fprintf(stderr, "border_mass : ouverture de '%s' impossible\n", expand_path);
            return 1;
        }
    }

    struct bm_collect_ctx collect;
    memset(&collect, 0, sizeof collect);
    long long completed_during_expansion =
        border_walk_expand_frontier(map, all, order, forks * 8,
                                     bm_collect_partial, &collect,
                                     save_rings_path != NULL ? bm_on_ring_found : NULL, &expand_rings);

    if (expand_rings.file != NULL && fclose(expand_rings.file) != 0) {
        fprintf(stderr, "border_mass : cloture de '%s' impossible (disque plein ?)\n", expand_path);
        return 1;
    }

    fprintf(stderr, "border_mass : %d partitions, %d worker(s)\n", collect.count, forks);

    if (collect.count == 0) {
        if (save_rings_path != NULL) {
            long long written = bm_rings_concat(save_rings_path, expand_path, NULL, 0);
            printf("%lld anneau(x) sauvegarde(s) dans %s\n", written, save_rings_path);
        } else {
            printf("masse totale des anneaux de bordure valides : %lld\n", completed_during_expansion);
        }
        free(collect.partitions);
        return 0;
    }

    /* Quota restant apres l'expansion, reparti entre les workers : pas de
       compteur partage ni d'IPC a arbitrer, chaque worker s'arrete sur son
       propre budget et le total ne peut pas depasser `--max-rings`. Le reste
       de la division va aux premiers workers, un de plus chacun — sans quoi
       `--max-rings 1` sur 2 workers donnerait 0 anneau, et toute valeur non
       multiple du nombre de workers perdrait jusqu'à `forks - 1` anneaux.
       `--max-rings` reste une borne SUPÉRIEURE : un worker dont les
       partitions s'épuisent avant son quota laisse sa part inutilisée, et
       rien ne la redistribue (il faudrait un compteur partagé entre process
       pour cela). Sans conséquence sur le jeu réel, où chaque partition
       contient plus d'anneaux qu'on n'en demandera jamais. */
    bd_ring_count_t rings_remaining = 0;
    if (save_rings_path != NULL && max_rings > (bd_ring_count_t)expand_rings.written) {
        rings_remaining = max_rings - (bd_ring_count_t)expand_rings.written;
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

            bd_ring_count_t budget = rings_remaining / (bd_ring_count_t)forks
                                     + ((bd_ring_count_t)w < rings_remaining % (bd_ring_count_t)forks ? 1 : 0);
            struct bm_rings_ctx worker_rings = { NULL, 0, budget, 1 };
            char worker_path[BM_RINGS_PATH_MAX];
            if (save_rings_path != NULL && budget > 0) {
                char tag[32];
                snprintf(tag, sizeof tag, "w%d", w);
                if (bm_rings_part_path(worker_path, sizeof worker_path, save_rings_path, tag) != 0) {
                    exit(1);
                }
                worker_rings.file = fopen(worker_path, "wb");
                if (worker_rings.file == NULL) {
                    fprintf(stderr, "border_mass : ouverture de '%s' impossible\n", worker_path);
                    exit(1);
                }
            }

            long long sub = bm_run_worker(w, forks, map, all, order, collect.partitions, collect.count,
                                          worker_rings.file != NULL ? &worker_rings : NULL);

            if (worker_rings.file != NULL && fclose(worker_rings.file) != 0) {
                fprintf(stderr, "border_mass : cloture de '%s' impossible (disque plein ?)\n", worker_path);
                exit(1);
            }
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

    if (save_rings_path != NULL) {
        long long written = bm_rings_concat(save_rings_path, expand_path, save_rings_path, forks);
        if (written < 0) {
            free(pipes);
            free(pids);
            free(collect.partitions);
            return 1;
        }
        printf("%lld anneau(x) sauvegarde(s) dans %s\n", written, save_rings_path);
    } else {
        printf("masse totale des anneaux de bordure valides : %lld\n", total);
    }

    free(pipes);
    free(pids);
    free(collect.partitions);
    return 0;
}
