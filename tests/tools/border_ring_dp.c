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

/* Répertoire des fichiers temporaires (niveaux forkés ET fragments du mode
   disque) — `/tmp` par défaut, comme le reste du fichier avant l'ajout du
   mode disque. `/tmp` est souvent une petite partition ou un tmpfs plafonné
   bien en-deçà de la RAM de la machine (indépendamment de sa taille) : un
   niveau en mode disque peut y déposer plusieurs dizaines de Go de fragments
   AVANT compactage (bruts, donc plus gros que leur forme finale
   dédupliquée), qui peuvent saturer `/tmp` même sur une machine par ailleurs
   bien dotée — observé en pratique (échec au niveau 21 sur la machine visée,
   2x10 cœurs/48 Go, cf. docs/tests_et_ci.md). `border_ring_dp_set_spill_dir`
   permet de rediriger vers un disque plus grand, comme `--stock-spill-dir`
   pour le stock principal (`core/stock_spill.c`). */
static const char *bd_spill_dir = "/tmp";

void border_ring_dp_set_spill_dir(const char *dir)
{
    bd_spill_dir = dir;
}

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

/* Taille RAM EXACTE qu'occupera un fragment sérialisé (en-tête key_len+count,
   format bd_level_write_file) une fois rechargé par bd_level_load_file — sans
   jamais le charger. bd_level_load_file appelle toujours
   bd_level_init(level, key_len, count*2+16) : la capacité finale est donc
   entièrement déterminée par ces deux nombres, lisibles depuis les 12
   premiers octets du fichier. Non-static uniquement pour être testée
   directement depuis test_border_ring_dp.c — jamais déclarée dans
   border_ring_dp.h, appelée uniquement en interne par le coordinateur. */
double bd_estimate_reload_bytes(int32_t key_len, uint64_t count)
{
    size_t hint = (size_t)(count * 2 + 16);
    size_t capacity = 1u << 4;
    while (capacity < hint) {
        capacity <<= 1;
    }
    return (double)capacity * ((size_t)key_len + sizeof(long long)) + (double)((capacity + 7) / 8);
}

/* Test-only : vérifie que l'estimation ci-dessus égale EXACTEMENT ce qu'un
   vrai bd_level_init(key_len, count*2+16) allouerait — verrouille que la
   formule ne diverge jamais silencieusement de l'allocation réelle. */
int bd_reload_estimate_matches_real_alloc_for_tests(int32_t key_len, uint64_t count)
{
    struct bd_level level;
    bd_level_init(&level, key_len, (size_t)count * 2 + 16);
    double real_bytes = bd_level_bytes(&level);
    bd_level_free(&level);
    return bd_estimate_reload_bytes(key_len, count) == real_bytes;
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

/* Un fwrite() qui echoue silencieusement (retour < nmemb, jamais verifie
   avant cette correction) laisse un fichier TRONQUE sans le signaler — vu
   une fois en pratique sur la machine visee (disque /tmp sature pendant
   l'eclatement d'un niveau a 14 Go, cf. docs/tests_et_ci.md) : la
   corruption ne se decouvrait qu'au COMPACTAGE suivant ("fragment tronque"),
   bien apres le worker fautif qui, lui, se terminait avec un code de succes.
   Toute ecriture de fragment passe desormais par ici, jamais un fwrite() nu,
   pour echouer bruyamment au bon endroit — meme philosophie que
   bd_level_alloc_or_die pour une allocation. */
static void bd_write_or_die(FILE *fp, const void *buf, size_t size, const char *context)
{
    if (fwrite(buf, size, 1, fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : ecriture impossible (%s, disque plein ?) — arret\n", context);
        exit(1);
    }
}

static void bd_close_or_die(FILE *fp, const char *context)
{
    /* fclose() peut echouer ICI meme si tous les fwrite() precedents ont
       « reussi » : les octets restaient dans le tampon stdio, jamais
       physiquement ecrits avant le vidage final — meme piege que ci-dessus,
       a l'autre bout de l'ecriture. */
    if (fclose(fp) != 0) {
        fprintf(stderr, "border_ring_count_dp : cloture de '%s' impossible (disque plein ?) — arret\n",
                context);
        exit(1);
    }
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
    bd_write_or_die(fp, &key_len, sizeof key_len, path);
    bd_write_or_die(fp, &count, sizeof count, path);
    for (size_t idx = 0; idx < level->capacity; idx++) {
        if (bd_level_is_occupied(level, idx)) {
            bd_write_or_die(fp, level->keys + idx * (size_t)level->key_len, (size_t)level->key_len, path);
            bd_write_or_die(fp, &level->values[idx], sizeof(long long), path);
        }
    }
    bd_close_or_die(fp, path);
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
    /* Assez pour bd_spill_dir (configurable, cf. border_ring_dp_set_spill_dir)
       + le suffixe — pas 64, insuffisant dès que bd_spill_dir n'est plus
       "/tmp". */
    char (*paths)[300] = malloc((size_t)nb_workers * sizeof *paths);
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
        snprintf(paths[w], sizeof paths[w], "%s/etii_bd_level_%d_%d", bd_spill_dir, (int)getpid(), w);

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
 * Scission par pile LIFO : au-delà de bd_split_threshold_bytes, un niveau
 * n'est plus tenu tout entier en mémoire — il est éclaté en K fragments sur
 * disque (K = bd_pick_nb_shards), un seul repris IMMÉDIATEMENT (assez petit
 * pour tenir large en mémoire), les K-1 autres empilés (LIFO, cf.
 * bd_pending_stack) pour être repris plus tard, chacun depuis la position où
 * il a été mis de côté.
 *
 * Remplace un mécanisme antérieur ("mode disque" permanent, deux vagues de
 * forks à CHAQUE position tant qu'un niveau restait trop gros) qui
 * réécrivait/relisait la totalité d'un niveau à chaque position tant qu'il
 * dépassait le seuil — sur une plage de plusieurs dizaines de positions
 * consécutives toutes trop grosses, ça multipliait le volume d'E/S par le
 * nombre de positions concernées (mesuré : ~777 Go d'E/S cumulées pour une
 * masse de pointe de 111 Go étalée sur ~7 positions). Ici, chaque fragment
 * mis de côté n'est écrit qu'UNE FOIS (à sa création) et relu qu'UNE FOIS (à
 * sa reprise) — jamais retouché entre les deux, quel que soit le nombre de
 * positions qui s'écoulent pendant qu'il attend sur la pile. L'ordre LIFO
 * (le dernier fragment créé est repris en premier) borne la profondeur de la
 * pile par le nombre de positions de l'anneau (59), pas par la largeur de
 * l'espace d'états — même raisonnement qu'une pile explicite remplaçant une
 * récursion en profondeur d'abord.
 *
 * Un fragment "canonique" (`shard_<d>.bin`) est toujours dédupliqué — même
 * format qu'un niveau en mémoire sérialisé par bd_level_write_file, jamais
 * une simple concaténation. Un fragment "brut" (`part_<dest>.bin`,
 * transitoire, supprimé dès qu'il est fusionné) ne l'est pas : deux entrées
 * du niveau source peuvent y déposer la même clé, à fusionner par
 * accumulation (bd_level_add) — exactement ce que bd_transition_parallel
 * fait déjà pour les niveaux locaux de ses workers, seule la granularité
 * change (par fragment plutôt que par le niveau entier). La scission d'UN
 * niveau (bd_level_to_shards) route séquentiellement chaque entrée vers son
 * fragment brut puis compacte (bd_compact_dir, forké) — jamais deux niveaux
 * scindés simultanément, contrairement à l'ancien mécanisme.
 */
struct bd_shard_set {
    /* Assez pour "/tmp/etii_bd_<pid>_s<n>" avec de la marge ; volontairement
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

/* Plus de constante calée à la main sur une machine précise : le budget vient
   de `--dp-max-ram-mo` (obligatoire dès que `--dp` est utilisé, cf.
   border_mass.c) via `border_ring_dp_set_max_ram_mo`, seule façon de faire
   varier ces deux seuils en production — les setters "_for_tests" ci-dessous
   restent réservés aux fixtures unitaires. bd_split_threshold_bytes déclenche
   la scission dès qu'un niveau dépasse le budget donné. bd_shard_target_bytes
   en dérive : une compaction peut faire tourner jusqu'à nb_workers fragments
   à la fois, donc nb_workers x bd_shard_target_bytes doit rester sous ce
   même budget — divisé par 3 pour laisser de la marge à l'OS et aux tampons
   d'E/S (rapport mesuré sur la machine ayant motivé ce mécanisme : 768 Mo x
   20 travailleurs = 15 Go sur un budget de 48 Go, soit /3,2). */
static double bd_shard_target_bytes = 768.0 * 1024.0 * 1024.0;
static double bd_split_threshold_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0;

/* Budget POOL (somme visee sur tous les slots actifs, cf. bd_run_opening) —
   distinct de bd_split_threshold_bytes : ce dernier est aussi manipule par
   les hooks test-only qui forcent des scissions a taille quasi nulle
   (border_ring_dp_set_disk_mode_min_bytes_for_tests), un usage sans rapport
   avec le budget RAM reel du pool. Les decoupler evite qu'un test qui force
   des scissions minuscules n'ecrase aussi, par effet de bord, le budget
   d'admission du pool. */
static double bd_pool_budget_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0;

/* Marge heuristique du mode SOLO, PAS un calcul exact (contrairement a
   bd_estimate_reload_bytes, cf. plus bas dans ce fichier) : /2 pour la
   co-residence ancien+nouveau niveau pendant toute la duree d'une
   transition, /2 supplementaire pour la non-deduplication entre les
   nb_workers tables locales de bd_transition_parallel — valeur a ajuster
   empiriquement une fois mesuree sur un run reel, meme demarche que le /3
   de bd_shard_target_bytes ci-dessus. Cf. spec § Comptabilite RAM.
   Déclarée ici (et non près de bd_effective_solo_budget_bytes, son seul
   lecteur, plus bas dans ce fichier) pour rester visible depuis
   border_ring_dp_set_max_ram_mo, son unique point d'écriture — même
   contrainte d'ordre que bd_split_threshold_bytes/bd_pool_budget_bytes
   ci-dessus. */
static double bd_solo_budget_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0 / 4.0;

/* Plancher bas pour bd_shard_target_bytes : évite qu'un budget RAM minuscule
   combiné à beaucoup de workers ne produise une cible de fragment
   dégénérée (quelques octets), qui exploserait BD_MAX_SHARDS pour rien. */
#define BD_SHARD_TARGET_BYTES_FLOOR (1.0 * 1024.0 * 1024.0)

/* Test-only, jamais dans border_ring_dp.h — même schéma que
   border_ring_dp_set_fork_min_states_for_tests : abaisser ces seuils permet à
   un fixture minuscule de déclencher réellement une scission (et plusieurs
   reprises en cascade) sans construire un niveau de plusieurs Go. */
void border_ring_dp_set_disk_mode_min_bytes_for_tests(double n)
{
    bd_split_threshold_bytes = n;
}

void border_ring_dp_set_shard_target_bytes_for_tests(double n)
{
    bd_shard_target_bytes = n;
}

void border_ring_dp_set_max_ram_mo(long mo, int nb_workers)
{
    double ram_bytes = (double)mo * 1024.0 * 1024.0;
    bd_split_threshold_bytes = ram_bytes;
    bd_pool_budget_bytes = ram_bytes;

    int workers = nb_workers < 1 ? 1 : nb_workers;
    double target = ram_bytes / ((double)workers * 3.0);
    bd_shard_target_bytes = target < BD_SHARD_TARGET_BYTES_FLOOR ? BD_SHARD_TARGET_BYTES_FLOOR : target;

    bd_solo_budget_bytes = ram_bytes / 4.0;
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

/* Fusionne les fragments bruts déposés sous `dir` (un par fragment
   destination, cf. bd_level_to_shards) en fragments canoniques `shard_<d>.bin`,
   un worker par plage contiguë de fragments destination — jamais plus d'UN
   fragment en mémoire à la fois par worker. */
static void bd_compact_dir(const char *dir, int nb_shards, int key_len, int nb_workers, size_t *out_total_used,
                            double *out_total_bytes, double *out_max_shard_bytes)
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
                char path[512];
                snprintf(path, sizeof path, "%s/part_%d.bin", dir, (int)d);
                bd_raw_merge_file(&level, path, key_len);
                unlink(path);
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
            bd_write_or_die(sf, &used_sum, sizeof used_sum, summary_paths[w]);
            bd_write_or_die(sf, &bytes_sum, sizeof bytes_sum, summary_paths[w]);
            bd_write_or_die(sf, &max_bytes, sizeof max_bytes, summary_paths[w]);
            bd_close_or_die(sf, summary_paths[w]);
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

/* Scinde un niveau encore en mémoire en `nb_shards` fragments sur disque, une
   fois bd_split_threshold_bytes franchi (cf. bd_run_opening) — simple
   répartition par hachage de clé (la clé et la valeur ne changent pas,
   contrairement à une transition), en un seul passage séquentiel sur les
   entrées de `level` avant compactage (bd_compact_dir, forké). */
static void bd_level_to_shards(const struct bd_level *level, const char *dir, int nb_shards, int nb_workers,
                                struct bd_shard_set *out)
{
    bd_mkdir_or_die(dir);

    FILE **out_files = malloc((size_t)nb_shards * sizeof *out_files);
    for (int d = 0; d < nb_shards; d++) {
        char path[512];
        snprintf(path, sizeof path, "%s/part_%d.bin", dir, d);
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
        bd_write_or_die(out_files[d], key, (size_t)level->key_len, dir);
        bd_write_or_die(out_files[d], &level->values[idx], sizeof(long long), dir);
    }
    for (int d = 0; d < nb_shards; d++) {
        bd_close_or_die(out_files[d], dir);
    }
    free(out_files);

    snprintf(out->dir, sizeof out->dir, "%s", dir);
    out->nb_shards = nb_shards;
    out->key_len = level->key_len;
    double max_shard_bytes;
    bd_compact_dir(dir, nb_shards, level->key_len, nb_workers, &out->total_used, &out->total_bytes,
                   &max_shard_bytes);
}

/* Une tranche mise de côté lors d'une scission (cf. bd_run_opening) : un
   fragment canonique unique à recharger, et la position de l'anneau où
   reprendre sa progression — jamais retouché entre les deux. */
struct bd_pending_slice {
    /* 512, pas 300 : construit par snprintf(..., "%s/shard_%d.bin", shards.dir, d)
       où shards.dir peut déjà remplir bd_shard_set.dir (128 octets) — sinon
       -Wformat-truncation (WERROR=1) refuse de compiler, gcc ne pouvant pas
       prouver l'absence de troncature même si aucun chemin réel n'en approche. */
    char shard_path[512];
    char shard_dir[128];
    int resume_pos;
};

/* Pile LIFO des tranches en attente — le dernier fragment créé est repris en
   premier (cf. le commentaire de tête de la section "Scission par pile
   LIFO"), jamais le plus ancien : ça borne la profondeur de la pile par le
   nombre de positions de l'anneau, pas par la largeur de l'espace d'états.
   Grandit par doublement, comme struct bm_collect_ctx dans border_mass.c. */
struct bd_pending_stack {
    struct bd_pending_slice *items;
    int count;
    int cap;
};

static void bd_pending_push(struct bd_pending_stack *stack, const char *path, const char *dir, int resume_pos)
{
    if (stack->count == stack->cap) {
        stack->cap = stack->cap == 0 ? 16 : stack->cap * 2;
        stack->items = realloc(stack->items, (size_t)stack->cap * sizeof *stack->items);
    }
    struct bd_pending_slice *slice = &stack->items[stack->count++];
    snprintf(slice->shard_path, sizeof slice->shard_path, "%s", path);
    snprintf(slice->shard_dir, sizeof slice->shard_dir, "%s", dir);
    slice->resume_pos = resume_pos;
}

/* Mode SOLO (un seul job, autorise a utiliser bd_transition_parallel) tant
   qu'aucun pool n'est deja en cours ET que la pile ne contient pas encore de
   quoi remplir tous les workers ; MODE POOL sinon. Jamais les deux modes
   actifs en meme temps (cf. spec) : un pool deja lance va jusqu'au bout de
   ses jobs actifs avant qu'on reevalue. Non-static, test-only — utilisee par
   le coordinateur (bd_run_opening). */
int bd_should_run_solo(int active, int stack_count, int nb_workers)
{
    return active == 0 && stack_count < nb_workers;
}

/* Resultat d'un job : soit il a ferme l'anneau (closed=1, total valide),
   soit il a du se scinder (closed=0, K fragments deposes dans shard_dir,
   tous a reprendre depuis resume_pos) — un job ne garde JAMAIS un fragment
   pour lui-meme apres une scission, cf. spec § Comportement uniforme d'un
   job. */
struct bd_job_result {
    long long total;
    int closed;
    char shard_dir[128];
    int nb_shards;
    int resume_pos;
};

/* Communication entre un job (potentiellement forke, cf. Tache 5) et le
   coordinateur : struct de taille fixe, ecrite/lue en un seul bloc — pas de
   variable-length, un job ne produit jamais qu'UNE scission avant de sortir
   (il ne garde jamais de fragment pour lui-meme, cf. bd_apply_job_result). */
void bd_job_result_write_or_die(const struct bd_job_result *r, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : ecriture du resultat '%s' impossible — arret\n", path);
        exit(1);
    }
    bd_write_or_die(fp, r, sizeof *r, path);
    bd_close_or_die(fp, path);
}

int bd_job_result_read(struct bd_job_result *r, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    int ok = (fread(r, sizeof *r, 1, fp) == 1);
    fclose(fp);
    return ok ? 0 : -1;
}

/* Coeur d'un job, commun aux modes SOLO et POOL — prend possession de
   `*level` (le libere avant de retourner, dans tous les cas). `allow_parallel`
   n'autorise bd_transition_parallel ET la compaction forkee de
   bd_level_to_shards QUE si vrai (mode SOLO) — en mode POOL (allow_parallel=0)
   un job reste strictement monoprocessus, cf. spec § Pourquoi fork / Non-objectifs
   (pas de fork imbrique). */
static struct bd_job_result bd_run_fragment_job(const struct bd_ctx *ctx, int8_t closure_target,
                                                 struct bd_level *level, int start_pos, int allow_parallel,
                                                 int nb_workers, double effective_budget_bytes)
{
    struct bd_job_result result;
    memset(&result, 0, sizeof result);

    struct bd_level cur = *level;
    int pos = start_pos;

    while (pos < BORDER_RING_LEN - 1) {
        int want_corner = ctx->is_corner_at[pos];
        struct bd_level next;
        if (allow_parallel && nb_workers > 1 && cur.used >= bd_fork_min_states) {
            bd_transition_parallel(ctx, want_corner, &cur, nb_workers, &next);
        } else {
            bd_level_init(&next, 1 + ctx->nb_classes, cur.used / 2 + 16);
            bd_transition_range(ctx, want_corner, &cur, 0, cur.capacity, &next);
        }
        bd_level_free(&cur);
        cur = next;
        pos++;

        double bytes = bd_level_bytes(&cur);
        fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go)\n", pos,
                BORDER_RING_LEN - 1, cur.used, bytes / (1024.0 * 1024.0 * 1024.0));

        /* cur.used > 1 : cf. la garde documentee dans border_ring_dp.h contre
           une scission degeneree d'un niveau a 0 ou 1 entree. */
        if (bytes >= effective_budget_bytes && cur.used > 1) {
            /* `pos` seul ne suffit pas a nommer le repertoire : la meme
               position d'anneau peut etre re-scindee plusieurs fois au cours
               d'un meme run (fragments soeurs repris depuis la pile, chacun
               pouvant a nouveau depasser le seuil exactement a cette
               position) — sans compteur, deux scissions a la meme position
               calculeraient le meme chemin, et bd_mkdir_or_die tolerant
               EEXIST, la seconde ecraserait silencieusement les fragments pas
               encore depiles de la premiere. `static` : ce compteur doit
               survivre aux appels successifs de bd_run_fragment_job depuis la
               boucle de bd_run_opening, un appel par fragment depile — pas
               juste a l'interieur d'un seul appel. */
            static int split_seq = 0;
            int nb_shards = bd_pick_nb_shards(bytes, nb_workers);
            char dir[128];
            snprintf(dir, sizeof dir, "%s/etii_bd_%d_s%d", bd_spill_dir, (int)getpid(), split_seq++);
            struct bd_shard_set shards;
            int compact_workers = allow_parallel ? nb_workers : 1;
            bd_level_to_shards(&cur, dir, nb_shards, compact_workers, &shards);
            bd_level_free(&cur);

            fprintf(stderr,
                    "border_ring_count_dp : position %d/%d, scission en %d fragments "
                    "(%.2f Go, %zu etats au total)\n",
                    pos, BORDER_RING_LEN - 1, shards.nb_shards,
                    shards.total_bytes / (1024.0 * 1024.0 * 1024.0), shards.total_used);

            result.closed = 0;
            snprintf(result.shard_dir, sizeof result.shard_dir, "%s", dir);
            result.nb_shards = nb_shards;
            result.resume_pos = pos;
            return result;
        }
    }

    long long total = bd_finalize_range(ctx, ctx->is_corner_at[BORDER_RING_LEN - 1], closure_target, &cur, 0,
                                         cur.capacity);
    bd_level_free(&cur);
    result.closed = 1;
    result.total = total;
    return result;
}

/* Repousse le resultat d'un job vers la pile partagee : accumule le total
   s'il a ferme l'anneau, ou empile les K fragments produits sinon — jamais
   les deux. */
static void bd_apply_job_result(struct bd_pending_stack *stack, long long *total, const struct bd_job_result *r)
{
    if (r->closed) {
        *total += r->total;
        return;
    }
    for (int d = r->nb_shards - 1; d >= 0; d--) {
        char path[512];
        snprintf(path, sizeof path, "%s/shard_%d.bin", r->shard_dir, d);
        bd_pending_push(stack, path, r->shard_dir, r->resume_pos);
    }
}

struct bd_active_job {
    pid_t pid;
    char result_path[300];
    double estimated_bytes;
};

static double bd_active_bytes_sum(const struct bd_active_job *jobs, int active)
{
    double sum = 0.0;
    for (int i = 0; i < active; i++) {
        sum += jobs[i].estimated_bytes;
    }
    return sum;
}

static int bd_find_slot(const struct bd_active_job *jobs, int active, pid_t pid)
{
    for (int i = 0; i < active; i++) {
        if (jobs[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static void bd_remove_slot(struct bd_active_job *jobs, int *active, int slot)
{
    jobs[slot] = jobs[(*active) - 1];
    (*active)--;
}

/* Lit l'en-tete (12 octets) d'un fragment DEJA sur disque pour estimer sa
   taille de rechargement, sans jamais le charger — cf. bd_estimate_reload_bytes
   (Tache 1). */
static double bd_pending_fragment_estimate_bytes(const char *shard_path)
{
    FILE *fp = fopen(shard_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : lecture de '%s' impossible pour estimation — arret\n",
                shard_path);
        exit(1);
    }
    int32_t key_len = 0;
    uint64_t count = 0;
    if (fread(&key_len, sizeof key_len, 1, fp) != 1 || fread(&count, sizeof count, 1, fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : en-tete de '%s' illisible — arret\n", shard_path);
        exit(1);
    }
    fclose(fp);
    return bd_estimate_reload_bytes(key_len, count);
}

/* Fork un job strictement monoprocessus (allow_parallel=0, cf. spec) pour le
   fragment `slice` — le charge, l'efface du disque, avance jusqu'a fermeture
   ou nouvelle scission, ecrit son resultat dans result_path. Retourne le pid
   de l'enfant. */
static pid_t bd_fork_pool_job(const struct bd_ctx *ctx, int8_t closure_target,
                               const struct bd_pending_slice *slice, int nb_workers,
                               double effective_budget_bytes, char *result_path, size_t result_path_size)
{
    static int job_seq = 0;
    snprintf(result_path, result_path_size, "%s/etii_bd_job_%d_%d.bin", bd_spill_dir, (int)getpid(),
             job_seq++);

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;   /* le caller (bd_run_opening) a jobs[]/active en portee pour nettoyer les freres */
    }
    if (pid == 0) {
        struct bd_level cur;
        bd_level_load_file(&cur, slice->shard_path);
        unlink(slice->shard_path);
        rmdir(slice->shard_dir);
        struct bd_job_result r =
            bd_run_fragment_job(ctx, closure_target, &cur, slice->resume_pos, /*allow_parallel=*/0, nb_workers,
                                 effective_budget_bytes);
        bd_job_result_write_or_die(&r, result_path);
        exit(0);
    }
    return pid;
}

/* Tue et attend tous les jobs actifs sauf, eventuellement, celui a
   l'indice skip_index (deja sorti proprement — jamais tue une deuxieme
   fois) — utilisee sur les deux chemins d'echec du coordinateur pool ou
   des freres pourraient encore tourner (fork() rate en cours d'admission,
   fichier resultat illisible). skip_index = -1 pour n'exclure personne. */
static void bd_abort_active_jobs(const struct bd_active_job *jobs, int active, int skip_index)
{
    for (int i = 0; i < active; i++) {
        if (i != skip_index) {
            kill(jobs[i].pid, SIGTERM);
        }
    }
    for (int i = 0; i < active; i++) {
        if (i != skip_index) {
            waitpid(jobs[i].pid, NULL, 0);
        }
    }
}

/* bd_solo_budget_bytes est déclarée plus haut dans ce fichier, à côté de
   bd_split_threshold_bytes/bd_pool_budget_bytes, pour rester visible depuis
   border_ring_dp_set_max_ram_mo — voir le commentaire à sa déclaration pour
   le détail de la marge heuristique appliquée. */
static double bd_effective_solo_budget_bytes(void)
{
    return bd_solo_budget_bytes;
}

double border_ring_dp_get_solo_budget_bytes_for_tests(void)
{
    return bd_solo_budget_bytes;
}

/* ===========================================================================
 * Orchestration : fait avancer un niveau, position par position, jusqu'à la
 * fermeture de l'anneau — TOUJOURS en mémoire (jamais de représentation
 * fragmentée "vivante" à travers plusieurs positions, contrairement à
 * l'ancien mécanisme). Quand un niveau dépasse bd_split_threshold_bytes, il
 * est scindé en K fragments (bd_level_to_shards) et TOUS empilés sur `stack`
 * (LIFO — cf. son commentaire, et bd_apply_job_result) : un job ne garde
 * jamais de fragment pour lui-même après une scission (cf. bd_run_fragment_job)
 * — le sommet de pile est repris au tour suivant, les autres restent en
 * attente, chacun depuis la position où il a été mis de côté. Une fois une
 * tranche fermée (fermeture de l'anneau atteinte), sa contribution s'ajoute
 * au total et la tranche suivante est dépilée — jusqu'à la pile vide. Aucune
 * tranche n'est jamais retouchée entre sa création et sa reprise : chaque
 * fragment n'est écrit qu'une fois et relu qu'une fois, quel que soit le
 * nombre de positions qui s'écoulent pendant qu'il attend.
 *
 * Deux modes de dispatch, jamais simultanés (cf. bd_should_run_solo) : SOLO
 * (un seul job actif, autorise a se paralleliser en interne via
 * bd_transition_parallel) tant que la pile n'a pas de quoi remplir tous les
 * workers, POOL (jusqu'a nb_workers jobs forkes concurrents, chacun
 * strictement monoprocessus) des que la pile en a assez — bascule reevaluee
 * a chaque tour de la boucle principale.
 */
static long long bd_run_opening(const struct bd_ctx *ctx, int8_t initial_required,
                                 const int8_t *initial_counts, int8_t closure_target, int nb_workers)
{
    int nb_workers_eff = nb_workers < 1 ? 1 : nb_workers;
    double budget_solo = bd_effective_solo_budget_bytes();
    double budget_pool_total = bd_pool_budget_bytes; /* somme visee sur TOUS les slots actifs */

    struct bd_pending_stack stack;
    memset(&stack, 0, sizeof stack);
    long long total = 0;

    /* Job d'amorcage (l'ouverture) : toujours SOLO, rien d'autre a
       repartir a cet instant. */
    struct bd_level seed;
    bd_level_init(&seed, 1 + ctx->nb_classes, 1 << 10);
    uint8_t key0[1 + BD_MAX_CLASSES];
    key0[0] = (uint8_t)initial_required;
    memcpy(key0 + 1, initial_counts, (size_t)ctx->nb_classes);
    bd_level_add(&seed, key0, 1);
    struct bd_job_result r0 =
        bd_run_fragment_job(ctx, closure_target, &seed, 1, /*allow_parallel=*/1, nb_workers_eff, budget_solo);
    bd_apply_job_result(&stack, &total, &r0);

    struct bd_active_job *jobs = malloc((size_t)nb_workers_eff * sizeof *jobs);
    int active = 0;

    while (stack.count > 0 || active > 0) {
        if (bd_should_run_solo(active, stack.count, nb_workers_eff)) {
            struct bd_pending_slice slice = stack.items[--stack.count];
            struct bd_level cur;
            bd_level_load_file(&cur, slice.shard_path);
            unlink(slice.shard_path);
            rmdir(slice.shard_dir);
            struct bd_job_result r = bd_run_fragment_job(ctx, closure_target, &cur, slice.resume_pos,
                                                          /*allow_parallel=*/1, nb_workers_eff, budget_solo);
            bd_apply_job_result(&stack, &total, &r);
            continue;
        }

        /* Mode POOL : ne consulte que le sommet de la pile (pas de recherche
           plus profonde pour un fragment plus petit qui tiendrait mieux —
           simplicite assumee, cf. spec § Risques connus). */
        while (active < nb_workers_eff && stack.count > 0) {
            double est = bd_pending_fragment_estimate_bytes(stack.items[stack.count - 1].shard_path);
            if (bd_active_bytes_sum(jobs, active) + est > budget_pool_total) {
                break;
            }
            struct bd_pending_slice slice = stack.items[--stack.count];
            pid_t pid = bd_fork_pool_job(ctx, closure_target, &slice, nb_workers_eff,
                                          budget_pool_total / (double)nb_workers_eff,
                                          jobs[active].result_path, sizeof jobs[active].result_path);
            if (pid < 0) {
                fprintf(stderr, "border_ring_count_dp : fork() a echoue pour un job du pool — arret\n");
                bd_abort_active_jobs(jobs, active, -1);
                exit(1);
            }
            jobs[active].pid = pid;
            jobs[active].estimated_bytes = est;
            active++;
        }

        if (active == 0) {
            fprintf(stderr,
                    "border_ring_count_dp : --dp-max-ram-mo trop bas pour traiter ne serait-ce qu'un "
                    "fragment en attente (meme reduit a sa part par worker) — arret\n");
            exit(1);
        }

        int status;
        pid_t done = waitpid(-1, &status, 0);
        int slot = bd_find_slot(jobs, active, done);
        if (slot < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : un job du pool a echoue (pid=%d, status=%d) — arret\n",
                    (int)done, status);
            for (int i = 0; i < active; i++) {
                if (jobs[i].pid != done) {
                    kill(jobs[i].pid, SIGTERM);
                    waitpid(jobs[i].pid, NULL, 0);
                }
            }
            exit(1);
        }

        struct bd_job_result r;
        if (bd_job_result_read(&r, jobs[slot].result_path) != 0) {
            fprintf(stderr, "border_ring_count_dp : resultat du job pid=%d illisible ('%s') — arret\n",
                    (int)done, jobs[slot].result_path);
            bd_abort_active_jobs(jobs, active, slot);
            exit(1);
        }
        unlink(jobs[slot].result_path);
        bd_apply_job_result(&stack, &total, &r);
        bd_remove_slot(jobs, &active, slot);
    }

    free(jobs);
    free(stack.items);
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
   deux fermetures différentes sous un seul poids.
 *
 * N'exécute qu'UNE SEULE fois `bd_run_opening` (sur le premier candidat
 * trouvé), puis multiplie par `nb_candidates` au lieu de sommer un
 * `bd_run_opening` complet par candidat : par symétrie de rotation à 90° du
 * plateau (cf. docs/superpowers/specs/2026-09-06-masse-bordure-design.md),
 * toute bordure valide utilise nécessairement les `nb_candidates` pièces-coin
 * réelles disponibles, une fois chacune — il y a exactement autant de
 * positions-coin sur l'anneau que de pièces-coin réelles, aucune
 * substitution possible. Faire tourner le plateau entier de 90° envoie donc
 * toute solution comptée pour un candidat donné vers une solution tout aussi
 * valide comptée pour le candidat suivant (même pièce, coin suivant,
 * réorientée) — les `nb_candidates` totaux par candidat sont donc
 * rigoureusement égaux, et pas seulement dans le cas où l'ouverture ne
 * change rien à la forme du reste de l'anneau. Vérifié empiriquement avant
 * ce changement (instrumentation temporaire, jamais committée) : les 4
 * candidats de `data/pieces16.csv` donnent chacun N=1 ; `data/pieces.csv` n'a
 * que 4 pièces à 2 faces à 0 (ids 1-4), sans couleurs partagées entre elles,
 * donc la prémisse (chaque anneau valide utilise les 4, une fois chacune)
 * tient aussi sur le jeu réel. Ce court-circuit économise ~(nb_candidates-1)
 * fois le coût du DP complet — le poste dominant du temps de calcul. */
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

    const struct part *first_cand = NULL;
    int nb_candidates = 0;
    for (int s = 0; s < bucket.size; s++) {
        const struct part *cand = &bucket.parts[s];
        if (cand->id <= 0 || id_to_class[cand->id] < 0) {
            continue;
        }
        if (first_cand == NULL) {
            first_cand = cand;
        }
        nb_candidates++;
    }
    if (first_cand == NULL) {
        return 0;
    }

    int8_t counts[BD_MAX_CLASSES];
    for (int c = 0; c < ctx->nb_classes; c++) {
        counts[c] = (int8_t)base_counts[c];
    }
    counts[id_to_class[first_cand->id]]--;

    /* case 1 : k4(LEFT) = grid[0][0].right ; derniere case : k1(TOP) = grid[0][0].bottom */
    long long n_single_opening = bd_run_opening(ctx, first_cand->right, counts, first_cand->bottom, nb_workers);
    return n_single_opening * (long long)nb_candidates;
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
