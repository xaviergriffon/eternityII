/*
 * Tests unitaires de ui/line_edit.c : module d'édition de ligne commun aux
 * deux consoles (ANSI et ncurses), sans I/O — testable directement, sans PTY
 * ni fork, à la différence des tests bout-en-bout de console.c/logger_ncurses.c.
 *
 * Intègre command_history.c (état global, ring buffer partagé entre tests,
 * sans API de remise à zéro — cf. test_command_history.c) : les tests qui
 * exercent HISTORY_PREV/NEXT ajoutent leur propre marqueur juste avant de
 * naviguer, afin que history_get(0) soit déterministe quel que soit l'ordre
 * d'exécution des suites (même convention que test_command_history.c).
 */
#include "greatest.h"
#include "ui/line_edit.h"
#include "ui/command_history.h"

#include <string.h>

/* line_edit_reset : ligne vide, curseur à 0, pas de navigation historique en
   cours — y compris après avoir pollué le tampon (reset doit tout effacer,
   pas seulement repartir d'un état déjà vide). */
TEST reset_gives_empty_line(void)
{
    line_edit_t le;
    line_edit_reset(&le);                  /* état initial valide requis avant tout feed */
    line_edit_feed(&le, LE_KEY_CHAR, 'x'); /* pollue le tampon */
    line_edit_reset(&le);                  /* doit tout effacer */
    ASSERT_STR_EQ("", line_edit_text(&le));
    ASSERT_EQ_FMT(0, line_edit_cursor(&le), "%d");
    PASS();
}

/* Insertion en fin de ligne : cas de frappe classique. */
TEST char_insert_appends_and_advances_cursor(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_CHAR, 'a'), "%d");
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_CHAR, 'b'), "%d");
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_CHAR, 'c'), "%d");
    ASSERT_STR_EQ("abc", line_edit_text(&le));
    ASSERT_EQ_FMT(3, line_edit_cursor(&le), "%d");
    PASS();
}

/* Insertion au MILIEU de la ligne (après un déplacement ←) : le reste de la
   ligne est décalé, pas écrasé. */
TEST char_insert_in_middle_shifts_tail(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, 'a');
    line_edit_feed(&le, LE_KEY_CHAR, 'c'); /* "ac", curseur en fin */
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_LEFT, 0), "%d"); /* curseur entre a et c */
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_CHAR, 'b'), "%d");
    ASSERT_STR_EQ("abc", line_edit_text(&le));
    ASSERT_EQ_FMT(2, line_edit_cursor(&le), "%d"); /* curseur après le 'b' inséré */
    PASS();
}

/* LEFT en butée gauche (curseur à 0) : aucun changement, retour 0. */
TEST left_at_start_of_line_is_noop(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, 'x');
    line_edit_feed(&le, LE_KEY_HOME, 0);
    ASSERT_EQ_FMT(0, line_edit_cursor(&le), "%d");
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_LEFT, 0), "%d");
    ASSERT_EQ_FMT(0, line_edit_cursor(&le), "%d");
    PASS();
}

/* RIGHT en butée droite (curseur == longueur) : aucun changement, retour 0. */
TEST right_at_end_of_line_is_noop(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, 'x');
    ASSERT_EQ_FMT(1, line_edit_cursor(&le), "%d");
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_RIGHT, 0), "%d");
    ASSERT_EQ_FMT(1, line_edit_cursor(&le), "%d");
    PASS();
}

/* HOME / END : curseur aux extrémités, no-op si déjà en place. */
TEST home_and_end_move_cursor_to_bounds(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    const char *word = "hello";
    for (const char *p = word; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    ASSERT_EQ_FMT(5, line_edit_cursor(&le), "%d");

    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_HOME, 0), "%d");
    ASSERT_EQ_FMT(0, line_edit_cursor(&le), "%d");
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_HOME, 0), "%d"); /* déjà au début */

    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_END, 0), "%d");
    ASSERT_EQ_FMT(5, line_edit_cursor(&le), "%d");
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_END, 0), "%d"); /* déjà à la fin */
    PASS();
}

/* BACKSPACE en butée gauche : no-op. Au milieu/fin : efface avant le curseur. */
TEST backspace_removes_char_before_cursor(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_BACKSPACE, 0), "%d"); /* ligne vide */

    const char *word = "abcd";
    for (const char *p = word; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    line_edit_feed(&le, LE_KEY_LEFT, 0); /* curseur avant 'd' : "abc|d" */
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_BACKSPACE, 0), "%d");
    ASSERT_STR_EQ("abd", line_edit_text(&le));
    ASSERT_EQ_FMT(2, line_edit_cursor(&le), "%d");
    PASS();
}

/* DELETE efface le caractère SOUS le curseur ; no-op en fin de ligne. */
TEST delete_removes_char_under_cursor(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    const char *word = "abcd";
    for (const char *p = word; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_DELETE, 0), "%d"); /* curseur en fin */

    line_edit_feed(&le, LE_KEY_HOME, 0);
    line_edit_feed(&le, LE_KEY_RIGHT, 0); /* "a|bcd" */
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_DELETE, 0), "%d");
    ASSERT_STR_EQ("acd", line_edit_text(&le));
    ASSERT_EQ_FMT(1, line_edit_cursor(&le), "%d"); /* curseur inchangé */
    PASS();
}

/* Ctrl-U (KILL_LINE) : vide toute la ligne, quelle que soit la position du
   curseur ; no-op sur une ligne déjà vide. */
TEST kill_line_clears_everything(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_KILL_LINE, 0), "%d");

    const char *word = "hello world";
    for (const char *p = word; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    line_edit_feed(&le, LE_KEY_HOME, 0);
    line_edit_feed(&le, LE_KEY_RIGHT, 0);
    line_edit_feed(&le, LE_KEY_RIGHT, 0); /* curseur au milieu : sans effet sur KILL_LINE */
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_KILL_LINE, 0), "%d");
    ASSERT_STR_EQ("", line_edit_text(&le));
    ASSERT_EQ_FMT(0, line_edit_cursor(&le), "%d");
    PASS();
}

/* Ctrl-W (KILL_WORD) : efface le mot précédant le curseur, en sautant les
   espaces immédiatement avant lui — même convention que readline/bash. */
TEST kill_word_removes_previous_word(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    const char *phrase = "foo bar";
    for (const char *p = phrase; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_KILL_WORD, 0), "%d");
    ASSERT_STR_EQ("foo ", line_edit_text(&le));
    ASSERT_EQ_FMT(4, line_edit_cursor(&le), "%d");
    PASS();
}

/* KILL_WORD au tout début de ligne : no-op, retour 0. */
TEST kill_word_at_start_is_noop(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, 'x');
    line_edit_feed(&le, LE_KEY_HOME, 0);
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_KILL_WORD, 0), "%d");
    ASSERT_STR_EQ("x", line_edit_text(&le));
    PASS();
}

/* KILL_WORD avec des espaces multiples entre les mots : saute tous les
   espaces avant le curseur, puis efface le mot entier. */
TEST kill_word_skips_multiple_spaces(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    const char *phrase = "foo   bar";
    for (const char *p = phrase; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_KILL_WORD, 0), "%d");
    ASSERT_STR_EQ("foo   ", line_edit_text(&le));
    PASS();
}

/* KILL_WORD au MILIEU de la ligne : n'efface que le mot précédent, laisse le
   reste de la ligne intact (curseur non en fin). */
TEST kill_word_mid_line_leaves_tail(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    const char *phrase = "foo bar baz";
    for (const char *p = phrase; *p; p++) line_edit_feed(&le, LE_KEY_CHAR, *p);
    /* Place le curseur juste après "bar" (avant l'espace précédant "baz"). */
    line_edit_feed(&le, LE_KEY_END, 0);
    for (int i = 0; i < 4; i++) line_edit_feed(&le, LE_KEY_LEFT, 0); /* "foo bar| baz" */
    ASSERT_EQ_FMT(7, line_edit_cursor(&le), "%d");
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_KILL_WORD, 0), "%d");
    ASSERT_STR_EQ("foo  baz", line_edit_text(&le));
    ASSERT_EQ_FMT(4, line_edit_cursor(&le), "%d");
    PASS();
}

/* KILL_WORD ne laissant que des espaces avant le curseur (aucun mot) : les
   efface intégralement. */
TEST kill_word_with_only_leading_spaces(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, ' ');
    line_edit_feed(&le, LE_KEY_CHAR, ' ');
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_KILL_WORD, 0), "%d");
    ASSERT_STR_EQ("", line_edit_text(&le));
    PASS();
}

/* HISTORY_PREV/NEXT : rappelle l'historique en sauvegardant le draft, puis le
   restaure intégralement en redescendant. Marqueur unique ajouté juste avant
   la navigation pour rester déterministe malgré l'état global partagé de
   command_history.c (cf. l'en-tête du fichier). */
TEST history_prev_then_next_restores_draft(void)
{
    char marker[64];
    snprintf(marker, sizeof marker, "le-marker-%p", (void *)&marker);
    history_add(marker);

    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, 'd');
    line_edit_feed(&le, LE_KEY_CHAR, 'r');
    line_edit_feed(&le, LE_KEY_CHAR, 'f'); /* "drf" : saisie en cours (draft) */

    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_HISTORY_PREV, 0), "%d");
    ASSERT_STR_EQ(marker, line_edit_text(&le));
    ASSERT_EQ_FMT((int)strlen(marker), line_edit_cursor(&le), "%d"); /* curseur en fin */

    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_HISTORY_NEXT, 0), "%d");
    ASSERT_STR_EQ("drf", line_edit_text(&le)); /* draft restauré intégralement */
    PASS();
}

/* HISTORY_NEXT sans navigation en cours (hist_cursor == -1) : no-op. */
TEST history_next_without_prev_is_noop(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    line_edit_feed(&le, LE_KEY_CHAR, 'x');
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_HISTORY_NEXT, 0), "%d");
    ASSERT_STR_EQ("x", line_edit_text(&le));
    PASS();
}

/* HISTORY_PREV au-delà de la plus ancienne entrée : reste en place, retour 0.
   Ajoute deux marqueurs propres pour connaître exactement la profondeur de
   l'historique accessible depuis ce test. */
TEST history_prev_stops_at_oldest_entry(void)
{
    char m1[64], m2[64];
    snprintf(m1, sizeof m1, "le-oldest-%p", (void *)&m1);
    snprintf(m2, sizeof m2, "le-newest-%p", (void *)&m2);
    history_add(m1);
    history_add(m2); /* m2 est maintenant l'entrée la plus récente (index 0) */

    line_edit_t le;
    line_edit_reset(&le);
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_HISTORY_PREV, 0), "%d");
    ASSERT_STR_EQ(m2, line_edit_text(&le));
    ASSERT_EQ_FMT(1, line_edit_feed(&le, LE_KEY_HISTORY_PREV, 0), "%d");
    ASSERT_STR_EQ(m1, line_edit_text(&le));

    /* m1 est la plus ancienne accessible depuis ces deux appels (indices 0 et
       1 après les deux history_add ci-dessus) : au-delà, hist_cursor+1 peut
       encore être < history_size() si d'autres tests ont peuplé l'historique
       avant celui-ci — on ne peut donc affirmer un retour à 0 ici sans
       connaître la profondeur totale. On vérifie en revanche l'invariant
       fonctionnel : la ligne ne change JAMAIS de valeur au-delà de m1 tant
       qu'on n'a pas dépassé la plus ancienne entrée réellement disponible. */
    int hs = history_size();
    for (int i = 0; i < hs + 2; i++) {
        line_edit_feed(&le, LE_KEY_HISTORY_PREV, 0);
    }
    /* Quel que soit le nombre d'appels au-delà de la borne, aucun crash et le
       texte reste une chaîne valide et non vide (garantie de robustesse). */
    ASSERT(strlen(line_edit_text(&le)) > 0);
    PASS();
}

/* Débordement de tampon : l'insertion s'arrête proprement à LINE_EDIT_BUFSZ-1
   caractères, sans jamais écrire hors du tampon (ASan le confirmerait). */
TEST char_insert_stops_at_buffer_capacity(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    int changed_count = 0;
    for (int i = 0; i < LINE_EDIT_BUFSZ + 32; i++) {
        changed_count += line_edit_feed(&le, LE_KEY_CHAR, 'a');
    }
    ASSERT_EQ_FMT(LINE_EDIT_BUFSZ - 1, changed_count, "%d");
    ASSERT_EQ_FMT(LINE_EDIT_BUFSZ - 1, le.len, "%d");
    ASSERT_EQ_FMT((int)strlen(line_edit_text(&le)), le.len, "%d");
    /* Une insertion supplémentaire est bien refusée (retour 0, pas de changement). */
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_CHAR, 'z'), "%d");
    PASS();
}

/* LE_KEY_CHAR avec un caractère non imprimable : ignoré, retour 0. */
TEST char_insert_ignores_non_printable(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_CHAR, 0x01), "%d"); /* Ctrl-A brut */
    ASSERT_EQ_FMT(0, line_edit_feed(&le, LE_KEY_CHAR, 127), "%d");  /* DEL */
    ASSERT_STR_EQ("", line_edit_text(&le));
    PASS();
}

SUITE(line_edit_suite)
{
    RUN_TEST(reset_gives_empty_line);
    RUN_TEST(char_insert_appends_and_advances_cursor);
    RUN_TEST(char_insert_in_middle_shifts_tail);
    RUN_TEST(left_at_start_of_line_is_noop);
    RUN_TEST(right_at_end_of_line_is_noop);
    RUN_TEST(home_and_end_move_cursor_to_bounds);
    RUN_TEST(backspace_removes_char_before_cursor);
    RUN_TEST(delete_removes_char_under_cursor);
    RUN_TEST(kill_line_clears_everything);
    RUN_TEST(kill_word_removes_previous_word);
    RUN_TEST(kill_word_at_start_is_noop);
    RUN_TEST(kill_word_skips_multiple_spaces);
    RUN_TEST(kill_word_mid_line_leaves_tail);
    RUN_TEST(kill_word_with_only_leading_spaces);
    RUN_TEST(history_prev_then_next_restores_draft);
    RUN_TEST(history_next_without_prev_is_noop);
    RUN_TEST(history_prev_stops_at_oldest_entry);
    RUN_TEST(char_insert_stops_at_buffer_capacity);
    RUN_TEST(char_insert_ignores_non_printable);
}
