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
#include "../readdata.h"
#include "../part.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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

SUITE(readdata_suite)
{
    RUN_TEST(read_parts_parses_a_well_formed_csv);
    RUN_TEST(read_parts_reads_exactly_ntiles);
}
