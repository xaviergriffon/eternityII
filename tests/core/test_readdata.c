/*
 * Tests unitaires du module readdata.c (parsing du fichier CSV de pièces).
 *
 * On écrit un petit CSV temporaire plutôt que de dépendre de pieces.csv /
 * pieces16.csv : le test est ainsi autonome et déterministe.
 *
 * Limite assumée : read_parts appelle exit(EXIT_FAILURE) sur erreur (fichier
 * absent, ligne malformée). greatest exécutant tout dans le même processus, on
 * ne teste donc QUE le chemin nominal — un exit() tuerait le runner entier.
 */
#include "greatest.h"
#include "core/readdata.h"
#include "core/part.h"
#include "fork_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* compute_grid n'est pas déclarée dans readdata.h (helper interne non statique). */
void compute_grid(struct possibility_packet *possibility, char *str_value);

/* Chemin partagé entre le parent et les fonctions-fils (copié par fork). */
static char g_csv_path[256];

/* Écrit `content` dans un fichier temporaire et renvoie son chemin (statique). */
static const char *write_temp_csv(const char *content)
{
    static char path[256];
    strcpy(path, "/tmp/etii_readdata_err_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    fputs(content, fp);
    fclose(fp);
    return path;
}

/* Fonction-fils : tente de lire le CSV pointé par g_csv_path (exit attendu). */
static void child_read_parts(void)
{
    struct array_part *a = read_parts(g_csv_path);
    free_array_part(a); /* non atteint si read_parts exit() */
}

/* Ordre des colonnes effectivement lu par read_parts : id top left bottom right. */
TEST read_parts_parses_a_well_formed_csv(void)
{
    char path[] = "/tmp/etii_readdata_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);

    FILE *fp = fdopen(fd, "w");
    ASSERT(fp != NULL);
    fputs("ntiles: 2\n"
          "1 10 20 30 40\n"
          "2 11 21 31 41\n",
          fp);
    fclose(fp);

    struct array_part *a = read_parts(path);
    unlink(path);

    ASSERT(a != NULL);
    ASSERT_EQ_FMT(2, a->size, "%d"); /* ntiles, hors pièce-bordure */

    /* pièce 0 : bordure insérée automatiquement, tous bords à 0 */
    ASSERT_EQ_FMT(0, (int)a->parts[0].id, "%d");
    ASSERT_EQ_FMT(0, (int)a->parts[0].top, "%d");
    ASSERT_EQ_FMT(0, (int)a->parts[0].left, "%d");

    /* pièce 1 : "1 10 20 30 40" -> id=1, top=10, left=20, bottom=30, right=40 */
    ASSERT_EQ_FMT(1, (int)a->parts[1].id, "%d");
    ASSERT_EQ_FMT(10, (int)a->parts[1].top, "%d");
    ASSERT_EQ_FMT(20, (int)a->parts[1].left, "%d");
    ASSERT_EQ_FMT(30, (int)a->parts[1].bottom, "%d");
    ASSERT_EQ_FMT(40, (int)a->parts[1].right, "%d");
    ASSERT_EQ_FMT(0, (int)a->parts[1].rotation, "%d");

    /* pièce 2 : "2 11 21 31 41" */
    ASSERT_EQ_FMT(2, (int)a->parts[2].id, "%d");
    ASSERT_EQ_FMT(11, (int)a->parts[2].top, "%d");
    ASSERT_EQ_FMT(41, (int)a->parts[2].right, "%d");

    free_array_part(a);
    PASS();
}

/*
 * Régression : read_parts ne doit lire QUE les ntiles pièces annoncées, ni
 * plus ni moins. L'ancienne boucle `while(!feof)` + `if(fscanf(...))` entrait
 * une itération de trop sur EOF (fscanf renvoie -1, traité comme « vrai »),
 * écrivant parts[np+1] hors du buffer (heap overflow d'un élément, détecté
 * sous AddressSanitizer). Ce test fixe le contrat sur a->size ; il est surtout
 * probant compilé avec -fsanitize=address (cf. tests/README.md).
 */
TEST read_parts_reads_exactly_ntiles(void)
{
    char path[] = "/tmp/etii_readdata_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);

    FILE *fp = fdopen(fd, "w");
    ASSERT(fp != NULL);
    /* Saut de ligne final : c'est le cas qui déclenchait l'itération de trop. */
    fputs("ntiles: 3\n"
          "1 10 20 30 40\n"
          "2 11 21 31 41\n"
          "3 12 22 32 42\n",
          fp);
    fclose(fp);

    struct array_part *a = read_parts(path);
    unlink(path);

    ASSERT(a != NULL);
    ASSERT_EQ_FMT(3, a->size, "%d");

    /* Dernière pièce bien lue (pas écrasée par une lecture parasite). */
    ASSERT_EQ_FMT(3, (int)a->parts[3].id, "%d");
    ASSERT_EQ_FMT(12, (int)a->parts[3].top, "%d");
    ASSERT_EQ_FMT(42, (int)a->parts[3].right, "%d");

    free_array_part(a);
    PASS();
}

/* --------------------------------------------------------------------------
 * Chemins d'erreur de read_parts (appellent exit) — testés via fork.
 * EXIT_FAILURE == 1.
 * ------------------------------------------------------------------------ */

TEST read_parts_missing_file_exits(void)
{
    strcpy(g_csv_path, "/tmp/etii_does_not_exist_zzz_4242");
    unlink(g_csv_path); /* on s'assure qu'il n'existe pas */
    ASSERT_EQ_FMT(EXIT_FAILURE, run_in_fork(child_read_parts, NULL), "%d");
    PASS();
}

TEST read_parts_bad_header_exits(void)
{
    const char *p = write_temp_csv("pas_de_ntiles_ici\n1 10 20 30 40\n");
    ASSERT(p != NULL);
    strcpy(g_csv_path, p);
    int code = run_in_fork(child_read_parts, NULL);
    unlink(g_csv_path);
    ASSERT_EQ_FMT(EXIT_FAILURE, code, "%d");
    PASS();
}

TEST read_parts_malformed_line_exits(void)
{
    const char *p = write_temp_csv("ntiles: 2\n1 10 20 30 40\nGARBAGE\n");
    ASSERT(p != NULL);
    strcpy(g_csv_path, p);
    int code = run_in_fork(child_read_parts, NULL);
    unlink(g_csv_path);
    ASSERT_EQ_FMT(EXIT_FAILURE, code, "%d");
    PASS();
}

TEST read_parts_too_many_pieces_exits(void)
{
    /* ntiles annonce 1 mais le fichier en contient 2 */
    const char *p = write_temp_csv("ntiles: 1\n1 10 20 30 40\n2 11 21 31 41\n");
    ASSERT(p != NULL);
    strcpy(g_csv_path, p);
    int code = run_in_fork(child_read_parts, NULL);
    unlink(g_csv_path);
    ASSERT_EQ_FMT(EXIT_FAILURE, code, "%d");
    PASS();
}

TEST read_parts_too_few_pieces_exits(void)
{
    /* ntiles annonce 3 mais le fichier n'en contient qu'1 */
    const char *p = write_temp_csv("ntiles: 3\n1 10 20 30 40\n");
    ASSERT(p != NULL);
    strcpy(g_csv_path, p);
    int code = run_in_fork(child_read_parts, NULL);
    unlink(g_csv_path);
    ASSERT_EQ_FMT(EXIT_FAILURE, code, "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * compute_grid : remplissage colonne par colonne depuis une liste d'entiers.
 * ------------------------------------------------------------------------ */

TEST compute_grid_fills_cells_in_order(void)
{
    struct possibility_packet *p = calloc(1, sizeof(struct possibility_packet));

    char values[] = "5 6 7"; /* x croît, y reste 0 tant que x < ETERN_SIZE */
    compute_grid(p, values);

    ASSERT_EQ_FMT(5, (int)p->grid[0][0], "%d");
    ASSERT_EQ_FMT(6, (int)p->grid[1][0], "%d");
    ASSERT_EQ_FMT(7, (int)p->grid[2][0], "%d");
    /* les pièces (valeurs >= 0) sont marquées utilisées (id % ETERN_PARTS) */
    ASSERT_EQ_FMT(1, (int)is_face_used(p->b_faceused, 5), "%d");

    free(p);
    PASS();
}

/* --------------------------------------------------------------------------
 * read_from_json : désérialisation alloc / x / y / grid.
 * read_from_json écrit sur stdout (printf de debug) -> on la museler le temps
 * de l'appel pour ne pas polluer la sortie du runner.
 * ------------------------------------------------------------------------ */

TEST read_from_json_parses_scalars_and_grid(void)
{
    int saved = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);

    struct possibility_packet *p =
        read_from_json("{\"alloc\": 5, \"x\": 3, \"y\": 7, \"grid\": [[1, 2], [3, 4]]}");

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(devnull);

    ASSERT(p != NULL);
    ASSERT_EQ_FMT(5, (int)p->alloc, "%d");
    ASSERT_EQ_FMT(3, (int)p->x, "%d");
    ASSERT_EQ_FMT(7, (int)p->y, "%d");
    /* grid alimentée par compute_grid : 1,2,3,4 en colonne 0 */
    ASSERT_EQ_FMT(1, (int)p->grid[0][0], "%d");
    ASSERT_EQ_FMT(2, (int)p->grid[1][0], "%d");

    free(p);
    PASS();
}

/* compute_grid : str_value sans aucun entier -> regexec renvoie REG_NOMATCH dès
 * la première passe (« groupe non trouvé »), la grille reste intacte. */
TEST compute_grid_no_match_leaves_grid_untouched(void)
{
    struct possibility_packet *p = calloc(1, sizeof(struct possibility_packet));
    p->grid[0][0] = -42; /* sentinelle : ne doit pas être écrasée */

    int saved = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);

    compute_grid(p, "aucun chiffre ici");

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(devnull);

    ASSERT_EQ_FMT(-42, (int)p->grid[0][0], "%d");
    free(p);
    PASS();
}

/* read_from_json : chaîne sans paire "clé": valeur -> REG_NOMATCH au 1er regexec,
 * possibility reste NULL (« groupe non trouvé »). */
TEST read_from_json_no_match_returns_null(void)
{
    int saved = dup(1);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);

    struct possibility_packet *p = read_from_json("pas de paires cle valeur ici");

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(devnull);

    ASSERT(p == NULL);
    PASS();
}

SUITE(readdata_suite)
{
    RUN_TEST(read_parts_parses_a_well_formed_csv);
    RUN_TEST(read_parts_reads_exactly_ntiles);
    RUN_TEST(read_parts_missing_file_exits);
    RUN_TEST(read_parts_bad_header_exits);
    RUN_TEST(read_parts_malformed_line_exits);
    RUN_TEST(read_parts_too_many_pieces_exits);
    RUN_TEST(read_parts_too_few_pieces_exits);
    RUN_TEST(compute_grid_fills_cells_in_order);
    RUN_TEST(compute_grid_no_match_leaves_grid_untouched);
    RUN_TEST(read_from_json_parses_scalars_and_grid);
    RUN_TEST(read_from_json_no_match_returns_null);
}
