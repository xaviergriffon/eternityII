/*
 * Tests unitaires du module command_history.c (historique de commandes en
 * mémoire, ring buffer de HISTORY_MAX=100 entrées).
 *
 * command_history.c n'a aucune dépendance hors libc mais conserve un état
 * global statique persistant entre les tests, sans API de remise à zéro. Les
 * tests sont donc écrits pour être indépendants de l'ordre d'exécution : ils
 * raisonnent en relatif (delta de history_size, dernières entrées ajoutées)
 * plutôt que sur une taille absolue.
 */
#include "greatest.h"
#include "ui/command_history.h"

#include <stddef.h>
#include <stdio.h>

#define HISTORY_MAX 100 /* doit correspondre à la constante de command_history.c */

/* history_add ignore les chaînes vides et NULL. */
TEST add_ignores_null_and_empty(void)
{
    int before = history_size();
    history_add(NULL);
    history_add("");
    ASSERT_EQ_FMT(before, history_size(), "%d");
    PASS();
}

/* Après un ajout, l'entrée la plus récente est à l'index 0. */
TEST get_returns_most_recent_at_index_zero(void)
{
    history_add("alpha");
    history_add("beta");
    ASSERT_STR_EQ("beta", history_get(0));
    ASSERT_STR_EQ("alpha", history_get(1));
    PASS();
}

/* Les doublons consécutifs sont ignorés (comportement shell). */
TEST add_ignores_consecutive_duplicates(void)
{
    history_add("uniqueA");
    int after_first = history_size();
    history_add("uniqueA");
    history_add("uniqueA");
    ASSERT_EQ_FMT(after_first, history_size(), "%d");
    ASSERT_STR_EQ("uniqueA", history_get(0));
    PASS();
}

/* Un doublon non consécutif est bien réenregistré. */
TEST add_keeps_non_consecutive_duplicates(void)
{
    history_add("dupX");
    history_add("between");
    history_add("dupX");
    ASSERT_STR_EQ("dupX", history_get(0));
    ASSERT_STR_EQ("between", history_get(1));
    ASSERT_STR_EQ("dupX", history_get(2));
    PASS();
}

/* Index hors bornes (négatif ou >= taille) renvoie NULL. */
TEST get_out_of_bounds_returns_null(void)
{
    history_add("only");
    ASSERT_EQ(NULL, history_get(-1));
    ASSERT_EQ(NULL, history_get(history_size())); /* premier index hors bornes */
    ASSERT_EQ(NULL, history_get(history_size() + 5));
    PASS();
}

/* history_size est plafonné à HISTORY_MAX même après débordement du ring. */
TEST size_is_capped_at_history_max(void)
{
    char buf[32];
    for (int i = 0; i < HISTORY_MAX + 50; i++) {
        snprintf(buf, sizeof(buf), "cmd-%d", i);
        history_add(buf);
    }
    ASSERT_EQ_FMT(HISTORY_MAX, history_size(), "%d");

    /* La dernière ajoutée reste accessible à l'index 0... */
    snprintf(buf, sizeof(buf), "cmd-%d", HISTORY_MAX + 50 - 1);
    ASSERT_STR_EQ(buf, history_get(0));

    /* ...et l'entrée la plus ancienne encore conservée est cohérente avec le
       ring (la plus vieille a été évincée). L'index HISTORY_MAX est hors bornes. */
    ASSERT_EQ(NULL, history_get(HISTORY_MAX));
    PASS();
}

SUITE(command_history_suite)
{
    RUN_TEST(add_ignores_null_and_empty);
    RUN_TEST(get_returns_most_recent_at_index_zero);
    RUN_TEST(add_ignores_consecutive_duplicates);
    RUN_TEST(add_keeps_non_consecutive_duplicates);
    RUN_TEST(get_out_of_bounds_returns_null);
    RUN_TEST(size_is_capped_at_history_max);
}
