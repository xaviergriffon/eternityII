#include "tools/border_ring_dp.h"

#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* Borne généreuse : le vrai jeu 256 pièces n'en produit que 15-18 (paires
   de couleurs "anneau" x forme coin/bord), le jeu 16 pièces des tests
   beaucoup moins encore (chaque pièce y forme sa propre classe). */
#define BD_MAX_CLASSES 64

/* Paire ORDONNÉE (couleur requise en entrée, couleur produite en sortie) —
   PAS une paire non ordonnée, cf. le commentaire de bd_build_classes pour
   pourquoi cette distinction est cruciale (32× de sur-comptage sinon). */
struct bd_class {
    int8_t color_a; /* couleur requise en entrée */
    int8_t color_b; /* couleur produite en sortie */
    int8_t is_corner; /* 2 faces à 0 (coin) vs 1 (bord) */
};

struct bd_ctx {
    struct bd_class classes[BD_MAX_CLASSES];
    int nb_classes;
    int8_t is_corner_at[BORDER_RING_LEN]; /* forme de chaque case de l'anneau */
};

/* ===========================================================================
 * Table de hachage à adressage ouvert représentant UN NIVEAU (une position
 * de l'anneau) — PAS toutes les positions mélangées comme la première
 * version de ce fichier. Clé = couleur requise + compteurs restants par
 * classe (`1 + nb_classes` octets, calculé sur `nb_classes` RÉEL, jamais sur
 * la borne `BD_MAX_CLASSES`). Stockage en tableaux parallèles (`keys` plat
 * sans alignement forcé, `values`, bitmap `occupied`) — un tableau de
 * `struct { clé; valeur; occupé; }` gâchait ~40 % de chaque emplacement en
 * remplissage d'alignement (mesuré : ~80 octets/emplacement contre ~38 avec
 * ce stockage, avant même le changement de granularité ci-dessous).
 *
 * `bd_level_add` ACCUMULE (contrairement à un memo classique) : plusieurs
 * états du niveau précédent peuvent transiter vers le MÊME état de ce
 * niveau (même couleur requise, mêmes compteurs restants) par des chemins
 * différents — leurs nombres de façons s'additionnent, ils ne s'écrasent
 * pas. C'est exactement la mémoïsation « par niveau » : un état à la
 * position P ne dépend jamais que du niveau P+1 déjà calculé, jamais des
 * autres positions — contrairement à la première version qui mémoïsait
 * TOUTES les positions dans une seule table globale (chaque entrée y portait
 * la position en plus, gâchant un octet ET empêchant de libérer les niveaux
 * déjà consommés). Ne garder que le niveau courant + le niveau suivant en
 * construction est ce qui permet de jeter le reste : c'est le vrai levier
 * mémoire, plus important que le gain d'octets par emplacement. */
struct bd_level {
    uint8_t *keys;      /* capacity * key_len octets */
    long long *values;  /* capacity emplacements */
    uint8_t *occupied;  /* bitmap, ceil(capacity/8) octets */
    size_t capacity;    /* toujours une puissance de 2 */
    size_t used;
    int key_len; /* 1 + nb_classes, fixe pour tout l'appel */
};

static double bd_level_bytes(const struct bd_level *level)
{
    return (double)level->capacity * ((size_t)level->key_len + sizeof(long long)) +
           (double)((level->capacity + 7) / 8);
}

/* Pas de repli silencieux sur un échec d'allocation : un niveau à moitié
   construit donnerait un total FAUX (sous-évalué) sans le signaler — mieux
   vaut échouer bruyamment (même philosophie que border_mass.c pour un
   worker en échec, cf. son commentaire d'en-tête). */
static void bd_level_alloc_or_die(struct bd_level *level)
{
    level->keys = malloc(level->capacity * (size_t)level->key_len);
    level->values = malloc(level->capacity * sizeof *level->values);
    level->occupied = calloc((level->capacity + 7) / 8, 1);
    if (level->keys == NULL || level->values == NULL || level->occupied == NULL) {
        fprintf(stderr,
                "border_ring_count_dp : allocation d'un niveau de %zu emplacements "
                "(%.2f Go) impossible — arret\n",
                level->capacity, bd_level_bytes(level) / (1024.0 * 1024.0 * 1024.0));
        exit(1);
    }
}

static void bd_level_init(struct bd_level *level, int key_len, size_t capacity_hint)
{
    /* Plancher volontairement bas (pas 1024) : le mode disque appelle ceci
       une fois par FRAGMENT, potentiellement des milliers de fois par
       transition — un plancher trop haut gâche des dizaines de Mo en
       emplacements vides et, pire, gonfle artificiellement la taille
       mesurée du niveau (bd_level_bytes), faussant le choix du nombre de
       fragments du niveau suivant (bd_pick_nb_shards) vers toujours plus de
       fragments. bd_level_add fait grossir la table au besoin (facteur de
       charge > 0,6), donc un plancher bas ne coûte qu'un ou deux
       agrandissements de plus sur les niveaux réellement gros. */
    level->capacity = 1u << 4;
    while (level->capacity < capacity_hint) {
        level->capacity <<= 1;
    }
    level->key_len = key_len;
    bd_level_alloc_or_die(level);
    level->used = 0;
}

static void bd_level_free(struct bd_level *level)
{
    free(level->keys);
    free(level->values);
    free(level->occupied);
    level->keys = NULL;
    level->values = NULL;
    level->occupied = NULL;
}

static int bd_level_is_occupied(const struct bd_level *level, size_t idx)
{
    return (level->occupied[idx >> 3] >> (idx & 7)) & 1;
}

static void bd_level_mark_occupied(struct bd_level *level, size_t idx)
{
    level->occupied[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static uint64_t bd_fnv1a(const uint8_t *bytes, int len)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < len; i++) {
        h ^= (uint64_t)bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void bd_level_grow(struct bd_level *level);

/* Insère `key` avec la valeur `delta`, ou l'AJOUTE à la valeur existante si
   `key` est déjà présente — jamais un simple écrasement, cf. le commentaire
   de `struct bd_level`. */
static void bd_level_add(struct bd_level *level, const uint8_t *key, long long delta)
{
    if (level->used * 10 >= level->capacity * 6) { /* facteur de charge > 0,6 */
        bd_level_grow(level);
    }
    uint64_t h = bd_fnv1a(key, level->key_len);
    size_t mask = level->capacity - 1;
    size_t idx = (size_t)h & mask;
    for (;;) {
        if (!bd_level_is_occupied(level, idx)) {
            memcpy(level->keys + idx * (size_t)level->key_len, key, (size_t)level->key_len);
            level->values[idx] = delta;
            bd_level_mark_occupied(level, idx);
            level->used++;
            return;
        }
        if (memcmp(level->keys + idx * (size_t)level->key_len, key, (size_t)level->key_len) == 0) {
            level->values[idx] += delta;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static void bd_level_grow(struct bd_level *level)
{
    struct bd_level old = *level;

    level->capacity = old.capacity * 2;
    bd_level_alloc_or_die(level);
    level->used = 0;

    for (size_t i = 0; i < old.capacity; i++) {
        if (bd_level_is_occupied(&old, i)) {
            /* Entrées déjà uniques (on rehache une table valide) : un simple
               bd_level_add fonctionne, aucune accumulation ne s'y déclenche
               jamais. */
            bd_level_add(level, old.keys + i * (size_t)old.key_len, old.values[i]);
        }
    }
    free(old.keys);
    free(old.values);
    free(old.occupied);
}

/* ===========================================================================
 * Construction des classes — INCHANGÉ depuis la version précédente.
 */

/* Construit les classes d'équivalence à partir des pièces réelles (rotation
   0 = pièce d'origine, cf. rotate_all_parts).
 *
 * ATTENTION — piège corrigé après une mesure qui donnait 128 au lieu de 4
 * sur `data/pieces16.csv` (32× trop) : la classe n'est PAS la paire NON
 * ORDONNÉE des deux faces "anneau" d'une pièce. Chaque pièce réelle a une
 * orientation FIXE, intrinsèque à sa géométrie — DEUX pièces qui partagent
 * les deux mêmes couleurs mais dans l'ordre inverse (l'une présente A en
 * entrée et B en sortie, l'autre B en entrée et A en sortie) ne sont PAS
 * interchangeables : traiter les deux orientations comme une seule classe
 * fait utiliser CHAQUE pièce dans les deux sens, alors qu'une pièce donnée
 * ne peut physiquement en fournir qu'un seul. La classe est donc la paire
 * ORDONNÉE (couleur requise en entrée, couleur produite en sortie).
 *
 * Cet ordre se lit directement sur le cycle des faces TOP(0)→RIGHT(1)→
 * BOTTOM(2)→LEFT(3)→TOP, dans le sens où tourne `rotatePart` (part.c) :
 * pour une pièce-bord (1 face à 0, en `zero_field`), la face "entrée"
 * (celle qui doit correspondre à la case déjà posée) est celle À UN CRAN
 * AVANT `zero_field` dans ce cycle, et la face "sortie" (celle que la case
 * suivante devra faire correspondre) est celle à UN CRAN APRÈS — invariant
 * par rotation (propriété du cycle de faces de la pièce, pas de son
 * orientation à un instant donné), donc calculable une seule fois sur la
 * rotation 0 et valable à N'IMPORTE QUELLE position de bord de même forme.
 * Vérifié en dérivant indépendamment `what_search_in_grid_to_key` sur les 4
 * segments du pourtour (ligne du haut, colonne droite, ligne du bas,
 * colonne gauche) : la règle « entrée = zero-1, sortie = zero+1 » reproduit
 * exactement les 4 formules k1..k4, dans les 2 sens de parcours.
 *
 * Une pièce-coin (2 faces à 0, forcément adjacentes en `z1`,`z2` avec
 * `z2 = (z1+1)%4`) suit la même logique : entrée = un cran AVANT `z1`,
 * sortie = un cran APRÈS `z2` — vérifié de la même façon contre
 * `what_search_in_grid_to_key(0,0,...)` (l'ouverture).
 *
 * `id_to_class[id]` permet de retrouver la classe d'un candidat réel
 * (nécessaire pour décrémenter le bon compteur à l'ouverture, cf.
 * bd_run_opening). */
static int bd_build_classes(struct array_part *all_rotate_parts, struct bd_ctx *ctx,
                             int counts[BD_MAX_CLASSES], int16_t *id_to_class, int n)
{
    int nb_classes = 0;
    for (int id = 1; id <= n; id++) {
        struct part *p = &all_rotate_parts->parts[id]; /* rotation 0 = pièce d'origine */
        int8_t vals[4] = { p->top, p->right, p->bottom, p->left };

        int zero_count = 0, zero_field = -1;
        for (int k = 0; k < 4; k++) {
            if (vals[k] == 0) {
                zero_count++;
                zero_field = k;
            }
        }
        id_to_class[id] = -1;
        if (zero_count != 1 && zero_count != 2) {
            continue; /* pièce intérieure (0 face à 0), ou forme inattendue : hors bordure */
        }
        int is_corner = (zero_count == 2);

        int8_t required_val, outgoing_val;
        if (is_corner) {
            int z1 = zero_field; /* le second zéro trouvé par la boucle ci-dessus */
            int z2;
            for (z2 = 0; z2 < 4; z2++) {
                if (z2 != z1 && vals[z2] == 0) {
                    break;
                }
            }
            if ((z2 + 1) % 4 == z1) {
                int t = z1;
                z1 = z2;
                z2 = t;
            }
            /* invariant : z2 == (z1+1)%4 (2 zéros adjacents, cf. commentaire ci-dessus) */
            required_val = vals[(z1 + 3) % 4];
            outgoing_val = vals[(z2 + 1) % 4];
        } else {
            required_val = vals[(zero_field + 3) % 4];
            outgoing_val = vals[(zero_field + 1) % 4];
        }

        int found = -1;
        for (int c = 0; c < nb_classes; c++) {
            if (ctx->classes[c].color_a == required_val && ctx->classes[c].color_b == outgoing_val &&
                ctx->classes[c].is_corner == is_corner) {
                found = c;
                break;
            }
        }
        if (found < 0) {
            found = nb_classes++;
            ctx->classes[found].color_a = required_val;
            ctx->classes[found].color_b = outgoing_val;
            ctx->classes[found].is_corner = (int8_t)is_corner;
            counts[found] = 0;
        }
        counts[found]++;
        id_to_class[id] = (int16_t)found;
    }
    return nb_classes;
}

/* ===========================================================================
 * Transition d'un niveau vers le suivant — cœur de la DP, appelé aussi bien
 * séquentiellement que par chaque worker forké sur SA plage d'indices
 * [start, end) du niveau courant (jamais sur des plages qui se chevauchent :
 * chaque worker écrit dans son PROPRE niveau local, fusionné par le parent
 * après coup — cf. bd_transition_parallel).
 */
static void bd_transition_range(const struct bd_ctx *ctx, int want_corner,
                                 const struct bd_level *level_cur, size_t start, size_t end,
                                 struct bd_level *level_next)
{
    int nb = ctx->nb_classes;
    uint8_t next_key[1 + BD_MAX_CLASSES];

    for (size_t idx = start; idx < end; idx++) {
        if (!bd_level_is_occupied(level_cur, idx)) {
            continue;
        }
        const uint8_t *key = level_cur->keys + idx * (size_t)level_cur->key_len;
        int8_t required = (int8_t)key[0];
        const int8_t *counts = (const int8_t *)(key + 1);
        long long ways = level_cur->values[idx];

        for (int c = 0; c < nb; c++) {
            if (ctx->classes[c].is_corner != want_corner || counts[c] == 0 ||
                ctx->classes[c].color_a != required) {
                continue;
            }
            next_key[0] = (uint8_t)ctx->classes[c].color_b;
            memcpy(next_key + 1, counts, (size_t)nb);
            next_key[1 + c]--;
            bd_level_add(level_next, next_key, ways * counts[c]);
        }
    }
}

/* Variante terminale (dernière case de l'anneau) : au lieu de produire un
   niveau suivant, cumule directement dans le total dès que la couleur
   produite ferme l'anneau sur la pièce d'ouverture. */
static long long bd_finalize_range(const struct bd_ctx *ctx, int want_corner, int8_t closure_target,
                                    const struct bd_level *level_cur, size_t start, size_t end)
{
    int nb = ctx->nb_classes;
    long long total = 0;

    for (size_t idx = start; idx < end; idx++) {
        if (!bd_level_is_occupied(level_cur, idx)) {
            continue;
        }
        const uint8_t *key = level_cur->keys + idx * (size_t)level_cur->key_len;
        int8_t required = (int8_t)key[0];
        const int8_t *counts = (const int8_t *)(key + 1);
        long long ways = level_cur->values[idx];

        for (int c = 0; c < nb; c++) {
            if (ctx->classes[c].is_corner != want_corner || counts[c] == 0 ||
                ctx->classes[c].color_a != required || ctx->classes[c].color_b != closure_target) {
                continue;
            }
            total += ways * counts[c];
        }
    }
    return total;
}

/* ===========================================================================
 * Parallélisation par forks : un niveau assez gros (cf. bd_fork_min_states)
 * est réparti en `nb_workers` plages d'indices contiguës, chacune traitée
 * par un process forké écrivant son niveau-suivant LOCAL dans un fichier
 * temporaire — jamais un pipe : la sortie sérialisée d'un worker peut
 * atteindre des dizaines de Mo, bien au-delà du tampon noyau d'un pipe
 * (~64 Ko), et le parent ne lit les workers qu'un par un (comme le fait déjà
 * border_mass.c pour --forks) — un pipe bloquerait le worker en écriture
 * pendant que le parent lit un autre worker, interblocage classique. Le
 * fichier n'a pas cette limite. Le parent attend TOUS les workers puis
 * fusionne leurs fichiers dans le niveau suivant via bd_level_add (les
 * clés produites par deux workers différents peuvent authentiquement
 * coïncider — deux états du niveau courant peuvent transiter vers le même
 * état suivant — d'où l'accumulation, pas une simple concaténation).
 *
 * Le seuil par défaut (50000) n'est jamais atteint par les petits fixtures
 * de test_border_ring_dp.c — `border_ring_dp_set_fork_min_states_for_tests`
 * (test-only, jamais déclarée dans border_ring_dp.h, cf. le même schéma que
 * `stock_spill_set_segment_bytes_for_tests`) l'abaisse pour exercer
 * réellement la parallélisation par forks dans un test, pas seulement le
 * chemin séquentiel. */
static size_t bd_fork_min_states = 50000;

void border_ring_dp_set_fork_min_states_for_tests(size_t n)
{
    bd_fork_min_states = n;
}

static void bd_level_write_file(const struct bd_level *level, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", path);
        exit(1);
    }
    int32_t key_len = level->key_len;
    uint64_t count = level->used;
    fwrite(&key_len, sizeof key_len, 1, fp);
    fwrite(&count, sizeof count, 1, fp);
    for (size_t idx = 0; idx < level->capacity; idx++) {
        if (bd_level_is_occupied(level, idx)) {
            fwrite(level->keys + idx * (size_t)level->key_len, (size_t)level->key_len, 1, fp);
            fwrite(&level->values[idx], sizeof(long long), 1, fp);
        }
    }
    fclose(fp);
}

static void bd_level_merge_file(struct bd_level *level, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : lecture de '%s' impossible — arret\n", path);
        exit(1);
    }
    int32_t key_len = 0;
    uint64_t count = 0;
    if (fread(&key_len, sizeof key_len, 1, fp) != 1 || fread(&count, sizeof count, 1, fp) != 1 ||
        key_len != level->key_len) {
        fprintf(stderr, "border_ring_count_dp : fichier de fusion '%s' corrompu — arret\n", path);
        exit(1);
    }
    uint8_t key[1 + BD_MAX_CLASSES];
    for (uint64_t i = 0; i < count; i++) {
        long long value;
        if (fread(key, (size_t)key_len, 1, fp) != 1 || fread(&value, sizeof value, 1, fp) != 1) {
            fprintf(stderr, "border_ring_count_dp : fichier de fusion '%s' tronque — arret\n", path);
            exit(1);
        }
        bd_level_add(level, key, value);
    }
    fclose(fp);
}

/* Tue et récolte les `count` premiers workers déjà forkés — même garde que
   `bm_abort_workers` (border_mass.c) pour la même raison : sans elle, un
   worker déjà lancé continuerait de tourner sans personne pour l'attendre
   si pipe()/fork() échoue en cours de boucle. */
static void bd_abort_workers(const pid_t *pids, int count)
{
    for (int i = 0; i < count; i++) {
        kill(pids[i], SIGTERM);
    }
    for (int i = 0; i < count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }
}

static void bd_transition_parallel(const struct bd_ctx *ctx, int want_corner,
                                    const struct bd_level *level_cur, int nb_workers,
                                    struct bd_level *level_next)
{
    char (*paths)[64] = malloc((size_t)nb_workers * sizeof *paths);
    pid_t *pids = malloc((size_t)nb_workers * sizeof *pids);
    size_t chunk = (level_cur->capacity + (size_t)nb_workers - 1) / (size_t)nb_workers;

    /* Vider les tampons stdio AVANT fork() : sinon chaque enfant hérite
       d'une copie du contenu déjà écrit (mais pas encore vidé, cf.
       "nindices"/"ntiles" au tout début de border_mass.c) et le revide
       indépendamment à son propre exit() — dupliquant ces lignes une fois
       par worker, à CHAQUE niveau forké. Même piège, même correctif que
       border_mass.c avant son propre fork() (cf. son commentaire). */
    fflush(stdout);
    fflush(stderr);

    for (int w = 0; w < nb_workers; w++) {
        snprintf(paths[w], sizeof paths[w], "/tmp/etii_bd_level_%d_%d", (int)getpid(), w);

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_ring_count_dp : fork() a echoue pour le worker %d — arret\n", w);
            bd_abort_workers(pids, w);
            exit(1);
        }
        if (pid == 0) {
            size_t start = (size_t)w * chunk;
            size_t end = start + chunk;
            if (end > level_cur->capacity) {
                end = level_cur->capacity;
            }
            struct bd_level local;
            bd_level_init(&local, level_cur->key_len, (end - start) / 4 + 16);
            bd_transition_range(ctx, want_corner, level_cur, start, end, &local);
            bd_level_write_file(&local, paths[w]);
            exit(0);
        }
        pids[w] = pid;
    }

    int any_failed = 0;
    for (int w = 0; w < nb_workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : le worker %d a echoue (status=%d) — arret\n", w, status);
            any_failed = 1;
        }
    }
    if (any_failed) {
        /* Best-effort : un worker qui échoue après que d'autres ont déjà
           écrit leur fichier ne doit pas laisser de déchets dans /tmp —
           unlink() sur un chemin jamais créé échoue silencieusement, sans
           conséquence. */
        for (int w = 0; w < nb_workers; w++) {
            unlink(paths[w]);
        }
        exit(1);
    }

    bd_level_init(level_next, level_cur->key_len, level_cur->used / 2 + 16);
    for (int w = 0; w < nb_workers; w++) {
        bd_level_merge_file(level_next, paths[w]);
        unlink(paths[w]);
    }
    free(paths);
    free(pids);
}

/* ===========================================================================
 * Mode disque : au-delà de bd_disk_mode_min_bytes, un niveau n'est plus
 * représenté par une seule table en mémoire mais par K fragments sur disque
 * (K = bd_pick_nb_shards), chacun assez petit pour tenir large en mémoire —
 * hachage de partitionnement externe classique (l'équivalent, pour un
 * GROUP BY, d'un tri externe pour un ORDER BY qui ne tient pas en RAM). Le
 * pic mémoire d'une transition devient K/nb_workers fragments simultanés au
 * lieu du niveau entier — c'est ce qui manquait à bd_transition_parallel
 * seule : elle bornait le CALCUL par plages d'indices, mais sa table de
 * sortie fusionnée par le parent restait un niveau ENTIER en mémoire (9,28 Go
 * mesurés au niveau 19 sur la machine visée, avant l'échec au niveau 20).
 *
 * Un fragment "canonique" (`shard_<d>.bin`) est toujours dédupliqué — même
 * format qu'un niveau en mémoire sérialisé par bd_level_write_file, jamais
 * une simple concaténation. Un fragment "brut" (`part_<source>_<dest>.bin`,
 * transitoire, supprimé dès qu'il est fusionné) ne l'est pas : deux sources
 * différentes peuvent y déposer la même clé, à fusionner par accumulation
 * (bd_level_add) — exactement ce que bd_transition_parallel fait déjà pour
 * les niveaux locaux de ses workers, seule la granularité change (par
 * fragment plutôt que par le niveau entier).
 *
 * Chaque transition en mode disque se fait en DEUX vagues de forks
 * successives, jamais chevauchées (la seconde attend que la première ait
 * fini) :
 *   1. Éclatement (bd_transition_disk) : chaque worker prend une plage
 *      contiguë de fragments SOURCE, charge chacun (petit, borné par
 *      construction) en mémoire l'un après l'autre, transite, et route
 *      chaque résultat par hachage de sa clé vers l'un des fichiers bruts
 *      qu'il tient ouverts pour toute sa plage — jamais un fichier par
 *      fragment source, ce qui ferait exploser le nombre de fichiers
 *      transitoires en (fragments source × fragments suivants).
 *   2. Compactage (bd_compact_dir) : chaque worker prend une plage contiguë
 *      de fragments DESTINATION, fusionne (accumule) les morceaux bruts
 *      qu'y ont déposés tous les workers de l'éclatement, et écrit LE
 *      fragment canonique correspondant.
 */
struct bd_shard_set {
    /* Assez pour "/tmp/etii_bd_<pid>_pos<pos>" avec de la marge ; volontairement
       petit (pas 512) pour que gcc puisse prouver, a la compilation, qu'aucune
       concatenation de suffixe ("/shard_%d.bin" etc.) dans un buffer de 512
       octets ne peut tronquer — sinon -Wformat-truncation (WERROR=1) refuse de
       compiler meme si aucun chemin reel ne s'en approche. */
    char dir[128];
    int nb_shards;
    int key_len;
    size_t total_used;
    double total_bytes;
};

/* Bornes calées sur la machine visée (2x10 cœurs, 48 Go) : une compaction ou
   un éclatement peut faire tourner jusqu'à nb_workers fragments à la fois,
   donc nb_workers x bd_shard_target_bytes doit rester très en-dessous de la
   RAM disponible — 768 Mo x 20 = 15 Go au pic sur 48 Go, large marge pour
   l'OS et les tampons d'E/S. bd_disk_mode_min_bytes fait basculer en mode
   disque BIEN avant qu'un niveau unique n'approche cette même limite (2 Go,
   très en-dessous des 9,28 Go qui ont fait échouer la version précédente) :
   l'objectif est de ne JAMAIS matérialiser un niveau entier au-delà de ce
   seuil, pas de rattraper après coup. */
static double bd_shard_target_bytes = 768.0 * 1024.0 * 1024.0;
static double bd_disk_mode_min_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0;

/* Test-only, jamais dans border_ring_dp.h — même schéma que
   border_ring_dp_set_fork_min_states_for_tests : abaisser ces deux seuils
   permet à un fixture minuscule d'exercer réellement le mode disque (et
   plusieurs fragments) sans construire un niveau de plusieurs Go. */
void border_ring_dp_set_disk_mode_min_bytes_for_tests(double n)
{
    bd_disk_mode_min_bytes = n;
}

void border_ring_dp_set_shard_target_bytes_for_tests(double n)
{
    bd_shard_target_bytes = n;
}

/* Borne haute généreuse : au-delà, on dégrade gracieusement (fragments plus
   gros que la cible) plutôt que de laisser exploser le nombre de fichiers
   transitoires ouverts simultanément par un worker. */
#define BD_MAX_SHARDS 4096

static int bd_pick_nb_shards(double total_bytes, int nb_workers)
{
    size_t want = (size_t)(total_bytes / bd_shard_target_bytes) + 1;
    size_t n = 1;
    while (n < want || n < (size_t)nb_workers) {
        if (n >= BD_MAX_SHARDS) {
            return BD_MAX_SHARDS;
        }
        n <<= 1;
    }
    return (int)n;
}

static int bd_shard_of(const uint8_t *key, int key_len, int nb_shards)
{
    return (int)(bd_fnv1a(key, key_len) % (uint64_t)nb_shards);
}

static void bd_mkdir_or_die(const char *dir)
{
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "border_ring_count_dp : creation du repertoire '%s' impossible — arret\n", dir);
        exit(1);
    }
}

static void bd_rm_shard_set(const struct bd_shard_set *set)
{
    for (int d = 0; d < set->nb_shards; d++) {
        char path[512];
        snprintf(path, sizeof path, "%s/shard_%d.bin", set->dir, d);
        unlink(path);
    }
    rmdir(set->dir);
}

/* Comme bd_level_merge_file, mais construit un niveau NEUF à partir d'un
   fragment canonique (jamais de fusion dans un niveau préexistant) — utilisé
   pour charger UN SEUL fragment à la fois, jamais plusieurs simultanément. */
static void bd_level_load_file(struct bd_level *level, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : lecture de '%s' impossible — arret\n", path);
        exit(1);
    }
    int32_t key_len = 0;
    uint64_t count = 0;
    if (fread(&key_len, sizeof key_len, 1, fp) != 1 || fread(&count, sizeof count, 1, fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : fragment '%s' corrompu — arret\n", path);
        exit(1);
    }
    bd_level_init(level, key_len, (size_t)count * 2 + 16);
    uint8_t key[1 + BD_MAX_CLASSES];
    for (uint64_t i = 0; i < count; i++) {
        long long value;
        if (fread(key, (size_t)key_len, 1, fp) != 1 || fread(&value, sizeof value, 1, fp) != 1) {
            fprintf(stderr, "border_ring_count_dp : fragment '%s' tronque — arret\n", path);
            exit(1);
        }
        bd_level_add(level, key, value);
    }
    fclose(fp);
}

/* Fusionne un fragment BRUT (non dédupliqué, pas d'en-tête — juste une suite
   de (clé, valeur), la taille se déduit de la fin de fichier) dans `level`
   par accumulation. Un fichier absent n'est pas une erreur : un worker
   d'éclatement ouvre et referme les fichiers de TOUS les fragments
   destination même quand il n'y route rien (fichier présent mais vide) —
   l'absence pure ne devrait donc jamais arriver, mais rester tolérant ici ne
   coûte rien et évite une dépendance fragile à cette garantie. */
static void bd_raw_merge_file(struct bd_level *level, const char *path, int key_len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return;
    }
    uint8_t key[1 + BD_MAX_CLASSES];
    for (;;) {
        if (fread(key, (size_t)key_len, 1, fp) != 1) {
            break;
        }
        long long value;
        if (fread(&value, sizeof value, 1, fp) != 1) {
            fprintf(stderr, "border_ring_count_dp : fragment brut '%s' tronque — arret\n", path);
            exit(1);
        }
        bd_level_add(level, key, value);
    }
    fclose(fp);
}

/* Variante de bd_transition_range qui ne mémoïse PAS en mémoire : chaque
   transition produite est écrite brute (non dédupliquée) dans le fichier de
   fragment destination désigné par le hachage de sa clé — permet de ne
   charger qu'UN fragment source à la fois, jamais le niveau entier. */
static void bd_transition_range_scatter(const struct bd_ctx *ctx, int want_corner,
                                         const struct bd_level *level_cur, int nb_shards_next,
                                         FILE **out_files)
{
    int nb = ctx->nb_classes;
    uint8_t next_key[1 + BD_MAX_CLASSES];

    for (size_t idx = 0; idx < level_cur->capacity; idx++) {
        if (!bd_level_is_occupied(level_cur, idx)) {
            continue;
        }
        const uint8_t *key = level_cur->keys + idx * (size_t)level_cur->key_len;
        int8_t required = (int8_t)key[0];
        const int8_t *counts = (const int8_t *)(key + 1);
        long long ways = level_cur->values[idx];

        for (int c = 0; c < nb; c++) {
            if (ctx->classes[c].is_corner != want_corner || counts[c] == 0 ||
                ctx->classes[c].color_a != required) {
                continue;
            }
            next_key[0] = (uint8_t)ctx->classes[c].color_b;
            memcpy(next_key + 1, counts, (size_t)nb);
            next_key[1 + c]--;
            long long delta = ways * counts[c];
            int d = bd_shard_of(next_key, 1 + nb, nb_shards_next);
            fwrite(next_key, (size_t)(1 + nb), 1, out_files[d]);
            fwrite(&delta, sizeof delta, 1, out_files[d]);
        }
    }
}

/* Fusionne les fragments bruts déposés sous `dir` (un par (source, fragment
   destination)) en fragments canoniques `shard_<d>.bin`, un worker par plage
   contiguë de fragments destination — jamais plus d'UN fragment en mémoire à
   la fois par worker. Réutilisé à la fois par bd_transition_disk
   (`nb_sources` = nombre de workers d'éclatement) et par bd_level_to_shards
   (`nb_sources` = 1, un seul passage séquentiel de répartition). */
static void bd_compact_dir(const char *dir, int nb_sources, int nb_shards, int key_len, int nb_workers,
                            size_t *out_total_used, double *out_total_bytes, double *out_max_shard_bytes)
{
    int workers = nb_workers < nb_shards ? nb_workers : nb_shards;
    if (workers < 1) {
        workers = 1;
    }

    fflush(stdout);
    fflush(stderr);

    pid_t *pids = malloc((size_t)workers * sizeof *pids);
    char (*summary_paths)[512] = malloc((size_t)workers * sizeof *summary_paths);
    size_t chunk = ((size_t)nb_shards + (size_t)workers - 1) / (size_t)workers;

    for (int w = 0; w < workers; w++) {
        snprintf(summary_paths[w], sizeof summary_paths[w], "%s/summary_%d.bin", dir, w);

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_ring_count_dp : fork() a echoue (compactage, worker %d) — arret\n", w);
            bd_abort_workers(pids, w);
            exit(1);
        }
        if (pid == 0) {
            size_t start = (size_t)w * chunk;
            size_t end = start + chunk;
            if (end > (size_t)nb_shards) {
                end = (size_t)nb_shards;
            }
            size_t used_sum = 0;
            double bytes_sum = 0.0, max_bytes = 0.0;
            for (size_t d = start; d < end; d++) {
                struct bd_level level;
                bd_level_init(&level, key_len, 1 << 4);
                for (int s = 0; s < nb_sources; s++) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/part_%d_%d.bin", dir, s, (int)d);
                    bd_raw_merge_file(&level, path, key_len);
                    unlink(path);
                }
                char out_path[512];
                snprintf(out_path, sizeof out_path, "%s/shard_%d.bin", dir, (int)d);
                bd_level_write_file(&level, out_path);
                used_sum += level.used;
                double bytes = bd_level_bytes(&level);
                bytes_sum += bytes;
                if (bytes > max_bytes) {
                    max_bytes = bytes;
                }
                bd_level_free(&level);
            }
            FILE *sf = fopen(summary_paths[w], "wb");
            if (sf == NULL) {
                fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", summary_paths[w]);
                exit(1);
            }
            fwrite(&used_sum, sizeof used_sum, 1, sf);
            fwrite(&bytes_sum, sizeof bytes_sum, 1, sf);
            fwrite(&max_bytes, sizeof max_bytes, 1, sf);
            fclose(sf);
            exit(0);
        }
        pids[w] = pid;
    }

    int any_failed = 0;
    for (int w = 0; w < workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : le compactage du worker %d a echoue (status=%d) — arret\n",
                    w, status);
            any_failed = 1;
        }
    }
    free(pids);
    if (any_failed) {
        for (int w = 0; w < workers; w++) {
            unlink(summary_paths[w]);
        }
        free(summary_paths);
        exit(1);
    }

    *out_total_used = 0;
    *out_total_bytes = 0.0;
    *out_max_shard_bytes = 0.0;
    for (int w = 0; w < workers; w++) {
        FILE *sf = fopen(summary_paths[w], "rb");
        size_t used_sum = 0;
        double bytes_sum = 0.0, max_bytes = 0.0;
        if (sf == NULL || fread(&used_sum, sizeof used_sum, 1, sf) != 1 ||
            fread(&bytes_sum, sizeof bytes_sum, 1, sf) != 1 || fread(&max_bytes, sizeof max_bytes, 1, sf) != 1) {
            fprintf(stderr, "border_ring_count_dp : resume '%s' illisible — arret\n", summary_paths[w]);
            exit(1);
        }
        fclose(sf);
        unlink(summary_paths[w]);
        *out_total_used += used_sum;
        *out_total_bytes += bytes_sum;
        if (max_bytes > *out_max_shard_bytes) {
            *out_max_shard_bytes = max_bytes;
        }
    }
    free(summary_paths);
}

/* Bascule ponctuelle (une seule fois par ouverture) d'un niveau encore en
   mémoire vers sa représentation en fragments sur disque, une fois le seuil
   bd_disk_mode_min_bytes franchi — simple répartition par hachage de clé (la
   clé et la valeur ne changent pas, contrairement à une transition), donc
   sans passer par bd_transition_range_scatter. Réutilise bd_compact_dir avec
   `nb_sources = 1` : un seul passage séquentiel a rempli les fragments
   bruts. */
static void bd_level_to_shards(const struct bd_level *level, const char *dir, int nb_shards, int nb_workers,
                                struct bd_shard_set *out)
{
    bd_mkdir_or_die(dir);

    FILE **out_files = malloc((size_t)nb_shards * sizeof *out_files);
    for (int d = 0; d < nb_shards; d++) {
        char path[512];
        snprintf(path, sizeof path, "%s/part_0_%d.bin", dir, d);
        out_files[d] = fopen(path, "wb");
        if (out_files[d] == NULL) {
            fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", path);
            exit(1);
        }
    }
    for (size_t idx = 0; idx < level->capacity; idx++) {
        if (!bd_level_is_occupied(level, idx)) {
            continue;
        }
        const uint8_t *key = level->keys + idx * (size_t)level->key_len;
        int d = bd_shard_of(key, level->key_len, nb_shards);
        fwrite(key, (size_t)level->key_len, 1, out_files[d]);
        fwrite(&level->values[idx], sizeof(long long), 1, out_files[d]);
    }
    for (int d = 0; d < nb_shards; d++) {
        fclose(out_files[d]);
    }
    free(out_files);

    snprintf(out->dir, sizeof out->dir, "%s", dir);
    out->nb_shards = nb_shards;
    out->key_len = level->key_len;
    double max_shard_bytes;
    bd_compact_dir(dir, 1, nb_shards, level->key_len, nb_workers, &out->total_used, &out->total_bytes,
                   &max_shard_bytes);
}

/* Transition disque -> disque : voir le commentaire de tête de cette section
   pour les deux vagues (éclatement puis compactage). Supprime le fragment
   set SOURCE une fois le suivant construit — jamais les deux sur disque en
   même temps plus longtemps que nécessaire. */
static void bd_transition_disk(const struct bd_ctx *ctx, int want_corner, const struct bd_shard_set *cur,
                                int nb_workers, const char *next_dir, int nb_shards_next,
                                struct bd_shard_set *next)
{
    bd_mkdir_or_die(next_dir);
    int scatter_workers = nb_workers < cur->nb_shards ? nb_workers : cur->nb_shards;
    if (scatter_workers < 1) {
        scatter_workers = 1;
    }

    fflush(stdout);
    fflush(stderr);

    pid_t *pids = malloc((size_t)scatter_workers * sizeof *pids);
    size_t chunk = ((size_t)cur->nb_shards + (size_t)scatter_workers - 1) / (size_t)scatter_workers;

    for (int w = 0; w < scatter_workers; w++) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_ring_count_dp : fork() a echoue (eclatement, worker %d) — arret\n", w);
            bd_abort_workers(pids, w);
            exit(1);
        }
        if (pid == 0) {
            FILE **out_files = malloc((size_t)nb_shards_next * sizeof *out_files);
            for (int d = 0; d < nb_shards_next; d++) {
                char path[512];
                snprintf(path, sizeof path, "%s/part_%d_%d.bin", next_dir, w, d);
                out_files[d] = fopen(path, "wb");
                if (out_files[d] == NULL) {
                    fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", path);
                    exit(1);
                }
            }
            size_t start = (size_t)w * chunk;
            size_t end = start + chunk;
            if (end > (size_t)cur->nb_shards) {
                end = (size_t)cur->nb_shards;
            }
            for (size_t s = start; s < end; s++) {
                char path[512];
                snprintf(path, sizeof path, "%s/shard_%d.bin", cur->dir, (int)s);
                struct bd_level shard;
                bd_level_load_file(&shard, path);
                bd_transition_range_scatter(ctx, want_corner, &shard, nb_shards_next, out_files);
                bd_level_free(&shard);
            }
            for (int d = 0; d < nb_shards_next; d++) {
                fclose(out_files[d]);
            }
            free(out_files);
            exit(0);
        }
        pids[w] = pid;
    }

    int any_failed = 0;
    for (int w = 0; w < scatter_workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : l'eclatement du worker %d a echoue (status=%d) — arret\n",
                    w, status);
            any_failed = 1;
        }
    }
    free(pids);
    if (any_failed) {
        exit(1);
    }

    next->key_len = cur->key_len;
    snprintf(next->dir, sizeof next->dir, "%s", next_dir);
    next->nb_shards = nb_shards_next;
    double max_shard_bytes;
    bd_compact_dir(next_dir, scatter_workers, nb_shards_next, cur->key_len, nb_workers, &next->total_used,
                   &next->total_bytes, &max_shard_bytes);

    bd_rm_shard_set(cur);
}

/* Variante fragmentée de bd_finalize_range : un worker par plage contiguë de
   fragments, chacun charge et clôt ses fragments un par un (jamais plus d'UN
   en mémoire à la fois), le parent additionne les sommes partielles. */
static long long bd_finalize_shards(const struct bd_ctx *ctx, int want_corner, int8_t closure_target,
                                     const struct bd_shard_set *set, int nb_workers)
{
    int workers = nb_workers < set->nb_shards ? nb_workers : set->nb_shards;
    if (workers < 1) {
        workers = 1;
    }

    fflush(stdout);
    fflush(stderr);

    pid_t *pids = malloc((size_t)workers * sizeof *pids);
    char (*paths)[512] = malloc((size_t)workers * sizeof *paths);
    size_t chunk = ((size_t)set->nb_shards + (size_t)workers - 1) / (size_t)workers;

    for (int w = 0; w < workers; w++) {
        snprintf(paths[w], sizeof paths[w], "%s/finalize_%d.bin", set->dir, w);

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_ring_count_dp : fork() a echoue (finalisation, worker %d) — arret\n", w);
            bd_abort_workers(pids, w);
            exit(1);
        }
        if (pid == 0) {
            size_t start = (size_t)w * chunk;
            size_t end = start + chunk;
            if (end > (size_t)set->nb_shards) {
                end = (size_t)set->nb_shards;
            }
            long long total = 0;
            for (size_t d = start; d < end; d++) {
                char path[512];
                snprintf(path, sizeof path, "%s/shard_%d.bin", set->dir, (int)d);
                struct bd_level level;
                bd_level_load_file(&level, path);
                total += bd_finalize_range(ctx, want_corner, closure_target, &level, 0, level.capacity);
                bd_level_free(&level);
            }
            FILE *fp = fopen(paths[w], "wb");
            if (fp == NULL) {
                fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", paths[w]);
                exit(1);
            }
            fwrite(&total, sizeof total, 1, fp);
            fclose(fp);
            exit(0);
        }
        pids[w] = pid;
    }

    int any_failed = 0;
    for (int w = 0; w < workers; w++) {
        int status;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr,
                    "border_ring_count_dp : la finalisation du worker %d a echoue (status=%d) — arret\n", w,
                    status);
            any_failed = 1;
        }
    }
    free(pids);
    if (any_failed) {
        for (int w = 0; w < workers; w++) {
            unlink(paths[w]);
        }
        free(paths);
        exit(1);
    }

    long long total = 0;
    for (int w = 0; w < workers; w++) {
        FILE *fp = fopen(paths[w], "rb");
        long long partial = 0;
        if (fp == NULL || fread(&partial, sizeof partial, 1, fp) != 1) {
            fprintf(stderr, "border_ring_count_dp : resultat '%s' illisible — arret\n", paths[w]);
            exit(1);
        }
        fclose(fp);
        unlink(paths[w]);
        total += partial;
    }
    free(paths);
    return total;
}

/* ===========================================================================
 * Orchestration : un niveau par position de l'anneau, du premier au dernier,
 * en ne gardant jamais que le niveau courant + le niveau en construction —
 * PAS une table unique mémoïsant les 59 positions à la fois. Deux
 * représentations possibles pour le niveau courant, jamais les deux à la
 * fois : en mémoire (`cur`, tant que bd_disk_mode_min_bytes n'est pas
 * franchi — chemin rapide sans E/S, celui qu'empruntent tous les fixtures de
 * test par défaut) ou en fragments sur disque (`cur_shards`, au-delà — cf.
 * le commentaire de tête de la section "Mode disque"). Le passage de l'un à
 * l'autre ne se fait qu'une fois par ouverture, jamais dans l'autre sens :
 * un niveau qui a dû passer sur disque reste géré par fragments même si son
 * compte diminue ensuite (les dernières positions, près de la fermeture,
 * restent bon marché à fragmenter par rapport au reste du calcul).
 */
static long long bd_run_opening(const struct bd_ctx *ctx, int8_t initial_required,
                                 const int8_t *initial_counts, int8_t closure_target, int nb_workers)
{
    struct bd_level cur;
    bd_level_init(&cur, 1 + ctx->nb_classes, 1 << 10);
    uint8_t key0[1 + BD_MAX_CLASSES];
    key0[0] = (uint8_t)initial_required;
    memcpy(key0 + 1, initial_counts, (size_t)ctx->nb_classes);
    bd_level_add(&cur, key0, 1);

    int disk_mode = 0;
    struct bd_shard_set cur_shards = { .dir = "", .nb_shards = 0, .key_len = 0, .total_used = 0,
                                        .total_bytes = 0.0 };

    for (int pos = 1; pos < BORDER_RING_LEN - 1; pos++) {
        int want_corner = ctx->is_corner_at[pos];

        if (!disk_mode) {
            struct bd_level next;
            if (nb_workers > 1 && cur.used >= bd_fork_min_states) {
                bd_transition_parallel(ctx, want_corner, &cur, nb_workers, &next);
            } else {
                bd_level_init(&next, 1 + ctx->nb_classes, cur.used / 2 + 16);
                bd_transition_range(ctx, want_corner, &cur, 0, cur.capacity, &next);
            }
            bd_level_free(&cur);
            cur = next;

            double bytes = bd_level_bytes(&cur);
            fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go)\n", pos + 1,
                    BORDER_RING_LEN - 1, cur.used, bytes / (1024.0 * 1024.0 * 1024.0));

            if (bytes >= bd_disk_mode_min_bytes) {
                int nb_shards = bd_pick_nb_shards(bytes, nb_workers);
                char dir[128];
                snprintf(dir, sizeof dir, "/tmp/etii_bd_%d_pos%d", (int)getpid(), pos);
                bd_level_to_shards(&cur, dir, nb_shards, nb_workers, &cur_shards);
                bd_level_free(&cur);
                disk_mode = 1;
                fprintf(stderr,
                        "border_ring_count_dp : position %d/%d, bascule en mode disque "
                        "(%d fragments, %.2f Go)\n",
                        pos + 1, BORDER_RING_LEN - 1, nb_shards,
                        cur_shards.total_bytes / (1024.0 * 1024.0 * 1024.0));
            }
        } else {
            int nb_shards_next = bd_pick_nb_shards(cur_shards.total_bytes, nb_workers);
            char dir[128];
            snprintf(dir, sizeof dir, "/tmp/etii_bd_%d_pos%d", (int)getpid(), pos);
            struct bd_shard_set next_shards;
            bd_transition_disk(ctx, want_corner, &cur_shards, nb_workers, dir, nb_shards_next, &next_shards);
            cur_shards = next_shards;

            fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go, %d fragments)\n",
                    pos + 1, BORDER_RING_LEN - 1, cur_shards.total_used,
                    cur_shards.total_bytes / (1024.0 * 1024.0 * 1024.0), cur_shards.nb_shards);
        }
    }

    long long total;
    if (!disk_mode) {
        total = bd_finalize_range(ctx, ctx->is_corner_at[BORDER_RING_LEN - 1], closure_target, &cur, 0,
                                   cur.capacity);
        bd_level_free(&cur);
    } else {
        total = bd_finalize_shards(ctx, ctx->is_corner_at[BORDER_RING_LEN - 1], closure_target, &cur_shards,
                                    nb_workers);
        bd_rm_shard_set(&cur_shards);
    }
    return total;
}

/* Boucle d'ouverture à `(0,0)` : réutilise exactement le même mécanisme que
   `border_walk_count` (what_search_in_grid_to_key + la map de lookup) pour
   énumérer les candidats réels, un par un — jamais regroupés en classe ici.
   Contrairement aux cases suivantes (où `required_color` disambigüe
   toujours sans ambiguïté quelle des deux couleurs de la classe sert
   d'entrée et laquelle sert de sortie), rien ne force qu'une paire de
   pièces-coin partageant la même classe présente sa fermeture (vers la
   dernière case) et sa sortie (vers la case 1) dans le même ordre — c'est
   une propriété de la pièce RÉELLE (son orientation d'origine dans le CSV),
   pas de la classe abstraite. Regrouper ici risquerait donc de mélanger
   deux fermetures différentes sous un seul poids ; comme il n'y a de toute
   façon que quatre pièces-coin réelles à essayer, le gain de regroupement
   serait nul. */
static long long bd_count_openings(map_big_array *map, struct array_part *all_rotate_parts,
                                    struct bd_ctx *ctx, const int16_t *id_to_class,
                                    const int base_counts[BD_MAX_CLASSES], int nb_workers)
{
    struct possibility_packet state;
    memset(&state, 0, sizeof state);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            state.grid[x][y] = -2;
        }
    }
    state.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    key_part key;
    what_search_in_grid_to_key(all_rotate_parts, &state, 0, 0, &key, (int8_t)map->sizearrayM);
    map_bucket bucket = map_bucket_packed(map, &key);

    long long total = 0;
    for (int s = 0; s < bucket.size; s++) {
        const struct part *cand = &bucket.parts[s];
        if (cand->id <= 0 || id_to_class[cand->id] < 0) {
            continue;
        }
        int8_t counts[BD_MAX_CLASSES];
        for (int c = 0; c < ctx->nb_classes; c++) {
            counts[c] = (int8_t)base_counts[c];
        }
        counts[id_to_class[cand->id]]--;

        /* case 1 : k4(LEFT) = grid[0][0].right ; derniere case : k1(TOP) = grid[0][0].bottom */
        total += bd_run_opening(ctx, cand->right, counts, cand->bottom, nb_workers);
    }
    return total;
}

long long border_ring_count_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers)
{
    int n = (all_rotate_parts->size - 1) / 4;

    struct bd_ctx *ctx = malloc(sizeof *ctx);
    int base_counts[BD_MAX_CLASSES];
    int16_t *id_to_class = malloc((size_t)(n + 1) * sizeof *id_to_class);

    ctx->nb_classes = bd_build_classes(all_rotate_parts, ctx, base_counts, id_to_class, n);

    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(order);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int x = order[i][0], y = order[i][1];
        ctx->is_corner_at[i] = (int8_t)((x == 0 || x == ETERN_SIZE - 1) &&
                                         (y == 0 || y == ETERN_SIZE - 1));
    }

    if (nb_workers < 1) {
        nb_workers = 1;
    }
    long long total = bd_count_openings(map, all_rotate_parts, ctx, id_to_class, base_counts, nb_workers);

    free(id_to_class);
    free(ctx);
    return total;
}
