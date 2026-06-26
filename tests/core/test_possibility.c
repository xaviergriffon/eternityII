/*
 * Tests unitaires de la logique pure de possibility.c.
 *
 * possibility.c mêle de la logique pure (validation, comparaison, encodage de
 * paquets) à des chemins qui appellent exit() (checkIfResultFound,
 * first_possibility) ou écrivent des fichiers. On ne teste ici QUE la logique
 * pure, sur des `possibility_packet` construits à la main (calloc → état zéro).
 *
 * Les fonctions de recherche (what_search*, possibility_has_a_next) sont
 * exercées sur de petits `array_part` / `map_big_array` construits à la main
 * (coordonnées explicites), donc indépendamment du parcours `directions` à 256
 * cases et sans pieces.csv.
 *
 * Build par défaut : ETERN_PARTS=256, ETERN_SIZE=16 actif
 * (masque de bits via set_face_used / is_face_used, déclarés inline dans
 * possibility.h). check_possibility impose en outre, en 256, la pièce genèse
 * 139 (rotation 2) en (7,8).
 */
#include "greatest.h"
#include "core/possibility.h"
#include "core/part.h"
#include "core/datamanager.h"
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

/* Fonction-fils : deux solutions consécutives dans un répertoire temporaire.
   log_solution ne quitte PAS ; on revient et _exit(0). Le test vérifie ensuite
   que deux fichiers distincts (solution_<pid>_0 et _1) ont été écrits. */
static void child_two_solutions(void)
{
    if (chdir(g_solution_dir) != 0) _exit(99);
    log_solution(g_poss, g_rot);
    log_solution(g_poss, g_rot);
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

#if ETERN_PARTS == 256
/* Paquet zéro (grid[7][8] == 0) : viole l'ancrage de la pièce genèse 139 r2.
   L'ancrage genèse n'existe qu'en puzzle 256 (cf. #if dans check_possibility). */
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
#endif /* ETERN_PARTS == 256 */

#if ETERN_PARTS != 256
/* En 4×4, check_possibility n'a pas l'ancrage genèse 256 (return -6) : on atteint
   donc la boucle de cohérence. La traversée commence en (0,0) (dirx[0]=diry[0]=0).
   Ces cas valident les rejets que jamais un solve valide ne produit (les `return
   -9` et `-7` ne sont couverts par aucun build sinon). */

/* Valeur de grille invalide (< 0) à la 1re case du parcours -> -7. */
TEST check_possibility_invalid_grid_value_is_minus_seven(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 1;
    set_face_used(p->b_faceused, 0, 1); /* faceused (1) >= alloc (1) */
    p->grid[0][0] = -1;                 /* (dirx[0],diry[0]) = case invalide */
    ASSERT_EQ_FMT(-7, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* Pièce en (0,0) dont la face TOP != bord (0) -> incohérence de bord -> -9. */
TEST check_possibility_border_mismatch_is_minus_nine(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 5, .right = 0, .bottom = 0, .left = 0 }, /* top=5 != bord */
    };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 1;
    set_face_used(p->b_faceused, 0, 1);
    p->grid[0][0] = 1; /* bord haut attendu = 0, mais la pièce a top = 5 */
    ASSERT_EQ_FMT(-9, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* Pièce en (0,0), bords corrects, mais son voisin de DROITE ne s'emboîte pas
   (P.right != Q.left) -> incohérence -> -9. */
TEST check_possibility_neighbor_mismatch_is_minus_nine(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 0, .left = 0 }, /* P : bords OK, right=2 */
        { .id = 2, .top = 0, .right = 0, .bottom = 0, .left = 9 }, /* Q : left=9 != 2 */
    };
    struct array_part rp = { .size = 3, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 1;
    set_face_used(p->b_faceused, 0, 1);
    p->grid[0][0] = 1; /* P, seule case visitée (alloc=1) */
    p->grid[1][0] = 2; /* Q à droite, voisin incohérent */
    ASSERT_EQ_FMT(-9, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}
#endif /* ETERN_PARTS != 256 */

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

    /* grille différente -> -5 (case (2,3) : valide en 4×4 comme en 16×16) */
    b->grid[2][3] = 42;
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
 * save_solution_csv : sérialisation CSV
 * ------------------------------------------------------------------------ */

TEST save_solution_csv_writes_header_and_rows(void)
{
    char path[] = "/tmp/etii_csv_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet();
    /* initialize all cells as empty (-2), then place 2 pieces */
    for (int i = 0; i < ETERN_SIZE; i++)
        for (int j = 0; j < ETERN_SIZE; j++)
            p->grid[i][j] = -2;
    p->grid[0][0] = 0;
    p->grid[0][1] = 0;
    p->alloc = 2;

    ASSERT_EQ_FMT(0, save_solution_csv(path, p, rp), "%d");

    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    char line[256];

    /* première ligne = en-tête */
    ASSERT(fgets(line, sizeof(line), f) != NULL);
    ASSERT_STR_EQ("row,col,piece_id,rotation,top,right,bottom,left\n", line);

    /* deux lignes de données */
    ASSERT(fgets(line, sizeof(line), f) != NULL); /* (0,0) */
    ASSERT(fgets(line, sizeof(line), f) != NULL); /* (0,1) */
    ASSERT(fgets(line, sizeof(line), f) == NULL); /* EOF */

    fclose(f);
    unlink(path);
    free(p);
    free_array_part(rp);
    PASS();
}

TEST save_solution_csv_null_parts_writes_minus_one_faces(void)
{
    char path[] = "/tmp/etii_csvn_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct possibility_packet *p = new_zeroed_packet();
    p->grid[1][2] = 5; /* rotation-index 5 → piece_id=5%ETERN_PARTS, rotation=5/ETERN_PARTS */
    p->alloc = 1;

    ASSERT_EQ_FMT(0, save_solution_csv(path, p, NULL), "%d");

    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    char line[256];
    ASSERT(fgets(line, sizeof(line), f) != NULL); /* skip header (présent) */
    ASSERT(fgets(line, sizeof(line), f) != NULL);
    fclose(f);
    unlink(path);
    free(p);

    /* la ligne doit se terminer par …,-1,-1,-1,-1 */
    ASSERT(strstr(line, ",-1,-1,-1,-1") != NULL);
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

    /* Nettoyage du fichier solution confiné dans le répertoire temporaire.
       log_solution écrit un nom unique solution_<pid>_<seq> (seq=0 pour le 1er
       et seul appel de ce fils fraîchement forké). */
    char sol[320];
    snprintf(sol, sizeof(sol), "%s/solution_%d_0.csv", g_solution_dir, (int)pid);
    unlink(sol);
    rmdir(g_solution_dir);

    free(g_poss);
    free_array_part(g_rot);
    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d");
    PASS();
}

/* Régression : deux solutions trouvées par le même processus ne doivent PAS
   s'écraser. log_solution numérote chaque fichier (solution_<pid>_<seq>). On
   appelle log_solution deux fois dans un fils (process frais -> seq 0 puis 1) et
   on vérifie que les DEUX fichiers existent et sont distincts. */
TEST log_solution_writes_distinct_files_for_each_solution(void)
{
    strcpy(g_solution_dir, "/tmp/etii_sol2_XXXXXX");
    ASSERT(mkdtemp(g_solution_dir) != NULL);

    g_poss = new_zeroed_packet();
    g_poss->alloc = ETERN_PARTS;
    g_rot = make_dummy_rotate_parts();

    pid_t pid = 0;
    int code = run_in_fork(child_two_solutions, &pid);

    char f0[320], f1[320];
    snprintf(f0, sizeof(f0), "%s/solution_%d_0.csv", g_solution_dir, (int)pid);
    snprintf(f1, sizeof(f1), "%s/solution_%d_1.csv", g_solution_dir, (int)pid);
    int has0 = (access(f0, F_OK) == 0);
    int has1 = (access(f1, F_OK) == 0);
    unlink(f0);
    unlink(f1);
    rmdir(g_solution_dir);

    free(g_poss);
    free_array_part(g_rot);

    ASSERT_EQ_FMT(0, code, "%d");   /* log_solution ne quitte pas : le fils revient */
    ASSERT(has0);                   /* 1re solution écrite */
    ASSERT(has1);                   /* 2e solution : nom distinct, aucun écrasement */
    PASS();
}

/* --------------------------------------------------------------------------
 * what_search / what_search_to_key2 / what_search_in_grid_to_key
 *
 * Petit jeu de pièces dont seuls les bords comptent. grid stocke ici des
 * indices directs dans all_rotate_parts->parts[] (ce qu'attendent ces
 * fonctions). Les voisins absents (bord de grille) donnent 0 ; les cases vides
 * (-2) donnent -1 (what_search) ou `all_face` (variantes ...to_key).
 * ------------------------------------------------------------------------ */

/* Au coin (0,0) d'un plateau vide : top/left = bord (0), right/bottom = vide (-1). */
TEST what_search_corner_of_empty_board(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 },
        { .id = 2, .top = 1, .right = 9, .bottom = 4, .left = 5 },
    };
    struct array_part rp = { .size = 3, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    key_part k = what_search(&rp, 0, 0, p);
    ASSERT_EQ_FMT(0, (int)k.k1, "%d");  /* TOP : bord */
    ASSERT_EQ_FMT(-1, (int)k.k2, "%d"); /* RIGHT : vide */
    ASSERT_EQ_FMT(-1, (int)k.k3, "%d"); /* BOTTOM : vide */
    ASSERT_EQ_FMT(0, (int)k.k4, "%d");  /* LEFT : bord */

    free(p);
    PASS();
}

/* Un voisin placé impose le bord opposé : voisin de droite -> sa face gauche. */
TEST what_search_reads_placed_neighbor(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 5 }, /* left = 5 */
    };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->grid[1][0] = 1; /* voisin de droite de (0,0) = index 1 */

    key_part k = what_search(&rp, 0, 0, p);
    ASSERT_EQ_FMT(5, (int)k.k2, "%d"); /* RIGHT = left du voisin droit */
    ASSERT_EQ_FMT(0, (int)k.k1, "%d"); /* TOP toujours bord */

    free(p);
    PASS();
}

/* Coin bas-droit avec voisins placés : RIGHT et BOTTOM sont des bords (0),
   TOP/LEFT lisent les faces des voisins du dessus/de gauche. Couvre les branches
   « bord droit/bas » et « voisin non-bord placé » de what_search (toutes
   atteignables, indépendantes de la taille du plateau). */
TEST what_search_bottom_right_with_neighbors(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 8, .left = 4 }, /* dessus : bottom = 8 */
        { .id = 2, .top = 1, .right = 6, .bottom = 3, .left = 5 }, /* gauche : right = 6  */
    };
    struct array_part rp = { .size = 3, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    int last = ETERN_SIZE - 1;
    p->grid[last][last - 1] = 1; /* voisin du dessus  */
    p->grid[last - 1][last] = 2; /* voisin de gauche  */

    key_part k = what_search(&rp, last, last, p);
    ASSERT_EQ_FMT(8, (int)k.k1, "%d"); /* TOP  = bottom du voisin du dessus */
    ASSERT_EQ_FMT(0, (int)k.k2, "%d"); /* RIGHT = bord (x+1 hors plateau)   */
    ASSERT_EQ_FMT(0, (int)k.k3, "%d"); /* BOTTOM = bord (y+1 hors plateau)  */
    ASSERT_EQ_FMT(6, (int)k.k4, "%d"); /* LEFT = right du voisin de gauche  */

    free(p);
    PASS();
}

/* Case intérieure (1,1) entourée de cases vides : les 4 voisins existent mais
   sont vides (-2) -> les 4 faces valent -1. Couvre les branches « voisin
   non-bord vide » TOP et LEFT de what_search. */
TEST what_search_interior_empty_neighbors(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 4 } };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    key_part k = what_search(&rp, 1, 1, p);
    ASSERT_EQ_FMT(-1, (int)k.k1, "%d"); /* TOP : voisin (1,0) vide   */
    ASSERT_EQ_FMT(-1, (int)k.k2, "%d"); /* RIGHT : voisin (2,1) vide */
    ASSERT_EQ_FMT(-1, (int)k.k3, "%d"); /* BOTTOM : voisin (1,2) vide */
    ASSERT_EQ_FMT(-1, (int)k.k4, "%d"); /* LEFT : voisin (0,1) vide  */

    free(p);
    PASS();
}

/* what_search_to_key2 : case courante, voisins vides encodés en `all_face`. */
TEST what_search_to_key2_uses_all_face_for_empty(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 0, .right = 1, .bottom = 1, .left = 0 } };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0;
    p->y = 0;

    key_part k;
    const int8_t all_face = 7;
    what_search_to_key2(&rp, p, &k, all_face);
    ASSERT_EQ_FMT(0, (int)k.k1, "%d");          /* TOP bord */
    ASSERT_EQ_FMT(all_face, (int)k.k2, "%d");   /* RIGHT vide -> all_face */
    ASSERT_EQ_FMT(all_face, (int)k.k3, "%d");   /* BOTTOM vide -> all_face */
    ASSERT_EQ_FMT(0, (int)k.k4, "%d");          /* LEFT bord */

    free(p);
    PASS();
}

/* what_search_in_grid_to_key : même chose pour une case (x,y) arbitraire. */
TEST what_search_in_grid_to_key_arbitrary_cell(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 4, .right = 1, .bottom = 1, .left = 6 } };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    /* place un voisin sous la case (1,1) : grid[1][2] index 1, son top = 4 -> k3
       (coordonnées valides en 4×4 comme en 16×16) */
    p->grid[1][2] = 1;

    key_part k;
    what_search_in_grid_to_key(&rp, p, 1, 1, &k, 7);
    ASSERT_EQ_FMT(7, (int)k.k1, "%d"); /* TOP vide -> all_face */
    ASSERT_EQ_FMT(4, (int)k.k3, "%d"); /* BOTTOM = top du voisin du dessous */

    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * possibility_has_a_next : au moins une pièce libre posable sur la case courante
 * ------------------------------------------------------------------------ */

/*
 * get_parts_bigarray indexe la table à plat avec les valeurs brutes de la clé
 * (sans convert_p), donc possibility_has_a_next ne donne un résultat valide que
 * si la case courante a une clé CONCRÈTE (pas de -1 « voisin vide »). On entoure
 * donc le coin (0,0) de voisins déjà placés : top/left = bord (0), right/bottom
 * = faces des voisins -> clé exacte (0, 2, 3, 0).
 */
TEST possibility_has_a_next_finds_and_excludes_used(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* coin cible */
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 2 }, /* voisin droit (left=2 -> k2) */
        { .id = 3, .top = 3, .right = 8, .bottom = 9, .left = 4 }, /* voisin bas (top=3 -> k3) */
    };
    struct array_part rp = { .size = 4, .parts = parts };
    int maxFace = search_max_face(&rp);
    map_big_array *map = buildBigArray(&rp, maxFace);

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0;
    p->y = 0;
    p->grid[1][0] = 2; /* voisin droit placé -> k2 = parts[2].left = 2 */
    p->grid[0][1] = 3; /* voisin bas placé   -> k3 = parts[3].top  = 3 */

    /* clé concrète (0,2,3,0) : seule la pièce coin 1 correspond, non utilisée. */
    ASSERT_EQ_FMT(1, possibility_has_a_next(p, map, &rp), "%d");

    /* On marque la pièce 1 (id-1 = 0) comme utilisée : plus aucune candidate. */
    set_face_used(p->b_faceused, 0, 1);
    ASSERT_EQ_FMT(0, possibility_has_a_next(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * search_possiblity_light : expansion d'un paquet (File de résultats)
 * ------------------------------------------------------------------------ */

/* Remplit idParts[p][r] = p + ETERN_PARTS*r (encodage des rotations). */
static void fill_id_parts(int16_t idParts[ETERN_PARTS][4])
{
    for (int p = 0; p < ETERN_PARTS; p++)
        for (int r = 0; r < 4; r++)
            idParts[p][r] = (int16_t)(p + ETERN_PARTS * r);
}

/* Deux pièces de mêmes bords -> deux successeurs générés sur la case vide. */
TEST search_light_expands_one_per_candidate(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 },
        { .id = 2, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* mêmes bords */
    };
    struct array_part rp = { .size = 3, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0; p->alloc = 0;

    key_part key = { .k1 = 0, .k2 = 2, .k3 = 3, .k4 = 0 }; /* match exact des 2 pièces */
    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    File result;
    init_file_with_cache(&result, 0, sizeof(struct possibility_packet));

    int max = search_possiblity_light(&result, &key, p, map, &rp, idParts);
    ASSERT_EQ_FMT(1, max, "%d");                 /* incAlloc = 0 + 1 */
    ASSERT_EQ_FMT(2ULL, (unsigned long long)result.size, "%llu"); /* un paquet par pièce */

    /* Le premier successeur a la case (0,0) remplie et alloc = 1. */
    struct possibility_packet out;
    scroll(&result, &out);
    ASSERT_EQ_FMT(1, (int)out.alloc, "%d");
    ASSERT(out.grid[0][0] != -2);
    while (result.size > 0) scroll(&result, &out); /* draine le reste */

    free_bigarray(map);
    free(p);
    PASS();
}

/* Case déjà remplie (indice fixe) : le paquet est simplement avancé. */
TEST search_light_skips_prefilled_cell(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0; p->alloc = 0;
    p->grid[0][0] = 1; /* case courante déjà remplie (!= -2) */

    key_part key = { .k1 = 0, .k2 = 2, .k3 = 3, .k4 = 0 };
    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    File result;
    init_file_with_cache(&result, 0, sizeof(struct possibility_packet));

    int max = search_possiblity_light(&result, &key, p, map, &rp, idParts);
    ASSERT_EQ_FMT(1, max, "%d");
    ASSERT_EQ_FMT(1ULL, (unsigned long long)result.size, "%llu"); /* un seul paquet avancé */

    struct possibility_packet out;
    scroll(&result, &out);
    ASSERT_EQ_FMT(1, (int)out.alloc, "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * forward_check_next_k : élagage des branches mortes
 * ------------------------------------------------------------------------ */

/* Une case libre sans aucune pièce candidate -> branche morte (0). */
TEST forward_check_detects_dead_cell(void)
{
    /* pièce non-coin (top=5) : la case (0,0) exige top=0 (bord) -> aucun candidat */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 5, .right = 6, .bottom = 7, .left = 8 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 0; /* la fenêtre commence à la case directions[0] = (0,0) */

    ASSERT_EQ_FMT(0, forward_check_next_k(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Toutes les cases de la fenêtre sont déjà remplies -> rien à élaguer (1). */
TEST forward_check_passes_when_cells_filled(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    /* grille toute à 0 (pas de -2) -> chaque case de la fenêtre est « remplie ». */
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 0;

    ASSERT_EQ_FMT(1, forward_check_next_k(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * search_possiblity_light_with_big_table : expansion + forward-check
 * ------------------------------------------------------------------------ */

/* Les pièces coins placées laissent des cases aval sans candidat : le
   forward-check élague toutes les branches -> big_table vide, retour 0. */
TEST search_big_table_prunes_dead_branches(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 },
        { .id = 2, .top = 0, .right = 2, .bottom = 3, .left = 0 },
    };
    struct array_part rp = { .size = 3, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0; p->alloc = 0;

    key_part key = { .k1 = 0, .k2 = 2, .k3 = 3, .k4 = 0 };
    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    big_table result;
    init_big_table(&result, 4, sizeof(struct possibility_packet));

    int ret = search_possiblity_light_with_big_table(&result, &key, p, map, &rp, idParts);
    ASSERT_EQ_FMT(0, ret, "%d");                              /* aucune branche retenue */
    ASSERT_EQ_FMT(0ULL, (unsigned long long)result.size, "%llu");

    clear_big_table(&result);
    free_bigarray(map);
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * what_search_to_key : variante écrivant la clé, all_face=-1 pour voisins vides
 * ------------------------------------------------------------------------ */

TEST what_search_to_key_empty_and_placed_neighbor(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 9 }, /* right=2 -> k2 */
    };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0;

    key_part k;
    what_search_to_key(&rp, p, &k, -1);
    ASSERT_EQ_FMT(0, (int)k.k1, "%d");  /* TOP bord */
    ASSERT_EQ_FMT(-1, (int)k.k2, "%d"); /* RIGHT vide -> -1 */
    ASSERT_EQ_FMT(-1, (int)k.k3, "%d"); /* BOTTOM vide -> -1 */
    ASSERT_EQ_FMT(0, (int)k.k4, "%d");  /* LEFT bord */

    /* place un voisin à droite : k2 = sa face gauche */
    p->grid[1][0] = 1; /* parts[1].left = 9 */
    what_search_to_key(&rp, p, &k, -1);
    ASSERT_EQ_FMT(9, (int)k.k2, "%d");

    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * print_possibility_packet : sérialisation JSON dans les logs (retour 0)
 * ------------------------------------------------------------------------ */

TEST print_possibility_packet_runs(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 3; p->x = 1; p->y = 2;

    int saved = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);

    int ret = print_possibility_packet(p);

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(devnull);

    ASSERT_EQ_FMT(0, ret, "%d");
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * possibility_all_has_a_next : toutes les cases libres ont-elles une suite ?
 * ------------------------------------------------------------------------ */

/* Grille entièrement « remplie » (aucune case -2) : rien à vérifier -> 1. */
TEST all_has_a_next_all_filled_returns_one(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet(); /* grid tout à 0 (pas de -2) */
    p->alloc = 0;

    ASSERT_EQ_FMT(1, possibility_all_has_a_next(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Une case libre dont la clé n'a aucune pièce candidate -> impasse (0). */
TEST all_has_a_next_dead_cell_returns_zero(void)
{
    /* pièce de bords 1 : le compartiment (0,0,0,0) reste vide. */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    /* grille « remplie » de 0 sauf la 1re case du parcours, laissée vide. */
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 0;
    p->grid[dirx[0]][diry[0]] = -2; /* voisins = parts[0] (bords 0) -> clé (0,0,0,0) */

    ASSERT_EQ_FMT(0, possibility_all_has_a_next(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

#if ETERN_PARTS == 256
/* ------------------------------------------------------------------------- *
 *  Map synthétique 256 pièces (sans pieces.csv) pour tester first_possibility *
 * ------------------------------------------------------------------------- *
 *
 * first_possibility (corps sous #if ETERN_PARTS == 256) place 5 pièces-indices
 * — 139, 208, 255, 181, 249 — via get_one_part sur des clés fixes, puis
 * développe la première case libre. La tester exige une vraie map 256, donc
 * jusqu'ici le CSV en CWD : on s'en affranchit avec une map « synthétique ».
 *
 * Algorithme : 256 pièces base (rotation 0) ; 5 portent EXACTEMENT les faces que
 * les clés recherchent (139 calibrée pour matcher en rotation 2, afin que la
 * grille y stocke id_for_rotated_part(139,2) == 651, l'ancrage exigé par
 * check_possibility) ; les 251 autres sont des tuiles intérieures identiques
 * (4,4,4,4) — couleur 4 absente de toutes les clés, et sans face bordure (0) :
 * aucune pièce n'est un coin, donc le développement de (0,0) [clé {0, libre,
 * libre, 0}] ne trouve aucun candidat et reste borné (pas d'explosion).
 *
 * Rappel rotation (rotatePart) : une rotation envoie (T,R,B,L) -> (L,T,R,B),
 * donc la rotation 2 donne (B,L,T,R). Pour que 139 présente {2,15,15,3} en r2,
 * sa base doit être (T=15, R=3, B=2, L=15).
 *
 * Le pipeline réel rotate_all_parts -> prepare_map_part est légitime ici : sous
 * ce #if, ETERN_PARTS vaut bien 256, donc l'indexation i + 256*r est correcte.
 */

/* Faces base (rotation 0) des pièces-indices ; commentaire = clé résolue. */
struct syn_index_face { int id; int8_t top, right, bottom, left; };
static const struct syn_index_face SYN_INDEX_FACES[] = {
    {139, 15,  3,  2, 15}, /* rotation 2 -> {2,15,15,3} (part_139_i8)            */
    {208, 13, 12,  3,  1}, /* rotation 0 -> {13,12,3,1}                          */
    {255, 13, 11, 13,  7}, /* rotation 0 -> {13,11,13,7}                         */
    {181,  7, 15,  5,  3}, /* rotation 0 -> {7,15,5,3}                           */
    {249,  8,  5,  9, 10}, /* rotation 0 -> {8,5,9,10}                           */
};

/* Construit l'array_part 256 base (mêmes conventions que read_parts : size=256,
 * parts[0] bouchon id 0, pièces réelles en 1..256). Si omit_id != 0, la
 * pièce-indice correspondante GARDE ses faces de filler : sa clé ne résout plus
 * (get_one_part -> NULL), ce qui sert à exercer les chemins fataux. */
static struct array_part *make_synthetic_base_256(int omit_id)
{
    struct array_part *a = malloc(sizeof *a);
    a->size = ETERN_PARTS;
    a->parts = calloc(ETERN_PARTS + 1, sizeof(struct part)); /* indices 0..256 */
    for (int i = 1; i <= ETERN_PARTS; i++) {
        a->parts[i].id = i;
        a->parts[i].rotation = 0;
        a->parts[i].top = a->parts[i].right = a->parts[i].bottom = a->parts[i].left = 4;
    }
    for (size_t k = 0; k < sizeof SYN_INDEX_FACES / sizeof SYN_INDEX_FACES[0]; k++) {
        const struct syn_index_face *s = &SYN_INDEX_FACES[k];
        if (s->id == omit_id) continue; /* laissé filler -> clé non résolue */
        a->parts[s->id].top    = s->top;
        a->parts[s->id].right  = s->right;
        a->parts[s->id].bottom = s->bottom;
        a->parts[s->id].left   = s->left;
    }
    return a;
}

/* Contrat de la map synthétique : chacune des 5 clés des indices résout vers une
 * pièce UNIQUE (get_one_part != NULL), 139 spécifiquement en rotation 2 (id
 * grille 651). C'est le socle des futurs tests de first_possibility. */
TEST synthetic_map_resolves_each_index_key(void)
{
    struct array_part *base = make_synthetic_base_256(0);
    struct array_part *rot  = rotate_all_parts(base);
    map_big_array     *map  = prepare_map_part(rot);

    struct part *p139 = get_one_part(map, (key_part){2, 15, 15, 3});
    ASSERT(p139 != NULL);
    ASSERT_EQ_FMT(139, (int)p139->id, "%d");
    ASSERT_EQ_FMT(2, (int)p139->rotation, "%d");
    ASSERT_EQ_FMT((int)id_for_rotated_part(139, 2),
                  (int)id_for_rotated_part(p139->id, p139->rotation), "%d");

    struct part *p208 = get_one_part(map, (key_part){13, 12, 3, 1});
    ASSERT(p208 != NULL); ASSERT_EQ_FMT(208, (int)p208->id, "%d");
    struct part *p255 = get_one_part(map, (key_part){13, 11, 13, 7});
    ASSERT(p255 != NULL); ASSERT_EQ_FMT(255, (int)p255->id, "%d");
    struct part *p181 = get_one_part(map, (key_part){7, 15, 5, 3});
    ASSERT(p181 != NULL); ASSERT_EQ_FMT(181, (int)p181->id, "%d");
    struct part *p249 = get_one_part(map, (key_part){8, 5, 9, 10});
    ASSERT(p249 != NULL); ASSERT_EQ_FMT(249, (int)p249->id, "%d");

    free_bigarray(map);
    free_array_part(rot);
    free_array_part(base);
    PASS();
}

/* Omettre une pièce-indice rend SA clé non résolue (NULL) sans affecter les
 * autres : c'est exactement l'entrée des chemins fataux de first_possibility. */
TEST synthetic_map_omitting_index_yields_null_key(void)
{
    struct array_part *base = make_synthetic_base_256(208);
    struct array_part *rot  = rotate_all_parts(base);
    map_big_array     *map  = prepare_map_part(rot);

    ASSERT(get_one_part(map, (key_part){13, 12, 3, 1}) == NULL); /* 208 absent  */
    ASSERT(get_one_part(map, (key_part){2, 15, 15, 3}) != NULL); /* 139 présent */

    free_bigarray(map);
    free_array_part(rot);
    free_array_part(base);
    PASS();
}

/* ------------------------------------------------------------------------- *
 *  first_possibility : chemins fataux + injection (via la map synthétique)    *
 * ------------------------------------------------------------------------- *
 *
 * first_possibility mute l'état global (datamanager, non_null_possibilities) et
 * peut appeler exit() (fatal_error) sur pièce-indice introuvable. On l'exécute
 * donc en processus fils (run_in_fork) : exit() n'y tue que le fils, et le fork
 * isole les effets de bord globaux du runner. Les assertions du chemin valide
 * sont encodées dans le code de sortie du fils. */

/* Id de la pièce-indice à omettre (lu par run_fp_omit dans le fils via fork). */
static int g_fp_omit_id;

/* Omet une pièce-indice : sa clé ne résout plus -> first_possibility doit appeler
 * fatal_error (part_139_i8 pour la genèse 139, get_one_part inline pour les 4
 * autres) -> exit(EXIT_FAILURE). */
static void run_fp_omit(void)
{
    struct array_part *base = make_synthetic_base_256(g_fp_omit_id);
    struct array_part *rot  = rotate_all_parts(base);
    map_big_array     *map  = prepare_map_part(rot);
    first_possibility(map, rot); /* doit exit(EXIT_FAILURE) avant de revenir */
}

/* Map valide + UN coin posable en (0,0) : first_possibility développe la case,
 * valide l'unique résultat (check_possibility/normalize) et l'injecte dans le
 * datamanager. Effets attendus : non_null_possibilities += 2 (genèse + résultat)
 * et exactement 1 possibilité injectée (datas_size +1). Encodés en code retour. */
static void run_fp_valid_injects(void)
{
    struct array_part *base = make_synthetic_base_256(0);
    /* Transforme la tuile filler id=1 en coin : top/left bord (0), right/bottom
       couleurs inédites (6,14) absentes des clés -> aucune rotation ne collisionne
       avec un indice, et seule la rotation 0 a (top=0 && left=0) -> 1 candidat. */
    base->parts[1].top = 0; base->parts[1].right = 6;
    base->parts[1].bottom = 14; base->parts[1].left = 0;

    struct array_part *rot = rotate_all_parts(base);
    map_big_array     *map = prepare_map_part(rot);

    unsigned long long before_data = datas_size();
    unsigned long long before_nn   = non_null_possibilities;

    first_possibility(map, rot);

    /* exit() (et non _exit) pour que gcov écrive la couverture du corps de
       first_possibility exercé ici — cf. status_zone_lifecycle_over_pty. */
    if (non_null_possibilities != before_nn + 2) exit(21);
    if (datas_size() != before_data + 1)         exit(22);
    exit(0); /* succès : flush gcov + atexit puis sortie nette */
}

/* Chaque pièce-indice manquante (genèse 139 + les 4 indices) rend
 * first_possibility fatal : couvre les 5 sites fatal_error de la fonction. */
TEST first_possibility_missing_each_index_is_fatal(void)
{
    const int ids[] = {139, 208, 255, 181, 249};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        g_fp_omit_id = ids[i];
        pid_t pid;
        int code = run_in_fork(run_fp_omit, &pid);
        ASSERT_EQ_FMTm("pièce-indice introuvable doit être fatale",
                       EXIT_FAILURE, code, "%d");
    }
    PASS();
}

TEST first_possibility_valid_injects_one_possibility(void)
{
    pid_t pid;
    int code = run_in_fork(run_fp_valid_injects, &pid);
    /* 0 = OK ; 21 = compteur non_null inattendu ; 22 = datas_size inattendu. */
    ASSERT_EQ_FMTm("first_possibility doit injecter 1 possibilité (code 0)", 0, code, "%d");
    PASS();
}
#endif /* ETERN_PARTS == 256 */

/* --------------------------------------------------------------------------
 * put_possibility : propagation d'erreur malloc
 * ------------------------------------------------------------------------ */

/* put_possibility retourne 0 quand sizeofvalue=0 (chemin malloc ← cache vide). */
TEST put_possibility_returns_zero_on_bad_sizeofvalue(void)
{
    File file;
    init_file_with_cache(&file, 0, 0); /* sizeofvalue=0 → chemin malloc échoue */
    struct possibility_packet p;
    memset(&p, 0, sizeof(p));
    int ret = put_possibility(&file, &p);
    ASSERT_EQ_FMT(0, ret, "%d");
    ASSERT_EQ_FMT(0ULL, (unsigned long long)file.size, "%llu");
    PASS();
}

/* search_possiblity_light s'arrête et retourne 0 quand put_possibility échoue. */
TEST search_light_aborts_on_put_failure(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 },
    };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0; p->alloc = 0;

    key_part key = { .k1 = 0, .k2 = 2, .k3 = 3, .k4 = 0 };
    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    /* sizeofvalue=0 → put_possibility retourne 0 au premier appel */
    File result;
    init_file_with_cache(&result, 0, 0);

    int max = search_possiblity_light(&result, &key, p, map, &rp, idParts);
    ASSERT_EQ_FMT(0, max, "%d");
    ASSERT_EQ_FMT(0ULL, (unsigned long long)result.size, "%llu");

    free_bigarray(map);
    free(p);
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
#if ETERN_PARTS == 256
    RUN_TEST(check_possibility_missing_genesis_anchor_is_minus_six);
    RUN_TEST(check_possibility_valid_genesis_is_zero);
    RUN_TEST(synthetic_map_resolves_each_index_key);
    RUN_TEST(synthetic_map_omitting_index_yields_null_key);
    RUN_TEST(first_possibility_missing_each_index_is_fatal);
    RUN_TEST(first_possibility_valid_injects_one_possibility);
#endif
#if ETERN_PARTS != 256
    RUN_TEST(check_possibility_invalid_grid_value_is_minus_seven);
    RUN_TEST(check_possibility_border_mismatch_is_minus_nine);
    RUN_TEST(check_possibility_neighbor_mismatch_is_minus_nine);
#endif
    RUN_TEST(compare_possibility_detects_each_difference);
    RUN_TEST(is_origin_of_recognizes_prefix);
    RUN_TEST(build_single_array_wraps_one_packet);
    RUN_TEST(generate_possibility_packet_encodes_grid);
    RUN_TEST(normalize_repairs_alloc_ahead_of_hole);
    RUN_TEST(normalize_leaves_conforming_packet_untouched);
    RUN_TEST(save_possibility_writes_packet_to_file);
    RUN_TEST(save_possibility_unwritable_exits);
    RUN_TEST(save_solution_csv_writes_header_and_rows);
    RUN_TEST(save_solution_csv_null_parts_writes_minus_one_faces);
    RUN_TEST(check_if_result_found_below_complete_is_noop);
    RUN_TEST(check_if_result_found_complete_exits_success);
    RUN_TEST(log_solution_writes_distinct_files_for_each_solution);
    RUN_TEST(what_search_corner_of_empty_board);
    RUN_TEST(what_search_reads_placed_neighbor);
    RUN_TEST(what_search_bottom_right_with_neighbors);
    RUN_TEST(what_search_interior_empty_neighbors);
    RUN_TEST(what_search_to_key2_uses_all_face_for_empty);
    RUN_TEST(what_search_in_grid_to_key_arbitrary_cell);
    RUN_TEST(possibility_has_a_next_finds_and_excludes_used);
    RUN_TEST(search_light_expands_one_per_candidate);
    RUN_TEST(search_light_skips_prefilled_cell);
    RUN_TEST(put_possibility_returns_zero_on_bad_sizeofvalue);
    RUN_TEST(search_light_aborts_on_put_failure);
    RUN_TEST(forward_check_detects_dead_cell);
    RUN_TEST(forward_check_passes_when_cells_filled);
    RUN_TEST(search_big_table_prunes_dead_branches);
    RUN_TEST(what_search_to_key_empty_and_placed_neighbor);
    RUN_TEST(print_possibility_packet_runs);
    RUN_TEST(all_has_a_next_all_filled_returns_one);
    RUN_TEST(all_has_a_next_dead_cell_returns_zero);
}
