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
#include <dirent.h>

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
    bd_ring_count_t *values; /* capacity emplacements */
    uint8_t *occupied;  /* bitmap, ceil(capacity/8) octets */
    size_t capacity;    /* toujours une puissance de 2 */
    size_t used;
    int key_len; /* 1 + nb_classes, fixe pour tout l'appel */
};

static double bd_level_bytes(const struct bd_level *level)
{
    return (double)level->capacity * ((size_t)level->key_len + sizeof(bd_ring_count_t)) +
           (double)((level->capacity + 7) / 8);
}

/* bd_ring_count_format : cf. border_ring_dp.h. Seul moyen d'afficher un
   bd_ring_count_t (__int128 n'a aucune conversion printf native). */
void bd_ring_count_format(bd_ring_count_t value, char *buf, size_t buflen)
{
    char tmp[BD_RING_COUNT_STRLEN];
    size_t i = 0;
    if (value == 0) {
        tmp[i++] = '0';
    } else {
        while (value > 0 && i < sizeof tmp) {
            tmp[i++] = (char)('0' + (int)(value % 10));
            value /= 10;
        }
    }
    size_t n = 0;
    while (i > 0 && n + 1 < buflen) {
        buf[n++] = tmp[--i];
    }
    if (buflen > 0) {
        buf[n] = '\0';
    }
}

/* Addition/multiplication protégées contre un dépassement de bd_ring_count_t
   (128 bits) : plutôt qu'un wrap silencieux (le piège corrigé ici — un
   `long long` débordait déjà sur le jeu réel, cf. le commentaire de tête de
   bd_ring_count_t dans border_ring_dp.h), échec bruyant immédiat, même
   philosophie que bd_level_alloc_or_die/bd_write_or_die. */
static bd_ring_count_t bd_count_add_or_die(bd_ring_count_t a, bd_ring_count_t b, const char *context)
{
    bd_ring_count_t r = a + b;
    if (r < a) {
        char sa[BD_RING_COUNT_STRLEN], sb[BD_RING_COUNT_STRLEN];
        bd_ring_count_format(a, sa, sizeof sa);
        bd_ring_count_format(b, sb, sizeof sb);
        fprintf(stderr,
                "border_ring_count_dp : depassement de bd_ring_count_t (128 bits) dans %s (%s + %s) — "
                "arret\n",
                context, sa, sb);
        exit(1);
    }
    return r;
}

static bd_ring_count_t bd_count_mul_or_die(bd_ring_count_t a, bd_ring_count_t b, const char *context)
{
    if (a != 0 && b > (bd_ring_count_t)-1 / a) {
        char sa[BD_RING_COUNT_STRLEN], sb[BD_RING_COUNT_STRLEN];
        bd_ring_count_format(a, sa, sizeof sa);
        bd_ring_count_format(b, sb, sizeof sb);
        fprintf(stderr,
                "border_ring_count_dp : depassement de bd_ring_count_t (128 bits) dans %s (%s * %s) — "
                "arret\n",
                context, sa, sb);
        exit(1);
    }
    return a * b;
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
    return (double)capacity * ((size_t)key_len + sizeof(bd_ring_count_t)) + (double)((capacity + 7) / 8);
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
static void bd_level_add(struct bd_level *level, const uint8_t *key, bd_ring_count_t delta)
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
            level->values[idx] = bd_count_add_or_die(level->values[idx], delta, "bd_level_add");
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
        bd_ring_count_t ways = level_cur->values[idx];

        for (int c = 0; c < nb; c++) {
            if (ctx->classes[c].is_corner != want_corner || counts[c] == 0 ||
                ctx->classes[c].color_a != required) {
                continue;
            }
            next_key[0] = (uint8_t)ctx->classes[c].color_b;
            memcpy(next_key + 1, counts, (size_t)nb);
            next_key[1 + c]--;
            bd_level_add(level_next, next_key,
                         bd_count_mul_or_die(ways, (bd_ring_count_t)counts[c], "bd_transition_range"));
        }
    }
}

/* Variante terminale (dernière case de l'anneau) : au lieu de produire un
   niveau suivant, cumule directement dans le total dès que la couleur
   produite ferme l'anneau sur la pièce d'ouverture. */
static bd_ring_count_t bd_finalize_range(const struct bd_ctx *ctx, int want_corner, int8_t closure_target,
                                          const struct bd_level *level_cur, size_t start, size_t end)
{
    int nb = ctx->nb_classes;
    bd_ring_count_t total = 0;

    for (size_t idx = start; idx < end; idx++) {
        if (!bd_level_is_occupied(level_cur, idx)) {
            continue;
        }
        const uint8_t *key = level_cur->keys + idx * (size_t)level_cur->key_len;
        int8_t required = (int8_t)key[0];
        const int8_t *counts = (const int8_t *)(key + 1);
        bd_ring_count_t ways = level_cur->values[idx];

        for (int c = 0; c < nb; c++) {
            if (ctx->classes[c].is_corner != want_corner || counts[c] == 0 ||
                ctx->classes[c].color_a != required || ctx->classes[c].color_b != closure_target) {
                continue;
            }
            total = bd_count_add_or_die(
                total, bd_count_mul_or_die(ways, (bd_ring_count_t)counts[c], "bd_finalize_range"),
                "bd_finalize_range");
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

/* PLANCHER du nombre d'emplacements de `cur` traités par appel à
 * `bd_transition_range` avant de recontrôler la taille de `next` (chemin
 * séquentiel de `bd_run_fragment_job` uniquement — cf. la section "Pause
 * mi-transition" plus bas) : le contrôle de budget existant ne s'exécutait
 * qu'UNE FOIS PAR POSITION, après que `next` ait été bâti EN ENTIER — un
 * niveau dont le facteur de branchement est grand pouvait donc dépasser le
 * budget de plusieurs fois avant que le moindre contrôle n'ait lieu, le pic
 * mémoire réel ayant déjà eu lieu (bug d'origine, OOM constaté en production
 * avec `--dp-max-ram-mo 30000 --forks 10` : jobs à 3-8,5 Gio de RSS réel
 * contre un budget nominal par job POOL de 1,5 Gio, cf. l'incident du
 * 2026-09-10). Un simple PLANCHER, PAS la taille de tronçon réellement
 * utilisée (`chunk_slots_effective`, cf. bd_run_fragment_job) : sur un
 * niveau réel, cette valeur seule aurait causé un second incident le
 * lendemain (2026-09-11, `--dp-max-ram-mo 35000 --forks 20` : plus de 4500
 * pauses en 8h, chacune réécrivant puis relisant l'intégralité d'un `next`
 * déjà à 1,4-2,8 Go — des dizaines de To d'E/S cumulées, cf.
 * BD_TRANSITION_CHUNK_DIVISOR). 65536 reste un plancher pertinent pour les
 * petits fixtures de test (`cur.capacity` minuscule, où la fraction
 * `capacity/BD_TRANSITION_CHUNK_DIVISOR` tomberait sous 1). Ajustable pour
 * les tests, même schéma que `bd_fork_min_states`/`bd_shard_target_bytes`. */
static size_t bd_transition_chunk_slots = 1 << 16;

void border_ring_dp_set_transition_chunk_slots_for_tests(size_t n)
{
    bd_transition_chunk_slots = n == 0 ? 1 : n;
}

/* Borne le nombre de pauses mi-transition POSSIBLES pour une seule
 * transition à `BD_TRANSITION_CHUNK_DIVISOR` (jamais plus), quelle que soit
 * l'échelle réelle du niveau traité — `chunk_slots_effective =
 * max(bd_transition_chunk_slots, cur.capacity / BD_TRANSITION_CHUNK_DIVISOR)`
 * dans `bd_run_fragment_job`. Introduit après l'incident du 2026-09-11 (cf.
 * le commentaire de `bd_transition_chunk_slots` ci-dessus) : un plancher
 * FIXE de tronçon, indépendant de la taille du niveau, faisait dépendre le
 * nombre de pauses de la capacité du niveau plutôt que de le borner — sur un
 * niveau à 16 777 216 emplacements avec un plancher à 65536, ça fait 256
 * tronçons possibles, chacun pouvant redéclencher une pause dès que `next`
 * dépasse le budget (ce qui reste vrai indéfiniment une fois franchi, `next`
 * ne fait que grossir) — d'où les >4500 pauses mesurées. 16 : compromis
 * mesuré nulle part encore, mais qui borne le pire cas à 16 réécritures
 * complètes de `next` par transition au lieu de centaines — à ajuster si la
 * mesure sur un run réel montre qu'il faut encore le baisser (moins de
 * pauses, plus de dépassement de budget par tronçon) ou le monter
 * (l'inverse). */
#define BD_TRANSITION_CHUNK_DIVISOR ((size_t)16)

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
            bd_write_or_die(fp, &level->values[idx], sizeof(bd_ring_count_t), path);
        }
    }
    bd_close_or_die(fp, path);
}

/* Comme bd_level_write_file, mais limitée aux emplacements [start, end) de
 * `level` — sert UNIQUEMENT à sérialiser le reliquat de `cur` pas encore
 * transité au moment d'une pause mi-transition (cf. bd_run_fragment_job) :
 * `level` reste vivant et pleinement utilisable après l'appel (contrairement
 * à bd_level_to_shards, qui consomme son entrée), donc pas de nettoyage ici.
 * Deux passes (compte puis écriture) plutôt qu'un tampon intermédiaire :
 * cohérent avec bd_compact_range qui fait de même sur un fragment complet. */
static void bd_level_write_slice_file(const struct bd_level *level, size_t start, size_t end, const char *path)
{
    uint64_t count = 0;
    for (size_t idx = start; idx < end; idx++) {
        if (bd_level_is_occupied(level, idx)) {
            count++;
        }
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", path);
        exit(1);
    }
    int32_t key_len = level->key_len;
    bd_write_or_die(fp, &key_len, sizeof key_len, path);
    bd_write_or_die(fp, &count, sizeof count, path);
    for (size_t idx = start; idx < end; idx++) {
        if (bd_level_is_occupied(level, idx)) {
            bd_write_or_die(fp, level->keys + idx * (size_t)level->key_len, (size_t)level->key_len, path);
            bd_write_or_die(fp, &level->values[idx], sizeof(bd_ring_count_t), path);
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
        bd_ring_count_t value;
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
 * Scission par pile LIFO : au-delà du budget effectif du job en cours
 * (`bd_solo_budget_bytes` en mode SOLO, `bd_pool_job_budget_bytes` en mode
 * POOL — chaque job lit le sien, cf. bd_run_fragment_job), un niveau n'est
 * plus tenu tout entier en mémoire — il est éclaté en K fragments sur disque
 * (K = bd_pick_nb_shards), TOUS empilés (LIFO, cf. bd_pending_stack) sur la
 * pile partagée `bd_pending_stack` — aucun n'est gardé résident par le job
 * qui vient de scinder, il repousse sa production entière et sort. Un
 * coordinateur central (`bd_run_opening`) redistribue ensuite les fragments
 * en attente entre les modes SOLO et POOL selon la profondeur de la pile
 * (`bd_should_run_solo`), chacun repris depuis la position où il a été mis
 * de côté.
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
 * fragment brut puis compacte (bd_compact_dir, forké seulement si
 * `nb_workers > 1` — un seul worker compacte directement, sans fork, cf. son
 * commentaire) — jamais deux niveaux scindés simultanément, contrairement à
 * l'ancien mécanisme.
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
   varier ces seuils en production — les setters "_for_tests" ci-dessous
   restent réservés aux fixtures unitaires. bd_solo_budget_bytes/
   bd_pool_job_budget_bytes (déclarés plus bas) déclenchent chacun la
   scission d'un job dans leur mode respectif dès que le niveau qu'il porte
   dépasse ce budget. bd_shard_target_bytes en dérive : une compaction peut
   faire tourner jusqu'à nb_workers fragments à la fois, donc nb_workers x
   bd_shard_target_bytes doit rester sous ce même budget — divisé par 3 pour
   laisser de la marge à l'OS et aux tampons d'E/S (rapport mesuré sur la
   machine ayant motivé ce mécanisme : 768 Mo x 20 travailleurs = 15 Go sur
   un budget de 48 Go, soit /3,2). */
static double bd_shard_target_bytes = 768.0 * 1024.0 * 1024.0;

/* Budget POOL (somme visee sur tous les slots actifs, cf. bd_run_opening) —
   distinct des seuils de scission par job (bd_solo_budget_bytes/
   bd_pool_job_budget_bytes) : ces derniers sont aussi manipules par les
   hooks test-only qui forcent des scissions a taille quasi nulle
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
   contrainte d'ordre que bd_pool_budget_bytes ci-dessus. */
static double bd_solo_budget_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0 / 4.0;

/* Seuil de scission PAR JOB en mode POOL (jamais l'admission — cf.
   bd_pool_budget_bytes ci-dessus, qui reste separee) — marge heuristique
   /2 pour la co-residence ancien+nouveau niveau pendant une transition
   (un job POOL est toujours sequentiel, jamais bd_transition_parallel,
   donc pas de marge supplementaire pour la non-deduplication entre
   workers comme bd_solo_budget_bytes en a besoin — /2 suffit ici, pas /4). */
static double bd_pool_job_budget_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0 / 2.0;

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
    bd_solo_budget_bytes = n;
    bd_pool_job_budget_bytes = n;
}

void border_ring_dp_set_shard_target_bytes_for_tests(double n)
{
    bd_shard_target_bytes = n;
}

void border_ring_dp_set_max_ram_mo(long mo, int nb_workers)
{
    double ram_bytes = (double)mo * 1024.0 * 1024.0;
    bd_pool_budget_bytes = ram_bytes;

    int workers = nb_workers < 1 ? 1 : nb_workers;
    double target = ram_bytes / ((double)workers * 3.0);
    bd_shard_target_bytes = target < BD_SHARD_TARGET_BYTES_FLOOR ? BD_SHARD_TARGET_BYTES_FLOOR : target;

    bd_solo_budget_bytes = ram_bytes / 4.0;
    bd_pool_job_budget_bytes = (ram_bytes / (double)workers) / 2.0;
}

/* Borne haute généreuse : au-delà, on dégrade gracieusement (fragments plus
   gros que la cible) plutôt que de laisser exploser le nombre de fichiers
   transitoires ouverts simultanément par un worker. */
#define BD_MAX_SHARDS 4096

/* Dimensionne SEULEMENT sur la taille à écouler (`total_bytes` /
   `bd_shard_target_bytes`) — plus de plancher `>= nb_workers` ici : le forcer
   fragmentait un niveau à peine au-dessus du seuil de scission en autant de
   petits fragments qu'il y a de workers configurés, même quand 2 auraient
   suffi à repasser sous le budget. `nb_workers` reste pertinent ailleurs
   (`bd_compact_dir` : degré de compaction parallèle, `bd_run_opening` :
   largeur du pool) mais n'a plus sa place dans le calcul de granularité —
   voir docs/tests_et_ci.md § Scission par pile LIFO. */
static int bd_pick_nb_shards(double total_bytes)
{
    size_t want = (size_t)(total_bytes / bd_shard_target_bytes) + 1;
    size_t n = 1;
    while (n < want) {
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
        bd_ring_count_t value;
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
        bd_ring_count_t value;
        if (fread(&value, sizeof value, 1, fp) != 1) {
            fprintf(stderr, "border_ring_count_dp : fragment brut '%s' tronque — arret\n", path);
            exit(1);
        }
        bd_level_add(level, key, value);
    }
    fclose(fp);
}

/* Compacte la plage [start, end) de fragments bruts destination sous `dir`
   en fragments canoniques `shard_<d>.bin`, et ecrit le resume (compte total,
   octets totaux, plus gros fragment) dans `summary_path` — corps commun aux
   deux chemins de bd_compact_dir (forke quand workers > 1, appele
   directement sinon, cf. son commentaire). */
static void bd_compact_range(const char *dir, size_t start, size_t end, int key_len, const char *summary_path)
{
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
    FILE *sf = fopen(summary_path, "wb");
    if (sf == NULL) {
        fprintf(stderr, "border_ring_count_dp : ecriture de '%s' impossible — arret\n", summary_path);
        exit(1);
    }
    bd_write_or_die(sf, &used_sum, sizeof used_sum, summary_path);
    bd_write_or_die(sf, &bytes_sum, sizeof bytes_sum, summary_path);
    bd_write_or_die(sf, &max_bytes, sizeof max_bytes, summary_path);
    bd_close_or_die(sf, summary_path);
}

/* Lit un fichier de resume ecrit par bd_compact_range et l'accumule dans
   les totaux `out_*` — factorise entre le chemin monoprocessus et la boucle
   de fusion des resumes forkes. */
static void bd_compact_summary_accumulate(const char *summary_path, size_t *out_total_used,
                                           double *out_total_bytes, double *out_max_shard_bytes)
{
    FILE *sf = fopen(summary_path, "rb");
    size_t used_sum = 0;
    double bytes_sum = 0.0, max_bytes = 0.0;
    if (sf == NULL || fread(&used_sum, sizeof used_sum, 1, sf) != 1 ||
        fread(&bytes_sum, sizeof bytes_sum, 1, sf) != 1 || fread(&max_bytes, sizeof max_bytes, 1, sf) != 1) {
        fprintf(stderr, "border_ring_count_dp : resume '%s' illisible — arret\n", summary_path);
        exit(1);
    }
    fclose(sf);
    unlink(summary_path);
    *out_total_used += used_sum;
    *out_total_bytes += bytes_sum;
    if (max_bytes > *out_max_shard_bytes) {
        *out_max_shard_bytes = max_bytes;
    }
}

/* Fusionne les fragments bruts déposés sous `dir` (un par fragment
   destination, cf. bd_level_to_shards) en fragments canoniques `shard_<d>.bin`,
   un worker par plage contiguë de fragments destination — jamais plus d'UN
   fragment en mémoire à la fois par worker. `workers == 1` ne forke PAS :
   un job du mode POOL est strictement monoprocessus (cf. spec § Pourquoi
   fork), et cette fonction est aussi appelee par un job POOL qui doit se
   scinder a nouveau (bd_run_fragment_job, allow_parallel=0 => compact_workers
   force a 1) — la forcer a forker malgre tout y creerait un petit-fils
   nested, contraire a l'invariant "jamais de fork imbrique en mode POOL"
   meme si, en pratique, un seul enfant a la fois. */
static void bd_compact_dir(const char *dir, int nb_shards, int key_len, int nb_workers, size_t *out_total_used,
                            double *out_total_bytes, double *out_max_shard_bytes)
{
    int workers = nb_workers < nb_shards ? nb_workers : nb_shards;
    if (workers < 1) {
        workers = 1;
    }

    *out_total_used = 0;
    *out_total_bytes = 0.0;
    *out_max_shard_bytes = 0.0;

    if (workers == 1) {
        char summary_path[512];
        snprintf(summary_path, sizeof summary_path, "%s/summary_0.bin", dir);
        bd_compact_range(dir, 0, (size_t)nb_shards, key_len, summary_path);
        bd_compact_summary_accumulate(summary_path, out_total_used, out_total_bytes, out_max_shard_bytes);
        return;
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
            bd_compact_range(dir, start, end, key_len, summary_paths[w]);
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

    for (int w = 0; w < workers; w++) {
        bd_compact_summary_accumulate(summary_paths[w], out_total_used, out_total_bytes, out_max_shard_bytes);
    }
    free(summary_paths);
}

/* Scinde un niveau encore en mémoire en `nb_shards` fragments sur disque, une
   fois le budget effectif du job (`bd_solo_budget_bytes` ou
   `bd_pool_job_budget_bytes` selon le mode, cf. bd_run_fragment_job) franchi
   — simple répartition par hachage de clé (la clé et la valeur ne changent
   pas, contrairement à une transition), en un seul passage séquentiel sur
   les entrées de `level` avant compactage (bd_compact_dir, forké seulement
   si `nb_workers > 1`). */
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
        bd_write_or_die(out_files[d], &level->values[idx], sizeof(bd_ring_count_t), dir);
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
    /* Reprise MI-TRANSITION uniquement (cf. bd_run_fragment_job, section
       "Pause mi-transition") : chaîne vide (défaut) pour une reprise
       normale au tout début de `resume_pos` (comportement historique —
       `shard_path` porte alors le niveau COMPLET à `resume_pos`). Non vide
       => `shard_path` ne porte QUE le reliquat de `cur` pas encore transité
       vers `resume_pos + 1`, et ce champ le niveau `resume_pos + 1`
       accumulé jusque-là — les deux à recharger et à CONTINUER de nourrir,
       jamais à écraser. */
    char next_partial_path[512];
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

static void bd_pending_push(struct bd_pending_stack *stack, const char *path, const char *dir, int resume_pos,
                             const char *next_partial_path)
{
    if (stack->count == stack->cap) {
        stack->cap = stack->cap == 0 ? 16 : stack->cap * 2;
        stack->items = realloc(stack->items, (size_t)stack->cap * sizeof *stack->items);
    }
    struct bd_pending_slice *slice = &stack->items[stack->count++];
    snprintf(slice->shard_path, sizeof slice->shard_path, "%s", path);
    snprintf(slice->shard_dir, sizeof slice->shard_dir, "%s", dir);
    slice->resume_pos = resume_pos;
    snprintf(slice->next_partial_path, sizeof slice->next_partial_path, "%s", next_partial_path);
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
   soit il a du se scinder — deux formes possibles, jamais les deux a la
   fois, distinguees par LEQUEL des deux groupes de champs ci-dessous est
   rempli :
     - scission normale (fin de position, K fragments COMPLETS deposes dans
       shard_dir, tous a reprendre depuis resume_pos) — comportement
       historique ;
     - pause MI-TRANSITION (leftover_path non vide, cf. bd_run_fragment_job
       section "Pause mi-transition") : un seul successeur, portant a la
       fois le reliquat de `cur` pas encore transite (leftover_path) et le
       niveau resume_pos+1 accumule jusque-la (next_partial_path) — jamais
       de fan-out ici, contrairement a une scission normale.
   Dans les deux cas, un job ne garde JAMAIS rien pour lui-meme, cf. spec §
   Comportement uniforme d'un job. */
struct bd_job_result {
    bd_ring_count_t total;
    int closed;
    char shard_dir[128];
    int nb_shards;
    int resume_pos;
    char leftover_path[512];
    char next_partial_path[512];
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

/* ===========================================================================
 * Persistance des niveaux pour la passe arrière (reconstruction) — cf.
 * `border_ring_reconstruct_dp` plus bas. La passe avant normale (comptage
 * seul) n'utilise jamais ceci (`persist_dir == NULL`) : chaque niveau reste
 * jeté dès que le suivant est construit, comme avant. Quand `persist_dir` est
 * fourni, CHAQUE niveau produit (y compris l'état reçu en entrée, avant toute
 * transition) est sérialisé sous `persist_dir/level_<pos>/` — un fichier par
 * job/fragment qui contribue à cette position, jamais fusionnés sur le
 * moment (un job du mode POOL ne voit qu'une fraction du niveau réel à une
 * position donnée) : la fusion se fait à la LECTURE, cf.
 * `bd_persist_load_merged`. */
static void bd_mkdir_p_or_die(const char *dir)
{
    bd_mkdir_or_die(dir);
}

static void bd_persist_snapshot(const char *persist_dir, int pos, const struct bd_level *level)
{
    if (persist_dir == NULL) {
        return;
    }
    static long bd_persist_seq = 0;
    char dir[512];
    snprintf(dir, sizeof dir, "%s/level_%d", persist_dir, pos);
    bd_mkdir_p_or_die(dir);
    char path[900];
    snprintf(path, sizeof path, "%s/frag_%d_%ld.bin", dir, (int)getpid(), bd_persist_seq++);
    bd_level_write_file(level, path);
}

/* Fusionne TOUS les fragments persistés à `pos` (un ou plusieurs jobs/fragments
   ont pu y contribuer) en un seul niveau en mémoire — utilisé par la
   reconstruction guidée, jamais par la passe avant normale. Répertoire absent
   (position jamais atteinte, ex. jeu trop petit) : niveau vide, pas une
   erreur — le DFS guidé n'y trouvera simplement aucune complétion possible. */
static void bd_persist_load_merged(const char *persist_dir, int pos, int key_len, struct bd_level *out)
{
    bd_level_init(out, key_len, 1 << 4);
    char dir[512];
    snprintf(dir, sizeof dir, "%s/level_%d", persist_dir, pos);
    DIR *dp = opendir(dir);
    if (dp == NULL) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[900];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        bd_level_merge_file(out, path);
    }
    closedir(dp);
}

/* Recherche pure (sans insertion), sur le même schéma d'adressage ouvert que
   bd_level_add — jamais utilisée par la passe avant (comptage), seulement par
   la reconstruction guidée pour tester si un état a une complétion connue. */
static int bd_level_lookup(const struct bd_level *level, const uint8_t *key, bd_ring_count_t *out_value)
{
    if (level->capacity == 0) {
        return 0;
    }
    uint64_t h = bd_fnv1a(key, level->key_len);
    size_t mask = level->capacity - 1;
    size_t idx = (size_t)h & mask;
    for (size_t probed = 0; probed < level->capacity; probed++) {
        if (!bd_level_is_occupied(level, idx)) {
            return 0;
        }
        if (memcmp(level->keys + idx * (size_t)level->key_len, key, (size_t)level->key_len) == 0) {
            *out_value = level->values[idx];
            return 1;
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

/* Supprime récursivement l'arborescence `persist_dir` créée par
   `bd_persist_snapshot` (sous-répertoires `level_*`) ET les fichiers
   `completion_*.bin` écrits directement sous `persist_dir` par
   `bd_build_and_persist_completions` — pour UNE reconstruction (un coin
   d'ouverture réel), jamais laissée traîner après un run réussi. Un
   `opendir()` qui échoue (entrée = fichier plat, pas un répertoire — le cas
   des `completion_*.bin`) n'est pas une erreur ici : `unlink()` directement
   dessus. Best-effort sur les erreurs de suppression individuelles (un
   fichier déjà absent n'est pas fatal ici, contrairement à l'écriture). */
static void bd_persist_cleanup(const char *persist_dir)
{
    DIR *base = opendir(persist_dir);
    if (base == NULL) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(base)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char path[900];
        snprintf(path, sizeof path, "%s/%s", persist_dir, ent->d_name);
        DIR *sub = opendir(path);
        if (sub == NULL) {
            unlink(path);
            continue;
        }
        struct dirent *fent;
        while ((fent = readdir(sub)) != NULL) {
            if (fent->d_name[0] == '.') {
                continue;
            }
            char fpath[1200];
            snprintf(fpath, sizeof fpath, "%s/%s", path, fent->d_name);
            unlink(fpath);
        }
        closedir(sub);
        rmdir(path);
    }
    closedir(base);
    rmdir(persist_dir);
}

/* Compteur test-only, meme schema/meme mise en garde que
   bd_pool_jobs_forked_for_tests_counter (declare plus bas, incremente par le
   PARENT apres un fork() reussi) : invisible d'un process PARENT si la pause
   survient dans un job POOL forke — un test qui veut l'observer doit rester
   en mode SOLO/job d'ouverture, jamais forcer le mode POOL. Seul moyen pour
   un test de prouver qu'une pause mi-transition a REELLEMENT eu lieu (cf.
   bd_run_fragment_job), pas seulement que le total final coïncide par une
   autre voie (scission normale de fin de position, ou pas de scission du
   tout sur un budget assez large). Déclaré ici (avant bd_run_fragment_job,
   pas à côté de son homologue POOL) : c'est bd_run_fragment_job lui-même,
   pas un caller, qui l'incrémente. */
static long bd_mid_transition_pauses_for_tests_counter = 0;

void border_ring_dp_reset_mid_transition_pauses_for_tests(void)
{
    bd_mid_transition_pauses_for_tests_counter = 0;
}

long border_ring_dp_get_mid_transition_pauses_for_tests(void)
{
    return bd_mid_transition_pauses_for_tests_counter;
}

/* Coeur d'un job, commun aux modes SOLO et POOL — prend possession de
   `*level` (le libere avant de retourner, dans tous les cas). `allow_parallel`
   n'autorise bd_transition_parallel ET la compaction forkee de
   bd_level_to_shards QUE si vrai (mode SOLO) — en mode POOL (allow_parallel=0)
   un job reste strictement monoprocessus, cf. spec § Pourquoi fork / Non-objectifs
   (pas de fork imbrique). `persist_dir` (NULL sauf pendant la reconstruction,
   cf. `border_ring_reconstruct_dp`) fait persister chaque niveau produit
   (y compris l'état reçu en entrée) au lieu de le jeter — cf. le commentaire
   de tête de `bd_persist_snapshot` ci-dessus.

   `resume_next_partial_path` (NULL sauf reprise d'une pause mi-transition,
   cf. section "Pause mi-transition" ci-dessous) : non-NULL => `*level` ne
   porte QUE le reliquat de `cur` pas encore transité vers `start_pos + 1` —
   ce niveau `start_pos + 1` complet a déjà été partiellement construit et
   snapshotté sous ce chemin, à recharger et à CONTINUER de nourrir plutôt
   qu'à reconstruire de zéro. Dans ce cas `*level` n'est PAS la position
   `start_pos` complète : elle ne doit surtout pas être re-snapshottée (la
   version complète l'a déjà été, avant la pause). */
static struct bd_job_result bd_run_fragment_job(const struct bd_ctx *ctx, int8_t closure_target,
                                                 struct bd_level *level, int start_pos, int allow_parallel,
                                                 int nb_workers, double effective_budget_bytes,
                                                 const char *persist_dir, const char *resume_next_partial_path)
{
    struct bd_job_result result;
    memset(&result, 0, sizeof result);

    struct bd_level cur = *level;
    /* Rend le contrat "prend possession de *level" auto-applicable : plus
       aucun appelant ne peut se retrouver avec des pointeurs pendants dans
       sa propre copie une fois cette fonction rentrée (elle libere `cur`,
       l'alias interne, dans tous les cas de sortie). */
    memset(level, 0, sizeof *level);
    int pos = start_pos;
    if (resume_next_partial_path == NULL) {
        bd_persist_snapshot(persist_dir, pos, &cur);
    }

    /* Consommé au plus une fois : seule la TOUTE PREMIÈRE transition de cet
       appel peut reprendre un `next` partiel (c'est exactement l'endroit où
       l'appel précédent s'est arrêté) — toute position suivante repart
       normalement de zéro. */
    int resuming = resume_next_partial_path != NULL;

    while (pos < BORDER_RING_LEN - 1) {
        int want_corner = ctx->is_corner_at[pos];
        struct bd_level next;
        if (!resuming && allow_parallel && nb_workers > 1 && cur.used >= bd_fork_min_states) {
            bd_transition_parallel(ctx, want_corner, &cur, nb_workers, &next);
        } else {
            /* ===================================================================
             * Pause mi-transition — chemin séquentiel UNIQUEMENT (jamais
             * bd_transition_parallel ci-dessus, Tâche future). Bug d'origine :
             * l'ancien code bâtissait `next` EN ENTIER (`bd_transition_range`
             * sur `0..cur.capacity` en un seul appel) avant le moindre contrôle
             * de taille, qui n'avait lieu qu'une fois par POSITION — un niveau
             * au facteur de branchement élevé pouvait donc dépasser le budget
             * de plusieurs fois avant qu'on s'en aperçoive, le pic mémoire réel
             * ayant déjà eu lieu (OOM constaté en production le 2026-09-10,
             * `--dp-max-ram-mo 30000 --forks 10` : jobs à 3-8,5 Gio de RSS
             * contre un budget nominal par job POOL de 1,5 Gio — cf.
             * docs/tests_et_ci.md § Scission par pile LIFO).
             *
             * Ici, `cur` est traitée par tronçons de `bd_transition_chunk_slots`
             * emplacements, avec un contrôle de `bd_level_bytes(&next)` après
             * CHAQUE tronçon. Si `next` dépasse le budget AVANT que tout `cur`
             * n'ait été consommé, on s'arrête : le reliquat de `cur` (les
             * emplacements pas encore traités) et `next` tel qu'accumulé
             * jusque-là sont sérialisés séparément et renvoyés comme un
             * successeur UNIQUE (jamais de fan-out en K fragments comme une
             * scission normale — cf. bd_job_result) à reprendre plus tard,
             * exactement à cette position. */
            if (resuming) {
                bd_level_load_file(&next, resume_next_partial_path);
                unlink(resume_next_partial_path);
            } else {
                bd_level_init(&next, 1 + ctx->nb_classes, cur.used / 2 + 16);
            }
            resuming = 0;

            /* `occupied_seen` compte les entrées OCCUPÉES de `cur` déjà
               traitées (pas les emplacements bruts parcourus, dont la
               plupart sont vides) — c'est LUI, pas `chunk_start`, qui
               détermine s'il reste vraiment du travail : un reliquat rechargé
               après une pause a presque toujours 0 entrée occupée (celles qui
               restaient à traiter au moment de la pause), mais SA capacité
               nominale (`bd_level_init` reparti d'un compte à 0, cf.
               bd_level_load_file) peut rester bien au-dessus de
               `bd_transition_chunk_slots` — sans cette garde, un reliquat
               vide continuerait à se re-scinder indéfiniment (une entrée
               vide « dépasse » n'importe quel budget aussi bas soit-il,
               exactement le même piège que la garde `cur.used > 1` plus bas,
               mais ici sur l'ENTRÉE plutôt que la SORTIE de la transition). */
            /* Taille de tronçon RÉELLEMENT utilisée : le plus grand entre le
               plancher `bd_transition_chunk_slots` et `cur.capacity /
               BD_TRANSITION_CHUNK_DIVISOR` — jamais `bd_transition_chunk_slots`
               seul. Correctif d'un incident de production (2026-09-10/11,
               `--dp-max-ram-mo 35000 --forks 20`) : à `bd_transition_chunk_slots`
               fixe (65536), un niveau réel (capacités observées 131072 à
               16777216, `next` déjà à 1,4-2,8 Go à chaque pause contre un
               budget POOL nominal de ~875 Mo) déclenchait des MILLIERS de
               pauses par transition — une fois `next` au-dessus du budget il
               y RESTE (il ne fait que grossir), donc CHAQUE tronçon suivant
               re-déclenchait une pause, chacune réécrivant PUIS relisant
               l'intégralité de `next` (déjà plusieurs Go) sur disque : plus
               de 4500 pauses mesurées sur un seul run, des dizaines de To
               d'E/S cumulées pour rien — le mécanisme protégeait bien contre
               l'OOM mais au prix d'un ralentissement catastrophique (run
               bloqué ~8h vers la position 25/59). Borner le nombre de
               tronçons à une fraction FIXE de `cur.capacity` (jamais plus de
               `BD_TRANSITION_CHUNK_DIVISOR` pauses par transition, quelle
               que soit l'échelle réelle) élimine cet effet — contrairement à
               `bd_transition_chunk_slots` seul, qui ne dit rien de la taille
               du niveau traité. Le plancher reste nécessaire pour les petits
               fixtures de test (`cur.capacity` minuscule, la fraction
               tomberait sous 1). */
            size_t chunk_slots_effective = cur.capacity / BD_TRANSITION_CHUNK_DIVISOR;
            if (chunk_slots_effective < bd_transition_chunk_slots) {
                chunk_slots_effective = bd_transition_chunk_slots;
            }

            size_t chunk_start = 0;
            size_t occupied_seen = 0;
            while (chunk_start < cur.capacity && occupied_seen < cur.used) {
                size_t chunk_end = chunk_start + chunk_slots_effective;
                if (chunk_end > cur.capacity) {
                    chunk_end = cur.capacity;
                }
                size_t occupied_before_chunk = occupied_seen;
                for (size_t idx = chunk_start; idx < chunk_end; idx++) {
                    if (bd_level_is_occupied(&cur, idx)) {
                        occupied_seen++;
                    }
                }
                bd_transition_range(ctx, want_corner, &cur, chunk_start, chunk_end, &next);
                chunk_start = chunk_end;

                /* Ne (re)contrôler la taille de `next` — et ne jamais se
                   mettre en pause — qu'après un tronçon ayant RÉELLEMENT
                   traité au moins une entrée occupée. Piège corrigé après
                   l'avoir vu boucler indéfiniment sous test
                   (`bd_transition_chunk_slots=1`, budget quasi nul) : un
                   reliquat rechargé a une capacité RECALCULÉE à partir de
                   son SEUL compte d'entrées (`bd_level_load_file`), pas de
                   l'étendue qu'il représentait avant sa pause — la ou les
                   entrées qu'il contient peuvent donc retomber À LA MÊME
                   position de hachage qu'avant (même clé, même capacité,
                   fonction de hachage déterministe) sans jamais tomber dans
                   le tout premier tronçon. Contrôler après un tronçon VIDE
                   (rien de nouveau dans `next`, `chunk_start` reparti de 0 à
                   chaque appel) répétait donc indéfiniment le même
                   diagnostic « il reste du travail, le budget est dépassé »
                   sans jamais avancer jusqu'à l'entrée réelle — un livelock,
                   pas juste une inefficacité. Ne contrôler qu'après un vrai
                   progrès garantit qu'aucune pause ne peut se reproduire à
                   l'identique : soit le tronçon courant a touché la seule
                   entrée restante (plus rien à faire, la garde `occupied_seen
                   < cur.used` de la boucle empêche déjà toute pause
                   inutile), soit il en reste d'autres, mais celle-ci vient
                   d'être durablement retirée de `cur` (jamais réinsérée). */
                double next_bytes = bd_level_bytes(&next);
                /* `next.used >= 1` : ne se met en pause qu'après un progrès
                   RÉEL (au moins un état produit) — pas de garde `> 1` ici,
                   contrairement au contrôle de fin de position plus bas : ce
                   dernier protège une SORTIE dégénérée qu'on s'apprêterait à
                   RE-scinder pour rien, alors qu'ici c'est `occupied_seen <
                   cur.used` ci-dessus qui protège l'ENTRÉE dégénérée — les
                   deux gardes répondent à des dégénérescences différentes,
                   pas la même. */
                if (occupied_seen > occupied_before_chunk && occupied_seen < cur.used &&
                    next_bytes >= effective_budget_bytes && next.used >= 1) {
                    static int mid_split_seq = 0;
                    bd_mid_transition_pauses_for_tests_counter++;
                    char leftover_path[512], next_partial_path[512];
                    snprintf(leftover_path, sizeof leftover_path, "%s/etii_bd_leftover_%d_%d.bin",
                             bd_spill_dir, (int)getpid(), mid_split_seq);
                    snprintf(next_partial_path, sizeof next_partial_path,
                             "%s/etii_bd_nextpartial_%d_%d.bin", bd_spill_dir, (int)getpid(), mid_split_seq);
                    mid_split_seq++;

                    size_t remaining = cur.capacity - chunk_start;
                    bd_level_write_slice_file(&cur, chunk_start, cur.capacity, leftover_path);
                    bd_level_write_file(&next, next_partial_path);
                    bd_level_free(&next);
                    bd_level_free(&cur);

                    fprintf(stderr,
                            "border_ring_count_dp : position %d/%d, pause mi-transition (%.2f Go, "
                            "%zu/%zu emplacements de la case courante restant a traiter)\n",
                            pos, BORDER_RING_LEN - 1, next_bytes / (1024.0 * 1024.0 * 1024.0), remaining,
                            remaining + chunk_start);

                    result.closed = 0;
                    snprintf(result.leftover_path, sizeof result.leftover_path, "%s", leftover_path);
                    snprintf(result.next_partial_path, sizeof result.next_partial_path, "%s",
                             next_partial_path);
                    result.resume_pos = pos;
                    return result;
                }
            }
        }
        bd_level_free(&cur);
        cur = next;
        pos++;
        bd_persist_snapshot(persist_dir, pos, &cur);

        double bytes = bd_level_bytes(&cur);
        fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go)\n", pos,
                BORDER_RING_LEN - 1, cur.used, bytes / (1024.0 * 1024.0 * 1024.0));

        /* `cur.used > 1` : un niveau à 0 ou 1 entrée ne peut physiquement
           pas être scindé en plusieurs fragments utiles — le scinder quand
           même produirait des fragments vides qui, une fois rechargés,
           rapportent la même capacité plancher (~200 octets, cf.
           bd_level_init) donc "dépassent" tout seuil assez bas, ce qui
           redéclencherait une scission indéfiniment (observé : boucle sans
           fin sur un seuil de test à 1 octet). Sans garde, ce n'est pas
           qu'un cas de test dégénéré : RIEN n'empêcherait la même
           situation en production si un budget venait à être fixé
           au-dessous de cette capacité plancher. */
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
            int nb_shards = bd_pick_nb_shards(bytes);
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

    bd_ring_count_t total = bd_finalize_range(ctx, ctx->is_corner_at[BORDER_RING_LEN - 1], closure_target, &cur,
                                               0, cur.capacity);
    bd_level_free(&cur);
    result.closed = 1;
    result.total = total;
    return result;
}

/* Repousse le resultat d'un job vers la pile partagee : accumule le total
   s'il a ferme l'anneau, empile SOIT les K fragments d'une scission normale
   SOIT l'unique successeur d'une pause mi-transition (leftover_path non
   vide, cf. bd_job_result) sinon — jamais deux de ces trois cas a la fois. */
static void bd_apply_job_result(struct bd_pending_stack *stack, bd_ring_count_t *total,
                                 const struct bd_job_result *r)
{
    if (r->closed) {
        *total = bd_count_add_or_die(*total, r->total, "bd_apply_job_result");
        char sr[BD_RING_COUNT_STRLEN], st[BD_RING_COUNT_STRLEN];
        bd_ring_count_format(r->total, sr, sizeof sr);
        bd_ring_count_format(*total, st, sizeof st);
        fprintf(stderr,
                "border_ring_count_dp : fragment ferme, +%s anneaux (total cumule %s, %d fragment(s) en attente)\n",
                sr, st, stack->count);
        return;
    }
    if (r->leftover_path[0] != '\0') {
        bd_pending_push(stack, r->leftover_path, "", r->resume_pos, r->next_partial_path);
        return;
    }
    for (int d = r->nb_shards - 1; d >= 0; d--) {
        char path[512];
        snprintf(path, sizeof path, "%s/shard_%d.bin", r->shard_dir, d);
        bd_pending_push(stack, path, r->shard_dir, r->resume_pos, "");
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
   (Tache 1). Retourne -1.0 (jamais une taille de fragment valide, toujours
   >= 0) sur echec au lieu d'appeler exit(1) directement : cette fonction est
   appelee depuis la boucle d'admission de bd_run_opening pendant que des
   freres peuvent deja tourner (active > 0) — sortir d'ici les laisserait
   orphelins, contrairement aux autres echecs de cette boucle qui passent
   tous par bd_abort_active_jobs avant d'arreter le programme. */
static double bd_pending_fragment_estimate_bytes(const char *shard_path)
{
    FILE *fp = fopen(shard_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : lecture de '%s' impossible pour estimation — arret\n",
                shard_path);
        return -1.0;
    }
    int32_t key_len = 0;
    uint64_t count = 0;
    if (fread(&key_len, sizeof key_len, 1, fp) != 1 || fread(&count, sizeof count, 1, fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : en-tete de '%s' illisible — arret\n", shard_path);
        fclose(fp);
        return -1.0;
    }
    fclose(fp);
    return bd_estimate_reload_bytes(key_len, count);
}

/* Compteur test-only, incremente par le PARENT juste apres un fork() du pool
   REUSSI (jamais par l'enfant : pas de memoire partagee entre process, donc
   rien a synchroniser) — seul moyen pour un test de distinguer "le mode POOL
   a reellement forke des jobs concurrents" de "le mode SOLO a tout traite
   sequentiellement en produisant, par coincidence, le meme total" : un test
   qui ne verifie que le total final ne peut pas faire cette distinction, cf.
   le commentaire de border_ring_count_dp_matches_brute_force_when_pool_mode_engages
   dans test_border_ring_dp.c. */
static long bd_pool_jobs_forked_for_tests_counter = 0;

void border_ring_dp_reset_pool_jobs_forked_for_tests(void)
{
    bd_pool_jobs_forked_for_tests_counter = 0;
}

long border_ring_dp_get_pool_jobs_forked_for_tests(void)
{
    return bd_pool_jobs_forked_for_tests_counter;
}

/* Fork un job strictement monoprocessus (allow_parallel=0, cf. spec) pour le
   fragment `slice` — le charge, l'efface du disque, avance jusqu'a fermeture
   ou nouvelle scission, ecrit son resultat dans result_path. Retourne le pid
   de l'enfant. */
static pid_t bd_fork_pool_job(const struct bd_ctx *ctx, int8_t closure_target,
                               const struct bd_pending_slice *slice, int nb_workers,
                               double effective_budget_bytes, char *result_path, size_t result_path_size,
                               const char *persist_dir)
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
    if (pid > 0) {
        bd_pool_jobs_forked_for_tests_counter++;
    }
    if (pid == 0) {
        struct bd_level cur;
        bd_level_load_file(&cur, slice->shard_path);
        unlink(slice->shard_path);
        rmdir(slice->shard_dir);
        struct bd_job_result r = bd_run_fragment_job(
            ctx, closure_target, &cur, slice->resume_pos, /*allow_parallel=*/0, nb_workers,
            effective_budget_bytes, persist_dir,
            slice->next_partial_path[0] != '\0' ? slice->next_partial_path : NULL);
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

/* bd_solo_budget_bytes/bd_pool_job_budget_bytes sont déclarées plus haut dans
   ce fichier, à côté de bd_pool_budget_bytes, pour rester visibles depuis
   border_ring_dp_set_max_ram_mo — voir les commentaires à leur déclaration
   pour le détail des marges heuristiques appliquées. */
static double bd_effective_solo_budget_bytes(void)
{
    return bd_solo_budget_bytes;
}

static double bd_effective_pool_job_budget_bytes(void)
{
    return bd_pool_job_budget_bytes;
}

double border_ring_dp_get_solo_budget_bytes_for_tests(void)
{
    return bd_solo_budget_bytes;
}

double border_ring_dp_get_pool_job_budget_bytes_for_tests(void)
{
    return bd_pool_job_budget_bytes;
}

/* ===========================================================================
 * Orchestration : fait avancer un niveau, position par position, jusqu'à la
 * fermeture de l'anneau — un niveau COMPLET reste toujours entièrement en
 * mémoire entre deux positions (jamais de représentation fragmentée
 * "vivante" à travers plusieurs positions, contrairement à l'ancien
 * mécanisme) ; PENDANT la construction d'une position, en revanche,
 * `bd_run_fragment_job` peut se mettre en pause mi-transition dès que le
 * niveau en cours de construction dépasse le budget, sans attendre qu'il
 * soit fini (cf. sa section "Pause mi-transition") — un garde-fou de plus,
 * jamais un troisième mode de dispatch : ça reste strictement interne à un
 * job, invisible du coordinateur au-delà du fragment de reprise qu'il
 * produit. Quand un niveau COMPLET dépasse le budget effectif du job qui le
 * porte (`bd_solo_budget_bytes` ou `bd_pool_job_budget_bytes` selon le mode,
 * cf. bd_run_fragment_job), il est scindé en K fragments
 * (bd_level_to_shards) et TOUS empilés sur `stack`
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
static bd_ring_count_t bd_run_opening(const struct bd_ctx *ctx, int8_t initial_required,
                                 const int8_t *initial_counts, int8_t closure_target, int nb_workers,
                                 const char *persist_dir)
{
    int nb_workers_eff = nb_workers < 1 ? 1 : nb_workers;
    double budget_solo = bd_effective_solo_budget_bytes();
    double budget_pool_total = bd_pool_budget_bytes; /* somme visee sur TOUS les slots actifs */

    struct bd_pending_stack stack;
    memset(&stack, 0, sizeof stack);
    bd_ring_count_t total = 0;

    /* Job d'amorcage (l'ouverture) : toujours SOLO, rien d'autre a
       repartir a cet instant. */
    struct bd_level seed;
    bd_level_init(&seed, 1 + ctx->nb_classes, 1 << 10);
    uint8_t key0[1 + BD_MAX_CLASSES];
    key0[0] = (uint8_t)initial_required;
    memcpy(key0 + 1, initial_counts, (size_t)ctx->nb_classes);
    bd_level_add(&seed, key0, 1);
    struct bd_job_result r0 = bd_run_fragment_job(ctx, closure_target, &seed, 1, /*allow_parallel=*/1,
                                                    nb_workers_eff, budget_solo, persist_dir, NULL);
    bd_apply_job_result(&stack, &total, &r0);

    struct bd_active_job *jobs = malloc((size_t)nb_workers_eff * sizeof *jobs);
    if (jobs == NULL) {
        fprintf(stderr, "border_ring_count_dp : allocation de %d emplacements de jobs impossible — arret\n",
                nb_workers_eff);
        exit(1);
    }
    int active = 0;

    while (stack.count > 0 || active > 0) {
        if (bd_should_run_solo(active, stack.count, nb_workers_eff)) {
            struct bd_pending_slice slice = stack.items[--stack.count];
            struct bd_level cur;
            bd_level_load_file(&cur, slice.shard_path);
            unlink(slice.shard_path);
            rmdir(slice.shard_dir);
            struct bd_job_result r = bd_run_fragment_job(
                ctx, closure_target, &cur, slice.resume_pos, /*allow_parallel=*/1, nb_workers_eff, budget_solo,
                persist_dir, slice.next_partial_path[0] != '\0' ? slice.next_partial_path : NULL);
            bd_apply_job_result(&stack, &total, &r);
            continue;
        }

        /* Mode POOL : ne consulte que le sommet de la pile (pas de recherche
           plus profonde pour un fragment plus petit qui tiendrait mieux —
           simplicite assumee, cf. spec § Risques connus). */
        while (active < nb_workers_eff && stack.count > 0) {
            const struct bd_pending_slice *top = &stack.items[stack.count - 1];
            double est = bd_pending_fragment_estimate_bytes(top->shard_path);
            /* Pause mi-transition (cf. bd_job_result) : la reprise recharge
               AUSSI next_partial_path — l'admission doit compter les deux
               fichiers, sinon un fragment sous-estime pourrait a nouveau
               depasser le budget reel une fois recharge (c'est exactement le
               genre d'ecart qui a cause l'OOM de production du 2026-09-10,
               une position plus haut : sous-estimer ce qu'une reprise va
               reellement allouer). */
            if (est >= 0.0 && top->next_partial_path[0] != '\0') {
                double est_next = bd_pending_fragment_estimate_bytes(top->next_partial_path);
                est = est_next < 0.0 ? est_next : est + est_next;
            }
            if (est < 0.0) {
                fprintf(stderr,
                        "border_ring_count_dp : estimation d'un fragment en attente impossible — arret\n");
                bd_abort_active_jobs(jobs, active, -1);
                exit(1);
            }
            if (bd_active_bytes_sum(jobs, active) + est > budget_pool_total) {
                break;
            }
            struct bd_pending_slice slice = stack.items[--stack.count];
            pid_t pid = bd_fork_pool_job(ctx, closure_target, &slice, nb_workers_eff,
                                          bd_effective_pool_job_budget_bytes(),
                                          jobs[active].result_path, sizeof jobs[active].result_path,
                                          persist_dir);
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

        int status = 0; /* waitpid peut echouer (ex: EINTR) sans jamais l'ecrire */
        pid_t done = waitpid(-1, &status, 0);
        int slot = bd_find_slot(jobs, active, done);
        if (slot < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : un job du pool a echoue (pid=%d, status=%d) — arret\n",
                    (int)done, status);
            bd_abort_active_jobs(jobs, active, slot);
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
 * plateau (cf. docs/conception/border_mass.md),
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
static bd_ring_count_t bd_count_openings(map_big_array *map, struct array_part *all_rotate_parts,
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
    bd_ring_count_t n_single_opening =
        bd_run_opening(ctx, first_cand->right, counts, first_cand->bottom, nb_workers, NULL);
    return bd_count_mul_or_die(n_single_opening, (bd_ring_count_t)nb_candidates, "bd_count_openings");
}

bd_ring_count_t border_ring_count_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers)
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
    bd_ring_count_t total = bd_count_openings(map, all_rotate_parts, ctx, id_to_class, base_counts, nb_workers);

    free(id_to_class);
    free(ctx);
    return total;
}

/* ===========================================================================
 * Reconstruction des anneaux réels (voir le commentaire de tête de
 * border_ring_dp.h et le plan approuvé pour le raisonnement complet).
 *
 * Principe : la DP normale ne conserve rien d'exploitable une fois le total
 * calculé (chaque niveau est jeté). Pour reconstruire, on calcule une SECONDE
 * DP — miroir de la première (classes avec couleur d'entrée/sortie
 * échangées, positions parcourues en sens inverse) — dont chaque niveau est
 * PERSISTÉ sur disque (bd_run_fragment_job(persist_dir=...)) plutôt que
 * jeté : le niveau miroir à la position `i` donne, pour tout état
 * (couleur requise, compteurs restants), le nombre de façons de COMPLÉTER
 * l'anneau réel à partir de la position `BORDER_RING_LEN - i` — l'oracle
 * d'élagage qui rend un DFS guidé sur les CLASSES (pas les pièces réelles)
 * praticable : à chaque position, seules les classes dont l'état résultant a
 * une complétion non nulle sont essayées, donc toute branche explorée mène
 * forcément à une fermeture valide.
 *
 * Une fois une SUITE DE CLASSES complète trouvée, elle est immédiatement
 * développée en pièces réelles (bd_expand_dfs) — un DFS restreint à la seule
 * classe choisie à chaque position, réutilisant la même mécanique de lookup
 * que `bw_dfs` (tests/tools/border_walk.c) : correction géométrique héritée
 * gratuitement, aucune logique de rotation dupliquée ici.
 */

/* Calcule, pour CHAQUE état occupant `forward_level` (le niveau réellement
 * atteignable à la position `pos` de la passe AVANT, persisté par
 * `bd_run_opening(..., persist_dir)`), le nombre de façons de COMPLÉTER
 * l'anneau jusqu'à la fermeture — la vraie table de « complétion » qui sert
 * d'oracle d'élagage au DFS guidé sur les classes. `completion_next` est la
 * table déjà calculée pour la position `pos + 1` (NULL seulement quand
 * `pos == BORDER_RING_LEN - 1`, où la fermeture se vérifie directement contre
 * `closure_target`, sans niveau suivant). ATTENTION à ne jamais confondre
 * ceci avec une DP miroir indépendante (tentative initiale, incorrecte) :
 * la complétion d'un état DOIT être calculée à partir des états RÉELLEMENT
 * atteints par la passe avant à cette position — recalculer un « niveau
 * arrière » depuis un point de départ générique produit un espace d'états
 * sans rapport avec celui réellement parcouru par la passe avant. */
static void bd_completion_step(const struct bd_ctx *ctx, int pos, int8_t closure_target,
                                const struct bd_level *forward_level, const struct bd_level *completion_next,
                                struct bd_level *completion_cur)
{
    int nb = ctx->nb_classes;
    int want_corner = ctx->is_corner_at[pos];
    bd_level_init(completion_cur, forward_level->key_len, forward_level->used * 2 + 16);

    for (size_t idx = 0; idx < forward_level->capacity; idx++) {
        if (!bd_level_is_occupied(forward_level, idx)) {
            continue;
        }
        const uint8_t *key = forward_level->keys + idx * (size_t)forward_level->key_len;
        int8_t required = (int8_t)key[0];
        const int8_t *counts = (const int8_t *)(key + 1);

        bd_ring_count_t comp = 0;
        for (int c = 0; c < nb; c++) {
            if (ctx->classes[c].is_corner != want_corner || counts[c] == 0 || ctx->classes[c].color_a != required) {
                continue;
            }
            if (pos == BORDER_RING_LEN - 1) {
                if (ctx->classes[c].color_b == closure_target) {
                    comp = bd_count_add_or_die(comp, (bd_ring_count_t)counts[c], "bd_completion_step");
                }
                continue;
            }
            uint8_t next_key[1 + BD_MAX_CLASSES];
            next_key[0] = (uint8_t)ctx->classes[c].color_b;
            memcpy(next_key + 1, counts, (size_t)nb);
            next_key[1 + c]--;
            bd_ring_count_t next_comp;
            if (bd_level_lookup(completion_next, next_key, &next_comp)) {
                comp = bd_count_add_or_die(
                    comp, bd_count_mul_or_die(next_comp, (bd_ring_count_t)counts[c], "bd_completion_step"),
                    "bd_completion_step");
            }
        }
        if (comp > 0) {
            bd_level_add(completion_cur, key, comp);
        }
    }
}

/* Balaie les niveaux AVANT persistés (positions BORDER_RING_LEN-1 downto 1)
 * et écrit, pour chacun, sa table de complétion sous
 * `persist_dir/completion_<pos>.bin` — un seul fichier par position (calculé
 * ici en un seul process, jamais scindé/forké, contrairement aux niveaux
 * avant qui peuvent l'être) : chargeable directement par `bd_level_load_file`,
 * sans fusion de fragments. */
static void bd_build_and_persist_completions(const struct bd_ctx *ctx, int8_t closure_target,
                                              const char *persist_dir)
{
    int nb = ctx->nb_classes;
    struct bd_level completion_next;
    int have_next = 0;

    for (int pos = BORDER_RING_LEN - 1; pos >= 1; pos--) {
        struct bd_level forward_level;
        bd_persist_load_merged(persist_dir, pos, 1 + nb, &forward_level);

        struct bd_level completion_cur;
        bd_completion_step(ctx, pos, closure_target, &forward_level, have_next ? &completion_next : NULL,
                            &completion_cur);
        bd_level_free(&forward_level);
        if (have_next) {
            bd_level_free(&completion_next);
        }

        char path[900];
        snprintf(path, sizeof path, "%s/completion_%d.bin", persist_dir, pos);
        bd_level_write_file(&completion_cur, path);

        completion_next = completion_cur;
        have_next = 1;
    }
    if (have_next) {
        bd_level_free(&completion_next);
    }
}

struct bd_reconstruct_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    const struct bd_ctx *ctx;      /* contexte direct (classes + is_corner_at) */
    const int16_t *id_to_class;
    const char *back_dir;          /* niveaux persistés de la passe arrière miroir */
    int8_t closure_target;         /* couleur de fermeture (passe avant) */
    int8_t class_seq[BORDER_RING_LEN]; /* indices 1..BORDER_RING_LEN-1 utilisés */
    border_ring_found_cb on_found;
    void *user_ctx;
    long long max_rings;
    long long delivered;
    int cached_pos;                /* position dont le niveau arrière est en cache, -1 = aucun */
    struct bd_level cached_level;
    struct possibility_packet board;
    uint16_t opening_id;
    uint8_t opening_rotation;
};

static void bd_expand_dfs(struct bd_reconstruct_ctx *rc, const int8_t order[BORDER_RING_LEN][2], int pos)
{
    if (rc->delivered >= rc->max_rings) {
        return;
    }
    if (pos == BORDER_RING_LEN) {
        rc->on_found(&rc->board, rc->user_ctx);
        rc->delivered++;
        return;
    }

    int8_t x = order[pos][0];
    int8_t y = order[pos][1];
    key_part key;
    what_search_in_grid_to_key(rc->all_rotate_parts, &rc->board, x, y, &key, (int8_t)rc->map->sizearrayM);
    map_bucket bucket = map_bucket_packed(rc->map, &key);
    int8_t want_class = rc->class_seq[pos];

    for (int s = 0; s < bucket.size; s++) {
        const struct part *cand = &bucket.parts[s];
        if (cand->id <= 0 || rc->id_to_class[cand->id] != want_class) {
            continue;
        }
        uint16_t face_idx = (uint16_t)(cand->id - 1);
        if (is_face_used(rc->board.b_faceused, face_idx)) {
            continue;
        }

        rc->board.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
        set_face_used(rc->board.b_faceused, face_idx, 1);
        rc->board.alloc = (uint16_t)(pos + 1);

        bd_expand_dfs(rc, order, pos + 1);

        set_face_used(rc->board.b_faceused, face_idx, 0);
        rc->board.grid[x][y] = -2;
        rc->board.alloc = (uint16_t)pos;

        if (rc->delivered >= rc->max_rings) {
            return;
        }
    }
}

/* Développe UNE suite de classes complète (rc->class_seq, positions
 * 1..BORDER_RING_LEN-1) en toutes les assignations de pièces réelles
 * possibles — une par combinaison de pièces disponibles à chaque position où
 * la classe choisie a plusieurs pièces encore libres, exactement comme un
 * DFS réel les essaierait une à une (cf. bw_dfs, tests/tools/border_walk.c,
 * dont ce DFS restreint reprend la mécanique de lookup telle quelle). */
static void bd_expand_class_sequence(struct bd_reconstruct_ctx *rc)
{
    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(order);

    memset(&rc->board, 0, sizeof rc->board);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            rc->board.grid[x][y] = -2;
        }
    }
    rc->board.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    int8_t x0 = order[0][0];
    int8_t y0 = order[0][1];
    rc->board.grid[x0][y0] = (int16_t)id_for_rotated_part(rc->opening_id, rc->opening_rotation);
    set_face_used(rc->board.b_faceused, (uint16_t)(rc->opening_id - 1), 1);
    rc->board.alloc = 1;

    bd_expand_dfs(rc, order, 1);
}

/* DFS guidé sur l'alphabet des CLASSES (~15-18, jamais les pièces réelles) —
 * élagué par la table de complétion persistée (`bd_build_and_persist_completions`) :
 * à la position `pos`, une classe candidate n'est essayée que si l'état
 * résultant a une complétion non nulle connue à la position `pos + 1`
 * (chargée à la demande, gardée en cache tant qu'on reste à la même
 * position — la descente en profondeur d'abord ne change la position
 * courante que de ±1 à la fois). Position `BORDER_RING_LEN - 1` (dernière) :
 * pas de niveau suivant à consulter, la fermeture se vérifie directement
 * contre `closure_target`, comme `bd_finalize_range`. */
static void bd_reconstruct_class_dfs(struct bd_reconstruct_ctx *rc, int pos, int8_t required, int8_t *counts)
{
    if (rc->delivered >= rc->max_rings) {
        return;
    }
    int nb = rc->ctx->nb_classes;

    if (pos == BORDER_RING_LEN - 1) {
        for (int c = 0; c < nb; c++) {
            if (rc->ctx->classes[c].is_corner != rc->ctx->is_corner_at[pos] || counts[c] == 0 ||
                rc->ctx->classes[c].color_a != required || rc->ctx->classes[c].color_b != rc->closure_target) {
                continue;
            }
            rc->class_seq[pos] = (int8_t)c;
            bd_expand_class_sequence(rc);
            if (rc->delivered >= rc->max_rings) {
                return;
            }
        }
        return;
    }

    if (rc->cached_pos != pos + 1) {
        if (rc->cached_pos != -1) {
            bd_level_free(&rc->cached_level);
        }
        char path[900];
        snprintf(path, sizeof path, "%s/completion_%d.bin", rc->back_dir, pos + 1);
        bd_level_load_file(&rc->cached_level, path);
        rc->cached_pos = pos + 1;
    }

    for (int c = 0; c < nb; c++) {
        if (rc->ctx->classes[c].is_corner != rc->ctx->is_corner_at[pos] || counts[c] == 0 ||
            rc->ctx->classes[c].color_a != required) {
            continue;
        }
        int8_t next_required = rc->ctx->classes[c].color_b;
        uint8_t key[1 + BD_MAX_CLASSES];
        key[0] = (uint8_t)next_required;
        memcpy(key + 1, counts, (size_t)nb);
        key[1 + c]--;

        bd_ring_count_t completion;
        if (!bd_level_lookup(&rc->cached_level, key, &completion)) {
            continue;
        }

        counts[c]--;
        rc->class_seq[pos] = (int8_t)c;
        bd_reconstruct_class_dfs(rc, pos + 1, next_required, counts);
        counts[c]++;

        if (rc->delivered >= rc->max_rings) {
            return;
        }
    }
}

/* Reconstruit et délivre, via on_found, chaque anneau de bordure réel
 * (jusqu'à max_rings) — cf. le commentaire de tête de cette section pour le
 * mécanisme (passe arrière miroir persistée + DFS de classes guidé par
 * l'oracle de complétion + expansion en pièces réelles). Contrairement à
 * bd_count_openings, énumère explicitement les nb_candidates pièces-coin
 * réelles (pas de raccourci ×4 : reconstruire une rotation géométrique du
 * paquet serait un risque de bug pour un gain minime, le coût de 4
 * reconstructions complètes restant négligeable). Le total délivré DOIT
 * correspondre à border_ring_count_dp — échec bruyant sinon (jamais un
 * fichier .back silencieusement incomplet, sauf si max_rings a
 * délibérément coupé la délivrance avant). */
long long border_ring_reconstruct_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers,
                                      long long max_rings, border_ring_found_cb on_found, void *user_ctx)
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
        ctx->is_corner_at[i] = (int8_t)((x == 0 || x == ETERN_SIZE - 1) && (y == 0 || y == ETERN_SIZE - 1));
    }

    if (nb_workers < 1) {
        nb_workers = 1;
    }

    bd_ring_count_t known_total = border_ring_count_dp(map, all_rotate_parts, nb_workers);

    struct possibility_packet empty_state;
    memset(&empty_state, 0, sizeof empty_state);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            empty_state.grid[x][y] = -2;
        }
    }
    empty_state.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    key_part key0;
    what_search_in_grid_to_key(all_rotate_parts, &empty_state, 0, 0, &key0, (int8_t)map->sizearrayM);
    map_bucket bucket0 = map_bucket_packed(map, &key0);

    long long total_delivered = 0;

    for (int s = 0; s < bucket0.size && total_delivered < max_rings; s++) {
        const struct part *cand = &bucket0.parts[s];
        if (cand->id <= 0 || id_to_class[cand->id] < 0) {
            continue;
        }

        int8_t counts[BD_MAX_CLASSES];
        for (int c = 0; c < ctx->nb_classes; c++) {
            counts[c] = (int8_t)base_counts[c];
        }
        counts[id_to_class[cand->id]]--;

        int8_t initial_required = cand->right;
        int8_t closure_target = cand->bottom;

        char persist_dir[300];
        snprintf(persist_dir, sizeof persist_dir, "%s/etii_bd_recon_%d_%d", bd_spill_dir, (int)getpid(), s);
        bd_mkdir_or_die(persist_dir);

        bd_ring_count_t forward_total_for_opening =
            bd_run_opening(ctx, initial_required, counts, closure_target, nb_workers, persist_dir);
        bd_build_and_persist_completions(ctx, closure_target, persist_dir);

        struct bd_reconstruct_ctx rc;
        memset(&rc, 0, sizeof rc);
        rc.map = map;
        rc.all_rotate_parts = all_rotate_parts;
        rc.ctx = ctx;
        rc.id_to_class = id_to_class;
        rc.back_dir = persist_dir;
        rc.closure_target = closure_target;
        rc.on_found = on_found;
        rc.user_ctx = user_ctx;
        rc.max_rings = max_rings - total_delivered;
        rc.delivered = 0;
        rc.cached_pos = -1;
        rc.opening_id = (uint16_t)cand->id;
        rc.opening_rotation = (uint8_t)cand->rotation;

        int8_t working_counts[BD_MAX_CLASSES];
        memcpy(working_counts, counts, sizeof counts);
        bd_reconstruct_class_dfs(&rc, 1, initial_required, working_counts);

        if (rc.cached_pos != -1) {
            bd_level_free(&rc.cached_level);
        }
        bd_persist_cleanup(persist_dir);

        if ((bd_ring_count_t)rc.delivered != forward_total_for_opening && rc.delivered < rc.max_rings) {
            char sf[BD_RING_COUNT_STRLEN];
            bd_ring_count_format(forward_total_for_opening, sf, sizeof sf);
            fprintf(stderr,
                    "border_ring_count_dp : reconstruction incomplete pour le coin d'ouverture id=%d — "
                    "%lld anneau(x) reconstruit(s), %s attendu(s) pour ce coin — arret\n",
                    cand->id, rc.delivered, sf);
            free(id_to_class);
            free(ctx);
            exit(1);
        }

        total_delivered += rc.delivered;
    }

    if ((bd_ring_count_t)total_delivered != known_total && total_delivered < max_rings) {
        char sk[BD_RING_COUNT_STRLEN];
        bd_ring_count_format(known_total, sk, sizeof sk);
        fprintf(stderr,
                "border_ring_count_dp : reconstruction incomplete — %lld anneau(x) reconstruit(s), "
                "%s attendu(s) (masse totale) — arret\n",
                total_delivered, sk);
        exit(1);
    }

    free(id_to_class);
    free(ctx);
    return total_delivered;
}
