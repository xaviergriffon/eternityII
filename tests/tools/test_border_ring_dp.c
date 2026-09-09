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

/* Test-only, jamais déclarées dans border_ring_dp.h — même schéma. Abaisser
   ces deux seuils force une scission (répartition en fragments, cf. le
   commentaire de tête de la section "Scission par pile LIFO" dans
   border_ring_dp.c) dès le premier niveau, sur un fixture minuscule, sans
   avoir à construire un niveau de plusieurs Go. */
void border_ring_dp_set_disk_mode_min_bytes_for_tests(double n);
void border_ring_dp_set_shard_target_bytes_for_tests(double n);

/* Test-only, jamais déclarées dans border_ring_dp.h — même schéma que
   border_ring_dp_set_fork_min_states_for_tests. */
double bd_estimate_reload_bytes(int32_t key_len, uint64_t count);
int bd_reload_estimate_matches_real_alloc_for_tests(int32_t key_len, uint64_t count);

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

TEST border_ring_count_dp_returns_zero_without_any_border_shaped_piece(void)
{
    struct array_part *all = brd_make_rotate_parts_no_border_piece();
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    ASSERT_EQ_FMT(0LL, border_ring_count_dp(map, all, 1), "%lld");

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
    long long dp = border_ring_count_dp(map, all, 1);

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
    long long dp = border_ring_count_dp(map, all, 1);

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
    long long dp = border_ring_count_dp(map, all, 4);
    border_ring_dp_set_fork_min_states_for_tests(50000);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Force une scission dès le premier niveau (seuil abaissé à 1 octet) avec
   une cible de fragment minuscule (32 octets, quelques entrées à peine) sur
   le fixture à multiplicité : tout niveau non vide dépasse ce seuil, donc
   CHAQUE position (y compris celles des tranches reprises depuis la pile)
   déclenche une nouvelle scission — verrouille tout le chemin externe par
   hachage (éclatement -> compactage -> reprise -> nouvelle scission...) en
   cascade sur plusieurs niveaux d'empilement, jamais exercé par les tests
   précédents qui ne dépassent jamais bd_split_threshold_bytes par défaut
   (2 Go). Sans l'accumulation lors du compactage (bd_level_add, pas un
   écrasement), deux fragments bruts déposant la même clé se marcheraient
   dessus au lieu de s'additionner — exactement le même risque que
   bd_transition_parallel, à la granularité du fragment plutôt que du niveau
   entier. Verrouille aussi la pile LIFO elle-même (bd_pending_stack) : si une
   tranche empilée était perdue, dupliquée, ou reprise à la mauvaise position,
   le total s'écarterait du brute-force. */
TEST border_ring_count_dp_matches_brute_force_when_sharded_to_disk(void)
{
    struct array_part *all = brd_make_rotate_parts(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_disk_mode_min_bytes_for_tests(1.0);
    border_ring_dp_set_shard_target_bytes_for_tests(32.0);
    long long dp = border_ring_count_dp(map, all, 4);
    border_ring_dp_set_disk_mode_min_bytes_for_tests(2.0 * 1024.0 * 1024.0 * 1024.0);
    border_ring_dp_set_shard_target_bytes_for_tests(768.0 * 1024.0 * 1024.0);

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
    long long dp = border_ring_count_dp(map, all, 1);

    ASSERT_EQ_FMT(4LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Même régression que border_ring_count_dp_matches_border_walk_count_on_real_pieces16,
   mais forcée en mode disque : la table des classes réelles (ordre des
   couleurs required/outgoing) doit rester correcte y compris quand chaque
   fragment ne voit qu'une fraction des états. */
TEST border_ring_count_dp_matches_border_walk_count_on_real_pieces16_sharded_to_disk(void)
{
    struct array_part *all = brd_make_rotate_parts_pieces16();
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_disk_mode_min_bytes_for_tests(1.0);
    border_ring_dp_set_shard_target_bytes_for_tests(32.0);
    long long dp = border_ring_count_dp(map, all, 4);
    border_ring_dp_set_disk_mode_min_bytes_for_tests(2.0 * 1024.0 * 1024.0 * 1024.0);
    border_ring_dp_set_shard_target_bytes_for_tests(768.0 * 1024.0 * 1024.0);

    ASSERT_EQ_FMT(4LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
#endif

TEST bd_estimate_reload_bytes_matches_known_capacity_growth(void)
{
    /* key_len=5, count=0 : hint=16, capacity reste 16 (deja >= hint). */
    ASSERT_EQ_FMT(16.0 * (5 + 8) + 2.0, bd_estimate_reload_bytes(5, 0), "%.1f");
    /* key_len=5, count=10 : hint=36, capacite double 16->32->64. */
    ASSERT_EQ_FMT(64.0 * (5 + 8) + 8.0, bd_estimate_reload_bytes(5, 10), "%.1f");
    PASS();
}

TEST bd_estimate_reload_bytes_matches_real_allocation_for_various_sizes(void)
{
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(5, 0));
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(5, 10));
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(20, 1000));
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(3, 1000003));
    PASS();
}

SUITE(border_ring_dp_suite)
{
    RUN_TEST(border_ring_count_dp_returns_zero_without_any_border_shaped_piece);
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_a_unique_ring);
    RUN_TEST(border_ring_count_dp_counts_class_multiplicity_correctly);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_forked);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_sharded_to_disk);
#if ETERN_PARTS == 16
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_real_pieces16);
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_real_pieces16_sharded_to_disk);
#endif
    RUN_TEST(bd_estimate_reload_bytes_matches_known_capacity_growth);
    RUN_TEST(bd_estimate_reload_bytes_matches_real_allocation_for_various_sizes);
}
