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

/* Test-only, jamais déclarée dans border_ring_dp.h — même schéma que les
   autres. Expose la marge heuristique du budget SOLO (Tâche 6) fixée en
   dernier lieu par border_ring_dp_set_max_ram_mo. */
double border_ring_dp_get_solo_budget_bytes_for_tests(void);

/* Test-only, jamais déclarée dans border_ring_dp.h — même schéma que les
   autres. Predicat pur de choix de mode (solo vs pool). */
int bd_should_run_solo(int active, int stack_count, int nb_workers);

/* Test-only, jamais déclarées dans border_ring_dp.h — même schéma que les
   autres. Compteur de fork() reussis dans bd_fork_pool_job (incremente par
   le PARENT, jamais l'enfant), seul moyen pour un test de prouver que le
   mode POOL a REELLEMENT forke des jobs concurrents, et pas seulement que
   le mode SOLO a produit, par coincidence, le meme total en traitant tout
   sequentiellement — un test qui ne verifie que le total final ne peut pas
   distinguer les deux (c'est exactement ce qui s'est produit une fois : un
   renommage de variable pendant un refactor avait rendu le hook
   border_ring_dp_set_disk_mode_min_bytes_for_tests sans effet, et le test
   pool-mode passait quand meme, via SOLO). */
void border_ring_dp_reset_pool_jobs_forked_for_tests(void);
long border_ring_dp_get_pool_jobs_forked_for_tests(void);

/* Struct de resultat d'un job, serialisable fichier. */
struct bd_job_result {
    long long total;
    int closed;
    char shard_dir[128];
    int nb_shards;
    int resume_pos;
};

/* Non-static, utilisees par le coordinateur (Tache 5) pour communiquer entre
   un job forke et le parent. */
void bd_job_result_write_or_die(const struct bd_job_result *r, const char *path);
int bd_job_result_read(struct bd_job_result *r, const char *path);

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
 * couleur requise unique, cf. `brd_required_face`) — `cur.used` (le nombre
 * d'états DISTINCTS occupant un niveau) y reste donc `== 1` du début à la fin
 * de la DP, quel que soit le nombre de pièces réelles dupliquées au sein
 * d'une classe (`nb_duplicates` ne fait que multiplier `ways`, jamais le
 * nombre d'états). La garde anti-dégénérescence de `bd_run_fragment_job`
 * (`cur.used > 1`, cf. Correctif 4) bloquerait alors TOUJOURS la scission —
 * les tests de scission sur disque ne testeraient jamais réellement ce
 * chemin, seuil de test ou pas (constaté en instrumentant temporairement le
 * calcul : `cur.used` valait 1 à CHAQUE position sur `brd_make_rotate_parts`
 * seul, y compris avec `nb_duplicates=2`). La pièce fourche crée une branche
 * MORTE (sa couleur de sortie ne correspond à rien d'attendu par la suite,
 * donc `border_walk_count` ne compte jamais de fermeture supplémentaire par
 * cette voie — le total brut attendu reste inchangé) qui coexiste avec la
 * branche réelle pendant EXACTEMENT une transition — assez pour que
 * `cur.used` passe à 2 et déclenche une scission réelle avec les seuils de
 * test quasi nuls (`border_ring_dp_set_disk_mode_min_bytes_for_tests(1.0)`).
 * `bd_pick_nb_shards` garantit `nb_shards >= nb_workers`, donc cette unique
 * scission suffit à elle seule à remplir la pile d'au moins `nb_workers`
 * fragments — assez pour que le mode POOL s'engage réellement (cf.
 * `bd_should_run_solo`), pas seulement le mode SOLO qui traiterait tout
 * séquentiellement. */
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
   le fixture À FOURCHE (brd_make_rotate_parts_with_fork, cf. son commentaire
   pour pourquoi brd_make_rotate_parts seul — multiplicité sans embranchement
   réel — ne peut JAMAIS satisfaire la garde `cur.used > 1` et ne
   testerait donc jamais réellement ce chemin, quel que soit le seuil) :
   `cur.used` passe à 2 exactement à la position où la fourche diverge,
   suffisant pour déclencher une scission réelle en K fragments (`bd_pick_nb_shards`
   garantissant K >= nb_workers) — verrouille tout le chemin externe par
   hachage (éclatement -> compactage -> reprise) sur des fragments réels,
   pas seulement sur un niveau qui ne dépasse jamais la garde de
   dégénérescence. Sans l'accumulation lors du compactage (bd_level_add, pas
   un écrasement), deux fragments bruts déposant la même clé se marcheraient
   dessus au lieu de s'additionner — exactement le même risque que
   bd_transition_parallel, à la granularité du fragment plutôt que du niveau
   entier. Verrouille aussi la pile LIFO elle-même (bd_pending_stack) : si une
   tranche empilée était perdue, dupliquée, ou reprise à la mauvaise position,
   le total s'écarterait du brute-force. */
TEST border_ring_count_dp_matches_brute_force_when_sharded_to_disk(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
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

/* Même fixture à fourche que border_ring_count_dp_matches_brute_force_when_sharded_to_disk
   (cf. son commentaire pour pourquoi une fourche réelle, pas juste
   `nb_duplicates`, est nécessaire pour franchir la garde `cur.used > 1`),
   mais avec un `nb_workers` assez petit (2) pour que l'unique scission
   qu'elle déclenche — K fragments, K >= nb_workers garanti par
   `bd_pick_nb_shards` — dépasse `nb_workers` fragments en attente sur la
   pile dès qu'elle survient, faisant basculer le coordinateur en mode POOL
   (`bd_should_run_solo`), pas seulement le mode SOLO déjà verrouillé par
   border_ring_count_dp_matches_brute_force_when_sharded_to_disk.
   Le total seul ne suffirait PAS a verrouiller ca : il est identique que le
   mode POOL ait reellement fork des jobs concurrents ou que le mode SOLO ait
   tout traite sequentiellement en tombant, par coincidence, sur le meme
   resultat — border_ring_dp_get_pool_jobs_forked_for_tests() est le seul
   temoin qui distingue les deux (cf. son commentaire), donc verrouille ici
   en plus du total. */
TEST border_ring_count_dp_matches_brute_force_when_pool_mode_engages(void)
{
    struct array_part *all = brd_make_rotate_parts_with_fork(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_disk_mode_min_bytes_for_tests(1.0);
    border_ring_dp_set_shard_target_bytes_for_tests(32.0);
    border_ring_dp_reset_pool_jobs_forked_for_tests();
    long long dp = border_ring_count_dp(map, all, 2);
    long long pool_jobs_forked = border_ring_dp_get_pool_jobs_forked_for_tests();
    border_ring_dp_set_disk_mode_min_bytes_for_tests(2.0 * 1024.0 * 1024.0 * 1024.0);
    border_ring_dp_set_shard_target_bytes_for_tests(768.0 * 1024.0 * 1024.0);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");
    /* >= 2 : nb_workers=2 ci-dessus, donc un vrai engagement du mode POOL
       doit forker au moins 2 jobs concurrents pour remplir le pool — pas
       seulement > 0, qui n'exclurait pas un pool degenere a 1 seul job. */
    ASSERT(pool_jobs_forked >= 2);

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

TEST bd_should_run_solo_picks_mode_from_queue_depth(void)
{
    /* Aucun job actif, peu de fragments en attente (< nb_workers) : solo. */
    ASSERT(bd_should_run_solo(0, 0, 4));
    ASSERT(bd_should_run_solo(0, 3, 4));
    /* Assez de fragments pour remplir tous les workers : pool. */
    ASSERT_FALSE(bd_should_run_solo(0, 4, 4));
    ASSERT_FALSE(bd_should_run_solo(0, 10, 4));
    /* Un job deja actif (pool en cours) : jamais solo tant qu'il tourne,
       meme si la pile s'est videe entre-temps — on laisse le pool en cours
       se terminer avant de rebasculer. */
    ASSERT_FALSE(bd_should_run_solo(1, 0, 4));
    PASS();
}

TEST border_ring_dp_set_max_ram_mo_derives_a_conservative_solo_budget(void)
{
    border_ring_dp_set_max_ram_mo(4096, 10);
    double solo_budget = border_ring_dp_get_solo_budget_bytes_for_tests();
    double raw_budget = 4096.0 * 1024.0 * 1024.0;

    /* La marge doit reserver une fraction reelle du budget brut : ni egale
       (aucune marge), ni degenere (proche de 0). */
    ASSERT(solo_budget < raw_budget);
    ASSERT(solo_budget > raw_budget / 10.0);

    border_ring_dp_set_max_ram_mo(2048, 4);
    PASS();
}

TEST bd_job_result_round_trips_through_a_file_when_closed(void)
{
    char path[] = "/tmp/etii_brd_result_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct bd_job_result written;
    memset(&written, 0, sizeof written);
    written.closed = 1;
    written.total = 4242;

    bd_job_result_write_or_die(&written, path);

    struct bd_job_result read_back;
    memset(&read_back, 0, sizeof read_back);
    ASSERT_EQ(0, bd_job_result_read(&read_back, path));
    ASSERT_EQ(1, read_back.closed);
    ASSERT_EQ_FMT(4242LL, read_back.total, "%lld");

    unlink(path);
    PASS();
}

TEST bd_job_result_round_trips_through_a_file_when_split(void)
{
    char path[] = "/tmp/etii_brd_result_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct bd_job_result written;
    memset(&written, 0, sizeof written);
    written.closed = 0;
    snprintf(written.shard_dir, sizeof written.shard_dir, "/tmp/etii_bd_test_dir");
    written.nb_shards = 7;
    written.resume_pos = 21;

    bd_job_result_write_or_die(&written, path);

    struct bd_job_result read_back;
    memset(&read_back, 0, sizeof read_back);
    ASSERT_EQ(0, bd_job_result_read(&read_back, path));
    ASSERT_EQ(0, read_back.closed);
    ASSERT_STR_EQ("/tmp/etii_bd_test_dir", read_back.shard_dir);
    ASSERT_EQ(7, read_back.nb_shards);
    ASSERT_EQ(21, read_back.resume_pos);

    unlink(path);
    PASS();
}

TEST bd_job_result_read_reports_failure_on_missing_file(void)
{
    ASSERT_EQ(-1, bd_job_result_read(&(struct bd_job_result){0}, "/tmp/etii_brd_does_not_exist"));
    PASS();
}

SUITE(border_ring_dp_suite)
{
    RUN_TEST(border_ring_count_dp_returns_zero_without_any_border_shaped_piece);
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_a_unique_ring);
    RUN_TEST(border_ring_count_dp_counts_class_multiplicity_correctly);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_forked);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_sharded_to_disk);
    RUN_TEST(border_ring_count_dp_matches_brute_force_when_pool_mode_engages);
#if ETERN_PARTS == 16
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_real_pieces16);
    RUN_TEST(border_ring_count_dp_matches_border_walk_count_on_real_pieces16_sharded_to_disk);
#endif
    RUN_TEST(bd_estimate_reload_bytes_matches_known_capacity_growth);
    RUN_TEST(bd_estimate_reload_bytes_matches_real_allocation_for_various_sizes);
    RUN_TEST(bd_should_run_solo_picks_mode_from_queue_depth);
    RUN_TEST(border_ring_dp_set_max_ram_mo_derives_a_conservative_solo_budget);
    RUN_TEST(bd_job_result_round_trips_through_a_file_when_closed);
    RUN_TEST(bd_job_result_round_trips_through_a_file_when_split);
    RUN_TEST(bd_job_result_read_reports_failure_on_missing_file);
}
