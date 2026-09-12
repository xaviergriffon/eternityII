/**
 * @file border_ring_dp.c
 * @brief Comptage exact de la masse des anneaux de bordure — DP par TRI
 * EXTERNE sur des classes de pièces interchangeables.
 *
 * Trois choses à savoir avant de toucher à ce fichier :
 *
 * 1. **Un niveau est un TABLEAU TRIÉ, pas une table de hachage.** Les états
 *    identiques se retrouvent par l'ORDRE (tri + fusion des clés égales
 *    adjacentes), jamais par co-résidence en mémoire. C'est ce qui permet à
 *    un niveau de dépasser la RAM sans perdre une seule fusion : il devient
 *    un fichier trié, lu séquentiellement par la position suivante.
 *
 * 2. **Un état tient dans une clé compacte.** Les compteurs par classe sont
 *    bornés par la multiplicité de leur classe, donc quelques bits chacun
 *    (cf. `struct bd_key_layout`) : 32 octets par état stocké, contre 111
 *    pour la version précédente.
 *
 * 3. **Le budget RAM ne pilote plus que la taille d'un tampon de tri.** Le
 *    dépasser ajoute un run à fusionner — ça ne change plus la forme du
 *    calcul.
 *
 * ## Ce que cette version remplace, et pourquoi
 *
 * La version précédente tenait chaque niveau dans une table de hachage à
 * adressage ouvert. Quand un niveau dépassait le budget, il était scindé par
 * hachage de la clé en K fragments empilés sur une pile LIFO, repris plus
 * tard et poursuivis INDÉPENDAMMENT jusqu'à la fermeture de l'anneau, par un
 * coordinateur à deux modes (SOLO/POOL) — avec, en plus, une « pause
 * mi-transition » qui sérialisait le niveau en construction dès qu'il
 * dépassait le budget.
 *
 * Le partitionnement par hachage ne sépare aucun doublon AU MOMENT de la
 * scission (même clé => même fragment). Mais dès la position suivante, deux
 * fragments différents produisent les mêmes clés — et ne les fusionnent plus
 * JAMAIS. La DP dégénérait en somme de sous-DP redondantes, et la
 * duplication composait à chaque position. Mesuré sur un run de production de
 * 27 h (`--dp-max-ram-mo 35000 --forks 10`, jeu 256) :
 *
 * - position 19, 109 077 416 états fusionnés, scindée en 16 fragments :
 *   chaque fragment produisait ~21 M états à la position 20, soit 336 M au
 *   total pour un facteur de branchement brut de x3,08 — autrement dit PLUS
 *   AUCUNE fusion à l'intérieur d'un fragment, là où la croissance fusionnée
 *   venait d'être mesurée à x1,89 ;
 * - à la position 45, 224 fragments portaient ensemble 3x10^9 états pour un
 *   niveau fusionné de quelques millions ;
 * - 56 % du travail total (1,67x10^10 états matérialisés) tombait dans les
 *   positions 43-59, où un niveau fusionné retombe à quelques milliers
 *   d'états puis à 8 ;
 * - 916 fragments créés pour 224 fermés : la pile ne se vidait pas.
 *
 * Le débit global était de 172 000 états/s pour 10 forks, alors qu'UN cœur
 * en mémoire en fait ~1,7 M/s — les 10 workers réunis tournaient 8,7x moins
 * vite qu'un seul process tenant son niveau en RAM, l'essentiel du temps
 * passant en E/S de pause (6768 pauses, chacune réécrivant puis relisant un
 * niveau de ~2,8 Gio).
 *
 * D'où cette réécriture. Le tri externe n'a aucune de ces faiblesses :
 * - un niveau reste UNIQUE et intégralement fusionné, quelle que soit sa
 *   taille par rapport à la RAM ;
 * - il est écrit une fois et relu une fois, séquentiellement ;
 * - la parallélisation par forks devient franche : chaque worker rend un
 *   fichier DÉJÀ TRIÉ, que le parent fusionne en une passe linéaire, au lieu
 *   de réinsérer une à une la production de ses workers dans une table de
 *   hachage (une partie série valant ~100 % du travail utile, donc un
 *   speedup borné à ~2 quel que soit `--forks`).
 *
 * ## Ce qui n'a PAS changé
 *
 * Le regroupement des pièces de bord en classes d'équivalence (paire
 * ORDONNÉE couleur-requise/couleur-produite + forme coin/bord, cf.
 * `bd_build_classes` — l'ordre est essentiel, pas un détail), la
 * pondération de chaque transition par le nombre de pièces encore
 * disponibles dans la classe (c'est elle qui transforme un compte de suites
 * de CLASSES en compte d'anneaux de pièces RÉELLES), le développement d'une
 * seule pièce-coin d'ouverture multiplié par 4 (symétrie de rotation, cf.
 * `bd_count_openings`), et la reconstruction des anneaux réels
 * (`border_ring_reconstruct_dp`).
 */
#include "tools/border_ring_dp.h"

#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>

/* Borne généreuse : le vrai jeu 256 pièces n'en produit que 28 (24 classes
   de bord + 4 de coin), le jeu 16 pièces des tests beaucoup moins encore
   (chaque pièce y forme sa propre classe). */
#define BD_MAX_CLASSES 64

/* Les couleurs sont des `int8_t` positifs : 128 entrées couvrent tout le
   domaine, sans jamais indexer hors bornes quel que soit le jeu de pièces. */
#define BD_MAX_COLORS 128

/* Répertoire des fichiers temporaires (tranches triées des transitions
   forkées, runs débordés par le trieur, niveaux persistés de la
   reconstruction) — `/tmp` par défaut. `/tmp` est souvent une petite
   partition ou un tmpfs plafonné bien en-deçà de la RAM de la machine
   (indépendamment de sa taille) : un niveau de la DP réelle pèse des
   dizaines de Go, qui peuvent saturer `/tmp` même sur une machine par
   ailleurs bien dotée — observé en pratique (échec au niveau 21 sur la
   machine visée, 2x10 cœurs/48 Go, cf. docs/tests_et_ci.md).
   `border_ring_dp_set_spill_dir` permet de rediriger vers un disque plus
   grand, comme `--stock-spill-dir` pour le stock principal
   (`core/stock_spill.c`). */
static const char *bd_spill_dir = "/tmp";

void border_ring_dp_set_spill_dir(const char *dir)
{
    bd_spill_dir = dir;
}

/* Paire ORDONNÉE (couleur requise en entrée, couleur produite en sortie) —
   PAS une paire non ordonnée, cf. le commentaire de bd_build_classes pour
   pourquoi cette distinction est cruciale (32x de sur-comptage sinon). */
struct bd_class {
    int8_t color_a; /* couleur requise en entrée */
    int8_t color_b; /* couleur produite en sortie */
    int8_t is_corner; /* 2 faces à 0 (coin) vs 1 (bord) */
};

/* ===========================================================================
 * Clé compacte : UN `uint64_t` par état, plus un octet par classe.
 *
 * Un état de la DP, c'est (couleur requise, compteurs restants par classe).
 * La version précédente le stockait littéralement — `1 + nb_classes` octets,
 * soit 29 sur le jeu réel, plus 16 octets de valeur et un bit d'occupation,
 * dans une table de hachage à adressage ouvert tenue sous 60 % de charge :
 * 111 octets par état RÉELLEMENT stocké (mesuré : 11,28 Gio pour 109 077 416
 * états à la position 19 du jeu 256).
 *
 * Or chaque compteur est borné par la multiplicité de SA classe (4 au plus
 * sur `data/pieces.csv`) : `ceil(log2(m_c + 1))` bits suffisent, soit 51
 * bits pour les 28 classes du jeu réel, plus 8 bits de couleur = 59 bits.
 * Un état stocké coûte donc 32 octets (clé + valeur, toutes deux sur 128
 * bits — cf. `bd_key_t` pour pourquoi la clé n'est pas un `uint64_t`) au lieu
 * de 111 — et sans facteur de charge à provisionner, puisque le stockage est
 * un TABLEAU TRIÉ, plus une table de hachage (cf. `struct bd_sorter`).
 *
 * La disposition est calculée à l'exécution (`bd_key_layout_init`) à partir
 * des multiplicités réelles, jamais figée : un jeu de pièces différent donne
 * d'autres largeurs. Si le total dépasse 64 bits, échec BRUYANT immédiat —
 * jamais une troncature silencieuse, qui ferait fusionner deux états
 * distincts et fausserait le compte sans rien signaler.
 *
 * `unit[c]` (= `1 << shift[c]`) rend la transition branchless : poser une
 * pièce de la classe `c` et passer à la couleur `b`, c'est
 * `(key & bd_counts_mask) - unit[c] | (b << color_shift)` — une soustraction
 * là où la version précédente recopiait `nb_classes` octets par successeur.
 */
/* 128 bits et non 64 : le jeu réel n'en demande que 59 (51 de compteurs +
   8 de couleur), mais les fixtures de test montées sur un plateau 16x16
   (`brd_make_rotate_parts`, 60 classes de multiplicité 1 par construction)
   en demandent 68 — et une clé trop étroite ne se remarquerait pas : elle
   ferait fusionner deux états distincts, donc rendrait un total faux, sans
   assertion ni débordement. Une seule largeur, valable pour tout jeu de
   pièces, plutôt qu'un chemin rapide 64 bits doublé d'un mode dégradé : une
   entrée pèse 32 octets au lieu de 24 (clé alignée sur 16), soit 33 % d'E/S
   en plus sur un niveau qui déborde — contre 111 octets par état dans la
   version à table de hachage. Spécialiser la clé en 64 bits quand la
   disposition y tient reste une optimisation ouverte, chiffrée à ces 33 %. */
typedef unsigned __int128 bd_key_t;

struct bd_key_layout {
    int shift[BD_MAX_CLASSES];
    bd_key_t mask[BD_MAX_CLASSES];  /* (1 << bits) - 1, NON décalé */
    bd_key_t unit[BD_MAX_CLASSES];  /* 1 << shift[c] */
    bd_key_t counts_mask;           /* tous les champs de compteurs réunis */
    int color_shift;
    int total_bits;
};

struct bd_ctx {
    struct bd_class classes[BD_MAX_CLASSES];
    int nb_classes;
    int8_t is_corner_at[BORDER_RING_LEN]; /* forme de chaque case de l'anneau */
    struct bd_key_layout layout;
    /* Index des classes par (couleur requise, forme) : la boucle chaude de
       `bd_transition` n'essaie plus les `nb_classes` classes pour n'en
       retenir qu'une poignée (28 tours pour ~3 succès sur le jeu réel), elle
       ne parcourt que les candidates possibles. Pur accélérateur, aucune
       sémantique propre : reconstruit par `bd_index_classes_by_color` à
       partir de `classes[]`, jamais renseigné à la main. */
    int8_t by_color[BD_MAX_COLORS][2][BD_MAX_CLASSES];
    int8_t by_color_n[BD_MAX_COLORS][2];
};

static void bd_key_layout_init(struct bd_key_layout *l, const int counts[BD_MAX_CLASSES], int nb_classes)
{
    int shift = 0;
    l->counts_mask = 0;
    for (int c = 0; c < nb_classes; c++) {
        int bits = 1;
        while ((1 << bits) <= counts[c]) {
            bits++;
        }
        l->shift[c] = shift;
        l->mask[c] = ((bd_key_t)1 << bits) - 1;
        l->unit[c] = (bd_key_t)1 << shift;
        l->counts_mask |= l->mask[c] << shift;
        shift += bits;
    }
    l->color_shift = shift;
    l->total_bits = shift + 8; /* couleur requise, telle quelle (int8_t positif) */
    if (l->total_bits > 128) {
        fprintf(stderr,
                "border_ring_count_dp : l'état d'un niveau demande %d bits (> 128) sur ce jeu de pièces "
                "— clé compacte impossible, arret\n",
                l->total_bits);
        exit(1);
    }
}

static inline uint64_t bd_key_count(const struct bd_key_layout *l, bd_key_t key, int c)
{
    return (uint64_t)((key >> l->shift[c]) & l->mask[c]);
}

static inline int8_t bd_key_color(const struct bd_key_layout *l, bd_key_t key)
{
    return (int8_t)((uint64_t)(key >> l->color_shift) & 0xFFu);
}

/* Successeur : mêmes compteurs moins un exemplaire de `c`, couleur requise
   remplacée par celle que produit `c`. Appelée une fois par arête de la DP —
   c'est LA boucle chaude du calcul. */
static inline bd_key_t bd_key_step(const struct bd_key_layout *l, bd_key_t key, int c, int8_t color_b)
{
    return ((key & l->counts_mask) - l->unit[c]) | ((bd_key_t)(uint8_t)color_b << l->color_shift);
}

static bd_key_t bd_key_make(const struct bd_key_layout *l, int8_t required, const int8_t *counts,
                             int nb_classes)
{
    bd_key_t key = 0;
    for (int c = 0; c < nb_classes; c++) {
        key |= (bd_key_t)counts[c] << l->shift[c];
    }
    return key | ((bd_key_t)(uint8_t)required << l->color_shift);
}

/* `by_color` est indexé par la couleur BRUTE (0..BD_MAX_COLORS-1) : toute
   couleur hors de ce domaine serait une lecture hors bornes silencieuse à
   chaque état du niveau. Vérifié pour les DEUX couleurs d'une classe, pas
   seulement celle d'entrée — c'est la couleur de SORTIE qui devient la
   couleur requise de l'état suivant, donc l'index de la transition d'après. */
static void bd_check_color_or_die(int color, const char *what)
{
    if (color < 0 || color >= BD_MAX_COLORS) {
        fprintf(stderr, "border_ring_count_dp : %s = %d hors du domaine [0,%d) — arret\n", what, color,
                BD_MAX_COLORS);
        exit(1);
    }
}

static void bd_index_classes_by_color(struct bd_ctx *ctx)
{
    memset(ctx->by_color_n, 0, sizeof ctx->by_color_n);
    for (int c = 0; c < ctx->nb_classes; c++) {
        int color = (int)ctx->classes[c].color_a;
        int shape = ctx->classes[c].is_corner ? 1 : 0;
        bd_check_color_or_die(color, "couleur d'entree d'une classe");
        bd_check_color_or_die((int)ctx->classes[c].color_b, "couleur de sortie d'une classe");
        ctx->by_color[color][shape][ctx->by_color_n[color][shape]++] = (int8_t)c;
    }
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
   (128 bits) : plutôt qu'un wrap silencieux (un `long long` débordait déjà
   sur le jeu réel, cf. le commentaire de tête de bd_ring_count_t dans
   border_ring_dp.h), échec bruyant immédiat, même philosophie que
   bd_write_or_die. */
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

/* ===========================================================================
 * Un état stocké : clé compacte + nombre de façons.
 *
 * La valeur est scindée en deux `uint64_t` plutôt que déclarée
 * `bd_ring_count_t` : un `unsigned __int128` s'aligne sur 16 octets, donc un
 * `struct { uint64_t; unsigned __int128; }` pèserait 32 octets dont 8 de
 * remplissage — un tiers du fichier de niveau et du tampon de tri gâché pour
 * rien. Trois `uint64_t` s'alignent sur 8 : 24 octets pleins, écrivables
 * tels quels en un seul `fwrite` du tableau. Verrouillé à la compilation
 * ci-dessous, pas seulement espéré : la taille de cette structure EST le
 * format de fichier des niveaux.
 */
struct bd_entry {
    bd_key_t key;
    uint64_t val_lo;
    uint64_t val_hi;
};

typedef char bd_entry_is_32_bytes[sizeof(struct bd_entry) == 32 ? 1 : -1];

static inline bd_ring_count_t bd_entry_value(const struct bd_entry *e)
{
    return ((bd_ring_count_t)e->val_hi << 64) | (bd_ring_count_t)e->val_lo;
}

static inline void bd_entry_set_value(struct bd_entry *e, bd_ring_count_t v)
{
    e->val_lo = (uint64_t)v;
    e->val_hi = (uint64_t)(v >> 64);
}

/* ===========================================================================
 * Un NIVEAU (une position de l'anneau) : un tableau TRIÉ par clé, sans
 * doublon — en RAM s'il y tient, sinon un fichier de même contenu et même
 * ordre. Les deux formes sont interchangeables pour tout ce qui les
 * consomme : la transition les lit séquentiellement (bd_level_reader), la
 * reconstruction les interroge par dichotomie (bd_level_lookup), et aucune
 * des deux n'a besoin de savoir laquelle elle a en main.
 *
 * C'est ce tri qui remplace la table de hachage de la version précédente, et
 * ce remplacement est le cœur de cette réécriture : une table de hachage ne
 * sait fusionner deux états identiques que si les deux tiennent en mémoire
 * EN MÊME TEMPS. Quand un niveau dépassait le budget, l'ancienne version le
 * scindait en fragments traités indépendamment jusqu'à la fermeture de
 * l'anneau — et deux fragments produisant plus tard la MÊME clé ne la
 * fusionnaient plus jamais. La DP dégénérait en somme de sous-DP
 * redondantes : mesuré sur un run de production de 27 h, la position 19
 * (109 077 416 états fusionnés) scindée en 16 fragments produisait à la
 * position 20 16 x 21 M = 336 M états, soit exactement le facteur de
 * branchement brut (x3,08) — plus aucune fusion à l'intérieur d'un fragment,
 * là où la croissance fusionnée venait d'être mesurée à x1,89. À la position
 * 45, 224 fragments portaient ensemble 3 x 10^9 états pour un niveau
 * fusionné de quelques millions. Un tri-fusion externe n'a pas cette
 * faiblesse : les doublons se retrouvent par l'ORDRE, jamais par la
 * co-résidence en mémoire, donc un niveau reste intégralement fusionné quelle
 * que soit sa taille par rapport à la RAM.
 */
#define BD_PATH_MAX 512

struct bd_level {
    struct bd_entry *entries; /* non-NULL => niveau résident en mémoire */
    size_t count;             /* nombre d'états, dans les deux formes */
    char path[BD_PATH_MAX];   /* non vide => niveau sur disque */
};

static void bd_path_or_die(char *dst, size_t dstlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void bd_path_or_die(char *dst, size_t dstlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(dst, dstlen, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= dstlen) {
        fprintf(stderr,
                "border_ring_count_dp : chemin de fichier temporaire trop long (%d octets, max %zu) — "
                "--spill-dir trop profond ? arret\n",
                written, dstlen - 1);
        exit(1);
    }
}

static void bd_write_or_die(FILE *fp, const void *buf, size_t size, size_t nmemb, const char *context)
{
    if (nmemb > 0 && fwrite(buf, size, nmemb, fp) != nmemb) {
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

static FILE *bd_fopen_or_die(const char *path, const char *mode)
{
    FILE *fp = fopen(path, mode);
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : ouverture de '%s' (%s) impossible — arret\n", path, mode);
        exit(1);
    }
    return fp;
}

/* Format d'un niveau sur disque : un compte, puis `count` entrées brutes
   déjà triées. Pas de key_len en en-tête (contrairement à la version
   précédente) : la clé fait toujours 8 octets, et la disposition des champs
   est recalculée à l'identique par chaque process depuis le même jeu de
   pièces — jamais lue d'un fichier. */
static void bd_entries_write_file(const struct bd_entry *entries, size_t count, const char *path)
{
    FILE *fp = bd_fopen_or_die(path, "wb");
    uint64_t n = count;
    bd_write_or_die(fp, &n, sizeof n, 1, path);
    bd_write_or_die(fp, entries, sizeof *entries, count, path);
    bd_close_or_die(fp, path);
}

static uint64_t bd_file_count(const char *path)
{
    FILE *fp = bd_fopen_or_die(path, "rb");
    uint64_t n = 0;
    if (fread(&n, sizeof n, 1, fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : en-tete de '%s' illisible — arret\n", path);
        exit(1);
    }
    fclose(fp);
    return n;
}

static void bd_level_free(struct bd_level *level)
{
    free(level->entries);
    level->entries = NULL;
    if (level->path[0] != '\0') {
        unlink(level->path);
        level->path[0] = '\0';
    }
    level->count = 0;
}

static double bd_level_bytes(const struct bd_level *level)
{
    return (double)level->count * (double)sizeof(struct bd_entry);
}

/* ===========================================================================
 * Tri et fusion des doublons — le remplaçant direct de `bd_level_add`.
 *
 * Tri à 3 voies (drapeau hollandais) et non un quicksort binaire : les clés
 * en DOUBLON sont la matière première de cette DP (c'est exactement ce qu'on
 * cherche à fusionner), et un pivot répété fait dégénérer un partitionnement
 * binaire en O(n^2) là où le partitionnement ternaire retire tous les
 * exemplaires du pivot en une passe. Récursion sur la PLUS PETITE moitié
 * puis boucle sur l'autre : profondeur de pile bornée par log2(n), jamais
 * par n.
 */
static inline void bd_entry_swap(struct bd_entry *a, struct bd_entry *b)
{
    struct bd_entry t = *a;
    *a = *b;
    *b = t;
}

static void bd_insertion_sort(struct bd_entry *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        struct bd_entry v = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1].key > v.key) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

static void bd_sort_entries(struct bd_entry *a, size_t n)
{
    while (n > 16) {
        /* Médiane de 3 : protège le cas déjà trié, fréquent ici (la sortie
           d'une transition est presque ordonnée quand l'entrée l'est). */
        size_t mid = n / 2;
        if (a[mid].key < a[0].key) {
            bd_entry_swap(&a[mid], &a[0]);
        }
        if (a[n - 1].key < a[0].key) {
            bd_entry_swap(&a[n - 1], &a[0]);
        }
        if (a[n - 1].key < a[mid].key) {
            bd_entry_swap(&a[n - 1], &a[mid]);
        }
        bd_key_t pivot = a[mid].key;

        size_t lt = 0, i = 0, gt = n;
        while (i < gt) {
            if (a[i].key < pivot) {
                bd_entry_swap(&a[lt++], &a[i++]);
            } else if (a[i].key > pivot) {
                bd_entry_swap(&a[i], &a[--gt]);
            } else {
                i++;
            }
        }
        if (lt < n - gt) {
            bd_sort_entries(a, lt);
            a += gt;
            n -= gt;
        } else {
            bd_sort_entries(a + gt, n - gt);
            n = lt;
        }
    }
    bd_insertion_sort(a, n);
}

/* Fusionne les clés égales ADJACENTES (donc toutes, sur un tableau trié) en
   sommant leurs nombres de façons — l'équivalent exact de l'accumulation que
   faisait `bd_level_add` en cas de collision de clé, mais par l'ordre plutôt
   que par le hachage. Retourne le nombre d'entrées distinctes. */
static size_t bd_compact_sorted(struct bd_entry *a, size_t n)
{
    if (n == 0) {
        return 0;
    }
    size_t w = 0;
    for (size_t i = 1; i < n; i++) {
        if (a[i].key == a[w].key) {
            bd_entry_set_value(&a[w], bd_count_add_or_die(bd_entry_value(&a[w]), bd_entry_value(&a[i]),
                                                           "bd_compact_sorted"));
        } else {
            a[++w] = a[i];
        }
    }
    return w + 1;
}

/* ===========================================================================
 * Lecture tamponnée d'un niveau ou d'un run — la seule façon de consommer
 * un fichier d'entrées, jamais un fread par entrée (24 octets par appel
 * système tamponné, c'est le facteur limitant d'une fusion k-voies).
 */
#define BD_READER_ENTRIES 8192

struct bd_reader {
    const struct bd_entry *ram; /* non-NULL => lecture en mémoire, pas de fichier */
    size_t ram_n;
    FILE *fp;
    struct bd_entry *buf;
    size_t buf_n;
    size_t buf_i;
    uint64_t remaining; /* entrées pas encore lues du fichier */
    char path[BD_PATH_MAX];
};

static void bd_reader_open_file(struct bd_reader *r, const char *path)
{
    memset(r, 0, sizeof *r);
    bd_path_or_die(r->path, sizeof r->path, "%s", path);
    r->fp = bd_fopen_or_die(path, "rb");
    if (fread(&r->remaining, sizeof r->remaining, 1, r->fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : en-tete de '%s' illisible — arret\n", path);
        exit(1);
    }
    r->buf = malloc(BD_READER_ENTRIES * sizeof *r->buf);
    if (r->buf == NULL) {
        fprintf(stderr, "border_ring_count_dp : tampon de lecture de '%s' impossible — arret\n", path);
        exit(1);
    }
}

static void bd_reader_open_level(struct bd_reader *r, const struct bd_level *level)
{
    if (level->entries != NULL) {
        memset(r, 0, sizeof *r);
        r->ram = level->entries;
        r->ram_n = level->count;
        return;
    }
    bd_reader_open_file(r, level->path);
}

static void bd_reader_close(struct bd_reader *r)
{
    if (r->fp != NULL) {
        fclose(r->fp);
        r->fp = NULL;
    }
    free(r->buf);
    r->buf = NULL;
}

/* Retourne l'entrée suivante, ou NULL à l'épuisement. Le pointeur rendu
   reste valide jusqu'au prochain appel (tampon réutilisé) — jamais au-delà. */
static const struct bd_entry *bd_reader_next(struct bd_reader *r)
{
    if (r->ram != NULL) {
        if (r->ram_n == 0) {
            return NULL;
        }
        r->ram_n--;
        return r->ram++;
    }
    if (r->buf_i == r->buf_n) {
        if (r->remaining == 0) {
            return NULL;
        }
        size_t want = r->remaining < BD_READER_ENTRIES ? (size_t)r->remaining : BD_READER_ENTRIES;
        if (fread(r->buf, sizeof *r->buf, want, r->fp) != want) {
            fprintf(stderr, "border_ring_count_dp : '%s' tronque — arret\n", r->path);
            exit(1);
        }
        r->remaining -= want;
        r->buf_n = want;
        r->buf_i = 0;
    }
    return &r->buf[r->buf_i++];
}

/* ===========================================================================
 * Écriture tamponnée d'un niveau, entrée par entrée mais par blocs sur le
 * disque — la sortie d'une fusion k-voies, dont on ne connaît le compte
 * qu'à la fin : l'en-tête est donc réécrit en place à la clôture.
 */
struct bd_writer {
    FILE *fp;
    struct bd_entry *buf;
    size_t n;
    uint64_t count;
    char path[BD_PATH_MAX];
};

static void bd_writer_open(struct bd_writer *w, const char *path)
{
    memset(w, 0, sizeof *w);
    bd_path_or_die(w->path, sizeof w->path, "%s", path);
    w->fp = bd_fopen_or_die(path, "wb");
    uint64_t placeholder = 0;
    bd_write_or_die(w->fp, &placeholder, sizeof placeholder, 1, path);
    w->buf = malloc(BD_READER_ENTRIES * sizeof *w->buf);
    if (w->buf == NULL) {
        fprintf(stderr, "border_ring_count_dp : tampon d'ecriture de '%s' impossible — arret\n", path);
        exit(1);
    }
}

static void bd_writer_flush(struct bd_writer *w)
{
    bd_write_or_die(w->fp, w->buf, sizeof *w->buf, w->n, w->path);
    w->n = 0;
}

static void bd_writer_push(struct bd_writer *w, const struct bd_entry *e)
{
    if (w->n == BD_READER_ENTRIES) {
        bd_writer_flush(w);
    }
    w->buf[w->n++] = *e;
    w->count++;
}

static uint64_t bd_writer_close(struct bd_writer *w)
{
    bd_writer_flush(w);
    if (fseek(w->fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "border_ring_count_dp : reecriture de l'en-tete de '%s' impossible — arret\n",
                w->path);
        exit(1);
    }
    bd_write_or_die(w->fp, &w->count, sizeof w->count, 1, w->path);
    bd_close_or_die(w->fp, w->path);
    free(w->buf);
    w->buf = NULL;
    w->fp = NULL;
    return w->count;
}

/* Ouvre une lecture bornée aux entrées [start, end) d'un niveau — la
   découpe que chaque worker forké reçoit. Sur un niveau sur disque, c'est un
   simple `fseek` : les entrées sont de taille fixe, donc l'entrée `start`
   est à un décalage calculable, jamais à chercher en lisant ce qui précède. */
static void bd_reader_open_range(struct bd_reader *r, const struct bd_level *level, size_t start, size_t end)
{
    if (end > level->count) {
        end = level->count;
    }
    if (start > end) {
        start = end;
    }
    if (level->entries != NULL) {
        memset(r, 0, sizeof *r);
        r->ram = level->entries + start;
        r->ram_n = end - start;
        return;
    }
    bd_reader_open_file(r, level->path);
    /* `fseeko`/`off_t` et non `fseek`/`long` : un niveau réel dépasse
       largement 2 Go, borne d'un `long` 32 bits. */
    off_t offset = (off_t)(sizeof(uint64_t) + start * sizeof(struct bd_entry));
    if (fseeko(r->fp, offset, SEEK_SET) != 0) {
        fprintf(stderr, "border_ring_count_dp : positionnement dans '%s' impossible — arret\n", level->path);
        exit(1);
    }
    r->remaining = (uint64_t)(end - start);
}

/* Charge un fichier trié comme niveau : en mémoire s'il tient dans le budget
   (le cas de la très grande majorité des positions), sinon laissé sur disque
   et consommé par lecture séquentielle. Le fichier n'est JAMAIS relu deux
   fois pour décider : le compte est dans son en-tête. */
static void bd_level_adopt_file(struct bd_level *level, const char *path, uint64_t count,
                                 size_t ram_limit_entries, struct bd_entry *reuse_buf, size_t reuse_cap)
{
    memset(level, 0, sizeof *level);
    level->count = (size_t)count;

    if (count <= ram_limit_entries) {
        struct bd_entry *dst = (reuse_buf != NULL && count <= reuse_cap)
                                    ? reuse_buf
                                    : malloc((size_t)count * sizeof *dst + sizeof *dst);
        if (dst == NULL) {
            fprintf(stderr,
                    "border_ring_count_dp : rechargement d'un niveau de %llu etats impossible — arret\n",
                    (unsigned long long)count);
            exit(1);
        }
        struct bd_reader r;
        bd_reader_open_file(&r, path);
        for (uint64_t i = 0; i < count; i++) {
            const struct bd_entry *e = bd_reader_next(&r);
            if (e == NULL) {
                fprintf(stderr, "border_ring_count_dp : '%s' tronque au rechargement — arret\n", path);
                exit(1);
            }
            dst[i] = *e;
        }
        bd_reader_close(&r);
        unlink(path);
        level->entries = dst;
        if (dst == reuse_buf) {
            return; /* tampon du trieur recyclé : pas de seconde allocation */
        }
        return;
    }
    bd_path_or_die(level->path, sizeof level->path, "%s", path);
}

/* Test-only : nombre de runs réellement déversés sur disque depuis la
   dernière remise à zéro — un test qui veut PROUVER que le chemin externe a
   servi doit le lire, pas se contenter d'un total juste (qu'un chemin
   tout-en-mémoire donnerait aussi). */
static long bd_runs_spilled_counter = 0;

void border_ring_dp_reset_runs_spilled_for_tests(void)
{
    bd_runs_spilled_counter = 0;
}

long border_ring_dp_get_runs_spilled_for_tests(void)
{
    return bd_runs_spilled_counter;
}

/* ===========================================================================
 * Fusion k-voies de runs triés : la seule opération qui rend un niveau plus
 * gros que la RAM manipulable SANS jamais perdre la fusion des doublons.
 *
 * Les clés égales venant de runs différents se retrouvent forcément côte à
 * côte en sortie (c'est la définition d'une fusion par ordre croissant) et
 * sont sommées à la volée — exactement ce que faisait `bd_level_add` sur une
 * collision, mais sans que les deux états aient à tenir en RAM simultanément.
 */
#define BD_MERGE_FANIN 64

/* Test-only (jamais déclaré dans border_ring_dp.h) : abaisser le degré de
   fusion force les tours de fusion intermédiaires (bd_sorter_merge_all) sur
   un fixture qui ne produirait jamais 64 runs par transition. */
static int bd_merge_fanin = BD_MERGE_FANIN;

void border_ring_dp_set_merge_fanin_for_tests(int fanin)
{
    bd_merge_fanin = fanin < 2 ? 2 : fanin;
}

struct bd_merge_node {
    struct bd_entry cur;
    int reader;
};

static void bd_heap_sift_up(struct bd_merge_node *h, int i)
{
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h[parent].cur.key <= h[i].cur.key) {
            break;
        }
        struct bd_merge_node t = h[parent];
        h[parent] = h[i];
        h[i] = t;
        i = parent;
    }
}

static void bd_heap_sift_down(struct bd_merge_node *h, int n, int i)
{
    for (;;) {
        int left = 2 * i + 1, right = left + 1, best = i;
        if (left < n && h[left].cur.key < h[best].cur.key) {
            best = left;
        }
        if (right < n && h[right].cur.key < h[best].cur.key) {
            best = right;
        }
        if (best == i) {
            return;
        }
        struct bd_merge_node t = h[best];
        h[best] = h[i];
        h[i] = t;
        i = best;
    }
}

/* Fusionne `nb` runs (au plus BD_MERGE_FANIN) dans `out_path`. Ne supprime
   pas les entrées : c'est l'appelant qui décide du sort des runs consommés. */
static uint64_t bd_merge_group(char paths[][BD_PATH_MAX], int nb, const char *out_path)
{
    /* `nb + 1` et non `nb` : `malloc(0)` a le droit de rendre NULL, ce que le
       contrôle ci-dessous prendrait pour un échec d'allocation. */
    struct bd_reader *readers = malloc((size_t)(nb + 1) * sizeof *readers);
    struct bd_merge_node *heap = malloc((size_t)(nb + 1) * sizeof *heap);
    if (readers == NULL || heap == NULL) {
        fprintf(stderr, "border_ring_count_dp : allocation de la fusion %d-voies impossible — arret\n", nb);
        exit(1);
    }

    int hn = 0;
    for (int i = 0; i < nb; i++) {
        bd_reader_open_file(&readers[i], paths[i]);
        const struct bd_entry *e = bd_reader_next(&readers[i]);
        if (e != NULL) {
            heap[hn].cur = *e;
            heap[hn].reader = i;
            hn++;
            bd_heap_sift_up(heap, hn - 1);
        }
    }

    struct bd_writer w;
    bd_writer_open(&w, out_path);

    struct bd_entry pending;
    int have_pending = 0;
    while (hn > 0) {
        struct bd_merge_node top = heap[0];
        if (have_pending && pending.key == top.cur.key) {
            bd_entry_set_value(&pending, bd_count_add_or_die(bd_entry_value(&pending),
                                                              bd_entry_value(&top.cur), "bd_merge_group"));
        } else {
            if (have_pending) {
                bd_writer_push(&w, &pending);
            }
            pending = top.cur;
            have_pending = 1;
        }

        const struct bd_entry *e = bd_reader_next(&readers[top.reader]);
        if (e != NULL) {
            heap[0].cur = *e;
            bd_heap_sift_down(heap, hn, 0);
        } else {
            heap[0] = heap[--hn];
            if (hn > 0) {
                bd_heap_sift_down(heap, hn, 0);
            }
        }
    }
    if (have_pending) {
        bd_writer_push(&w, &pending);
    }

    uint64_t count = bd_writer_close(&w);
    for (int i = 0; i < nb; i++) {
        bd_reader_close(&readers[i]);
    }
    free(readers);
    free(heap);
    return count;
}

/* ===========================================================================
 * Trieur : accumule des couples (clé, façons) sans aucune contrainte
 * d'ordre, et rend un niveau trié et dédupliqué — en RAM tant que le budget
 * le permet, sinon par runs déversés puis fusionnés.
 *
 * Le tampon n'est PAS vidé dès qu'il est plein : il est d'abord trié et
 * compacté sur place, et n'est déversé que si la compaction n'a pas libéré
 * la moitié de sa capacité. Sur cette DP, où le même état est atteint par
 * beaucoup de chemins, cette compaction préalable évite la plupart des
 * déversements — un niveau qui tiendrait « juste » dans le budget une fois
 * fusionné ne touche jamais le disque, même si le flot brut d'entrées
 * dépasse plusieurs fois la capacité du tampon.
 */
struct bd_sorter {
    struct bd_entry *buf;
    size_t cap; /* en entrées */
    size_t n;
    char (*runs)[BD_PATH_MAX];
    int nb_runs;
    int runs_cap;
    char tag[48];
};

/* Plancher de capacité : en dessous, le trieur passerait son temps à
   déverser des runs minuscules (et les fixtures de test, qui fixent des
   budgets volontairement absurdes pour exercer le chemin disque, ne
   progresseraient plus du tout). */
#define BD_SORTER_MIN_ENTRIES ((size_t)1024)

static void bd_sorter_init(struct bd_sorter *s, size_t cap_entries, const char *tag)
{
    memset(s, 0, sizeof *s);
    /* Le plancher BD_SORTER_MIN_ENTRIES est appliqué par
       `bd_sorter_entries_for`, jamais ici : les tests forcent volontairement
       des capacités d'une ou deux entrées pour exercer le chemin externe, et
       un plancher appliqué à cet endroit les ramènerait silencieusement au
       chemin tout-en-mémoire (le total resterait juste, le témoin de
       déversement resterait à zéro — exactement le genre de hook devenu sans
       effet que ce fichier a déjà connu). */
    s->cap = cap_entries < 1 ? 1 : cap_entries;
    s->buf = malloc(s->cap * sizeof *s->buf);
    if (s->buf == NULL) {
        fprintf(stderr,
                "border_ring_count_dp : allocation du tampon de tri (%zu entrees, %.2f Go) impossible — "
                "arret\n",
                s->cap, (double)s->cap * sizeof *s->buf / (1024.0 * 1024.0 * 1024.0));
        exit(1);
    }
    bd_path_or_die(s->tag, sizeof s->tag, "%s", tag);
}

static void bd_sorter_free(struct bd_sorter *s)
{
    free(s->buf);
    s->buf = NULL;
    for (int i = 0; i < s->nb_runs; i++) {
        unlink(s->runs[i]);
    }
    free(s->runs);
    s->runs = NULL;
    s->nb_runs = 0;
}

static void bd_sorter_spill(struct bd_sorter *s)
{
    if (s->nb_runs == s->runs_cap) {
        s->runs_cap = s->runs_cap == 0 ? 8 : s->runs_cap * 2;
        s->runs = realloc(s->runs, (size_t)s->runs_cap * sizeof *s->runs);
        if (s->runs == NULL) {
            fprintf(stderr, "border_ring_count_dp : allocation de la liste des runs impossible — arret\n");
            exit(1);
        }
    }
    bd_path_or_die(s->runs[s->nb_runs], BD_PATH_MAX, "%s/etii_bd_run_%d_%s_%d.bin", bd_spill_dir,
                   (int)getpid(), s->tag, s->nb_runs);
    bd_entries_write_file(s->buf, s->n, s->runs[s->nb_runs]);
    s->nb_runs++;
    s->n = 0;
    bd_runs_spilled_counter++;
}

static void bd_sorter_make_room(struct bd_sorter *s)
{
    bd_sort_entries(s->buf, s->n);
    s->n = bd_compact_sorted(s->buf, s->n);
    if (s->n * 2 > s->cap) {
        bd_sorter_spill(s);
    }
}

static inline void bd_sorter_push(struct bd_sorter *s, bd_key_t key, bd_ring_count_t value)
{
    if (s->n == s->cap) {
        bd_sorter_make_room(s);
    }
    s->buf[s->n].key = key;
    bd_entry_set_value(&s->buf[s->n], value);
    s->n++;
}

/* Réduit les runs à au plus `bd_merge_fanin` par fusions successives, puis
   fusionne le reste dans `out_path`. Sans ce palier, un budget minuscule
   (fixtures de test) pourrait produire des milliers de runs et ouvrir autant
   de descripteurs d'un coup. */
static uint64_t bd_sorter_merge_all(struct bd_sorter *s, const char *out_path)
{
    int round = 0;
    while (s->nb_runs > bd_merge_fanin) {
        int groups = 0;
        for (int i = 0; i < s->nb_runs; i += bd_merge_fanin) {
            int nb = s->nb_runs - i < bd_merge_fanin ? s->nb_runs - i : bd_merge_fanin;
            char merged[BD_PATH_MAX];
            bd_path_or_die(merged, sizeof merged, "%s/etii_bd_mrg_%d_%s_%d_%d.bin", bd_spill_dir,
                           (int)getpid(), s->tag, round, groups);
            bd_merge_group(&s->runs[i], nb, merged);
            for (int k = 0; k < nb; k++) {
                unlink(s->runs[i + k]);
            }
            bd_path_or_die(s->runs[groups], BD_PATH_MAX, "%s", merged);
            groups++;
        }
        s->nb_runs = groups;
        round++;
    }
    /* Un seul run : le renommer SUFFIT — il est déjà trié et dédupliqué.
       Sans ce raccourci, le cas le plus fréquent du chemin externe (le
       tampon a débordé une fois, puis la compaction a suffi) recopierait
       intégralement un niveau de plusieurs dizaines de Go pour rien. */
    if (s->nb_runs == 1 && rename(s->runs[0], out_path) == 0) {
        s->nb_runs = 0;
        return bd_file_count(out_path);
    }
    uint64_t count = bd_merge_group(s->runs, s->nb_runs, out_path);
    for (int i = 0; i < s->nb_runs; i++) {
        unlink(s->runs[i]);
    }
    s->nb_runs = 0;
    return count;
}

/* Clôt le trieur sur un NIVEAU : en mémoire si rien n'a jamais été déversé
   (le cas de tous les petits niveaux et de toutes les fixtures de test),
   sinon un fichier trié — rechargé en mémoire si, une fois les doublons
   fusionnés, il y tient finalement (fréquent : c'est le flot BRUT qui
   dépasse le budget, pas toujours le niveau fusionné). */
static void bd_sorter_finish_level(struct bd_sorter *s, struct bd_level *level, const char *out_path)
{
    memset(level, 0, sizeof *level);

    if (s->nb_runs == 0) {
        bd_sort_entries(s->buf, s->n);
        s->n = bd_compact_sorted(s->buf, s->n);
        /* Rendre au système ce que la compaction a libéré : sans ce
           `realloc`, le niveau garderait son tampon à sa capacité PLEINE
           pendant que la transition suivante en alloue un second aussi
           grand — deux fois le budget demandé, au lieu d'une. */
        struct bd_entry *shrunk = realloc(s->buf, (s->n + 1) * sizeof *s->buf);
        level->entries = shrunk != NULL ? shrunk : s->buf;
        level->count = s->n;
        s->buf = NULL; /* propriété transférée au niveau */
        bd_sorter_free(s);
        return;
    }

    if (s->n > 0) {
        bd_sort_entries(s->buf, s->n);
        s->n = bd_compact_sorted(s->buf, s->n);
        bd_sorter_spill(s);
    }
    uint64_t count = bd_sorter_merge_all(s, out_path);

    /* Le tampon de tri, désormais inutile, sert de destination au
       rechargement quand le niveau fusionné tient finalement dedans (cas
       fréquent : c'est le flot BRUT qui déborde, pas toujours le niveau une
       fois les doublons fusionnés) — jamais une seconde allocation de la
       même taille à côté de la première. */
    struct bd_entry *reuse = s->buf;
    size_t cap = s->cap;
    s->buf = NULL;
    bd_level_adopt_file(level, out_path, count, cap, reuse, cap);
    if (level->entries == reuse) {
        /* Même raison que sur le chemin tout-en-mémoire : rendre la part du
           tampon que le niveau n'occupe pas, sinon la transition suivante
           allouerait son propre tampon PAR-DESSUS celui-ci. */
        struct bd_entry *shrunk = realloc(reuse, (level->count + 1) * sizeof *reuse);
        if (shrunk != NULL) {
            level->entries = shrunk;
        }
    } else {
        free(reuse);
    }
    bd_sorter_free(s);
}

/* Clôt le trieur sur un FICHIER trié, toujours — la forme que rend un worker
   forké à son parent (cf. bd_transition_parallel) : le parent n'a plus qu'à
   fusionner les `nb_workers` fichiers, une opération linéaire et séquentielle,
   là où la version précédente lui faisait RÉINSÉRER une à une, dans une table
   de hachage, la totalité de la production de ses workers — soit autant de
   travail que la transition entière, donc un speedup borné à 2 quel que soit
   le nombre de workers. */
static void bd_sorter_finish_file(struct bd_sorter *s, const char *out_path)
{
    if (s->n > 0 || s->nb_runs == 0) {
        bd_sort_entries(s->buf, s->n);
        s->n = bd_compact_sorted(s->buf, s->n);
        bd_sorter_spill(s);
    }
    bd_sorter_merge_all(s, out_path);
    bd_sorter_free(s);
}

/* ===========================================================================
 * Construction des classes — INCHANGÉ depuis la version précédente.
 */

/* Construit les classes d'équivalence à partir des pièces réelles (rotation
 * 0 = pièce d'origine, cf. rotate_all_parts).
 *
 * ATTENTION — piège corrigé après une mesure qui donnait 128 au lieu de 4
 * sur `data/pieces16.csv` (32x trop) : la classe n'est PAS la paire NON
 * ORDONNÉE des deux faces "anneau" d'une pièce. Chaque pièce réelle a une
 * orientation FIXE, intrinsèque à sa géométrie — DEUX pièces qui partagent
 * les deux mêmes couleurs mais dans l'ordre inverse (l'une présente A en
 * entrée et B en sortie, l'autre B en entrée et A en sortie) ne sont PAS
 * interchangeables : traiter les deux orientations comme une seule classe
 * fait utiliser CHAQUE pièce dans les deux sens, alors qu'une pièce donnée
 * ne peut physiquement en fournir qu'un seul. La classe est donc la paire
 * ORDONNÉE (couleur requise en entrée, couleur produite en sortie).
 *
 * Cet ordre se lit directement sur le cycle des faces TOP(0)->RIGHT(1)->
 * BOTTOM(2)->LEFT(3)->TOP, dans le sens où tourne `rotatePart` (part.c) :
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
            if (nb_classes == BD_MAX_CLASSES) {
                fprintf(stderr, "border_ring_count_dp : plus de %d classes de bordure — arret\n",
                        BD_MAX_CLASSES);
                exit(1);
            }
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
 * Transition d'un niveau vers le suivant — cœur de la DP.
 *
 * Un état du niveau `pos` produit un successeur par classe encore disponible
 * dont la couleur d'entrée correspond à celle qu'il exige et dont la forme
 * (coin/bord) correspond à celle de la case `pos` : chaque successeur est
 * simplement POUSSÉ dans le trieur, pondéré par le nombre de pièces encore
 * disponibles dans la classe choisie (c'est ce facteur, et lui seul, qui
 * transforme un compte de suites de CLASSES en compte d'anneaux de pièces
 * RÉELLES). Aucune fusion n'a lieu ici : elle est entièrement déléguée au
 * tri, et c'est exactement ce qui permet à un niveau de dépasser la RAM sans
 * jamais perdre un doublon en route.
 */
static void bd_transition_range(const struct bd_ctx *ctx, int pos, const struct bd_level *cur, size_t start,
                                 size_t end, struct bd_sorter *out)
{
    const struct bd_key_layout *l = &ctx->layout;
    int shape = ctx->is_corner_at[pos] ? 1 : 0;

    struct bd_reader r;
    bd_reader_open_range(&r, cur, start, end);
    const struct bd_entry *e;
    while ((e = bd_reader_next(&r)) != NULL) {
        bd_key_t key = e->key;
        int color = (int)(uint8_t)bd_key_color(l, key);
        bd_ring_count_t ways = bd_entry_value(e);
        const int8_t *cand = ctx->by_color[color][shape];
        int nb = ctx->by_color_n[color][shape];
        for (int i = 0; i < nb; i++) {
            int c = cand[i];
            uint64_t avail = bd_key_count(l, key, c);
            if (avail == 0) {
                continue;
            }
            bd_sorter_push(out, bd_key_step(l, key, c, ctx->classes[c].color_b),
                           bd_count_mul_or_die(ways, (bd_ring_count_t)avail, "bd_transition_range"));
        }
    }
    bd_reader_close(&r);
}

/* Variante terminale (dernière case de l'anneau) : au lieu de produire un
   niveau suivant, cumule directement dans le total dès que la couleur
   produite ferme l'anneau sur la pièce d'ouverture. */
static bd_ring_count_t bd_finalize(const struct bd_ctx *ctx, int pos, int8_t closure_target,
                                    const struct bd_level *cur)
{
    const struct bd_key_layout *l = &ctx->layout;
    int shape = ctx->is_corner_at[pos] ? 1 : 0;
    bd_ring_count_t total = 0;

    struct bd_reader r;
    bd_reader_open_level(&r, cur);
    const struct bd_entry *e;
    while ((e = bd_reader_next(&r)) != NULL) {
        bd_key_t key = e->key;
        int color = (int)(uint8_t)bd_key_color(l, key);
        bd_ring_count_t ways = bd_entry_value(e);
        const int8_t *cand = ctx->by_color[color][shape];
        int nb = ctx->by_color_n[color][shape];
        for (int i = 0; i < nb; i++) {
            int c = cand[i];
            if (ctx->classes[c].color_b != closure_target) {
                continue;
            }
            uint64_t avail = bd_key_count(l, key, c);
            if (avail == 0) {
                continue;
            }
            total = bd_count_add_or_die(
                total, bd_count_mul_or_die(ways, (bd_ring_count_t)avail, "bd_finalize"), "bd_finalize");
        }
    }
    bd_reader_close(&r);
    return total;
}

/* ===========================================================================
 * Budget mémoire — UN seul nombre, celui que l'utilisateur donne.
 *
 * La version précédente dérivait de `--dp-max-ram-mo` quatre seuils
 * différents (`/4` en mode SOLO, `/(workers*2)` par job du pool,
 * `/(workers*3)` par fragment, plus un plafond d'admission) : avec
 * `--dp-max-ram-mo 35000 --forks 10`, un job ne pouvait tenir que 1,75 Go —
 * 5 % de ce qui avait été demandé. Et comme le seuil de SCISSION était ce
 * budget par job, ajouter des workers fragmentait la DP plus tôt, donc
 * dupliquait davantage de travail : la parallélisation créait elle-même le
 * problème qu'elle était censée résoudre.
 *
 * Ici le budget est le budget : il dimensionne le tampon de tri, et rien
 * d'autre n'en dépend. Le dépasser n'a plus de conséquence algorithmique —
 * ça ajoute un run trié à fusionner, jamais un fragment qui cesserait de
 * fusionner ses doublons avec les autres.
 */
static double bd_max_ram_bytes = 1024.0 * 1024.0 * 1024.0;

void border_ring_dp_set_max_ram_mo(long mo)
{
    bd_max_ram_bytes = (double)mo * 1024.0 * 1024.0;
}

/* Test-only : force la capacité du tampon de tri, plancher compris — seul
   moyen de faire déborder le trieur sur des fixtures minuscules et
   d'exercer réellement runs + fusion k-voies + niveau résident sur disque,
   pas seulement le chemin tout-en-mémoire. Jamais déclarée dans
   border_ring_dp.h (même schéma que stock_spill_set_segment_bytes_for_tests). */
static size_t bd_forced_sorter_entries = 0;

void border_ring_dp_set_sorter_capacity_for_tests(size_t entries)
{
    bd_forced_sorter_entries = entries;
}

/* Nombre d'entrées qu'un niveau peut compter tout en restant résident : le
   budget, ou la capacité forcée par les tests (qui doit primer, sinon le
   chemin « niveau resté sur disque » ne serait jamais exercé sur des
   fixtures minuscules). */
static size_t bd_ram_limit_entries(void)
{
    if (bd_forced_sorter_entries > 0) {
        return bd_forced_sorter_entries;
    }
    return (size_t)(bd_max_ram_bytes / (double)sizeof(struct bd_entry));
}

static size_t bd_level_ram_entries(const struct bd_level *level)
{
    return level->entries != NULL ? level->count : 0;
}

/* Capacité du tampon de tri pour UNE transition : tout le budget moins ce
   que le niveau courant occupe déjà en RAM (il est lu pendant toute la
   transition, donc les deux coexistent forcément) — et divisé entre les
   workers quand la transition est forkée, puisqu'ils tournent en même temps.
   Le niveau courant, lui, n'est pas dupliqué par le fork : les workers ne
   font que le LIRE, donc il reste partagé en copie-sur-écriture. */
static size_t bd_sorter_entries_for(const struct bd_level *cur, int nb_workers)
{
    if (bd_forced_sorter_entries > 0) {
        return bd_forced_sorter_entries;
    }
    double used = (double)bd_level_ram_entries(cur) * (double)sizeof(struct bd_entry);
    double avail = bd_max_ram_bytes - used;
    if (avail < 0.0) {
        avail = 0.0;
    }
    double per_worker = avail / (double)(nb_workers < 1 ? 1 : nb_workers) / (double)sizeof(struct bd_entry);
    if (per_worker < (double)BD_SORTER_MIN_ENTRIES) {
        return BD_SORTER_MIN_ENTRIES;
    }
    return (size_t)per_worker;
}

/* Seuil de fork d'une transition : en deçà, le coût d'un fork + d'une fusion
   de fichiers dépasse le gain. Ajustable pour les tests (même schéma que
   border_ring_dp_set_sorter_capacity_for_tests). */
static size_t bd_fork_min_states = 50000;

void border_ring_dp_set_fork_min_states_for_tests(size_t n)
{
    bd_fork_min_states = n;
}

static void bd_mkdir_or_die(const char *dir)
{
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "border_ring_count_dp : creation du repertoire '%s' impossible — arret\n", dir);
        exit(1);
    }
}

/* ===========================================================================
 * Parallélisation par forks : chaque worker trie SA tranche du niveau
 * courant et rend UN fichier trié ; le parent les fusionne.
 *
 * C'est le même découpage que la version précédente (des plages disjointes
 * d'un niveau immuable), mais la remise au parent change de nature : il
 * fusionne des flots DÉJÀ TRIÉS, en une passe linéaire, au lieu de
 * réinsérer une à une dans une table de hachage la totalité de ce que ses
 * workers ont produit. L'ancienne remise coûtait, à elle seule, autant que
 * la transition séquentielle entière — la partie série d'Amdahl valait donc
 * ~100 % du travail utile et le speedup était borné à ~2, quel que soit
 * `--forks`. Avec des flots triés, la partie série retombe à une fusion
 * k-voies : bornée par les E/S, pas par le calcul.
 *
 * Fork et non threads, comme partout ailleurs dans ce projet : isolation
 * mémoire et de panne, aucune synchronisation partagée à auditer. Le tri
 * externe rend ce choix gratuit, là où il coûtait cher auparavant.
 */
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

static void bd_transition_parallel(const struct bd_ctx *ctx, int pos, const struct bd_level *cur,
                                    int nb_workers, const char *out_path, struct bd_level *next)
{
    char (*parts)[BD_PATH_MAX] = malloc((size_t)nb_workers * sizeof *parts);
    pid_t *pids = malloc((size_t)nb_workers * sizeof *pids);
    if (parts == NULL || pids == NULL) {
        fprintf(stderr, "border_ring_count_dp : allocation des %d workers impossible — arret\n", nb_workers);
        exit(1);
    }
    size_t chunk = (cur->count + (size_t)nb_workers - 1) / (size_t)nb_workers;
    size_t cap = bd_sorter_entries_for(cur, nb_workers);

    /* Vider les tampons stdio AVANT fork() : sinon chaque enfant hérite d'une
       copie du contenu déjà écrit (mais pas encore vidé) et le revide
       indépendamment à son propre exit(), dupliquant ces lignes une fois par
       worker, à CHAQUE niveau forké. */
    fflush(stdout);
    fflush(stderr);

    for (int w = 0; w < nb_workers; w++) {
        bd_path_or_die(parts[w], BD_PATH_MAX, "%s/etii_bd_part_%d_%d_%d.bin", bd_spill_dir, (int)getpid(), pos,
                       w);
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_ring_count_dp : fork() a echoue pour le worker %d — arret\n", w);
            bd_abort_workers(pids, w);
            exit(1);
        }
        if (pid == 0) {
            size_t start = (size_t)w * chunk;
            struct bd_sorter s;
            char tag[48];
            bd_path_or_die(tag, sizeof tag, "w%d", w);
            bd_sorter_init(&s, cap, tag);
            bd_transition_range(ctx, pos, cur, start, start + chunk, &s);
            bd_sorter_finish_file(&s, parts[w]);
            /* exit() et jamais _exit() : un _exit() dans un fils saute le
               vidage gcov/llvm-cov (piège macOS documenté dans AGENTS.md). */
            exit(0);
        }
        pids[w] = pid;
    }

    int failed = 0;
    for (int w = 0; w < nb_workers; w++) {
        int status = 0;
        waitpid(pids[w], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : le worker %d a echoue (status=%d) — arret\n", w, status);
            failed = 1;
        }
    }
    if (failed) {
        for (int w = 0; w < nb_workers; w++) {
            unlink(parts[w]);
        }
        exit(1);
    }

    uint64_t count = bd_merge_group(parts, nb_workers, out_path);
    for (int w = 0; w < nb_workers; w++) {
        unlink(parts[w]);
    }
    free(parts);
    free(pids);

    bd_level_adopt_file(next, out_path, count, bd_sorter_entries_for(cur, 1), NULL, 0);
}

/* ===========================================================================
 * Persistance d'un niveau (reconstruction uniquement — `--save-rings`).
 * Une COPIE, jamais un renommage : le niveau reste vivant et sera libéré
 * normalement (donc son propre fichier supprimé) une fois la position
 * suivante construite.
 */
static void bd_level_persist(const char *persist_dir, int pos, const struct bd_level *level)
{
    char path[BD_PATH_MAX];
    bd_path_or_die(path, sizeof path, "%s/level_%d.bin", persist_dir, pos);
    if (level->entries != NULL) {
        bd_entries_write_file(level->entries, level->count, path);
        return;
    }
    struct bd_reader r;
    struct bd_writer w;
    bd_reader_open_level(&r, level);
    bd_writer_open(&w, path);
    const struct bd_entry *e;
    while ((e = bd_reader_next(&r)) != NULL) {
        bd_writer_push(&w, e);
    }
    bd_reader_close(&r);
    bd_writer_close(&w);
}

static void bd_level_load_persisted(const char *persist_dir, int pos, struct bd_level *level)
{
    char path[BD_PATH_MAX];
    bd_path_or_die(path, sizeof path, "%s/level_%d.bin", persist_dir, pos);
    uint64_t count = bd_file_count(path);
    /* Chargé en RAM tant que le budget le permet, laissé sur disque sinon —
       `bd_level_lookup` interroge les deux formes indifféremment. Le fichier
       persisté n'est PAS consommé ici (contrairement à bd_level_adopt_file
       sur un fichier temporaire) : il sert encore à la position suivante. */
    struct bd_level tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.count = (size_t)count;
    if (count <= bd_ram_limit_entries()) {
        struct bd_reader r;
        bd_reader_open_file(&r, path);
        tmp.entries = malloc((size_t)count * sizeof *tmp.entries + sizeof *tmp.entries);
        if (tmp.entries == NULL) {
            fprintf(stderr, "border_ring_count_dp : chargement de '%s' impossible — arret\n", path);
            exit(1);
        }
        for (uint64_t i = 0; i < count; i++) {
            const struct bd_entry *e = bd_reader_next(&r);
            if (e == NULL) {
                fprintf(stderr, "border_ring_count_dp : '%s' tronque — arret\n", path);
                exit(1);
            }
            tmp.entries[i] = *e;
        }
        bd_reader_close(&r);
    } else {
        bd_path_or_die(tmp.path, sizeof tmp.path, "%s", path);
    }
    *level = tmp;
}

/* Recherche dichotomique d'une clé — la contrepartie directe du lookup par
   hachage de la version précédente, valable telle quelle sur un niveau
   résident en mémoire comme sur un niveau resté sur disque (les entrées sont
   de taille fixe, donc la i-ème est à un décalage calculable). */
static int bd_level_lookup(const struct bd_level *level, bd_key_t key, bd_ring_count_t *out_value)
{
    size_t lo = 0, hi = level->count;
    FILE *fp = NULL;
    if (level->entries == NULL) {
        if (level->count == 0) {
            return 0;
        }
        fp = bd_fopen_or_die(level->path, "rb");
    }
    int found = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        struct bd_entry probe;
        if (fp != NULL) {
            off_t offset = (off_t)(sizeof(uint64_t) + mid * sizeof(struct bd_entry));
            if (fseeko(fp, offset, SEEK_SET) != 0 || fread(&probe, sizeof probe, 1, fp) != 1) {
                fprintf(stderr, "border_ring_count_dp : lecture dichotomique de '%s' impossible — arret\n",
                        level->path);
                exit(1);
            }
        } else {
            probe = level->entries[mid];
        }
        if (probe.key == key) {
            *out_value = bd_entry_value(&probe);
            found = 1;
            break;
        }
        if (probe.key < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (fp != NULL) {
        fclose(fp);
    }
    return found;
}

/* ===========================================================================
 * Orchestration : une boucle, une position après l'autre, du premier niveau
 * à la fermeture de l'anneau.
 *
 * C'est tout. Plus de pile LIFO de fragments, plus de coordinateur à deux
 * modes SOLO/POOL, plus de pause mi-transition, plus de scission : un niveau
 * trop gros pour la RAM devient un fichier trié, que la position suivante lit
 * séquentiellement — et reste un niveau UNIQUE et ENTIÈREMENT FUSIONNÉ, ce
 * qu'aucun découpage en fragments indépendants ne pouvait garantir.
 */
static bd_ring_count_t bd_run_opening(const struct bd_ctx *ctx, int8_t initial_required,
                                       const int8_t *initial_counts, int8_t closure_target, int nb_workers,
                                       const char *persist_dir)
{
    bd_check_color_or_die((int)initial_required, "couleur requise a l'ouverture");
    bd_check_color_or_die((int)closure_target, "couleur de fermeture");

    struct bd_level cur;
    memset(&cur, 0, sizeof cur);
    cur.entries = malloc(sizeof *cur.entries);
    if (cur.entries == NULL) {
        fprintf(stderr, "border_ring_count_dp : allocation du niveau initial impossible — arret\n");
        exit(1);
    }
    cur.entries[0].key = bd_key_make(&ctx->layout, initial_required, initial_counts, ctx->nb_classes);
    bd_entry_set_value(&cur.entries[0], 1);
    cur.count = 1;

    int pos = 1;
    if (persist_dir != NULL) {
        bd_level_persist(persist_dir, pos, &cur);
    }

    while (pos < BORDER_RING_LEN - 1) {
        char out_path[BD_PATH_MAX];
        bd_path_or_die(out_path, sizeof out_path, "%s/etii_bd_level_%d_%d.bin", bd_spill_dir, (int)getpid(),
                       pos + 1);

        struct bd_level next;
        if (nb_workers > 1 && cur.count >= bd_fork_min_states) {
            bd_transition_parallel(ctx, pos, &cur, nb_workers, out_path, &next);
        } else {
            struct bd_sorter s;
            bd_sorter_init(&s, bd_sorter_entries_for(&cur, 1), "seq");
            bd_transition_range(ctx, pos, &cur, 0, cur.count, &s);
            bd_sorter_finish_level(&s, &next, out_path);
        }

        bd_level_free(&cur);
        cur = next;
        pos++;

        fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go, %s)\n", pos,
                BORDER_RING_LEN - 1, cur.count, bd_level_bytes(&cur) / (1024.0 * 1024.0 * 1024.0),
                cur.entries != NULL ? "en RAM" : "sur disque");

        if (persist_dir != NULL) {
            bd_level_persist(persist_dir, pos, &cur);
        }
    }

    bd_ring_count_t total = bd_finalize(ctx, BORDER_RING_LEN - 1, closure_target, &cur);
    bd_level_free(&cur);
    return total;
}

static void bd_ctx_init(struct bd_ctx *ctx, struct array_part *all_rotate_parts, int counts[BD_MAX_CLASSES],
                         int16_t *id_to_class, int n)
{
    ctx->nb_classes = bd_build_classes(all_rotate_parts, ctx, counts, id_to_class, n);
    bd_key_layout_init(&ctx->layout, counts, ctx->nb_classes);
    bd_index_classes_by_color(ctx);

    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(order);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int x = order[i][0], y = order[i][1];
        ctx->is_corner_at[i] =
            (int8_t)((x == 0 || x == ETERN_SIZE - 1) && (y == 0 || y == ETERN_SIZE - 1));
    }
}

/* Boucle d'ouverture à `(0,0)` : réutilise exactement le même mécanisme que
   `border_walk_count` (what_search_in_grid_to_key + la map de lookup) pour
   énumérer les candidats réels, un par un — jamais regroupés en classe ici.
   Contrairement aux cases suivantes (où `required_color` disambigüe toujours
   sans ambiguïté quelle des deux couleurs de la classe sert d'entrée et
   laquelle sert de sortie), rien ne force qu'une paire de pièces-coin
   partageant la même classe présente sa fermeture (vers la dernière case) et
   sa sortie (vers la case 1) dans le même ordre — c'est une propriété de la
   pièce RÉELLE (son orientation d'origine dans le CSV), pas de la classe
   abstraite. Regrouper ici risquerait donc de mélanger deux fermetures
   différentes sous un seul poids.
 *
 * N'exécute qu'UNE SEULE fois `bd_run_opening` (sur le premier candidat
 * trouvé), puis multiplie par `nb_candidates` au lieu de sommer un
 * `bd_run_opening` complet par candidat : par symétrie de rotation à 90° du
 * plateau (cf. docs/conception/border_mass.md), toute bordure valide utilise
 * nécessairement les `nb_candidates` pièces-coin réelles disponibles, une
 * fois chacune — il y a exactement autant de positions-coin sur l'anneau que
 * de pièces-coin réelles, aucune substitution possible. Faire tourner le
 * plateau entier de 90° envoie donc toute solution comptée pour un candidat
 * donné vers une solution tout aussi valide comptée pour le candidat suivant
 * (même pièce, coin suivant, réorientée) — les `nb_candidates` totaux par
 * candidat sont donc rigoureusement égaux, et pas seulement dans le cas où
 * l'ouverture ne change rien à la forme du reste de l'anneau. Vérifié
 * empiriquement avant ce changement (instrumentation temporaire, jamais
 * committée) : les 4 candidats de `data/pieces16.csv` donnent chacun N=1 ;
 * `data/pieces.csv` n'a que 4 pièces à 2 faces à 0 (ids 1-4), sans couleurs
 * partagées entre elles, donc la prémisse (chaque anneau valide utilise les
 * 4, une fois chacune) tient aussi sur le jeu réel. Ce court-circuit
 * économise ~(nb_candidates-1) fois le coût du DP complet — le poste
 * dominant du temps de calcul. */
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
    if (ctx == NULL || id_to_class == NULL) {
        fprintf(stderr, "border_ring_count_dp : allocation du contexte impossible — arret\n");
        exit(1);
    }
    bd_ctx_init(ctx, all_rotate_parts, base_counts, id_to_class, n);

    if (nb_workers < 1) {
        nb_workers = 1;
    }
    bd_ring_count_t total = bd_count_openings(map, all_rotate_parts, ctx, id_to_class, base_counts, nb_workers);

    free(id_to_class);
    free(ctx);
    return total;
}

/* ===========================================================================
 * Reconstruction des anneaux réels (cf. le commentaire de tête de
 * border_ring_dp.h).
 *
 * Principe : la DP normale ne conserve rien d'exploitable une fois le total
 * calculé (chaque niveau est jeté dès le suivant construit). Pour
 * reconstruire, la même DP est rejouée avec `persist_dir` non NULL — chaque
 * niveau est alors COPIÉ sous `persist_dir/level_<pos>.bin` plutôt que jeté —
 * puis une passe arrière calcule, pour chaque état réellement atteint, le
 * nombre de façons de COMPLÉTER l'anneau à partir de là. Cette table sert
 * d'oracle d'élagage à un DFS guidé sur l'alphabet des CLASSES (jamais sur
 * les pièces réelles) : à chaque position, seules les classes dont l'état
 * résultant a une complétion non nulle sont essayées, donc toute branche
 * explorée mène forcément à une fermeture valide. Une suite de classes
 * complète est ensuite développée en TOUTES les assignations de pièces
 * réelles possibles (bd_expand_dfs).
 */

/* Calcule, pour CHAQUE état du niveau AVANT persisté à la position `pos`, le
 * nombre de façons de COMPLÉTER l'anneau jusqu'à la fermeture.
 * `completion_next` est la table déjà calculée pour `pos + 1` (NULL seulement
 * quand `pos == BORDER_RING_LEN - 1`, où la fermeture se vérifie directement
 * contre `closure_target`). ATTENTION à ne jamais confondre ceci avec une DP
 * miroir indépendante (tentative initiale, incorrecte) : la complétion d'un
 * état DOIT être calculée à partir des états RÉELLEMENT atteints par la passe
 * avant à cette position.
 *
 * Les états sont parcourus dans l'ordre du niveau avant, donc dans l'ordre
 * croissant des clés : la table produite est triée par construction, sans
 * aucun tri supplémentaire, et `bd_level_lookup` peut l'interroger par
 * dichotomie telle quelle. */
static void bd_completion_step(const struct bd_ctx *ctx, int pos, int8_t closure_target,
                                const struct bd_level *forward_level, const struct bd_level *completion_next,
                                const char *out_path)
{
    const struct bd_key_layout *l = &ctx->layout;
    int shape = ctx->is_corner_at[pos] ? 1 : 0;

    struct bd_reader r;
    struct bd_writer w;
    bd_reader_open_level(&r, forward_level);
    bd_writer_open(&w, out_path);

    const struct bd_entry *e;
    while ((e = bd_reader_next(&r)) != NULL) {
        bd_key_t key = e->key;
        int color = (int)(uint8_t)bd_key_color(l, key);
        const int8_t *cand = ctx->by_color[color][shape];
        int nb = ctx->by_color_n[color][shape];

        bd_ring_count_t comp = 0;
        for (int i = 0; i < nb; i++) {
            int c = cand[i];
            uint64_t avail = bd_key_count(l, key, c);
            if (avail == 0) {
                continue;
            }
            if (pos == BORDER_RING_LEN - 1) {
                if (ctx->classes[c].color_b == closure_target) {
                    comp = bd_count_add_or_die(comp, (bd_ring_count_t)avail, "bd_completion_step");
                }
                continue;
            }
            bd_ring_count_t next_comp;
            if (bd_level_lookup(completion_next, bd_key_step(l, key, c, ctx->classes[c].color_b),
                                 &next_comp)) {
                comp = bd_count_add_or_die(
                    comp, bd_count_mul_or_die(next_comp, (bd_ring_count_t)avail, "bd_completion_step"),
                    "bd_completion_step");
            }
        }
        if (comp > 0) {
            struct bd_entry out = { .key = key, .val_lo = 0, .val_hi = 0 };
            bd_entry_set_value(&out, comp);
            bd_writer_push(&w, &out);
        }
    }
    bd_reader_close(&r);
    bd_writer_close(&w);
}

static void bd_level_load_path(const char *path, struct bd_level *level)
{
    uint64_t count = bd_file_count(path);
    memset(level, 0, sizeof *level);
    level->count = (size_t)count;
    if (count <= bd_ram_limit_entries()) {
        struct bd_reader r;
        bd_reader_open_file(&r, path);
        level->entries = malloc((size_t)count * sizeof *level->entries + sizeof *level->entries);
        if (level->entries == NULL) {
            fprintf(stderr, "border_ring_count_dp : chargement de '%s' impossible — arret\n", path);
            exit(1);
        }
        for (uint64_t i = 0; i < count; i++) {
            const struct bd_entry *e = bd_reader_next(&r);
            if (e == NULL) {
                fprintf(stderr, "border_ring_count_dp : '%s' tronque — arret\n", path);
                exit(1);
            }
            level->entries[i] = *e;
        }
        bd_reader_close(&r);
    } else {
        bd_path_or_die(level->path, sizeof level->path, "%s", path);
    }
}

/* Un niveau chargé depuis un fichier PERSISTÉ ne possède pas ce fichier : le
   libérer ne doit jamais le supprimer (bd_level_free le ferait). */
static void bd_level_release_borrowed(struct bd_level *level)
{
    free(level->entries);
    memset(level, 0, sizeof *level);
}

/* Balaie les niveaux AVANT persistés (positions BORDER_RING_LEN-1 downto 1)
 * et écrit, pour chacun, sa table de complétion sous
 * `persist_dir/completion_<pos>.bin`. */
static void bd_build_and_persist_completions(const struct bd_ctx *ctx, int8_t closure_target,
                                              const char *persist_dir)
{
    struct bd_level completion_next;
    memset(&completion_next, 0, sizeof completion_next);
    int have_next = 0;

    for (int pos = BORDER_RING_LEN - 1; pos >= 1; pos--) {
        struct bd_level forward_level;
        bd_level_load_persisted(persist_dir, pos, &forward_level);

        char path[BD_PATH_MAX];
        bd_path_or_die(path, sizeof path, "%s/completion_%d.bin", persist_dir, pos);
        bd_completion_step(ctx, pos, closure_target, &forward_level, have_next ? &completion_next : NULL,
                            path);
        bd_level_release_borrowed(&forward_level);
        if (have_next) {
            bd_level_release_borrowed(&completion_next);
        }

        bd_level_load_path(path, &completion_next);
        have_next = 1;
    }
    if (have_next) {
        bd_level_release_borrowed(&completion_next);
    }
}

struct bd_reconstruct_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    const struct bd_ctx *ctx;      /* contexte direct (classes + is_corner_at) */
    const int16_t *id_to_class;
    const char *back_dir;          /* niveaux de complétion persistés */
    int8_t closure_target;         /* couleur de fermeture (passe avant) */
    int8_t class_seq[BORDER_RING_LEN]; /* indices 1..BORDER_RING_LEN-1 utilisés */
    border_ring_found_cb on_found;
    void *user_ctx;
    long long max_rings;
    long long delivered;
    int cached_pos;                /* position dont la table de complétion est en cache, -1 = aucune */
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

/* DFS guidé sur l'alphabet des CLASSES — élagué par la table de complétion
 * persistée : à la position `pos`, une classe candidate n'est essayée que si
 * l'état résultant a une complétion non nulle connue à la position `pos + 1`
 * (chargée à la demande, gardée en cache tant qu'on reste à la même position
 * — la descente en profondeur d'abord ne change la position courante que de
 * ±1 à la fois). Position `BORDER_RING_LEN - 1` (dernière) : pas de niveau
 * suivant à consulter, la fermeture se vérifie directement contre
 * `closure_target`, comme `bd_finalize`. */
/* Charge la table de complétion de `want_pos` si ce n'est pas déjà celle en
 * cache. À appeler avant CHAQUE lookup, jamais une seule fois en entrée de
 * frame : une récursion descend d'un cran et remplace le cache par le sien,
 * donc au retour la frame appelante n'a plus la table de SA position. La
 * version précédente ne rechargeait qu'en entrée de fonction et interrogeait
 * ensuite, pour tous les candidats suivants de sa boucle, la table laissée
 * par sa descendance — un lookup qui échoue élague alors une branche
 * pourtant valide, et la reconstruction se termine en « reconstruction
 * incomplete » (le garde-fou final de border_ring_reconstruct_dp) plutôt
 * qu'en résultat faux silencieux. Invisible sur les fixtures existantes,
 * d'où sa survie jusqu'ici. */
static void bd_reconstruct_ensure_cache(struct bd_reconstruct_ctx *rc, int want_pos)
{
    if (rc->cached_pos == want_pos) {
        return;
    }
    if (rc->cached_pos != -1) {
        bd_level_release_borrowed(&rc->cached_level);
    }
    char path[BD_PATH_MAX];
    bd_path_or_die(path, sizeof path, "%s/completion_%d.bin", rc->back_dir, want_pos);
    bd_level_load_path(path, &rc->cached_level);
    rc->cached_pos = want_pos;
}

static void bd_reconstruct_class_dfs(struct bd_reconstruct_ctx *rc, int pos, bd_key_t key)
{
    if (rc->delivered >= rc->max_rings) {
        return;
    }
    const struct bd_key_layout *l = &rc->ctx->layout;
    int shape = rc->ctx->is_corner_at[pos] ? 1 : 0;
    int color = (int)(uint8_t)bd_key_color(l, key);
    const int8_t *cand = rc->ctx->by_color[color][shape];
    int nb = rc->ctx->by_color_n[color][shape];

    if (pos == BORDER_RING_LEN - 1) {
        for (int i = 0; i < nb; i++) {
            int c = cand[i];
            if (rc->ctx->classes[c].color_b != rc->closure_target || bd_key_count(l, key, c) == 0) {
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

    for (int i = 0; i < nb; i++) {
        int c = cand[i];
        if (bd_key_count(l, key, c) == 0) {
            continue;
        }
        bd_key_t next_key = bd_key_step(l, key, c, rc->ctx->classes[c].color_b);
        bd_reconstruct_ensure_cache(rc, pos + 1);
        bd_ring_count_t completion;
        if (!bd_level_lookup(&rc->cached_level, next_key, &completion)) {
            continue;
        }

        rc->class_seq[pos] = (int8_t)c;
        bd_reconstruct_class_dfs(rc, pos + 1, next_key);

        if (rc->delivered >= rc->max_rings) {
            return;
        }
    }
}

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
        char path[BD_PATH_MAX + 256];
        snprintf(path, sizeof path, "%s/%s", persist_dir, ent->d_name);
        unlink(path);
    }
    closedir(base);
    rmdir(persist_dir);
}

/* Reconstruit et délivre, via on_found, chaque anneau de bordure réel
 * (jusqu'à max_rings). Contrairement à bd_count_openings, énumère
 * explicitement les nb_candidates pièces-coin réelles (pas de raccourci ×4 :
 * reconstruire une rotation géométrique du paquet serait un risque de bug
 * pour un gain minime, le coût de 4 reconstructions complètes restant
 * négligeable). Le total délivré DOIT correspondre à border_ring_count_dp —
 * échec bruyant sinon (jamais un fichier .back silencieusement incomplet,
 * sauf si max_rings a délibérément coupé la délivrance avant). */
long long border_ring_reconstruct_dp(map_big_array *map, struct array_part *all_rotate_parts, int nb_workers,
                                      long long max_rings, border_ring_found_cb on_found, void *user_ctx)
{
    int n = (all_rotate_parts->size - 1) / 4;

    struct bd_ctx *ctx = malloc(sizeof *ctx);
    int base_counts[BD_MAX_CLASSES];
    int16_t *id_to_class = malloc((size_t)(n + 1) * sizeof *id_to_class);
    if (ctx == NULL || id_to_class == NULL) {
        fprintf(stderr, "border_ring_count_dp : allocation du contexte impossible — arret\n");
        exit(1);
    }
    bd_ctx_init(ctx, all_rotate_parts, base_counts, id_to_class, n);

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

        char persist_dir[BD_PATH_MAX - 64];
        bd_path_or_die(persist_dir, sizeof persist_dir, "%s/etii_bd_recon_%d_%d", bd_spill_dir, (int)getpid(),
                       s);
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

        bd_reconstruct_class_dfs(&rc, 1, bd_key_make(&ctx->layout, initial_required, counts, ctx->nb_classes));

        if (rc.cached_pos != -1) {
            bd_level_release_borrowed(&rc.cached_level);
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

/* ===========================================================================
 * Points d'entrée test-only — jamais déclarés dans border_ring_dp.h, appelés
 * uniquement par tests/tools/test_border_ring_dp.c (même schéma que
 * `stock_spill_set_segment_bytes_for_tests`). Ils exposent les deux briques
 * PURES sur lesquelles repose tout le reste du fichier : si l'une d'elles
 * cède, le total final est faux sans que rien d'autre ne le signale.
 */

/* Trie puis fusionne `n` couples (clé, valeur) — exactement ce que fait le
   trieur quand son tampon est plein. Réécrit le résultat dans `keys`/`values`
   et retourne le nombre d'entrées distinctes. */
size_t bd_sort_and_compact_for_tests(uint64_t *keys, uint64_t *values, size_t n)
{
    struct bd_entry *a = malloc((n + 1) * sizeof *a);
    if (a == NULL) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        a[i].key = (bd_key_t)keys[i];
        bd_entry_set_value(&a[i], (bd_ring_count_t)values[i]);
    }
    bd_sort_entries(a, n);
    size_t out = bd_compact_sorted(a, n);
    for (size_t i = 0; i < out; i++) {
        keys[i] = (uint64_t)a[i].key;
        values[i] = (uint64_t)bd_entry_value(&a[i]);
    }
    free(a);
    return out;
}

/* Vérifie, pour un jeu de multiplicités donné, que la clé compacte est
   RÉVERSIBLE sur TOUS les vecteurs de compteurs possibles (donc injective :
   deux états distincts ne peuvent pas se retrouver sous la même clé, ce qui
   les ferait fusionner à tort et fausserait le total sans rien signaler) et
   que `bd_key_step` décrémente bien la classe visée, elle seule, en posant la
   couleur produite. Retourne 1 si tout concorde. */
int bd_key_layout_roundtrip_for_tests(const int *counts_max, int nb_classes)
{
    int counts_copy[BD_MAX_CLASSES];
    for (int c = 0; c < nb_classes; c++) {
        counts_copy[c] = counts_max[c];
    }
    struct bd_key_layout layout;
    bd_key_layout_init(&layout, counts_copy, nb_classes);

    int8_t vec[BD_MAX_CLASSES];
    memset(vec, 0, sizeof vec);
    const int8_t colors[] = { 0, 7, 42, (int8_t)127 };

    for (;;) {
        for (size_t ci = 0; ci < sizeof colors / sizeof colors[0]; ci++) {
            bd_key_t key = bd_key_make(&layout, colors[ci], vec, nb_classes);
            if (bd_key_color(&layout, key) != colors[ci]) {
                return 0;
            }
            for (int c = 0; c < nb_classes; c++) {
                if (bd_key_count(&layout, key, c) != (uint64_t)vec[c]) {
                    return 0;
                }
            }
            for (int c = 0; c < nb_classes; c++) {
                if (vec[c] == 0) {
                    continue;
                }
                bd_key_t next = bd_key_step(&layout, key, c, (int8_t)3);
                if (bd_key_color(&layout, next) != 3) {
                    return 0;
                }
                for (int k = 0; k < nb_classes; k++) {
                    uint64_t want = (uint64_t)vec[k] - (k == c ? 1u : 0u);
                    if (bd_key_count(&layout, next, k) != want) {
                        return 0;
                    }
                }
            }
        }

        /* Incrémente le vecteur en base mixte : parcourt TOUS les états
           possibles, jamais un échantillon. */
        int c = 0;
        while (c < nb_classes && vec[c] == (int8_t)counts_max[c]) {
            vec[c++] = 0;
        }
        if (c == nb_classes) {
            return 1;
        }
        vec[c]++;
    }
}
