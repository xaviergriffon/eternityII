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
#include <fcntl.h>

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

/* Réduit au silence stdout/stderr le temps d'un appel qui logge un errno
 * attendu (chemin non inscriptible, etc.), sans passer par un fork. */
static int g_silence_fd1 = -1, g_silence_fd2 = -1;
static void silence_stdio(void)
{
    fflush(stdout); fflush(stderr);
    g_silence_fd1 = dup(1); g_silence_fd2 = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1); dup2(devnull, 2); close(devnull);
}
static void restore_stdio(void)
{
    fflush(stdout); fflush(stderr);
    dup2(g_silence_fd1, 1); dup2(g_silence_fd2, 2);
    close(g_silence_fd1); close(g_silence_fd2);
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

/* directions[] corrompu (doublon -> un indice jamais visité) -> -1.
 * directions est un vrai global mutable (static_variables.c) : on le corrompt
 * temporairement puis on le restaure, quel que soit le résultat du test. */
TEST test_directions_detects_missing_cell(void)
{
    uint8_t saved0 = directions[0];
    directions[0] = directions[1]; /* doublon : l'indice saved0 n'est plus visité */

    int r = test_directions();

    directions[0] = saved0; /* restauration avant toute assertion */
    ASSERT_EQ_FMT(-1, r, "%d");
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

/* Même garde que ci-dessus, mais côté y : jamais exercé par le test précédent
 * (x seul suffit à faire échouer le || avant d'évaluer y). */
TEST check_possibility_out_of_bounds_y_is_minus_two(void)
{
    struct array_part *rp = make_dummy_rotate_parts();
    struct possibility_packet *p = new_zeroed_packet();
    p->x = 0;
    p->y = ETERN_SIZE; /* hors plateau */
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

/* rotateParts == NULL : check_possibility charge parts_files elle-même
 * (read_parts + rotate_all_parts, libérés en sortie via le label cleanup).
 * Branche jamais exercée par les autres tests, qui passent tous un
 * rotateParts construit à la main. packet == NULL suffit à vérifier que le
 * chargement a bien eu lieu sans planter, avant le contrôle de packet. */
TEST check_possibility_loads_parts_when_rotate_parts_null(void)
{
    ASSERT_EQ_FMT(-1, check_possibility(NULL, NULL), "%d");
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

/* Paquet genèse minimal valide : ancrage 139 r2 posé, seule case non vide de
 * la grille -> le contrôle de cohérence porte sur cette unique case (toutes
 * ses voisines sont vides -> bypass des 4 côtés) -> 0. rp doit couvrir
 * l'indice id_for_rotated_part(139,2) (651 en 256) : make_dummy_rotate_parts
 * (size=4) ne suffit plus depuis que check_possibility scanne TOUTES les
 * cases non vides (site 4 de PR1), plus seulement les alloc premières du
 * parcours -- (7,8) n'est pas dans les 0 premières cases (alloc=0) mais EST
 * non vide. */
TEST check_possibility_valid_genesis_is_zero(void)
{
    struct array_part *rp = malloc(sizeof(struct array_part));
    rp->size = id_for_rotated_part(139, 2) + 1;
    rp->parts = calloc((size_t)rp->size, sizeof(struct part));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->grid[7][8] = id_for_rotated_part(139, 2);
    ASSERT_EQ_FMT(0, check_possibility(p, rp), "%d");
    free(p);
    free_array_part(rp);
    PASS();
}

/*
 * check_possibility : voisins TOP/LEFT non encore posés (grid == -2), au sein
 * de la boucle de cohérence (p < alloc). Cas symétrique de
 * check_possibility_consistent_interior_neighbors_is_zero (build 16, RIGHT et
 * BOTTOM) : sous le nouveau parcours du build 256 (directions[]/dirx[]/diry[],
 * app/static_variables.c), les 23 premiers indices (p=0..22) visitent (0,0)
 * puis longent le bord haut jusqu'à (15,0), avec trois courtes incursions
 * verticales en (2,*), (13,*) et (15,*) et un dernier saut en (14,2) -- sans
 * jamais visiter (14,1) avant l'indice 23 (exclu par alloc=23). Cela reproduit
 * les deux cas symétriques recherchés :
 *   - p=3 = (2,1) : voisin GAUCHE (1,1) jamais posé (hors de ce parcours) -> -1.
 *   - p=22 = (14,2) : voisin HAUT (14,1) posé plus tard (indice 23, exclu par
 *     alloc=23) -> -1.
 * Résultat attendu : 0 (aucune incohérence, les -1 court-circuitent la
 * comparaison).
 */
TEST check_possibility_top_left_empty_neighbors_is_zero(void)
{
    /* parts[1..23] correspondent aux 23 premières cases du parcours (indices
       0..22 de dirx[]/diry[]), dans l'ordre. parts[0] est un bouchon inutilisé.
       Dimensionné jusqu'à id_for_rotated_part(139,2) inclus (651) : depuis que
       check_possibility scanne TOUTES les cases non vides (site 4 de PR1),
       l'ancrage genèse posé en (7,8) est lui aussi validé (ses voisines sont
       toutes vides ici -> bypass des 4 côtés, seul l'indice doit être dans
       les bornes de rp). */
    static struct part parts[652];
    memset(parts, 0, sizeof(parts));
    for (int i = 0; i <= 23; i++) parts[i].id = i;

    int color = 100;
    int L[15];
    for (int i = 0; i < 15; i++) L[i] = color++;
    int V2a = color++, V2b = color++;
    int V13a = color++, V13b = color++;
    int V15a = color++, V15b = color++;
    int W1 = color++, W2 = color++;

    /* Chaîne horizontale du bord haut (0,0)..(15,0) -> gridvals 1,2,3,6,7,8,9,
       10,11,12,13,14,15,16,19,20 (les gridvals 4,5,17,18,21..23 sont les
       incursions verticales / le saut final, hors de cette chaîne). */
    parts[1].top = 0; parts[1].left = 0; parts[1].right = (int8_t)L[0];               /* (0,0) */
    parts[2].top = 0; parts[2].left = (int8_t)L[0]; parts[2].right = (int8_t)L[1];    /* (1,0) */
    parts[3].top = 0; parts[3].left = (int8_t)L[1]; parts[3].right = (int8_t)L[2];    /* (2,0) */
    parts[3].bottom = (int8_t)V2a;
    parts[4].top = (int8_t)V2a; parts[4].bottom = (int8_t)V2b;                        /* (2,1) */
    parts[5].top = (int8_t)V2b;                                                        /* (2,2) */
    parts[6].top = 0; parts[6].left = (int8_t)L[2]; parts[6].right = (int8_t)L[3];    /* (3,0) */
    parts[7].top = 0; parts[7].left = (int8_t)L[3]; parts[7].right = (int8_t)L[4];    /* (4,0) */
    parts[8].top = 0; parts[8].left = (int8_t)L[4]; parts[8].right = (int8_t)L[5];    /* (5,0) */
    parts[9].top = 0; parts[9].left = (int8_t)L[5]; parts[9].right = (int8_t)L[6];    /* (6,0) */
    parts[10].top = 0; parts[10].left = (int8_t)L[6]; parts[10].right = (int8_t)L[7]; /* (7,0) */
    parts[11].top = 0; parts[11].left = (int8_t)L[7]; parts[11].right = (int8_t)L[8]; /* (8,0) */
    parts[12].top = 0; parts[12].left = (int8_t)L[8]; parts[12].right = (int8_t)L[9]; /* (9,0) */
    parts[13].top = 0; parts[13].left = (int8_t)L[9]; parts[13].right = (int8_t)L[10]; /* (10,0) */
    parts[14].top = 0; parts[14].left = (int8_t)L[10]; parts[14].right = (int8_t)L[11]; /* (11,0) */
    parts[15].top = 0; parts[15].left = (int8_t)L[11]; parts[15].right = (int8_t)L[12]; /* (12,0) */
    parts[16].top = 0; parts[16].left = (int8_t)L[12]; parts[16].right = (int8_t)L[13]; /* (13,0) */
    parts[16].bottom = (int8_t)V13a;
    parts[17].top = (int8_t)V13a; parts[17].bottom = (int8_t)V13b;                     /* (13,1) */
    parts[18].top = (int8_t)V13b; parts[18].right = (int8_t)W1;                        /* (13,2) */
    parts[19].top = 0; parts[19].left = (int8_t)L[13]; parts[19].right = (int8_t)L[14]; /* (14,0) */
    parts[20].top = 0; parts[20].left = (int8_t)L[14]; parts[20].right = 0;            /* (15,0) : coin bord droit */
    parts[20].bottom = (int8_t)V15a;
    parts[21].top = (int8_t)V15a; parts[21].right = 0; parts[21].bottom = (int8_t)V15b; /* (15,1) */
    parts[22].top = (int8_t)V15b; parts[22].right = 0; parts[22].left = (int8_t)W2;    /* (15,2) */
    parts[23].left = (int8_t)W1; parts[23].right = (int8_t)W2;                         /* (14,2) : TOP (14,1) non posé -> -1 */

    struct array_part rp = { .size = 652, .parts = parts };

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    p->grid[0][0] = 1;
    p->grid[1][0] = 2;
    p->grid[2][0] = 3;
    p->grid[2][1] = 4;
    p->grid[2][2] = 5;
    p->grid[3][0] = 6;
    p->grid[4][0] = 7;
    p->grid[5][0] = 8;
    p->grid[6][0] = 9;
    p->grid[7][0] = 10;
    p->grid[8][0] = 11;
    p->grid[9][0] = 12;
    p->grid[10][0] = 13;
    p->grid[11][0] = 14;
    p->grid[12][0] = 15;
    p->grid[13][0] = 16;
    p->grid[13][1] = 17;
    p->grid[13][2] = 18;
    p->grid[14][0] = 19;
    p->grid[15][0] = 20;
    p->grid[15][1] = 21;
    p->grid[15][2] = 22;
    p->grid[14][2] = 23;
    p->grid[7][8] = id_for_rotated_part(139, 2); /* ancrage genèse */

    p->alloc = 23;
    ASSERT_EQ_FMT((int8_t)2, dirx[3], "%d");
    ASSERT_EQ_FMT((int8_t)1, diry[3], "%d");
    ASSERT_EQ_FMT((int8_t)14, dirx[22], "%d");
    ASSERT_EQ_FMT((int8_t)2, diry[22], "%d");
    ASSERT_EQ_FMT((int8_t)14, dirx[23], "%d");
    ASSERT_EQ_FMT((int8_t)1, diry[23], "%d");
    for (int id = 1; id <= 23; id++) set_face_used(p->b_faceused, id - 1, 1);

    ASSERT_EQ_FMT(0, check_possibility(p, &rp), "%d");

    free(p);
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
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 1;
    set_face_used(p->b_faceused, 0, 1); /* faceused (1) >= alloc (1) */
    p->grid[0][0] = -1;                 /* (dirx[0],diry[0]) = case invalide, != -2 -> scannée */
    ASSERT_EQ_FMT(-7, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* Valeur de grille >= rotateParts->size (hors bornes POSITIF, jamais exercé
 * par check_possibility_invalid_grid_value_is_minus_seven qui ne teste que le
 * côté négatif de la même condition) -> -7. */
TEST check_possibility_grid_value_too_large_is_minus_seven(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1 } }; /* rp.size = 2 */
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 1;
    set_face_used(p->b_faceused, 0, 1);
    p->grid[0][0] = 2; /* >= rp.size (2) : hors bornes */
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

/* --------------------------------------------------------------------------
 * check_possibility : voisins d'INTÉRIEUR (y-1>=0 / x-1>=0), jamais atteints
 * par les tests ci-dessus (alloc=1 -> seule (0,0) est visitée, où y-1<0 et
 * x-1<0 systématiquement -> toujours le cas « bord de plateau »). On construit
 * ici une chaîne de 5 pièces couvrant (0,0),(0,1),(0,2),(0,3),(1,0) : les
 * cases (0,1..3) exercent TOP avec un voisin déjà placé, (1,0) exerce LEFT
 * avec un voisin déjà placé. La traversée column-major du build 16 pièces
 * (dirx={0,0,0,0,1,...}, diry={0,1,2,3,0,...}) rend ce parcours possible avec
 * alloc=5.
 * ------------------------------------------------------------------------ */

/* Pièce en (0,0) dont la face LEFT != bord (0) : seule la branche LEFT échoue
 * (top=0 passe le check TOP, les voisins droite/bas sont vides). NB : la forme
 * « voisin gauche posé mais incompatible » est inatteignable — le check RIGHT
 * du voisin (comparaison symétrique, indice de parcours plus petit) court-
 * circuite toujours avant ; seule la variante bord de plateau prend la branche. */
TEST check_possibility_left_border_mismatch_is_minus_nine(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 0, .bottom = 0, .left = 7 }, /* left=7 != bord */
    };
    struct array_part rp = { .size = 2, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 1;
    set_face_used(p->b_faceused, 0, 1);
    p->grid[0][0] = 1; /* bord gauche attendu = 0, la pièce annonce left = 7 */
    ASSERT_EQ_FMT(-9, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* Chaîne verticale (0,0)-(0,3) + (1,0) entièrement cohérente -> 0. */
TEST check_possibility_consistent_interior_neighbors_is_zero(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0,  .right = 11, .bottom = 21, .left = 0 },
        { .id = 2, .top = 21, .right = 12, .bottom = 22, .left = 0 },
        { .id = 3, .top = 22, .right = 13, .bottom = 23, .left = 0 },
        { .id = 4, .top = 23, .right = 14, .bottom = 0,  .left = 0 },
        { .id = 5, .top = 0,  .right = 0,  .bottom = 0,  .left = 11 },
    };
    struct array_part rp = { .size = 6, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 5;
    for (int id = 1; id <= 5; id++) set_face_used(p->b_faceused, id - 1, 1);
    p->grid[0][0] = 1; p->grid[0][1] = 2; p->grid[0][2] = 3; p->grid[0][3] = 4;
    p->grid[1][0] = 5;
    ASSERT_EQ_FMT(0, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* Même chaîne, mais (0,1).top ne correspond pas à (0,0).bottom -> BOTTOM
 * incohérent détecté en visitant (0,0) (ligne 1178, jamais atteinte ailleurs). */
TEST check_possibility_bottom_mismatch_is_minus_nine(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0,  .right = 11, .bottom = 21, .left = 0 },
        { .id = 2, .top = 99, .right = 12, .bottom = 22, .left = 0 }, /* top != 21 */
        { .id = 3, .top = 22, .right = 13, .bottom = 23, .left = 0 },
        { .id = 4, .top = 23, .right = 14, .bottom = 0,  .left = 0 },
        { .id = 5, .top = 0,  .right = 0,  .bottom = 0,  .left = 11 },
    };
    struct array_part rp = { .size = 6, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 5;
    for (int id = 1; id <= 5; id++) set_face_used(p->b_faceused, id - 1, 1);
    p->grid[0][0] = 1; p->grid[0][1] = 2; p->grid[0][2] = 3; p->grid[0][3] = 4;
    p->grid[1][0] = 5;
    ASSERT_EQ_FMT(-9, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* Même chaîne, mais (1,0).left ne correspond pas à (0,0).right -> LEFT
 * incohérent détecté en visitant (1,0) (ligne 1193, jamais atteinte ailleurs :
 * x-1>=0 exige une case hors de la 1re colonne dans le parcours). */
TEST check_possibility_left_mismatch_is_minus_nine(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0,  .right = 11, .bottom = 21, .left = 0 },
        { .id = 2, .top = 21, .right = 12, .bottom = 22, .left = 0 },
        { .id = 3, .top = 22, .right = 13, .bottom = 23, .left = 0 },
        { .id = 4, .top = 23, .right = 14, .bottom = 0,  .left = 0 },
        { .id = 5, .top = 0,  .right = 0,  .bottom = 0,  .left = 99 }, /* left != 11 */
    };
    struct array_part rp = { .size = 6, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 5;
    for (int id = 1; id <= 5; id++) set_face_used(p->b_faceused, id - 1, 1);
    p->grid[0][0] = 1; p->grid[0][1] = 2; p->grid[0][2] = 3; p->grid[0][3] = 4;
    p->grid[1][0] = 5;
    ASSERT_EQ_FMT(-9, check_possibility(p, &rp), "%d");
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * check_possibility : couverture à 100% d'un plateau TROUÉ (§8 du document
 * de conception, docs/conception/mrv_moteur_unique.md — critère de succès
 * explicite de PR1). Avant la conversion, la boucle de cohérence ne
 * balayait que les `alloc` premières cases du parcours directions[] ; ici
 * `alloc` est délibérément bas (1) alors que la grille porte 3 cases posées,
 * DEUX D'ENTRE ELLES adjacentes et border-INCOHÉRENTES, toutes deux "au-delà"
 * de alloc — exactement le scénario mesuré en §2.1 (indices/pièces posés
 * hors de portée du curseur). L'ancienne implémentation (p < alloc) ne les
 * aurait jamais visitées et aurait rendu 0 (paquet jugé valide à tort) ;
 * check_possibility doit désormais couvrir TOUTES les cases non vides, quel
 * que soit alloc, et détecter l'incohérence -> -9.
 * ------------------------------------------------------------------------ */
TEST check_possibility_covers_all_placed_cells_on_a_holed_board(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 0, .bottom = 0, .left = 0 },   /* (0,0), isolée */
        { .id = 2, .top = 0, .right = 9, .bottom = 0, .left = 0 },   /* (2,2) */
        { .id = 3, .top = 0, .right = 0, .bottom = 0, .left = 8 },   /* (3,2) : left=8 != right(2,2)=9 */
    };
    struct array_part rp = { .size = 4, .parts = parts };

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    /* alloc prétend qu'une seule pièce est posée -- la grille en porte 3. */
    p->alloc = 1;
    p->grid[0][0] = 1;
    p->grid[2][2] = 2;
    p->grid[3][2] = 3;
    for (int id = 1; id <= 3; id++) set_face_used(p->b_faceused, id - 1, 1);

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
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++) {
            anc->grid[x][y] = -2;
            desc->grid[x][y] = -2;
        }

    /* desc et anc ont tous deux une pièce posée à la même case (l'inclusion
     * porte sur l'ENSEMBLE des cases non vides, pas un préfixe de parcours) ;
     * desc a en plus une seconde case posée AILLEURS, hors de tout ordre de
     * parcours -- anc reste bien un sous-ensemble de desc. */
    anc->grid[dirx[0]][diry[0]] = 77;
    desc->grid[dirx[0]][diry[0]] = 77;
    desc->grid[dirx[ETERN_PARTS - 1]][diry[ETERN_PARTS - 1]] = 88;
    anc->alloc = 1;
    desc->alloc = 2; /* desc descend plus loin */

    ASSERT_EQ_FMT(1, is_origin_of(anc, desc), "%d"); /* ancêtre confirmé */

    /* alloc égal ou supérieur -> -1 */
    anc->alloc = 2;
    ASSERT_EQ_FMT(-1, is_origin_of(anc, desc), "%d");
    anc->alloc = 1;

    /* divergence sur une case posée de anc -> -2 */
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
 * possibility_placed_count : définition canonique de alloc (VERSION 13)
 * ------------------------------------------------------------------------ */

/* Grille entièrement vide -> 0. */
TEST placed_count_empty_grid_is_zero(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    ASSERT_EQ_FMT(0, possibility_placed_count(p), "%d");

    free(p);
    PASS();
}

/* Cases posées "au-delà" d'un curseur (scénario §2.1 du document de
 * conception : indices officiels posés hors ordre par rapport à
 * directions[]) -> compte EXACT, peu importe la position dans le parcours. */
TEST placed_count_counts_holes_beyond_a_cursor(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    /* Trois cases posées à des indices de parcours arbitraires et non
       contigus, comme le seraient des indices officiels. */
    p->grid[dirx[0]][diry[0]] = 1;
    p->grid[dirx[ETERN_PARTS / 2]][diry[ETERN_PARTS / 2]] = 2;
    p->grid[dirx[ETERN_PARTS - 1]][diry[ETERN_PARTS - 1]] = 3;

    ASSERT_EQ_FMT(3, possibility_placed_count(p), "%d");

    free(p);
    PASS();
}

/* Plateau complet -> ETERN_PARTS. */
TEST placed_count_full_grid_is_etern_parts(void)
{
    struct possibility_packet *p = new_zeroed_packet(); /* grid tout à 0 (pas de -2) */

    ASSERT_EQ_FMT(ETERN_PARTS, possibility_placed_count(p), "%d");

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
 * fprint_possibility_packet : export JSON vers un fichier arbitraire (P5,
 * commandes console `print`/`printFile`/`printAnalysed [fichier]`)
 * ------------------------------------------------------------------------ */

/* Écrit un paquet dans un fichier temporaire et vérifie le format JSON
   attendu (mêmes champs que print_possibility_packet, destination différente). */
TEST fprint_possibility_packet_writes_json(void)
{
    char path[] = "/tmp/etii_fprint_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 7;
    p->x = 3;
    p->y = 4;

    FILE *out = fopen(path, "w");
    ASSERT(out != NULL);
    ASSERT_EQ_FMT(0, fprint_possibility_packet(out, p), "%d");
    fclose(out);

    FILE *in = fopen(path, "r");
    ASSERT(in != NULL);
    char buf[8192] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, in);
    fclose(in);
    unlink(path);
    (void)n;

    ASSERT(strstr(buf, "\"alloc\": 7") != NULL);
    ASSERT(strstr(buf, "\"x\": 3") != NULL);
    ASSERT(strstr(buf, "\"y\": 4") != NULL);
    ASSERT(strstr(buf, "\"grid\": [[") != NULL);

    free(p);
    PASS();
}

/* Flux déjà fermé (ou en lecture seule) -> fprintf échoue -> -1, sans crash. */
TEST fprint_possibility_packet_reports_write_failure(void)
{
    char path[] = "/tmp/etii_fprint_ro_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 1;

    FILE *ro = fopen(path, "r"); /* ouvert en LECTURE seule : écrire échoue */
    ASSERT(ro != NULL);
    ASSERT_EQ_FMT(-1, fprint_possibility_packet(ro, p), "%d");
    fclose(ro);
    unlink(path);

    free(p);
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

/* Chemin non inscriptible : contrairement à save_possibility, save_solution_csv
 * NE quitte PAS le processus — elle journalise et retourne -1. Pas de fork
 * nécessaire. */
TEST save_solution_csv_unwritable_returns_minus_one(void)
{
    struct possibility_packet *p = new_zeroed_packet();

    silence_stdio();
    int ret = save_solution_csv("/etii_nonexistent_dir_zzz/out.csv", p, NULL);
    restore_stdio();

    ASSERT_EQ_FMT(-1, ret, "%d");
    free(p);
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
 * what_search / what_search_to_key / what_search_in_grid_to_key
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

/* what_search_to_key : case courante, voisins vides encodés en `all_face`. */
TEST what_search_to_key_uses_all_face_for_empty(void)
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
    what_search_to_key(&rp, p, &k, all_face);
    ASSERT_EQ_FMT(0, (int)k.k1, "%d");          /* TOP bord */
    ASSERT_EQ_FMT(all_face, (int)k.k2, "%d");   /* RIGHT vide -> all_face */
    ASSERT_EQ_FMT(all_face, (int)k.k3, "%d");   /* BOTTOM vide -> all_face */
    ASSERT_EQ_FMT(0, (int)k.k4, "%d");          /* LEFT bord */

    free(p);
    PASS();
}

/* what_search_to_key : voisins TOP et LEFT *présents* (branches partId >= 0).
 * Les autres tests placent la case courante en haut/à gauche (y=0 / x=0), laissant
 * les branches « voisin du dessus/de gauche posé » jamais exécutées. On place donc
 * la case en (1,1) — coordonnées valides en 4×4 comme en 16×16 — avec une pièce
 * posée au-dessus et à gauche, et les voisins droite/bas vides. */
TEST what_search_to_key_uses_neighbor_faces_when_present(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 4 }, /* voisin du dessus -> k1 = bottom = 3 */
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 8 }, /* voisin de gauche -> k4 = right  = 6 */
    };
    struct array_part rp = { .size = 3, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 1;
    p->y = 1;
    p->grid[1][0] = 1; /* TOP présent  : grid[x][y-1] */
    p->grid[0][1] = 2; /* LEFT présent : grid[x-1][y] */

    key_part k;
    const int8_t all_face = 7;
    what_search_to_key(&rp, p, &k, all_face);
    ASSERT_EQ_FMT(3, (int)k.k1, "%d");        /* TOP présent  -> parts[1].bottom */
    ASSERT_EQ_FMT(all_face, (int)k.k2, "%d"); /* RIGHT vide   -> all_face        */
    ASSERT_EQ_FMT(all_face, (int)k.k3, "%d"); /* BOTTOM vide  -> all_face        */
    ASSERT_EQ_FMT(6, (int)k.k4, "%d");        /* LEFT présent -> parts[2].right  */

    /* Mêmes cases haut/gauche mais VIDES (indice valide, contenu < 0) : branche
       partId < 0 -> all_face, distincte du bord (k=0) et du voisin posé. */
    p->grid[1][0] = -2; /* TOP  interne vide */
    p->grid[0][1] = -2; /* LEFT interne vide */
    what_search_to_key(&rp, p, &k, all_face);
    ASSERT_EQ_FMT(all_face, (int)k.k1, "%d"); /* TOP  interne vide -> all_face */
    ASSERT_EQ_FMT(all_face, (int)k.k4, "%d"); /* LEFT interne vide -> all_face */

    free(p);
    PASS();
}

/* what_search_to_key : bords RIGHT/BOTTOM hors plateau + voisins BAS/DROITE
 * *présents*. Le test Tier A couvrait TOP/LEFT ; restaient les branches
 * `xp >= ETERN_SIZE` (RIGHT bord), `yp >= ETERN_SIZE` (BOTTOM bord) et le voisin
 * BAS posé (partId >= 0). On combine deux évaluations sur le même paquet. */
TEST what_search_to_key_right_bottom_edges_and_present(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 1, .right = 2, .bottom = 3, .left = 4 }, /* voisin BAS   -> k3 = top   = 1 */
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 8 }, /* voisin DROITE -> k2 = left  = 8 */
    };
    struct array_part rp = { .size = 3, .parts = parts };
    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    key_part k;
    const int8_t all_face = 7;

    /* (1) coin bas-droit : RIGHT et BOTTOM hors plateau -> k2 = k3 = 0. */
    p->x = ETERN_SIZE - 1;
    p->y = ETERN_SIZE - 1;
    what_search_to_key(&rp, p, &k, all_face);
    ASSERT_EQ_FMT(0, (int)k.k2, "%d"); /* RIGHT  hors plateau (xp >= SIZE) */
    ASSERT_EQ_FMT(0, (int)k.k3, "%d"); /* BOTTOM hors plateau (yp >= SIZE) */

    /* (2) case (1,1) avec voisins DROITE et BAS posés (branche partId >= 0). */
    p->x = 1;
    p->y = 1;
    p->grid[2][1] = 2; /* RIGHT présent : grid[x+1][y] */
    p->grid[1][2] = 1; /* BOTTOM présent : grid[x][y+1] */
    what_search_to_key(&rp, p, &k, all_face);
    ASSERT_EQ_FMT(8, (int)k.k2, "%d"); /* RIGHT présent  -> parts[2].left */
    ASSERT_EQ_FMT(1, (int)k.k3, "%d"); /* BOTTOM présent -> parts[1].top  */

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

/* Clé sans AUCUN candidat dans la map (compartiment vide, search->size == 0) :
 * branche jamais exercée par possibility_has_a_next_finds_and_excludes_used
 * (qui trouve toujours au moins un compartiment non vide). Coin (0,0) avec
 * voisins droit ET bas placés (clé concrète, cf. commentaire de get_parts_bigarray
 * plus haut : pas de -1 « voisin vide » sinon l'indexation brute déraille), mais
 * dont la combinaison ne correspond à aucune pièce de rp. */
TEST possibility_has_a_next_empty_compartment_returns_zero(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* ne matche pas la clé visée */
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 99 }, /* voisin droit : left=99 -> k2 */
        { .id = 3, .top = 88, .right = 8, .bottom = 9, .left = 4 }, /* voisin bas : top=88 -> k3 */
    };
    struct array_part rp = { .size = 4, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0;
    p->grid[1][0] = 2; /* voisin droit -> k2 = 99 */
    p->grid[0][1] = 3; /* voisin bas   -> k3 = 88 */

    /* Clé (0, 99, 88, 0) : aucune pièce de rp ne présente cette combinaison. */
    ASSERT_EQ_FMT(0, possibility_has_a_next(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Liste de candidats mêlant un bouchon id=0 (ignoré), une pièce déjà utilisée
 * (ignorée) et une pièce libre (trouvée) : couvre les deux sous-conditions de
 * `id != 0 && !is_face_used(...)` que le test "finds_and_excludes_used" (une
 * seule pièce candidate à la fois) n'exerce pas simultanément. */
TEST possibility_has_a_next_skips_zero_id_and_used_piece(void)
{
    struct part parts[] = {
        { .id = 0, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* bouchon : même clé, ignoré */
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* utilisée */
        { .id = 2, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* libre, même clé */
        { .id = 3, .top = 5, .right = 6, .bottom = 7, .left = 2 },
        { .id = 4, .top = 3, .right = 8, .bottom = 9, .left = 4 },
    };
    struct array_part rp = { .size = 5, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->x = 0; p->y = 0;
    p->grid[1][0] = 3; /* voisin droit -> k2 = parts[3].left = 2 */
    p->grid[0][1] = 4; /* voisin bas   -> k3 = parts[4].top  = 3 */

    set_face_used(p->b_faceused, 0, 1); /* pièce 1 (id-1=0) déjà utilisée */
    /* clé (0,2,3,0) : compartiment = [1 (utilisée), 2 (libre)] -> trouve la 2. */
    ASSERT_EQ_FMT(1, possibility_has_a_next(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * search_possiblity_light : expansion d'un paquet (File de résultats)
 *
 * Depuis PR1 (docs/conception/mrv_moteur_unique.md), la fonction choisit
 * elle-même la case à développer (`light_choose_cell`, MRV : la case vide la
 * plus contrainte) au lieu de recevoir une clé pré-calculée sur la case du
 * curseur -- elle ne prend donc plus de `key_part *key` en paramètre.
 *
 * Les tests ci-dessous utilisent une map "uniforme" (même technique que
 * `make_uniform_map` dans tests/core/test_etii_search.c et
 * `make_uniform_map_fwd` plus bas dans ce fichier) : CHAQUE case vide résout
 * à la MÊME liste de candidats, quelle que soit sa clé réelle. Nécessaire ici
 * parce que light_choose_cell balaie TOUTES les cases vides de la grille pour
 * trouver la plus contrainte -- avec une vraie `buildBigArray` construite sur
 * 2-3 pièces synthétiques, la quasi-totalité des ~250 autres cases vides
 * n'auraient AUCUN candidat (carte non représentative d'un vrai plateau) et
 * seraient jugées mortes avant même d'atteindre la case visée par le test.
 * La map uniforme élimine ce faux négatif : peu importe la case choisie, le
 * résultat de la recherche est le même, donc seul le comportement réellement
 * testé (filtrage id=0/déjà utilisé, saut des cases pré-remplies, échec de
 * put_possibility) varie d'un test à l'autre.
 * ------------------------------------------------------------------------ */

/* Remplit idParts[p][r] = p + ETERN_PARTS*r (encodage des rotations). */
static void fill_id_parts(int16_t idParts[ETERN_PARTS][4])
{
    for (int p = 0; p < ETERN_PARTS; p++)
        for (int r = 0; r < 4; r++)
            idParts[p][r] = (int16_t)(p + ETERN_PARTS * r);
}

/* Map "uniforme" : cf. note de section ci-dessus. */
static map_big_array *sl_make_uniform_map(struct array_part *cand)
{
    static map_big_array map;
    static struct array_part flat[3 * 3 * 3 * 3];
    map.sizearray = 3;
    map.sizearrayM = 2;
    map.arena = NULL;
    map.flat = flat;
    for (int i = 0; i < 3 * 3 * 3 * 3; i++) flat[i] = *cand;
    return &map;
}

/* Deux pièces libres -> deux successeurs générés sur la case choisie. Grille
 * entièrement vide : toutes les cases sont à égalité (map uniforme), donc
 * light_choose_cell retient la première balayée, (0,0) (balayage x puis y,
 * cf. sa doc dans possibility.c). */
TEST search_light_expands_one_per_candidate(void)
{
    struct part cand[2] = { { .id = 1 }, { .id = 2 } };
    struct array_part candlist = { .size = 2, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);
    struct array_part rp = { .size = 0, .parts = NULL }; /* jamais lu : aucune case pré-remplie */

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    File result;
    init_file(&result, sizeof(struct possibility_packet));

    int max = search_possiblity_light(&result, p, map, &rp, idParts);
    ASSERT_EQ_FMT(1, max, "%d");
    ASSERT_EQ_FMT(2ULL, (unsigned long long)result.size, "%llu"); /* un paquet par pièce */

    /* Le premier successeur a la case (0,0) remplie et alloc = 1. */
    struct possibility_packet out;
    scroll(&result, &out);
    ASSERT_EQ_FMT(1, (int)out.alloc, "%d");
    ASSERT(out.grid[0][0] != -2);
    while (result.size > 0) scroll(&result, &out); /* draine le reste */

    free(p);
    PASS();
}

/* Compartiment mêlant un bouchon id=0 (ignoré, `continue`) et une pièce déjà
 * utilisée (ignorée) à côté d'une pièce libre : un seul successeur produit,
 * pour la pièce libre uniquement. Jamais exercé par
 * search_light_expands_one_per_candidate (aucun id=0 ni pièce déjà utilisée
 * dans son compartiment). */
TEST search_light_skips_zero_id_and_used_candidate(void)
{
    struct part cand[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } }; /* bouchon, utilisée, libre */
    struct array_part candlist = { .size = 3, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);
    struct array_part rp = { .size = 0, .parts = NULL };

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    set_face_used(p->b_faceused, 0, 1); /* pièce 1 (id-1=0) déjà utilisée */

    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    File result;
    init_file(&result, sizeof(struct possibility_packet));

    int max = search_possiblity_light(&result, p, map, &rp, idParts);
    ASSERT_EQ_FMT(1, max, "%d");
    ASSERT_EQ_FMT(1ULL, (unsigned long long)result.size, "%llu"); /* un seul successeur (pièce 2) */

    struct possibility_packet out;
    scroll(&result, &out);
    ASSERT(is_face_used(out.b_faceused, 1)); /* pièce 2 (id-1=1) placée */

    free(p);
    PASS();
}

/* Case (0,0) déjà remplie (indice fixe, comme les indices officiels §2.1 du
 * document de conception) : light_choose_cell la saute -- elle ne balaie que
 * les cases VIDES -- et retient la case vide suivante dans son ordre de
 * balayage (x puis y), (0,1). `alloc` du successeur est recompté : 1 case
 * pré-remplie + 1 nouvelle = 2, jamais un curseur avancé de +1. */
TEST search_light_skips_prefilled_cell(void)
{
    struct part cand[1] = { { .id = 9 } };
    struct array_part candlist = { .size = 1, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);

    /* rp : lue pour (0,0) en tant que voisine de (0,1) (TOP) -- doit couvrir
       l'indice placé ci-dessous, ses faces réelles n'important pas (map
       uniforme, indépendante de la clé). */
    struct part rp_parts[10];
    memset(rp_parts, 0, sizeof(rp_parts));
    struct array_part rp = { .size = 10, .parts = rp_parts };

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->grid[0][0] = 3; /* case (0,0) déjà remplie (indice fixe), != -2 */

    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    File result;
    init_file(&result, sizeof(struct possibility_packet));

    int max = search_possiblity_light(&result, p, map, &rp, idParts);
    ASSERT_EQ_FMT(2, max, "%d"); /* 1 case pré-remplie + 1 nouvelle, recompté */
    ASSERT_EQ_FMT(1ULL, (unsigned long long)result.size, "%llu");

    struct possibility_packet out;
    scroll(&result, &out);
    ASSERT_EQ_FMT(2, (int)out.alloc, "%d");
    ASSERT(out.grid[0][1] != -2); /* première case VIDE balayée après (0,0) */
    ASSERT_EQ_FMT(3, (int)out.grid[0][0], "%d"); /* case pré-remplie inchangée */

    free(p);
    PASS();
}

/* put_possibility échoue (sizeofvalue=0) sur la case choisie : retourne 0
 * sans planter. La branche "case déjà occupée -> avancer le curseur" de
 * l'ancienne implémentation a disparu (light_choose_cell ne sélectionne
 * jamais une case pleine) ; son pendant "échec de put_possibility" est donc
 * désormais couvert par le même chemin que search_light_aborts_on_put_failure
 * (plus bas dans ce fichier), qui exerce déjà l'échec sur une case vide --
 * un seul chemin de code, un seul test suffit. */

/* Dernière case du parcours : une seule case vide dans toute la grille (les
 * ETERN_PARTS-1 autres sont "remplies" par le bouchon 0 par défaut de
 * new_zeroed_packet) -- light_choose_cell la trouve nécessairement. La pièce
 * placée complète le plateau -> checkIfResultFound sort EXIT_SUCCESS ;
 * exécuté en fork. */
static struct array_part *sl_rp;
static map_big_array *sl_map;
static struct possibility_packet *sl_p;
static int16_t sl_idParts[ETERN_PARTS][4];
static char sl_solution_dir[256];

static void sl_child_completes_board(void)
{
    if (chdir(sl_solution_dir) != 0) _exit(97);
    File result;
    init_file(&result, sizeof(struct possibility_packet));
    int max = search_possiblity_light(&result, sl_p, sl_map, sl_rp, sl_idParts);
    (void)max; /* ne devrait pas revenir : checkIfResultFound exit() avant */
    _exit(98);
}

TEST search_light_completes_board_skips_forward_check(void)
{
    struct part cand[1] = { { .id = 9 } };
    struct array_part candlist = { .size = 1, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);

    /* rp couvre les valeurs de grille possibles : 0 (bouchon par défaut,
       partout ailleurs) et 9 (la pièce placée) -- log_solution (déclenchée
       par checkIfResultFound) lit all_rotate_part->parts[] pour LES 256
       cases du plateau complété. */
    static struct part rp_parts[10];
    memset(rp_parts, 0, sizeof(rp_parts));
    static struct array_part rp = { .size = 10 };
    rp.parts = rp_parts;

    struct possibility_packet *p = new_zeroed_packet(); /* grid=0 partout : bouchon valide pour log_solution */
    p->grid[ETERN_SIZE - 1][ETERN_SIZE - 1] = -2; /* seule case vide de toute la grille */

    sl_map = map; sl_rp = &rp; sl_p = p;
    fill_id_parts(sl_idParts);
    strcpy(sl_solution_dir, "/tmp/etii_sl_last_XXXXXX");
    ASSERT(mkdtemp(sl_solution_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(sl_child_completes_board, &pid);

    char sol[320];
    snprintf(sol, sizeof(sol), "%s/solution_%d_0.csv", sl_solution_dir, (int)pid);
    unlink(sol);
    rmdir(sl_solution_dir);
    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d");

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

    unsigned long long fc_cells_before = fc_cells_studied;
    ASSERT_EQ_FMT(0, forward_check_next_k(p, map, &rp), "%d");
    /* Une seule case inspectée (morte dès la première) créditée au flux
       « études de prunage » (dont prunage/s des rapports check). */
    ASSERT_EQ_FMT(fc_cells_before + 1, fc_cells_studied, "%llu");

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

    unsigned long long fc_cells_before = fc_cells_studied;
    ASSERT_EQ_FMT(1, forward_check_next_k(p, map, &rp), "%d");
    /* Cases remplies sautées : aucune étude de prunage créditée. */
    ASSERT_EQ_FMT(fc_cells_before, fc_cells_studied, "%llu");

    free_bigarray(map);
    free(p);
    PASS();
}

/* La case vide est la TOUTE DERNIÈRE du parcours (dirx[ETERN_PARTS-1]) et
 * c'est la SEULE case vide de la grille : depuis PR1, forward_check_next_k
 * ne borne plus sa fenêtre sur `alloc` (qui n'indexe plus une position de
 * curseur) mais balaie directions[] en cherchant les K premières cases
 * VIDES -- ce test verrouille qu'atteindre une case vide au tout dernier
 * indice du parcours ne déborde pas dirx[]/diry[] (boucle bornée par
 * `c < ETERN_PARTS`) et qu'elle est bien inspectée (pas de case morte ici,
 * un candidat existe -> 1, une seule case créditée). */
TEST forward_check_finds_late_empty_cell_without_overrun(void)
{
    struct part cand[1] = { { .id = 9 } };
    struct array_part candlist = { .size = 1, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);

    /* rp : lue pour les voisines de la case vide (toutes "remplies" à 0 par
       défaut) -- ses faces réelles n'importent pas (map uniforme). */
    static struct part rp_parts[4];
    memset(rp_parts, 0, sizeof(rp_parts));
    struct array_part rp = { .size = 4, .parts = rp_parts };

    /* grid=0 partout (bouchon) -> "remplie" partout, sauf la toute dernière
       case du parcours. */
    struct possibility_packet *p = new_zeroed_packet();
    p->grid[dirx[ETERN_PARTS - 1]][diry[ETERN_PARTS - 1]] = -2;

    unsigned long long fc_cells_before = fc_cells_studied;
    ASSERT_EQ_FMT(1, forward_check_next_k(p, map, &rp), "%d");
    ASSERT_EQ_FMT(fc_cells_before + 1, fc_cells_studied, "%llu");

    free(p);
    PASS();
}

/* Compartiment mêlant un bouchon id=0 (ignoré) et une pièce libre à côté d'une
 * pièce déjà utilisée : le forward-check trouve la pièce libre -> vivant (1).
 * Jamais exercé par forward_check_detects_all_candidates_already_used (un seul
 * candidat, pas de bouchon id=0 dans la liste). */
/* Map "uniforme" (`sl_make_uniform_map`, définie plus haut pour les tests
 * search_possiblity_light -- réutilisée ici) : CHAQUE clé renvoie la même
 * liste [bouchon id=0, pièce déjà utilisée, pièce libre]. Nécessaire ici car
 * la fenêtre de forward-check inspecte FORWARD_CHECK_K cases (pas seulement
 * la 1re) : avec une vraie buildBigArray, la case suivante du parcours
 * calculerait une clé différente (son voisin gauche redevient "vide" ->
 * incompatible avec le compartiment ciblé) et ferait échouer le test avant
 * même d'atteindre la pièce libre. */
TEST forward_check_skips_zero_id_finds_free_candidate(void)
{
    struct part cand[3] = { { .id = 0 }, { .id = 1 }, { .id = 2 } }; /* bouchon, utilisée, libre */
    struct array_part list = { .size = 3, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&list);
    struct array_part rp = { .size = 3, .parts = cand }; /* faces non pertinentes (map uniforme) */

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 0;
    set_face_used(p->b_faceused, 0, 1); /* pièce 1 (id-1=0) déjà utilisée */

    ASSERT_EQ_FMT(1, forward_check_next_k(p, map, &rp), "%d");

    free(p);
    PASS();
}

/* Case libre avec des pièces candidates dans la map, mais TOUTES déjà
 * marquées utilisées (b_faceused) -> branche morte (0). Différent de
 * forward_check_detects_dead_cell (aucun candidat DU TOUT dans la map). */
TEST forward_check_detects_all_candidates_already_used(void)
{
    /* pièce coin valide pour (0,0) (top=0, left=0 : bords) mais DÉJÀ utilisée. */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 0, .right = 6, .bottom = 7, .left = 0 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 0;
    set_face_used(p->b_faceused, 0, 1); /* id 1 (seul candidat) déjà utilisée */

    ASSERT_EQ_FMT(0, forward_check_next_k(p, map, &rp), "%d");

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

/* Régression : grille entièrement remplie du pire cas en largeur d'affichage
 * — id tourné maximal 4*ETERN_PARTS (id + ETERN_PARTS*rotation, cf.
 * id_for_rotated_part), 4 chiffres par case en build 256. L'ancien budget du
 * buffer de print_possibility_packet (5 chars/case, commentaire "3 chiffres +
 * espace + virgule") était sous-dimensionné pour ce cas et débordait le tas ;
 * ce test doit rester silencieux sous ASan (make test ASAN=1). */
TEST print_possibility_packet_survives_max_width_grid(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = ETERN_PARTS;
    p->x = 0; p->y = 0;
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = 4 * ETERN_PARTS; /* pire cas en largeur (4 chiffres en build 256) */

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
 * log_error_possibility_packet : variante ERREUR de print_possibility_packet
 * (voir possibility.h) — même JSON, mais via log_error() pour persister dans
 * events.log (contrairement à print_possibility_packet/log_info, jamais
 * écrite dans ce journal). Réservée aux sites qui diagnostiquent une erreur
 * (ex. paquet en cause lors d'un problème de communication avec le serveur).
 * ------------------------------------------------------------------------ */

TEST log_error_possibility_packet_persists_to_events_log(void)
{
    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 3; p->x = 1; p->y = 2;

    unlink("events.log");
    int saved = dup(2);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);

    int ret = log_error_possibility_packet(p);

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(devnull);

    ASSERT_EQ_FMT(0, ret, "%d");

    FILE *f = fopen("events.log", "r");
    ASSERT(f != NULL);
    char line[512] = {0};
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    (void)n;
    ASSERT(strstr(line, "\"alloc\": 3") != NULL);
    ASSERT(strstr(line, "\"x\": 1") != NULL);
    ASSERT(strstr(line, "\"y\": 2") != NULL);
    unlink("events.log");
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

/* Optimisation « candidat unique » : quand la case courante n'a qu'UNE seule
 * pièce candidate, possibility_all_has_a_next la place immédiatement (mute le
 * paquet) au lieu de se contenter de répondre 1. Jamais exercé par
 * all_has_a_next_all_filled_returns_one (aucune case libre) ni
 * all_has_a_next_dead_cell_returns_zero (0 candidat, pas 1). Même montage que
 * possibility_has_a_next_finds_and_excludes_used (clé concrète (0,2,3,0) via
 * voisins droit/bas placés) pour éviter la collision avec le bouchon id=0
 * (faces (0,0,0,0)) qui rendrait le compartiment de taille 2. */
TEST all_has_a_next_single_candidate_places_piece(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* coin, seul candidat */
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 2 }, /* voisin droit */
        { .id = 3, .top = 3, .right = 8, .bottom = 9, .left = 4 }, /* voisin bas */
    };
    struct array_part rp = { .size = 4, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet(); /* reste = 0 partout (grid "remplie") */
    p->alloc = 0;
    p->grid[dirx[0]][diry[0]] = -2;    /* case courante : seule case libre */
    p->grid[1][0] = 2;                 /* voisin droit -> k2 = 2 */
    p->grid[0][1] = 3;                 /* voisin bas   -> k3 = 3 */

    ASSERT_EQ_FMT(1, possibility_all_has_a_next(p, map, &rp), "%d");
    /* La pièce 1 (seule candidate de la clé (0,2,3,0)) a été placée. */
    ASSERT(p->grid[dirx[0]][diry[0]] != -2);
    ASSERT(is_face_used(p->b_faceused, 0));

    free_bigarray(map);
    free(p);
    PASS();
}

/* Compartiment à PLUSIEURS candidats dont un id != 0 déjà utilisé (ignoré) et
 * un autre libre (trouvé) : couvre la sous-condition `is_face_used(...) == 0`
 * fausse, jamais atteinte par all_has_a_next_dead_cell_returns_zero (aucun
 * candidat du tout) ni all_has_a_next_single_candidate_places_piece
 * (compartiment de taille 1, jamais 2). Compartiment de taille 2 : pas de
 * placement forcé (résultat 1, mais case laissée vide). */
TEST all_has_a_next_multi_candidate_skips_used_piece(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* déjà utilisée */
        { .id = 2, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* libre, même clé */
        { .id = 3, .top = 5, .right = 6, .bottom = 7, .left = 2 }, /* voisin droit */
        { .id = 4, .top = 3, .right = 8, .bottom = 9, .left = 4 }, /* voisin bas */
    };
    struct array_part rp = { .size = 5, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 0;
    p->grid[dirx[0]][diry[0]] = -2;
    p->grid[1][0] = 3; /* voisin droit -> k2 = 2 */
    p->grid[0][1] = 4; /* voisin bas   -> k3 = 3 */
    set_face_used(p->b_faceused, 0, 1); /* pièce 1 déjà utilisée */

    ASSERT_EQ_FMT(1, possibility_all_has_a_next(p, map, &rp), "%d");
    /* Compartiment de taille 2 : pas de placement forcé, la case reste vide. */
    ASSERT_EQ_FMT(-2, (int)p->grid[dirx[0]][diry[0]], "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Variante comptée : le balayage complet d'une grille « remplie » (aucune case
 * -2, résultat 1 à chaque itération) examine toutes les cases de alloc à
 * ETERN_PARTS -> out_cells_studied = ETERN_PARTS. C'est l'unité créditée au
 * compteur de coups des pruners (une étude par case, comme la recherche). */
/* Map "uniforme" (cf. `sl_make_uniform_map`, définie plus haut) : chaque case
 * vide, qu'elle soit de bord (clé avec un 0 de bordure) ou d'intérieur (clé
 * "toute face"), trouve exactement les 2 mêmes candidats jamais utilisés --
 * aucune case n'est morte, aucune n'est forcée (2 candidats, pas 1), donc le
 * balayage va bien jusqu'au bout de la grille sans early-exit. Une vraie
 * `buildBigArray` sur 1-2 pièces synthétiques échouerait ici : une pièce ne
 * peut pas simultanément satisfaire une bordure (0 exigé) et l'intérieur
 * (couleur non nulle exigée par le compartiment "toute face", cf. la note de
 * section de search_possiblity_light plus haut). */
TEST all_has_a_next_counted_full_scan_counts_all_cells(void)
{
    struct part cand[2] = { { .id = 1 }, { .id = 2 } };
    struct array_part candlist = { .size = 2, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);
    struct array_part rp = { .size = 0, .parts = NULL };

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;
    p->alloc = 0;

    unsigned int cells = 0;
    ASSERT_EQ_FMT(1, possibility_all_has_a_next_counted(p, map, &rp, &cells), "%d");
    ASSERT_EQ_FMT((unsigned int)ETERN_PARTS, cells, "%u");

    free(p);
    PASS();
}

/* Correction PR1 (site 5, docs/conception/mrv_moteur_unique.md) : les cases
 * déjà remplies ne sont plus re-balayées -- alloc n'indexe plus une position
 * de curseur (`possibility_placed_count`), le balayage saute directement les
 * cases non vides plutôt que de les compter comme "études de prunage".
 * Grille entièrement remplie (grid=0 partout, aucune case -2) -> 0 case
 * étudiée, contrairement à l'ancien comportement qui aurait compté
 * ETERN_PARTS cases (le bug documenté au §2.3 du document de conception). */
TEST all_has_a_next_counted_skips_already_filled_cells(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet(); /* grid=0 partout : "remplie" */
    p->alloc = 0; /* n'a plus d'effet sur le balayage : la grille décide seule */

    unsigned int cells = 0;
    ASSERT_EQ_FMT(1, possibility_all_has_a_next_counted(p, map, &rp, &cells), "%d");
    ASSERT_EQ_FMT(0u, cells, "%u");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Variante comptée : une impasse sur la première case du parcours arrête le
 * balayage immédiatement -> une seule case examinée. */
TEST all_has_a_next_counted_dead_first_cell_counts_one(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 0;
    p->grid[dirx[0]][diry[0]] = -2; /* clé (0,0,0,0) : aucun candidat */

    unsigned int cells = 0;
    ASSERT_EQ_FMT(0, possibility_all_has_a_next_counted(p, map, &rp, &cells), "%d");
    ASSERT_EQ_FMT(1u, cells, "%u");

    /* NULL accepté : même verdict, pas de comptage. */
    p->grid[dirx[0]][diry[0]] = -2;
    ASSERT_EQ_FMT(0, possibility_all_has_a_next_counted(p, map, &rp, NULL), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Variante comptée : plateau déjà complet (alloc = ETERN_PARTS), le balayage
 * ne fait aucune itération -> 0 case examinée (l'appelant crédite alors un
 * coup forfaitaire). */
TEST all_has_a_next_counted_complete_board_counts_zero(void)
{
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = ETERN_PARTS;

    unsigned int cells = 42;
    ASSERT_EQ_FMT(1, possibility_all_has_a_next_counted(p, map, &rp, &cells), "%d");
    ASSERT_EQ_FMT(0u, cells, "%u");

    free_bigarray(map);
    free(p);
    PASS();
}

#if ETERN_PARTS == 256
/* Régression : une case totalement non contrainte NE DOIT PAS interrompre le
 * balayage. Avant le correctif, dès qu'une case libre avait ses 4 clés égales
 * à all_face (aucun voisin posé, pas de bord), la boucle faisait
 * `result = 1; break;` et arrêtait d'examiner les cases suivantes du parcours
 * `directions[]` -- alors qu'une case plus loin peut être morte (voisins posés
 * dont la combinaison de bords ne correspond à aucune pièce). Ce test
 * construit exactement ce plateau :
 *   - c=23 -> (dirx[23],diry[23]) = (14,1) : case intérieure, 4 voisins vides
 *     -> clé (all_face,all_face,all_face,all_face), satisfiable par
 *     construction (le compartiment "toute face" contient toutes les pièces).
 *   - c=40 -> (dirx[40],diry[40]) = (13,13) : case intérieure entourée de 4
 *     pièces déjà posées dont la combinaison de bords ne correspond à AUCUNE
 *     pièce du jeu -> impasse.
 * (14,1) et (13,13) n'ont aucun voisin en commun : le premier reste bien
 * totalement non contraint quels que soient les voisins placés autour du
 * second. L'ancien code (break) renvoie 1 sans jamais atteindre c=40 ; le
 * nouveau code doit poursuivre le balayage et renvoyer 0. */
TEST all_has_a_next_unconstrained_cell_does_not_hide_later_dead_cell(void)
{
    struct part parts[] = {
        { .id = 0 }, /* bouchon */
        { .id = 1, .top = 9, .right = 9, .bottom = 9, .left = 9 }, /* candidat générique */
        /* voisins posés autour de la case morte (13,13) -- ids 2..5, faces
         * choisies pour qu'aucune pièce de rp n'ait simultanément
         * top=8,right=7,bottom=6,left=6 (clé exacte de (13,13)). */
        { .id = 2, .top = 0, .right = 0, .bottom = 6, .left = 0 }, /* voisin haut (13,12): bottom=6 -> k1 */
        { .id = 3, .top = 0, .right = 0, .bottom = 0, .left = 7 }, /* voisin droit (14,13): left=7   -> k2 */
        { .id = 4, .top = 8, .right = 0, .bottom = 0, .left = 0 }, /* voisin bas (13,14): top=8      -> k3 */
        { .id = 5, .top = 0, .right = 6, .bottom = 0, .left = 0 }, /* voisin gauche (12,13): right=6 -> k4 */
    };
    struct array_part rp = { .size = 6, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2; /* tout libre par défaut */

    /* Cases déjà posées : les 4 voisins de la case morte (13,13). */
    p->grid[13][12] = 2;
    p->grid[14][13] = 3;
    p->grid[13][14] = 4;
    p->grid[12][13] = 5;

    /* Démarre le balayage à c=23 = (14,1), une case non contrainte du
     * parcours. c=40 = (13,13) est atteinte plus loin. */
    p->alloc = 23;
    ASSERT_EQ_FMT((int8_t)14, dirx[23], "%d");
    ASSERT_EQ_FMT((int8_t)1, diry[23], "%d");
    ASSERT_EQ_FMT((int8_t)13, dirx[40], "%d");
    ASSERT_EQ_FMT((int8_t)13, diry[40], "%d");

    ASSERT_EQ_FMT(0, possibility_all_has_a_next(p, map, &rp), "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Point fixe (§4.6a) : un forçage TARDIF (case visitée plus loin dans le
 * balayage) peut consommer le candidat d'une case DÉJÀ examinée plus tôt dans
 * le MÊME balayage, la laissant sans aucune suite -- un seul balayage ne le
 * détecte jamais (avant ce correctif, possibility_all_has_a_next_counted
 * renvoyait 1 à tort dans ce cas précis). Cf. le commentaire historique
 * au-dessus de remove_possibilities_with_no_next (src/core/datamanager.c) qui
 * documentait déjà ce trou.
 *
 * Montage : 3 coins mutuellement non adjacents (aucun voisin en commun).
 *   - p0=(0,0), premier du parcours (dirx[0]=diry[0]=0) : 2 candidats libres
 *     {X=id1, Y=id2} à sa clé (0,8,9,0). Examiné en premier, aucun des deux
 *     n'est encore utilisé -> pas de forçage (compartiment de taille 2), la
 *     case reste vide et n'est PAS réexaminée plus tard dans CE balayage.
 *   - p1=(15,15) : sa clé (7,0,0,6) n'a qu'UN candidat, une seconde
 *     représentation de Y -> forçage, Y consommé.
 *   - p2=(0,15) : sa clé (5,4,0,0) n'a qu'UN candidat, une seconde
 *     représentation de X -> forçage, X consommé.
 * Après le 1er balayage complet, X et Y sont tous deux utilisés mais p0 (déjà
 * passé) n'a jamais été réexaminé : le point fixe relance un 2e balayage
 * (repart de alloc=0) qui découvre p0 sans aucune suite -> 0.
 */
TEST all_has_a_next_fixpoint_detects_cascading_forced_dead_cell(void)
{
    struct part parts[] = {
        { .id = 0 },                                                /* [0] bouchon */
        { .id = 1, .top = 0, .right = 8, .bottom = 9, .left = 0 },  /* [1] X @ clé p0 */
        { .id = 2, .top = 0, .right = 8, .bottom = 9, .left = 0 },  /* [2] Y @ clé p0 */
        { .id = 3, .left = 8 },                                     /* [3] voisin (1,0)   : gauche=8 -> k2 p0 */
        { .id = 4, .top = 9 },                                      /* [4] voisin (0,1)   : haut=9   -> k3 p0 */
        { .id = 5, .bottom = 7 },                                   /* [5] voisin (15,14) : bas=7    -> k1 p1 */
        { .id = 6, .right = 6 },                                    /* [6] voisin (14,15) : droite=6 -> k4 p1 */
        { .id = 7, .bottom = 5 },                                   /* [7] voisin (0,14)  : bas=5    -> k1 p2 */
        { .id = 8, .left = 4 },                                     /* [8] voisin (1,15)  : gauche=4 -> k2 p2 */
        { .id = 2, .top = 7, .right = 0, .bottom = 0, .left = 6 },  /* [9]  Y @ clé p1 (seul candidat, forcé) */
        { .id = 1, .top = 5, .right = 4, .bottom = 0, .left = 0 },  /* [10] X @ clé p2 (seul candidat, forcé) */
    };
    struct array_part rp = { .size = 11, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    p->grid[0][0] = -2;   /* p0 */
    p->grid[15][15] = -2; /* p1 */
    p->grid[0][15] = -2;  /* p2 */
    p->grid[1][0] = 3;
    p->grid[0][1] = 4;
    p->grid[15][14] = 5;
    p->grid[14][15] = 6;
    p->grid[0][14] = 7;
    p->grid[1][15] = 8;
    p->alloc = 0;

    ASSERT_EQ_FMT((int8_t)0, dirx[0], "%d");
    ASSERT_EQ_FMT((int8_t)0, diry[0], "%d");

    unsigned int cells = 0;
    ASSERT_EQ_FMT(0, possibility_all_has_a_next_counted(p, map, &rp, &cells), "%d");
    /* Détecté seulement au 2e balayage : le point fixe a dû relancer un
     * balayage complet -- mais depuis PR1 (site 5, docs/conception/
     * mrv_moteur_unique.md), chaque balayage saute les cases déjà remplies
     * au lieu de les recompter : passe 1 examine p0, p1, p2 (3 cases, les
     * SEULES vides) ; passe 2 ne réexamine QUE p0 (1 case, désormais morte).
     * Total 4, très inférieur à ETERN_PARTS (verrouille l'absence de
     * rebalayage des 253 cases déjà remplies). */
    ASSERT_EQ_FMT(4u, cells, "%u");
    /* X et Y ont bien été consommés par p1/p2 avant que p0 ne soit rejugé. */
    ASSERT(is_face_used(p->b_faceused, 0)); /* X = id 1 */
    ASSERT(is_face_used(p->b_faceused, 1)); /* Y = id 2 */
    /* p0 lui-même n'a jamais été forcé (2 candidats au moment de son examen). */
    ASSERT_EQ_FMT(-2, (int)p->grid[0][0], "%d");

    free_bigarray(map);
    free(p);
    PASS();
}

/* Contrepartie : un forçage isolé qui ne provoque AUCUNE cascade (la case
 * forcée n'est le voisin d'aucune autre case encore libre) doit toujours
 * renvoyer 1 -- non-régression du comportement historique
 * (all_has_a_next_single_candidate_places_piece) sous la nouvelle boucle à
 * point fixe : le forçage déclenche bien un 2e balayage (cf. commentaire
 * d'implémentation), mais ce 2e balayage ne doit rien invalider. */
TEST all_has_a_next_fixpoint_isolated_force_still_returns_one(void)
{
    struct part parts[] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 2, .bottom = 3, .left = 0 }, /* coin, seul candidat */
        { .id = 2, .top = 5, .right = 6, .bottom = 7, .left = 2 }, /* voisin droit */
        { .id = 3, .top = 3, .right = 8, .bottom = 9, .left = 4 }, /* voisin bas */
    };
    struct array_part rp = { .size = 4, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet *p = new_zeroed_packet();
    p->alloc = 0;
    p->grid[dirx[0]][diry[0]] = -2;
    p->grid[1][0] = 2;
    p->grid[0][1] = 3;

    unsigned int cells = 0;
    ASSERT_EQ_FMT(1, possibility_all_has_a_next_counted(p, map, &rp, &cells), "%d");
    ASSERT(p->grid[dirx[0]][diry[0]] != -2);
    ASSERT(is_face_used(p->b_faceused, 0));

    free_bigarray(map);
    free(p);
    PASS();
}

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

/* Couleurs EXPOSÉES (post-rotation, cf. les commentaires "-> {...}" de
 * SYN_INDEX_FACES) par les 5 pièces-indices une fois posées -- ce que leurs
 * voisines doivent trouver en face pour ne pas être des impasses. Depuis PR1
 * (docs/conception/mrv_moteur_unique.md), `search_possiblity_light` choisit
 * la case la plus contrainte (MRV) sur TOUTE la grille au lieu de la case
 * fixe `directions[0]` : elle balaie donc aussi les voisines des indices, et
 * `light_choose_cell` interrompt tout le balayage dès qu'une case CONTRAINTE
 * n'a aucun candidat (sous-arbre mort, cf. sa doc). Avec un filler
 * uniformément coloré (4 partout), ces voisines n'ont structurellement AUCUN
 * candidat -- ce n'est pas un défaut du choix de case, c'est cette carte
 * synthétique qui ne serait de toute façon jamais un vrai puzzle résoluble.
 * `make_synthetic_base_256` dédie donc quelques ids de filler à des couleurs
 * "pont" reproduisant exactement ces bords exposés. */
static const int8_t SYN_BRIDGE_COLORS[] = { 1, 2, 3, 5, 7, 8, 9, 10, 11, 12, 13, 15 };

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
    /* ids 2..13 : fillers "pont", un par couleur exposée nécessaire (uniforme
     * sur les 4 côtés -- la rotation n'a donc aucune incidence). id=1 reste
     * réservé à la pièce coin de run_fp_valid_injects. */
    for (size_t k = 0; k < sizeof SYN_BRIDGE_COLORS / sizeof SYN_BRIDGE_COLORS[0]; k++) {
        int id = (int)(2 + k);
        a->parts[id].top = a->parts[id].right = a->parts[id].bottom = a->parts[id].left = SYN_BRIDGE_COLORS[k];
    }
    /* id=14 : filler "bord", UN seul côté à 0 (bordure), les 3 autres à une
     * couleur inédite (20) -- ses 4 rotations couvrent chacune des 4 bordures
     * (haut/droite/bas/gauche) prises isolément. Sans cette pièce, toute case
     * de bord NON coin (ex. (0,5), (5,0)…) est structurellement une impasse
     * (aucun filler n'expose 0 nulle part ailleurs qu'au coin (0,0) via la
     * pièce id=1) -- et depuis PR1, `light_choose_cell` balaie TOUTE la
     * grille, donc atteint forcément ces bords avant de choisir une case. */
    a->parts[14].top = 0; a->parts[14].right = 20; a->parts[14].bottom = 20; a->parts[14].left = 20;
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
    init_file(&file, 0); /* sizeofvalue=0 → chemin malloc échoue */
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
    struct part cand[1] = { { .id = 1 } };
    struct array_part candlist = { .size = 1, .parts = cand };
    map_big_array *map = sl_make_uniform_map(&candlist);
    struct array_part rp = { .size = 0, .parts = NULL };

    struct possibility_packet *p = new_zeroed_packet();
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            p->grid[x][y] = -2;

    int16_t idParts[ETERN_PARTS][4];
    fill_id_parts(idParts);

    /* sizeofvalue=0 → put_possibility retourne 0 au premier appel */
    File result;
    init_file(&result, 0);

    int max = search_possiblity_light(&result, p, map, &rp, idParts);
    ASSERT_EQ_FMT(0, max, "%d");
    ASSERT_EQ_FMT(0ULL, (unsigned long long)result.size, "%llu");

    free(p);
    PASS();
}

SUITE(possibility_suite)
{
    RUN_TEST(test_directions_covers_every_cell);
    RUN_TEST(test_directions_detects_missing_cell);
    RUN_TEST(decode_direction_runs);
    RUN_TEST(check_possibility_null_packet_is_minus_one);
    RUN_TEST(check_possibility_out_of_bounds_is_minus_two);
    RUN_TEST(check_possibility_out_of_bounds_y_is_minus_two);
    RUN_TEST(check_possibility_alloc_too_large_is_minus_four);
    RUN_TEST(check_possibility_alloc_exceeds_faceused_is_minus_five);
    RUN_TEST(check_possibility_loads_parts_when_rotate_parts_null);
#if ETERN_PARTS == 256
    RUN_TEST(check_possibility_missing_genesis_anchor_is_minus_six);
    RUN_TEST(check_possibility_valid_genesis_is_zero);
    RUN_TEST(check_possibility_top_left_empty_neighbors_is_zero);
    RUN_TEST(synthetic_map_resolves_each_index_key);
    RUN_TEST(synthetic_map_omitting_index_yields_null_key);
    RUN_TEST(first_possibility_missing_each_index_is_fatal);
    RUN_TEST(first_possibility_valid_injects_one_possibility);
#endif
#if ETERN_PARTS != 256
    RUN_TEST(check_possibility_invalid_grid_value_is_minus_seven);
    RUN_TEST(check_possibility_grid_value_too_large_is_minus_seven);
    RUN_TEST(check_possibility_border_mismatch_is_minus_nine);
    RUN_TEST(check_possibility_neighbor_mismatch_is_minus_nine);
    RUN_TEST(check_possibility_left_border_mismatch_is_minus_nine);
    RUN_TEST(check_possibility_consistent_interior_neighbors_is_zero);
    RUN_TEST(check_possibility_bottom_mismatch_is_minus_nine);
    RUN_TEST(check_possibility_left_mismatch_is_minus_nine);
    RUN_TEST(check_possibility_covers_all_placed_cells_on_a_holed_board);
#endif
    RUN_TEST(compare_possibility_detects_each_difference);
    RUN_TEST(is_origin_of_recognizes_prefix);
    RUN_TEST(build_single_array_wraps_one_packet);
    RUN_TEST(generate_possibility_packet_encodes_grid);
    RUN_TEST(placed_count_empty_grid_is_zero);
    RUN_TEST(placed_count_counts_holes_beyond_a_cursor);
    RUN_TEST(placed_count_full_grid_is_etern_parts);
    RUN_TEST(save_possibility_writes_packet_to_file);
    RUN_TEST(save_possibility_unwritable_exits);
    RUN_TEST(fprint_possibility_packet_writes_json);
    RUN_TEST(fprint_possibility_packet_reports_write_failure);
    RUN_TEST(save_solution_csv_writes_header_and_rows);
    RUN_TEST(save_solution_csv_null_parts_writes_minus_one_faces);
    RUN_TEST(save_solution_csv_unwritable_returns_minus_one);
    RUN_TEST(check_if_result_found_below_complete_is_noop);
    RUN_TEST(check_if_result_found_complete_exits_success);
    RUN_TEST(log_solution_writes_distinct_files_for_each_solution);
    RUN_TEST(what_search_corner_of_empty_board);
    RUN_TEST(what_search_reads_placed_neighbor);
    RUN_TEST(what_search_bottom_right_with_neighbors);
    RUN_TEST(what_search_interior_empty_neighbors);
    RUN_TEST(what_search_to_key_uses_all_face_for_empty);
    RUN_TEST(what_search_to_key_uses_neighbor_faces_when_present);
    RUN_TEST(what_search_to_key_right_bottom_edges_and_present);
    RUN_TEST(what_search_in_grid_to_key_arbitrary_cell);
    RUN_TEST(possibility_has_a_next_finds_and_excludes_used);
    RUN_TEST(possibility_has_a_next_empty_compartment_returns_zero);
    RUN_TEST(possibility_has_a_next_skips_zero_id_and_used_piece);
    RUN_TEST(search_light_expands_one_per_candidate);
    RUN_TEST(search_light_skips_zero_id_and_used_candidate);
    RUN_TEST(search_light_skips_prefilled_cell);
    RUN_TEST(put_possibility_returns_zero_on_bad_sizeofvalue);
    RUN_TEST(search_light_aborts_on_put_failure);
    RUN_TEST(search_light_completes_board_skips_forward_check);
    RUN_TEST(forward_check_detects_dead_cell);
    RUN_TEST(forward_check_passes_when_cells_filled);
    RUN_TEST(forward_check_finds_late_empty_cell_without_overrun);
    RUN_TEST(forward_check_skips_zero_id_finds_free_candidate);
    RUN_TEST(forward_check_detects_all_candidates_already_used);
    RUN_TEST(what_search_to_key_empty_and_placed_neighbor);
    RUN_TEST(print_possibility_packet_runs);
    RUN_TEST(print_possibility_packet_survives_max_width_grid);
    RUN_TEST(log_error_possibility_packet_persists_to_events_log);
    RUN_TEST(all_has_a_next_all_filled_returns_one);
    RUN_TEST(all_has_a_next_dead_cell_returns_zero);
    RUN_TEST(all_has_a_next_single_candidate_places_piece);
    RUN_TEST(all_has_a_next_multi_candidate_skips_used_piece);
    RUN_TEST(all_has_a_next_counted_full_scan_counts_all_cells);
    RUN_TEST(all_has_a_next_counted_skips_already_filled_cells);
    RUN_TEST(all_has_a_next_counted_dead_first_cell_counts_one);
    RUN_TEST(all_has_a_next_counted_complete_board_counts_zero);
#if ETERN_PARTS == 256
    RUN_TEST(all_has_a_next_unconstrained_cell_does_not_hide_later_dead_cell);
    RUN_TEST(all_has_a_next_fixpoint_detects_cascading_forced_dead_cell);
    RUN_TEST(all_has_a_next_fixpoint_isolated_force_still_returns_one);
#endif
}
