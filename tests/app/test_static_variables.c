/*
 * Tests unitaires de static_variables.c — pour l'instant le parsing des options
 * globales de la ligne de commande (parse_cli_options).
 *
 * Régression visée : l'option --stop-on-solution doit être reconnue à n'importe
 * quelle position, positionner le drapeau global, et être RETIRÉE de argv sans
 * abîmer les arguments positionnels des modes (tcpserver/tcpclient/…). Une
 * erreur ici décalerait les arguments (nb_threads lu sur le mauvais token, etc.).
 */
#include "greatest.h"
#include "app/static_variables.h"

#include <string.h>

/* parse_cli_options positionne le global stop_on_solution : on le remet à 0
   avant chaque cas pour l'isolation. */

TEST flag_absent_leaves_argv_and_flag_untouched(void)
{
    stop_on_solution = 0;
    const char *argv[] = {"prog", "tcpserver", "1", "data/pieces16.csv"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(4, argc, "%d");
    ASSERT_EQ_FMT(0, stop_on_solution, "%d");
    ASSERT_STR_EQ("tcpserver", argv[1]);
    ASSERT_STR_EQ("1", argv[2]);
    ASSERT_STR_EQ("data/pieces16.csv", argv[3]);
    PASS();
}

TEST flag_at_end_is_stripped_and_sets_global(void)
{
    stop_on_solution = 0;
    const char *argv[] = {"prog", "tcpserver", "1", "data/pieces16.csv", "--stop-on-solution"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(4, argc, "%d");          /* l'option a été retirée */
    ASSERT_EQ_FMT(1, stop_on_solution, "%d");
    /* Les arguments positionnels restent intacts et dans l'ordre. */
    ASSERT_STR_EQ("tcpserver", argv[1]);
    ASSERT_STR_EQ("1", argv[2]);
    ASSERT_STR_EQ("data/pieces16.csv", argv[3]);
    PASS();
}

TEST flag_in_the_middle_does_not_shift_positional_args(void)
{
    stop_on_solution = 0;
    const char *argv[] = {"prog", "tcpclient", "--stop-on-solution", "localhost", "2", "1000"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(5, argc, "%d");
    ASSERT_EQ_FMT(1, stop_on_solution, "%d");
    /* localhost/2/1000 doivent se retrouver compactés derrière le mode, sans
       trou laissé par l'option supprimée. */
    ASSERT_STR_EQ("tcpclient", argv[1]);
    ASSERT_STR_EQ("localhost", argv[2]);
    ASSERT_STR_EQ("2", argv[3]);
    ASSERT_STR_EQ("1000", argv[4]);
    PASS();
}

SUITE(static_variables_suite)
{
    RUN_TEST(flag_absent_leaves_argv_and_flag_untouched);
    RUN_TEST(flag_at_end_is_stripped_and_sets_global);
    RUN_TEST(flag_in_the_middle_does_not_shift_positional_args);
}
