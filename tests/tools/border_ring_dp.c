#include "tools/border_ring_dp.h"

#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

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
    level->capacity = 1u << 10;
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
 * Orchestration : un niveau par position de l'anneau, du premier au dernier,
 * en ne gardant jamais que le niveau courant + le niveau en construction —
 * PAS une table unique mémoïsant les 59 positions à la fois (la version
 * précédente de ce fichier). Parallélise la transition d'un niveau dès que
 * ce niveau dépasse bd_fork_min_states ET que nb_workers > 1 (forker pour
 * un niveau minuscule coûterait plus cher que ça ne rapporte).
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

    for (int pos = 1; pos < BORDER_RING_LEN - 1; pos++) {
        struct bd_level next;
        int want_corner = ctx->is_corner_at[pos];
        if (nb_workers > 1 && cur.used >= bd_fork_min_states) {
            bd_transition_parallel(ctx, want_corner, &cur, nb_workers, &next);
        } else {
            bd_level_init(&next, 1 + ctx->nb_classes, cur.used / 2 + 16);
            bd_transition_range(ctx, want_corner, &cur, 0, cur.capacity, &next);
        }
        bd_level_free(&cur);
        cur = next;

        fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go)\n", pos + 1,
                BORDER_RING_LEN - 1, cur.used, bd_level_bytes(&cur) / (1024.0 * 1024.0 * 1024.0));
    }

    long long total = bd_finalize_range(ctx, ctx->is_corner_at[BORDER_RING_LEN - 1], closure_target,
                                         &cur, 0, cur.capacity);
    bd_level_free(&cur);
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
