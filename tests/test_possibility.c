/*
 * Tests unitaires de la logique pure de possibility.c.
 *
 * possibility.c mêle de la logique pure (validation, comparaison, encodage de
 * paquets) à des chemins qui appellent exit() (checkIfResultFound,
 * first_possibility) ou écrivent des fichiers. On ne teste ici QUE la logique
 * pure, sur des `possibility_packet` construits à la main (calloc → état zéro).
 *
 * Le seul symbole externe de datamanager appelé par le module (add_possibility)
 * est fourni en no-op par tests/stubs.c, ce qui évite de lier toute la chaîne
 * réseau.
 *
 * Build par défaut : ETERN_PARTS=256, ETERN_SIZE=16, FACES_USED_BITS actif
 * (masque de bits via set_face_used / is_face_used, déclarés inline dans
 * possibility.h). check_possibility impose en outre, en 256, la pièce genèse
 * 139 (rotation 2) en (7,8).
 */
#include "greatest.h"
#include "../possibility.h"
#include "../part.h"
#include "fork_assert.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* directions / dirx / diry sont des globaux définis dans static_variables.c
   (déclarés via static_variables.h, inclus par possibility.h). */

/* Petit tableau de rotations non vide pour passer un pointeur valide à
   check_possibility (évite la lecture de pieces.csv quand rotateParts != NULL). */
static struct array_part *make_dummy_rotate_parts(void)
{
    struct array_part *a = malloc(sizeof(struct array_part));
    a->size = 4;
    a->parts = calloc(a->size, sizeof(struct part));
    for (int i = 0; i < a->size; i++) {
        a->parts[i].id = i;
    }
    return a;
}

static struct possibility_packet *new_zeroed_packet(void)
{
    return calloc(1, sizeof(struct possibility_packet));
}

/* Contexte partagé avec les fonctions-fils (copié par fork). */
static struct possibility_packet *g_poss;
static struct array_part *g_rot;
static char g_solution_dir[256];
static char g_save_path[256];

/* Fonction-fils : grille complète -> checkIfResultFound exit(EXIT_SUCCESS).
   On se place d'abord dans un répertoire temporaire pour confiner le fichier
   ./solution_<pid> écrit avant l'exit. */
static void child_check_result_found(void)
{
    if (chdir(g_solution_dir) != 0) _exit(99);
    checkIfResultFound(g_poss, g_rot);
}

/* Fonction-fils : chemin non inscriptible -> save_possibility exit(EXIT_FAILURE). */
static void child_save_unwritable(void)
{
    save_possibility(g_save_path, g_poss);
}

/* --------------------------------------------------------------------------
 * test_directions / decode_direction
 * ------------------------------------------------------------------------ */

/* Le tableau directions couvre exactement toutes les cases [0, ETERN_PARTS[. */
TEST test_directions_covers_every_cell(void)
{
    ASSERT_EQ_FMT(0, test_directions(), "%d");
    PASS();
}

/* decode_direction n'a pas de valeur de retour signifiante (toujours 0) : on
   couvre simplement son exécution. */
TEST decode_direction_runs(void)
{
    ASSERT_EQ_FMT(0, decode_direction(), "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * check_possibility : codes de retour de validation
 * ------------------------------------------------------------------------ */

TEST check_possibility_null_packet_is_minus_one(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    ASSERT_EQ_FMT(-1, check_possibility(NULL, rp), "%d");
    free_array_part(rp);
    PASS();
}

TEST check_possibility_out_of_bounds_is_minus_two(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet();
    p->x = ETERN_SIZE; /* hors plateau */
    ASSERT_EQ_FMT(-2, check_possibility(p, rp), "%d");
    free(p);
    free_array_part(rp);
    PASS();
}

TEST check_possibility_alloc_too_large_is_minus_four(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = ETERN_PARTS + 1;
    ASSERT_EQ_FMT(-4, check_possibility(p, rp), "%d");
    free(p);
    free_array_part(rp);
    PASS();
}

TEST check_possibility_alloc_exceeds_faceused_is_minus_five(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 2;       /* prétend 2 pièces posées... */
    /* ...mais aucun bit faceused activé -> faceused(0) < alloc(2) */
    ASSERT_EQ_FMT(-5, check_possibility(p, rp), "%d");
    free(p);
    free_array_part(rp);
    PASS();
}

/* Paquet zéro (grid[7][8] == 0) : viole l'ancrage de la pièce genèse 139 r2. */
TEST check_possibility_missing_genesis_anchor_is_minus_six(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet(); /* grid[7][8] = 0 */
    ASSERT_EQ_FMT(-6, check_possibility(p, rp), "%d");
    free(p);
    free_array_part(rp);
    PASS();
}

/* Paquet genèse minimal valide : ancrage 139 r2 posé, alloc=0 (aucune case du
   parcours encore décidée) -> la boucle de cohérence ne s'exécute pas -> 0. */
TEST check_possibility_valid_genesis_is_zero(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet();
    p->grid[7][8] = id_for_rotated_part(139, 2);
    ASSERT_EQ_FMT(0, check_possibility(p, rp), "%d");
    free(p);
    free_array_part(rp);
    PASS();
}

/* --------------------------------------------------------------------------
 * compare_possibility : première différence rencontrée
 * ------------------------------------------------------------------------ */

TEST compare_possibility_detects_each_difference(void)
{
    struct possibility_packet *a = new_zeroed_packet();
    struct possibility_packet *b = new_zeroed_packet();

    /* identiques -> 0 */
    ASSERT_EQ_FMT(0, compare_possibility(a, b), "%d");

    /* alloc différent -> -2 */
    b->alloc = 1;
    ASSERT_EQ_FMT(-2, compare_possibility(a, b), "%d");
    b->alloc = 0;

    /* position différente -> -3 */
    b->x = 2;
    ASSERT_EQ_FMT(-3, compare_possibility(a, b), "%d");
    b->x = 0;

    /* masque de pièces utilisées différent -> -4 */
    set_face_used(b->b_faceused, 5, 1);
    ASSERT_EQ_FMT(-4, compare_possibility(a, b), "%d");
    set_face_used(b->b_faceused, 5, 0);

    /* grille différente -> -5 */
    b->grid[3][4] = 42;
    ASSERT_EQ_FMT(-5, compare_possibility(a, b), "%d");

    /* gestion du NULL : un seul NULL -> 0, deux NULL -> -1 */
    ASSERT_EQ_FMT(0, compare_possibility(a, NULL), "%d");
    ASSERT_EQ_FMT(-1, compare_possibility(NULL, NULL), "%d");

    free(a);
    free(b);
    PASS();
}

/* --------------------------------------------------------------------------
 * is_origin_of : relation d'ancêtre (préfixe de parcours)
 * ------------------------------------------------------------------------ */

TEST is_origin_of_recognizes_prefix(void)
{
    struct possibility_packet *anc = new_zeroed_packet();
    struct possibility_packet *desc = new_zeroed_packet();

    /* desc place une pièce sur la 1re case du parcours, anc non. */
    desc->grid[dirx[0]][diry[0]] = 77;
    anc->grid[dirx[0]][diry[0]] = 77;
    anc->alloc = 1;
    desc->alloc = 2; /* desc descend plus loin */

    ASSERT_EQ_FMT(1, is_origin_of(anc, desc), "%d"); /* ancêtre confirmé */

    /* alloc égal ou supérieur -> -1 */
    anc->alloc = 2;
    ASSERT_EQ_FMT(-1, is_origin_of(anc, desc), "%d");
    anc->alloc = 1;

    /* divergence sur une case allouée -> -2 */
    desc->grid[dirx[0]][diry[0]] = 99;
    ASSERT_EQ_FMT(-2, is_origin_of(anc, desc), "%d");

    /* NULL -> 0 */
    ASSERT_EQ_FMT(0, is_origin_of(NULL, desc), "%d");
    ASSERT_EQ_FMT(0, is_origin_of(anc, NULL), "%d");

    free(anc);
    free(desc);
    PASS();
}

/* --------------------------------------------------------------------------
 * build_single_array_possibility_packet / free
 * ------------------------------------------------------------------------ */

TEST build_single_array_wraps_one_packet(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 7;
    p->x = 3;

    array_possibility_packet *arr = build_single_array_possibility_packet(p);
    ASSERT(arr != NULL);
    ASSERT_EQ_FMT(1, arr->size, "%d");
    ASSERT_EQ_FMT(7, (int)arr->possibilities[0].alloc, "%d"); /* copie fidèle */
    ASSERT_EQ_FMT(3, (int)arr->possibilities[0].x, "%d");
    free_array_possibility_packet(arr);

    /* NULL -> tableau de taille 0 */
    array_possibility_packet *empty = build_single_array_possibility_packet(NULL);
    ASSERT_EQ_FMT(0, empty->size, "%d");
    free_array_possibility_packet(empty);

    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * generate_possibility_packet : encodage de la grille
 * ------------------------------------------------------------------------ */

TEST generate_possibility_packet_encodes_grid(void)
{
    struct part *etern[ETERN_SIZE][ETERN_SIZE];
    memset(etern, 0, sizeof(etern));

    struct part piece = { .id = 5, .rotation = 1 };
    etern[2][3] = &piece;

    struct possibility_packet *p = generate_possibility_packet(2, 3, etern, DIR_RIGHT);
    ASSERT(p != NULL);
    ASSERT_EQ_FMT(2, (int)p->x, "%d");
    ASSERT_EQ_FMT(3, (int)p->y, "%d");

    /* case occupée : encodée en id_for_rotated_part */
    ASSERT_EQ_FMT((int)id_for_rotated_part(5, 1), (int)p->grid[2][3], "%d");
    /* la pièce id=5 est marquée utilisée (base 0 : id-1 = 4) */
    ASSERT_EQ_FMT(1, (int)is_face_used(p->b_faceused, 4), "%d");
    /* case vide : -2 */
    ASSERT_EQ_FMT(-2, (int)p->grid[0][0], "%d");

    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * normalize_possibility_packet : réparation de l'invariant de parcours
 * ------------------------------------------------------------------------ */

/* alloc en avance sur la première case vide -> reculé et (x,y) repositionné. */
TEST normalize_repairs_alloc_ahead_of_hole(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    /* grille entièrement vide (-2 partout) -> firstHole = 0 */
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    p->alloc = 5;
    p->x = 9;
    p->y = 9;

    ASSERT_EQ_FMT(1, normalize_possibility_packet(p), "%d"); /* réparé */
    ASSERT_EQ_FMT(0, (int)p->alloc, "%d");                   /* reculé sur le trou */
    ASSERT_EQ_FMT((int)dirx[0], (int)p->x, "%d");
    ASSERT_EQ_FMT((int)diry[0], (int)p->y, "%d");

    free(p);
    PASS();
}

/* Paquet déjà conforme (aucun trou avant alloc, (x,y) = directions[alloc]) -> 0. */
TEST normalize_leaves_conforming_packet_untouched(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    /* grille sans aucune case vide (0 partout, pas -2) -> firstHole = ETERN_PARTS */
    p->alloc = 3;
    p->x = dirx[3];
    p->y = diry[3];

    ASSERT_EQ_FMT(0, normalize_possibility_packet(p), "%d"); /* rien à réparer */
    ASSERT_EQ_FMT(3, (int)p->alloc, "%d");

    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * save_possibility : sérialisation binaire
 * ------------------------------------------------------------------------ */

TEST save_possibility_writes_packet_to_file(void)
{
    char path[] = "/tmp/etii_save_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 42;
    p->x = 5;
    ASSERT_EQ_FMT(0, save_possibility(path, p), "%d");

    /* relecture : les octets écrits doivent reconstituer le paquet. */
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    struct possibility_packet q;
    size_t n = fread(&q, sizeof(q), 1, f);
    fclose(f);
    unlink(path);

    ASSERT_EQ_FMT(1, (int)n, "%d");
    ASSERT_EQ_FMT(42, (int)q.alloc, "%d");
    ASSERT_EQ_FMT(5, (int)q.x, "%d");

    free(p);
    PASS();
}

/* Chemin non inscriptible -> exit(EXIT_FAILURE), testé en fork. */
TEST save_possibility_unwritable_exits(void)
{
    strcpy(g_save_path, "/etii_nonexistent_dir_zzz/out.bin");
    g_poss = new_zeroed_packet();
    int code = run_in_fork(child_save_unwritable, NULL);
    free(g_poss);
    ASSERT_EQ_FMT(EXIT_FAILURE, code, "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * checkIfResultFound : détection de solution complète
 * ------------------------------------------------------------------------ */

/* Grille incomplète (alloc < ETERN_PARTS) : ne fait rien, ne sort pas. */
TEST check_if_result_found_below_complete_is_noop(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 1;
    struct array_part *rp = make_dummy_rotate_parts();

    checkIfResultFound(p, rp); /* doit revenir sans exit ni crash */

    free(p);
    free_array_part(rp);
    PASS();
}

/* Grille complète (alloc == ETERN_PARTS) : exit(EXIT_SUCCESS), testé en fork. */
TEST check_if_result_found_complete_exits_success(void)
{
    strcpy(g_solution_dir, "/tmp/etii_sol_XXXXXX");
    ASSERT(mkdtemp(g_solution_dir) != NULL);

    g_poss = new_zeroed_packet();
    g_poss->alloc = ETERN_PARTS;      /* grille « complète » */
    g_rot = make_dummy_rotate_parts(); /* parts[0] valide (grid zéro -> index 0) */

    pid_t pid = 0;
    int code = run_in_fork(child_check_result_found, &pid);

    /* Nettoyage du fichier solution confiné dans le répertoire temporaire. */
    char sol[320];
    snprintf(sol, sizeof(sol), "%s/solution_%d", g_solution_dir, (int)pid);
    unlink(sol);
    rmdir(g_solution_dir);

    free(g_poss);
    free_array_part(g_rot);
    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d");
    PASS();
}

SUITE(possibility_suite)
{
    RUN_TEST(test_directions_covers_every_cell);
    RUN_TEST(decode_direction_runs);
    RUN_TEST(check_possibility_null_packet_is_minus_one);
    RUN_TEST(check_possibility_out_of_bounds_is_minus_two);
    RUN_TEST(check_possibility_alloc_too_large_is_minus_four);
    RUN_TEST(check_possibility_alloc_exceeds_faceused_is_minus_five);
    RUN_TEST(check_possibility_missing_genesis_anchor_is_minus_six);
    RUN_TEST(check_possibility_valid_genesis_is_zero);
    RUN_TEST(compare_possibility_detects_each_difference);
    RUN_TEST(is_origin_of_recognizes_prefix);
    RUN_TEST(build_single_array_wraps_one_packet);
    RUN_TEST(generate_possibility_packet_encodes_grid);
    RUN_TEST(normalize_repairs_alloc_ahead_of_hole);
    RUN_TEST(normalize_leaves_conforming_packet_untouched);
    RUN_TEST(save_possibility_writes_packet_to_file);
    RUN_TEST(save_possibility_unwritable_exits);
    RUN_TEST(check_if_result_found_below_complete_is_noop);
    RUN_TEST(check_if_result_found_complete_exits_success);
}
