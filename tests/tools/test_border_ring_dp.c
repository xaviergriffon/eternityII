/*
 * Tests de tests/tools/border_ring_dp.c — comptage exact de la masse de
 * bordure par programmation dynamique sur des classes de pièces
 * interchangeables (voir border_ring_dp.h pour le raisonnement complet).
 *
 * La garantie centrale à verrouiller ici, que le fixture "anneau unique" de
 * test_border_walk.c (chaque classe de multiplicité 1) ne peut PAS tester :
 * quand plusieurs pièces réelles partagent la même classe, le total doit
 * bien compter les PERMUTATIONS de pièces réelles (poids = compteur restant
 * à chaque usage), pas seulement les motifs de couleurs abstraits.
 */
#include "greatest.h"

#include "tools/border_ring_dp.h"
#include "tools/border_walk.h"
#include "core/core_static_variables.h"
#include "core/readdata.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Test-only, jamais déclarée dans border_ring_dp.h — même schéma que
   stock_spill_set_segment_bytes_for_tests (tests/core/test_stock_spill.c) :
   abaisse le seuil de déclenchement des forks pour qu'un fixture minuscule
   suffise à exercer réellement bd_transition_parallel, pas seulement le
   chemin séquentiel (le seuil par défaut, 50000, n'est jamais atteint par
   les petits fixtures ci-dessous). */
void border_ring_dp_set_fork_min_states_for_tests(size_t n);

/* Test-only, jamais déclarée dans border_ring_dp.h — même schéma. Force la
   capacité du tampon de tri, PLANCHER COMPRIS (contrairement à
   border_ring_dp_set_max_ram_mo, qui ne descend jamais sous
   BD_SORTER_MIN_ENTRIES) : seul moyen de faire déborder le trieur sur un
   fixture minuscule et d'exercer pour de bon runs sur disque, fusion
   k-voies et niveau resté résident sur disque. 0 remet le calcul normal. */
void border_ring_dp_set_sorter_capacity_for_tests(size_t entries);

/* Test-only, jamais déclarées dans border_ring_dp.h — même schéma. Comptent
   les runs RÉELLEMENT déversés sur disque : un test qui ne vérifie que le
   total ne peut pas distinguer « le chemin externe a servi et il est juste »
   de « tout est resté en mémoire, le chemin externe n'a jamais tourné » —
   c'est exactement le piège qui avait laissé passer un hook devenu sans
   effet après un renommage, dans la version précédente de ce fichier. */
void border_ring_dp_reset_runs_spilled_for_tests(void);
long border_ring_dp_get_runs_spilled_for_tests(void);

/* Test-only, jamais déclarées dans border_ring_dp.h — les deux briques PURES
   du moteur : le tri+fusion qui remplace la table de hachage, et la clé
   compacte qui rend un état stockable sur 8 octets. */
void border_ring_dp_set_merge_fanin_for_tests(int fanin);

size_t bd_sort_and_compact_for_tests(uint64_t *keys, uint64_t *values, size_t n);
int bd_key_layout_roundtrip_for_tests(const int *counts_max, int nb_classes);

#define BRD_EDGE_BASE 12
#define BRD_INTERIOR_PLACEHOLDER 11

/* Identique à bw_required_face (tests/tools/test_border_walk.c) : couleur
   requise sur la face de `ring[i]` tournée vers `(nx,ny)`. */
static int brd_required_face(const int8_t ring[BORDER_RING_LEN][2], int i, int nx, int ny)
{
    if (nx < 0 || nx >= ETERN_SIZE || ny < 0 || ny >= ETERN_SIZE) {
        return 0;
    }
    int prev = (i - 1 + BORDER_RING_LEN) % BORDER_RING_LEN;
    int next = (i + 1) % BORDER_RING_LEN;
    if (ring[prev][0] == nx && ring[prev][1] == ny) {
        return BRD_EDGE_BASE + prev;
    }
    if (ring[next][0] == nx && ring[next][1] == ny) {
        return BRD_EDGE_BASE + i;
    }
    return BRD_INTERIOR_PLACEHOLDER;
}

/* Écrit un jeu de ETERN_PARTS pièces formant un anneau unique par
   construction (comme bw_make_rotate_parts(1)), avec en plus
   `nb_duplicates` pièces surnuméraires dupliquant la position ring[1] (une
   arête, jamais un coin) — `nb_duplicates` pièces interchangeables de plus
   pour ce même point de l'anneau, exerçant la multiplicité de classe que le
   fixture à multiplicité 1 ne peut pas exercer. Exige
   ETERN_PARTS >= BORDER_RING_LEN + nb_duplicates. */
static struct array_part *brd_make_rotate_parts(int nb_duplicates)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    char path[] = "/tmp/etii_brd_pieces_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fprintf(fp, "ntiles: %d\n", ETERN_PARTS);

    for (int id = 1; id <= ETERN_PARTS; id++) {
        int top, right, bottom, left;
        int ring_i = -1;
        if (id <= BORDER_RING_LEN) {
            ring_i = id - 1;
        } else if (id <= BORDER_RING_LEN + nb_duplicates) {
            ring_i = 1;
        }
        if (ring_i >= 0) {
            int x = ring[ring_i][0];
            int y = ring[ring_i][1];
            top = brd_required_face(ring, ring_i, x, y - 1);
            right = brd_required_face(ring, ring_i, x + 1, y);
            bottom = brd_required_face(ring, ring_i, x, y + 1);
            left = brd_required_face(ring, ring_i, x - 1, y);
        } else {
            int base = id % 7;
            top = base + 1;
            left = base + 2;
            bottom = base + 3;
            right = base + 4;
        }
        fprintf(fp, "%d %d %d %d %d\n", id, top, left, bottom, right);
    }
    fclose(fp);

    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

/* Comme brd_make_rotate_parts, mais ajoute UNE pièce-arête de plus, à un
   nouvel id `BORDER_RING_LEN + nb_duplicates + 1` : même couleur d'ENTRÉE que
   ring[1] (donc candidate valide à la MÊME position que le vrai chemin de
   l'anneau) mais une couleur de SORTIE inédite (`fork_color`, jamais utilisée
   ailleurs) — donc une CLASSE distincte, contrairement à `nb_duplicates` qui
   ne fait que multiplier le poids d'une classe déjà existante.
 *
 * Nécessaire pour que `border_ring_count_dp_matches_brute_force_when_sharded_to_disk`
 * et `..._when_pool_mode_engages` exercent réellement une scission : la
 * topologie de `brd_make_rotate_parts` seule garantit qu'à CHAQUE position il
 * n'existe qu'UNE SEULE classe candidate (chaque position de l'anneau a une
 * couleur requise unique, cf. `brd_required_face`) — un niveau y compte donc
 * TOUJOURS exactement UN état, du début à la fin de la DP, quel que soit le
 * nombre de pièces réelles dupliquées au sein d'une classe (`nb_duplicates`
 * ne fait que multiplier le nombre de FAÇONS, jamais le nombre d'états).
 * Conséquence : le tampon de tri n'y déborde jamais, quelle que soit sa
 * capacité — un seul successeur poussé par transition ne remplit rien. Les
 * tests du chemin externe (runs sur disque, fusion k-voies, niveau resté
 * résident sur disque) ne testeraient donc jamais rien sur ce fixture, hook
 * ou pas.
 *
 * La pièce fourche crée une branche MORTE (sa couleur de sortie ne
 * correspond à rien d'attendu par la suite, donc `border_walk_count` ne
 * compte jamais de fermeture supplémentaire par cette voie — le total brut
 * attendu reste inchangé) qui coexiste avec la branche réelle pendant
 * EXACTEMENT une transition : assez pour qu'un niveau compte 2 états et que
 * le tampon de tri, forcé à 1 entrée par
 * `border_ring_dp_set_sorter_capacity_for_tests`, déborde pour de bon. */
static struct array_part *brd_make_rotate_parts_with_fork(int nb_duplicates)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    char path[] = "/tmp/etii_brd_fork_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fprintf(fp, "ntiles: %d\n", ETERN_PARTS);

    int fork_id = BORDER_RING_LEN + nb_duplicates + 1;
    /* > tout id d'arête réel (BRD_EDGE_BASE + BORDER_RING_LEN - 1 au plus) et
       tient dans un int8_t (cf. struct part) pour n'importe quelle taille de
       plateau compilée. */
    int fork_color = BRD_EDGE_BASE + BORDER_RING_LEN + 20;

    for (int id = 1; id <= ETERN_PARTS; id++) {
        int top, right, bottom, left;
        int ring_i = -1;
        if (id <= BORDER_RING_LEN) {
            ring_i = id - 1;
        } else if (id <= BORDER_RING_LEN + nb_duplicates) {
            ring_i = 1;
        }
        if (ring_i >= 0) {
            int x = ring[ring_i][0];
            int y = ring[ring_i][1];
            top = brd_required_face(ring, ring_i, x, y - 1);
            right = brd_required_face(ring, ring_i, x + 1, y);
            bottom = brd_required_face(ring, ring_i, x, y + 1);
            left = brd_required_face(ring, ring_i, x - 1, y);
        } else if (id == fork_id) {
            int x = ring[1][0];
            int y = ring[1][1];
            top = brd_required_face(ring, 1, x, y - 1);
            right = brd_required_face(ring, 1, x + 1, y);
            bottom = brd_required_face(ring, 1, x, y + 1);
            left = brd_required_face(ring, 1, x - 1, y);
            /* La face "sortante" de ring[1] (celle qui vaut BRD_EDGE_BASE+1,
               cf. brd_required_face : `return BRD_EDGE_BASE + i` quand le
               voisin est ring[next]) devient fork_color — seule face
               modifiée, donc même forme (une seule face à 0, arête) et même
               couleur d'entrée que ring[1]. */
            int outgoing_marker = BRD_EDGE_BASE + 1;
            if (top == outgoing_marker) top = fork_color;
            else if (right == outgoing_marker) right = fork_color;
            else if (bottom == outgoing_marker) bottom = fork_color;
            else if (left == outgoing_marker) left = fork_color;
        } else {
            int base = id % 7;
            top = base + 1;
            left = base + 2;
            bottom = base + 3;
            right = base + 4;
        }
        fprintf(fp, "%d %d %d %d %d\n", id, top, left, bottom, right);
    }
    fclose(fp);

    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

/* Jeu sans aucune pièce en forme de bordure (aucune face à 0) — comme
   border_walk_count_returns_zero_without_any_border_shaped_piece. */
static struct array_part *brd_make_rotate_parts_no_border_piece(void)
{
    char path[] = "/tmp/etii_brd_none_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fprintf(fp, "ntiles: %d\n", ETERN_PARTS);
    for (int id = 1; id <= ETERN_PARTS; id++) {
        int base = id % 7;
        fprintf(fp, "%d %d %d %d %d\n", id, base + 1, base + 2, base + 3, base + 4);
    }
    fclose(fp);

    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

/* `--max-rings` doit pouvoir viser la masse RÉELLE (~3,8x10^37), 19 ordres
   de grandeur au-dessus d'un `long long`. La régression que ce test
   verrouille est silencieuse par nature : `strtoll`/`strtoull` saturent à
   leur maximum ET rendent une valeur d'apparence valide, donc un plafond
   énorme devenait un plafond ~10^19 fois plus petit sans le moindre message.
   L'aller-retour parse -> format est la seule vérification qui ne peut pas
   passer sur une valeur tronquée. */
TEST bd_ring_count_parse_round_trips_values_far_beyond_64_bits(void)
{
    static const char *const values[] = {
        "0",
        "1",
        "18446744073709551615",                     /* 2^64-1 : dernière valeur qu'un strtoull rend juste */
        "18446744073709551616",                     /* 2^64   : la première qu'il saturerait */
        "38000000000000000000000000000000000000",   /* ~3,8x10^37, l'estimation de la masse réelle */
        "340282366920938463463374607431768211455",  /* 2^128-1, le maximum représentable */
    };

    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        bd_ring_count_t parsed = 0;
        ASSERT_EQ_FMT(0, bd_ring_count_parse(values[i], &parsed), "%d");

        char back[BD_RING_COUNT_STRLEN];
        bd_ring_count_format(parsed, back, sizeof back);
        ASSERT_STR_EQ(values[i], back);
    }
    PASS();
}

/* Une valeur d'option est exacte ou refusée, jamais tronquée à son préfixe
   numérique comme le ferait `strtoull` (qui accepte « 12a » en rendant 12, et
   « -1 » en rendant un très grand non signé). */
TEST bd_ring_count_parse_rejects_anything_that_is_not_a_plain_decimal(void)
{
    static const char *const bad[] = {
        "",
        " 12",
        "12 ",
        "12a",
        "-1",
        "+1",
        "0x10",
        "1e9",
        "340282366920938463463374607431768211456", /* 2^128, débordement d'un cran */
        "999999999999999999999999999999999999999999",
    };

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bd_ring_count_t parsed = 12345;
        ASSERT_EQ_FMT(-1, bd_ring_count_parse(bad[i], &parsed), "%d");
        /* Rejet = sortie intacte : l'appelant ne doit pas hériter d'un
           plafond a moitié lu. */
        ASSERT(parsed == (bd_ring_count_t)12345);
    }

    bd_ring_count_t parsed = 0;
    ASSERT_EQ_FMT(-1, bd_ring_count_parse(NULL, &parsed), "%d");
    PASS();
}

TEST border_ring_count_dp_returns_zero_without_any_border_shaped_piece(void)
{
    struct array_part *all = brd_make_rotate_parts_no_border_piece();
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    ASSERT_EQ_FMT(0LL, (long long)border_ring_count_dp(map, all, 1), "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Sans multiplicité (chaque classe a un unique membre) : même valeur que
   border_walk_count sur le même fixture, EXACTEMENT le cas déjà verrouillé
   par test_border_walk.c — non-régression avant d'ajouter la multiplicité. */
TEST border_ring_count_dp_matches_border_walk_count_on_a_unique_ring(void)
{
    struct array_part *all = brd_make_rotate_parts(0);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);
    long long dp = (long long)border_ring_count_dp(map, all, 1);

    ASSERT_EQ_FMT(4LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Avec 2 pièces surnuméraires dupliquant une même position d'arête : cette
   position a désormais 3 pièces interchangeables, donc 4 (ouvertures) × 3
   (choix à cette position) = 12 anneaux complets — border_ring_count_dp
   doit calculer la même chose que border_walk_count, PAS 4 : si la
   pondération par `counts[c]` (ways = pièces réelles restantes) régressait
   vers un simple "existe/n'existe pas", ce test échouerait en trouvant 4
   au lieu de 12. */
TEST border_ring_count_dp_counts_class_multiplicity_correctly(void)
{
    struct array_part *all = brd_make_rotate_parts(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);
    long long dp = (long long)border_ring_count_dp(map, all, 1);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Force bd_transition_parallel a s'engager des le premier niveau (seuil
   abaisse a 1 etat) sur le fixture a multiplicite : verrouille le chemin
   fork+fichier-temporaire+fusion, jamais exerce par les tests precedents
   (nb_workers=1 ne fork jamais, et le seuil par defaut n'est pas atteint
   par un fixture aussi petit). Sans la fusion par accumulation
   (bd_level_add, pas un simple ecrasement), deux workers produisant le
   meme etat suivant se marcheraient dessus au lieu de s'additionner. */
TEST border_ring_count_dp_matches_brute_force_when_forked(void)
{
    struct array_part *all = brd_make_rotate_parts(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_fork_min_states_for_tests(1);
    long long dp = (long long)border_ring_count_dp(map, all, 4);
    border_ring_dp_set_fork_min_states_for_tests(50000);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

#if ETERN_PARTS == 16
/* Contenu de data/pieces16.csv, embarqué pour rester indépendant du CWD
   (même convention que tests/core/test_solution16.c). Le vrai jeu 16 pièces
   contient, entre autres, les pièces bord id=4 et id=16 : mêmes deux
   couleurs "anneau" {1,2}, mais orientation INVERSE l'une de l'autre (id=16
   présente 2 en entrée / 1 en sortie, id=4 l'inverse) — exactement le motif
   dont la confusion (paire non ordonnée au lieu de paire ordonnée) faisait
   passer border_ring_count_dp de 4 (correct) à 128 (32× trop) avant
   correction. Aucun fixture synthétique construit à la main n'aurait
   probablement reproduit ce piège aussi fidèlement que les vraies données —
   ce test verrouille directement le cas réel qui l'a révélé. */
static const char *BRD_PIECES16_CSV =
    "ntiles: 16\n"
    "1 3 0 1 5\n"
    "2 2 4 0 0\n"
    "3 0 0 1 2\n"
    "4 1 7 2 0\n"
    "5 8 6 6 8\n"
    "6 7 3 0 4\n"
    "7 5 7 6 6\n"
    "8 8 3 0 3\n"
    "9 1 0 3 7\n"
    "10 0 4 2 0\n"
    "11 6 5 7 7\n"
    "12 1 0 0 3\n"
    "13 6 5 8 5\n"
    "14 0 4 8 4\n"
    "15 0 2 5 4\n"
    "16 2 8 1 0\n";

static struct array_part *brd_make_rotate_parts_pieces16(void)
{
    char path[] = "/tmp/etii_brd_pieces16_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fputs(BRD_PIECES16_CSV, fp);
    fclose(fp);

    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

TEST border_ring_count_dp_matches_border_walk_count_on_real_pieces16(void)
{
    struct array_part *all = brd_make_rotate_parts_pieces16();
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);
    long long dp = (long long)border_ring_count_dp(map, all, 1);

    ASSERT_EQ_FMT(4LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Même régression que border_ring_count_dp_matches_border_walk_count_on_real_pieces16,
   mais forcée sur le chemin EXTERNE (tampon de tri réduit à 2 entrées) : la
   table des classes réelles (ordre des couleurs required/outgoing) doit
   rester correcte quand les états transitent par des runs sur disque et une
   fusion k-voies plutôt que par un tableau résident. */
TEST border_ring_count_dp_matches_border_walk_count_on_real_pieces16_spilled(void)
{
    struct array_part *all = brd_make_rotate_parts_pieces16();
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_reset_runs_spilled_for_tests();
    border_ring_dp_set_sorter_capacity_for_tests(2);
    long long dp = (long long)border_ring_count_dp(map, all, 4);
    border_ring_dp_set_sorter_capacity_for_tests(0);
    long spilled = border_ring_dp_get_runs_spilled_for_tests();

    ASSERT_EQ_FMT(4LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");
    ASSERT(spilled > 0);

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
#endif


/* ===========================================================================
 * Les deux briques pures du moteur de tri externe.
 */

/* Le tri + la fusion des clés égales REMPLACENT l'accumulation par hachage de
   la version précédente : c'est ici, et nulle part ailleurs, que deux chemins
   menant au même état voient leurs nombres de façons s'additionner. Une
   régression qui écraserait au lieu d'additionner (ou qui laisserait deux
   entrées de même clé côte à côte) fausserait tous les totaux d'un facteur
   dépendant des données — donc sans jamais échouer franchement. */
TEST bd_sort_and_compact_sorts_and_sums_duplicate_keys(void)
{
    uint64_t keys[] =   { 7, 3, 7, 1, 3, 7, 9, 1 };
    uint64_t values[] = { 10, 20, 30, 40, 50, 60, 70, 80 };

    size_t n = bd_sort_and_compact_for_tests(keys, values, 8);

    ASSERT_EQ_FMT((size_t)4, n, "%zu");
    ASSERT_EQ_FMT(1ULL, (unsigned long long)keys[0], "%llu");
    ASSERT_EQ_FMT(120ULL, (unsigned long long)values[0], "%llu"); /* 40 + 80 */
    ASSERT_EQ_FMT(3ULL, (unsigned long long)keys[1], "%llu");
    ASSERT_EQ_FMT(70ULL, (unsigned long long)values[1], "%llu"); /* 20 + 50 */
    ASSERT_EQ_FMT(7ULL, (unsigned long long)keys[2], "%llu");
    ASSERT_EQ_FMT(100ULL, (unsigned long long)values[2], "%llu"); /* 10 + 30 + 60 */
    ASSERT_EQ_FMT(9ULL, (unsigned long long)keys[3], "%llu");
    ASSERT_EQ_FMT(70ULL, (unsigned long long)values[3], "%llu");
    PASS();
}

/* Deux entrées adverses pour un quicksort : tout égal (le cas que le
   partitionnement À 3 VOIES retire en une passe, et qu'un partitionnement
   binaire ferait dégénérer en O(n^2) — sur cette DP, les doublons SONT la
   matière première), et strictement décroissant. Le résultat doit rester
   juste dans les deux cas. */
TEST bd_sort_and_compact_handles_all_equal_and_reversed_input(void)
{
    enum { N = 512 };
    uint64_t keys[N], values[N];

    for (int i = 0; i < N; i++) {
        keys[i] = 42;
        values[i] = 1;
    }
    ASSERT_EQ_FMT((size_t)1, bd_sort_and_compact_for_tests(keys, values, N), "%zu");
    ASSERT_EQ_FMT(42ULL, (unsigned long long)keys[0], "%llu");
    ASSERT_EQ_FMT((unsigned long long)N, (unsigned long long)values[0], "%llu");

    for (int i = 0; i < N; i++) {
        keys[i] = (uint64_t)(N - i);
        values[i] = 2;
    }
    ASSERT_EQ_FMT((size_t)N, bd_sort_and_compact_for_tests(keys, values, N), "%zu");
    for (int i = 0; i < N; i++) {
        ASSERT_EQ_FMT((unsigned long long)(i + 1), (unsigned long long)keys[i], "%llu");
        ASSERT_EQ_FMT(2ULL, (unsigned long long)values[i], "%llu");
    }
    PASS();
}

/* La clé compacte doit être INJECTIVE : deux états distincts sous la même
   clé se fusionneraient à tort, et le total serait faux sans que rien ne le
   signale (ni assertion, ni dépassement — juste un chiffre erroné). Vérifié
   par réversibilité sur TOUS les vecteurs de compteurs possibles du jeu de
   multiplicités donné, pas sur un échantillon. Les multiplicités choisies
   reproduisent celles du vrai jeu 256 pièces (4, 3, 2 et 1 exemplaires), y
   compris le cas m=1 qui tient sur un seul bit. */
TEST bd_key_layout_is_reversible_on_every_state(void)
{
    const int counts[] = { 4, 3, 2, 1, 2, 1 };
    ASSERT(bd_key_layout_roundtrip_for_tests(counts, 6));
    PASS();
}

/* ===========================================================================
 * Le chemin externe de bout en bout.
 */

/* Tampon de tri réduit à une entrée : CHAQUE état produit devient son propre
   run sur disque, donc toute la chaîne externe (déversement, fusion k-voies,
   niveau resté résident sur disque, relecture séquentielle par la position
   suivante) est traversée à chaque position — sur un fixture minuscule dont
   le total est connu par force brute. C'est le test qui remplace ceux de
   l'ancienne scission par fragments : la garantie n'est plus « les fragments
   se recombinent » mais « le niveau n'est jamais fragmenté du tout ».
   `spilled > 0` verrouille que le chemin a RÉELLEMENT servi : sans ce
   témoin, un hook devenu sans effet (déjà vécu une fois sur ce fichier)
   laisserait le test passer par le chemin tout-en-mémoire. */
TEST border_ring_count_dp_matches_brute_force_when_the_sorter_spills(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_reset_runs_spilled_for_tests();
    border_ring_dp_set_sorter_capacity_for_tests(1);
    long long dp = (long long)border_ring_count_dp(map, all, 1);
    border_ring_dp_set_sorter_capacity_for_tests(0);
    long spilled = border_ring_dp_get_runs_spilled_for_tests();

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");
    ASSERT(spilled > 0);

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Contre-épreuve du test précédent : avec un budget confortable, AUCUN run ne
   doit partir sur disque. Sans cette vérification, `spilled > 0` ne prouve
   rien (un compteur incrémenté inconditionnellement passerait les deux). */
TEST border_ring_count_dp_never_touches_disk_when_the_budget_fits(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    border_ring_dp_reset_runs_spilled_for_tests();
    long long dp = (long long)border_ring_count_dp(map, all, 1);

    ASSERT_EQ_FMT(12LL, dp, "%lld");
    ASSERT_EQ_FMT(0L, border_ring_dp_get_runs_spilled_for_tests(), "%ld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Déversement ET forks simultanés : chaque worker rend un fichier trié, que
   le parent fusionne. Deux workers peuvent produire authentiquement la même
   clé (deux états du niveau courant transitant vers le même état suivant) —
   si la fusion écrasait au lieu d'additionner, le total s'écarterait ici. */
TEST border_ring_count_dp_matches_brute_force_when_spilling_and_forked(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_fork_min_states_for_tests(1);
    border_ring_dp_set_sorter_capacity_for_tests(1);
    long long dp = (long long)border_ring_count_dp(map, all, 4);
    border_ring_dp_set_sorter_capacity_for_tests(0);
    border_ring_dp_set_fork_min_states_for_tests(50000);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Degré de fusion abaissé à 2 : la fusion devient RÉCURSIVE (tours
   intermédiaires de bd_sorter_merge_all) au lieu d'une passe unique. Ce
   palier n'existe que pour ne jamais ouvrir des milliers de descripteurs
   d'un coup ; s'il perdait ou dupliquait un run entre deux tours, le total
   s'en écarterait — aucun autre test ne le traverse, le degré réel (64)
   n'étant jamais atteint par un fixture de cette taille. */
TEST border_ring_count_dp_matches_brute_force_across_several_merge_rounds(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_sorter_capacity_for_tests(1);
    border_ring_dp_set_merge_fanin_for_tests(2);
    long long dp = (long long)border_ring_count_dp(map, all, 1);
    border_ring_dp_set_merge_fanin_for_tests(64);
    border_ring_dp_set_sorter_capacity_for_tests(0);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Collecte les anneaux réels délivrés par border_ring_reconstruct_dp, pour
   les compter et vérifier qu'ils sont tous distincts. */
struct brd_recon_ctx {
    struct possibility_packet *found;
    int count;
    int cap;
};

static int brd_on_ring_found(const struct possibility_packet *ring, void *ctx_)
{
    struct brd_recon_ctx *ctx = (struct brd_recon_ctx *)ctx_;
    if (ctx->count == ctx->cap) {
        ctx->cap = (ctx->cap == 0) ? 8 : ctx->cap * 2;
        ctx->found = realloc(ctx->found, (size_t)ctx->cap * sizeof *ctx->found);
    }
    ctx->found[ctx->count++] = *ring;
    return 0;
}

/* Le total réel reconstruit (border_ring_reconstruct_dp) doit correspondre
   exactement à border_ring_count_dp, sur le fixture SANS multiplicité (4
   anneaux, chacun sa propre suite de classes — aucune expansion combinatoire
   à cette étape) — non-régression avant le fixture à multiplicité. */
TEST border_ring_reconstruct_dp_matches_count_on_a_unique_ring(void)
{
    struct array_part *all = brd_make_rotate_parts(0);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long total = (long long)border_ring_count_dp(map, all, 1);
    ASSERT_EQ_FMT(4LL, total, "%lld");

    struct brd_recon_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    long long delivered = border_ring_reconstruct_dp(map, all, 1, 1000, brd_on_ring_found, &ctx);

    ASSERT_EQ_FMT(total, delivered, "%lld");
    ASSERT_EQ(4, ctx.count);

    for (int i = 0; i < ctx.count; i++) {
        ASSERT_EQ_FMT(BORDER_RING_LEN, possibility_placed_count(&ctx.found[i]), "%d");
        for (int j = i + 1; j < ctx.count; j++) {
            ASSERT(compare_possibility(&ctx.found[i], &ctx.found[j]) != 0);
        }
    }

    free(ctx.found);
    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Un callback qui demande l'arrêt tronque la reconstruction SANS déclencher
   le garde-fou « reconstruction incomplete » (qui fait exit(1), donc ferait
   mourir le binaire de test — l'assertion de comptage ci-dessous n'est
   atteinte que si le garde-fou s'est bien tu). Même raisonnement que
   `max_rings` : une troncature demandée n'est pas une reconstruction ratée.
   Le fixture porte 4 anneaux ; on coupe au 2e. */
struct brd_stop_ctx {
    int calls;
    int stop_after;
};

static int brd_on_ring_found_stopping(const struct possibility_packet *ring, void *ctx_)
{
    struct brd_stop_ctx *ctx = (struct brd_stop_ctx *)ctx_;
    (void)ring;
    ctx->calls++;
    return ctx->calls >= ctx->stop_after;
}

TEST border_ring_reconstruct_dp_honours_a_stopping_callback(void)
{
    struct array_part *all = brd_make_rotate_parts(0);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    struct brd_stop_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.stop_after = 2;

    long long delivered = border_ring_reconstruct_dp(map, all, 1, 1000, brd_on_ring_found_stopping, &ctx);

    ASSERT_EQ_FMT(2LL, delivered, "%lld");
    ASSERT_EQ_FMT(2, ctx.calls, "%d");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Avec 2 pièces surnuméraires (12 anneaux réels attendus, cf.
   border_ring_count_dp_counts_class_multiplicity_correctly) : le nombre de
   SUITES DE CLASSES distinctes doit être strictement inférieur à 12 (la
   classe dupliquée, utilisée une fois par anneau, se développe en plusieurs
   anneaux réels par suite de classes) alors que le nombre d'anneaux RÉELS
   délivrés doit rester exactement 12 — c'est la distinction clarifiée
   pendant la conception (cf. AGENTS.md/plan) : la masse compte des pièces
   réelles, pas des suites de classes. */
TEST border_ring_reconstruct_dp_expands_class_multiplicity_to_all_real_rings(void)
{
    struct array_part *all = brd_make_rotate_parts(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long total = (long long)border_ring_count_dp(map, all, 1);
    ASSERT_EQ_FMT(12LL, total, "%lld");

    struct brd_recon_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    long long delivered = border_ring_reconstruct_dp(map, all, 1, 1000, brd_on_ring_found, &ctx);

    ASSERT_EQ_FMT(total, delivered, "%lld");
    ASSERT_EQ(12, ctx.count);

    for (int i = 0; i < ctx.count; i++) {
        ASSERT_EQ_FMT(BORDER_RING_LEN, possibility_placed_count(&ctx.found[i]), "%d");
        for (int j = i + 1; j < ctx.count; j++) {
            ASSERT(compare_possibility(&ctx.found[i], &ctx.found[j]) != 0);
        }
    }

    free(ctx.found);
    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Même reconstruction, mais sur le fixture À FOURCHE (le seul dont un niveau
   compte plus d'UN état — cf. brd_make_rotate_parts_with_fork : sans
   embranchement réel, chaque position n'a qu'une classe candidate et le
   tampon de tri ne déborde jamais, quelle que soit sa capacité) et avec un
   tampon réduit à une entrée : les
   niveaux avant ET les tables de complétion restent alors sur DISQUE, donc
   `bd_level_lookup` travaille par dichotomie sur fichier (fseek + lecture
   d'une entrée par sonde) au lieu d'un tableau résident. Aucun autre test ne
   traverse ce chemin, et il porte tout l'élagage du DFS guidé : un lookup
   sur fichier qui se tromperait de position couperait des branches valides,
   et la reconstruction s'arrêterait sur « reconstruction incomplete ».
   Verrouille aussi, au passage, le rafraîchissement du cache de complétion
   après chaque récursion (bd_reconstruct_ensure_cache) : la version
   précédente ne rechargeait qu'en entrée de frame et interrogeait ensuite la
   table laissée par sa descendance. */
TEST border_ring_reconstruct_dp_matches_count_when_levels_live_on_disk(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    struct brd_recon_ctx ctx;
    memset(&ctx, 0, sizeof ctx);

    border_ring_dp_reset_runs_spilled_for_tests();
    border_ring_dp_set_sorter_capacity_for_tests(1);
    long long delivered = border_ring_reconstruct_dp(map, all, 1, 1000, brd_on_ring_found, &ctx);
    border_ring_dp_set_sorter_capacity_for_tests(0);

    ASSERT_EQ_FMT(12LL, delivered, "%lld");
    ASSERT_EQ(12, ctx.count);
    ASSERT(border_ring_dp_get_runs_spilled_for_tests() > 0);

    for (int i = 0; i < ctx.count; i++) {
        ASSERT_EQ_FMT(BORDER_RING_LEN, possibility_placed_count(&ctx.found[i]), "%d");
        for (int j = i + 1; j < ctx.count; j++) {
            ASSERT(compare_possibility(&ctx.found[i], &ctx.found[j]) != 0);
        }
    }

    free(ctx.found);
    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* max_rings coupe la délivrance avant terme : le total délivré doit être
   exactement le plafond demandé (pas d'échec bruyant — ce cas est
   explicitement distingué d'un vrai écart, cf. border_ring_reconstruct_dp). */
TEST border_ring_reconstruct_dp_stops_at_max_rings(void)
{
    struct array_part *all = brd_make_rotate_parts(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    struct brd_recon_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    long long delivered = border_ring_reconstruct_dp(map, all, 1, 3, brd_on_ring_found, &ctx);

    ASSERT_EQ_FMT(3LL, delivered, "%lld");
    ASSERT_EQ(3, ctx.count);

    free(ctx.found);
    free_bigarray(map);
    free_array_part(all);
    PASS();
}

SUITE(border_ring_dp_suite)
{
    RUN_TEST(bd_ring_count_parse_round_trips_values_far_beyond_64_bits);
    RUN_TEST(bd_ring_count_parse_rejects_anything_that_is_not_a_plain_decimal);
    RUN_TEST(border_ring_count_dp_returns_zero_without_any_border_shaped_piece);
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_a_unique_ring);
    RUN_TEST(border_ring_count_dp_counts_class_multiplicity_correctly);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_forked);

    RUN_TEST(bd_sort_and_compact_sorts_and_sums_duplicate_keys);
    RUN_TEST(bd_sort_and_compact_handles_all_equal_and_reversed_input);
    RUN_TEST(bd_key_layout_is_reversible_on_every_state);

    RUN_TEST(border_ring_count_dp_matches_brute_force_when_the_sorter_spills);
    RUN_TEST(border_ring_count_dp_never_touches_disk_when_the_budget_fits);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_spilling_and_forked);
    RUN_TEST(border_ring_count_dp_matches_brute_force_across_several_merge_rounds);

    RUN_TEST(border_ring_reconstruct_dp_matches_count_on_a_unique_ring);
    RUN_TEST(border_ring_reconstruct_dp_honours_a_stopping_callback);
    RUN_TEST(border_ring_reconstruct_dp_expands_class_multiplicity_to_all_real_rings);
    RUN_TEST(border_ring_reconstruct_dp_matches_count_when_levels_live_on_disk);
    RUN_TEST(border_ring_reconstruct_dp_stops_at_max_rings);
#if ETERN_PARTS == 16
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_real_pieces16);
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_real_pieces16_spilled);
#endif
}
