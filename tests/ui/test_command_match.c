/*
 * Tests unitaires du module command_match.c (appariement approximatif de
 * commandes par distance de Levenshtein).
 *
 * Module pur extrait de command_lines.c (Phase 4a du découpage couverture) :
 * la liste des commandes est passée en paramètre, donc le test n'a besoin ni
 * de la table `commands[]` ni d'aucune dépendance de command_lines.c
 * (datamanager, sockets…).
 */
#include "greatest.h"
#include "ui/command_match.h"

#include <string.h>

/* Échantillon de commandes connues, calqué sur quelques entrées réelles. */
static const char *const KNOWN[] = {
    "sorta", "sortd", "backup", "restore", "help", "exit", "print", "split",
};
static const int KNOWN_N = (int)(sizeof(KNOWN) / sizeof(KNOWN[0]));

/* --------------------------------------------------------------------------
 * levenshtein
 * ------------------------------------------------------------------------ */

TEST levenshtein_identical_is_zero(void)
{
    ASSERT_EQ_FMT(0, levenshtein("backup", "backup"), "%d");
    PASS();
}

TEST levenshtein_empty_operands(void)
{
    ASSERT_EQ_FMT(4, levenshtein("", "abcd"), "%d");   /* 4 insertions */
    ASSERT_EQ_FMT(3, levenshtein("xyz", ""), "%d");    /* 3 suppressions */
    ASSERT_EQ_FMT(0, levenshtein("", ""), "%d");
    PASS();
}

TEST levenshtein_counts_single_edits(void)
{
    ASSERT_EQ_FMT(1, levenshtein("backup", "backup_"), "%d"); /* 1 insertion */
    ASSERT_EQ_FMT(1, levenshtein("backup", "backp"), "%d");   /* 1 suppression */
    ASSERT_EQ_FMT(1, levenshtein("backup", "backip"), "%d");  /* 1 substitution */
    ASSERT_EQ_FMT(3, levenshtein("kitten", "sitting"), "%d"); /* cas classique */
    PASS();
}

/* La distance est symétrique. */
TEST levenshtein_is_symmetric(void)
{
    ASSERT_EQ_FMT(levenshtein("restore", "store"),
                  levenshtein("store", "restore"), "%d");
    PASS();
}

/* --------------------------------------------------------------------------
 * closest_command
 * ------------------------------------------------------------------------ */

/* Une faute de frappe proche d'une commande connue est suggérée. */
TEST closest_command_suggests_near_typo(void)
{
    /* "bakup" -> "backup" (distance 1, seuil = 6/3 = 2) */
    const char *s = closest_command("bakup", KNOWN, KNOWN_N);
    ASSERT(s != NULL);
    ASSERT_STR_EQ("backup", s);

    /* "helo" -> "help" (distance 1, seuil borné à 1) */
    s = closest_command("helo", KNOWN, KNOWN_N);
    ASSERT(s != NULL);
    ASSERT_STR_EQ("help", s);
    PASS();
}

/* Une saisie trop éloignée de toute commande ne suggère rien (NULL). */
TEST closest_command_returns_null_when_too_far(void)
{
    /* aucune commande connue n'est à <= seuil de "zzzzzz" */
    ASSERT_EQ(NULL, closest_command("zzzzzz", KNOWN, KNOWN_N));
    PASS();
}

/* Une correspondance exacte renvoie la commande elle-même (distance 0). */
TEST closest_command_exact_match(void)
{
    const char *s = closest_command("restore", KNOWN, KNOWN_N);
    ASSERT(s != NULL);
    ASSERT_STR_EQ("restore", s);
    PASS();
}

/* Le seuil est relatif à la longueur de la commande (1/3, borné [1,3]) :
   une commande courte tolère moins d'écart qu'une longue. */
TEST closest_command_threshold_scales_with_length(void)
{
    /* "exit" (len 4, seuil = max(1, 4/3) = 1) : 2 éditions -> trop loin */
    ASSERT_EQ(NULL, closest_command("exot1", KNOWN, KNOWN_N));

    /* "restore" (len 7, seuil = 7/3 = 2) : "restor" (1) reste accepté */
    const char *s = closest_command("restor", KNOWN, KNOWN_N);
    ASSERT(s != NULL);
    ASSERT_STR_EQ("restore", s);
    PASS();
}

SUITE(command_match_suite)
{
    RUN_TEST(levenshtein_identical_is_zero);
    RUN_TEST(levenshtein_empty_operands);
    RUN_TEST(levenshtein_counts_single_edits);
    RUN_TEST(levenshtein_is_symmetric);
    RUN_TEST(closest_command_suggests_near_typo);
    RUN_TEST(closest_command_returns_null_when_too_far);
    RUN_TEST(closest_command_exact_match);
    RUN_TEST(closest_command_threshold_scales_with_length);
}
