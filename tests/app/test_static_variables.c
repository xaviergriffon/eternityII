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

/* --expand-level <n> : option VALUÉE. Les DEUX tokens (option + valeur) sont
   retirés d'argv, le niveau atterrit dans expand_min_level, et les arguments
   positionnels du mode restent intacts. */
TEST expand_level_strips_option_and_value_sets_global(void)
{
    expand_min_level = 0;
    stop_on_solution = 0;
    const char *argv[] = {"prog", "tcpserver", "--expand-level", "4", "8", "data/pieces.csv"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(4, argc, "%d");              /* option + valeur retirées (6 → 4) */
    ASSERT_EQ_FMT(4, expand_min_level, "%d");
    ASSERT_STR_EQ("tcpserver", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);               /* nb_threads non décalé */
    ASSERT_STR_EQ("data/pieces.csv", argv[3]);
    PASS();
}

/* Valeur absente (option en dernière position) : ignorée sans lire hors argv,
   expand_min_level reste à 0, l'option est tout de même consommée. */
TEST expand_level_without_value_is_ignored(void)
{
    expand_min_level = 0;
    const char *argv[] = {"prog", "tcpserver", "--expand-level"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");              /* seul le token option est retiré */
    ASSERT_EQ_FMT(0, expand_min_level, "%d");
    ASSERT_STR_EQ("tcpserver", argv[1]);
    PASS();
}

/* Valeur négative : bornée à 0 (pas d'expansion), plutôt qu'un niveau absurde. */
TEST expand_level_negative_clamped_to_zero(void)
{
    expand_min_level = 7;                        /* valeur résiduelle à écraser */
    const char *argv[] = {"prog", "tcpserver", "--expand-level", "-3"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ_FMT(0, expand_min_level, "%d");
    PASS();
}

/* Coexistence avec --stop-on-solution : les deux options position-indépendantes
   sont reconnues et retirées, la valeur de --expand-level est bien consommée
   (pas prise pour --stop-on-solution ni pour un argument positionnel). */
TEST expand_level_coexists_with_stop_on_solution(void)
{
    expand_min_level = 0;
    stop_on_solution = 0;
    const char *argv[] = {"prog", "--expand-level", "3", "tcpserver", "8", "--stop-on-solution"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(3, argc, "%d");              /* 6 - 2 (expand+val) - 1 (stop) */
    ASSERT_EQ_FMT(3, expand_min_level, "%d");
    ASSERT_EQ_FMT(1, stop_on_solution, "%d");
    ASSERT_STR_EQ("tcpserver", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);
    PASS();
}

/* --http-port <n> : option VALUÉE, même schéma que --expand-level. Valeur dans
   [1, 65535] : les deux tokens sont retirés d'argv, HTTP_PORT est fixé. */
TEST http_port_strips_option_and_value_sets_global(void)
{
    HTTP_PORT = 0;
    const char *argv[] = {"prog", "tcpserver", "--http-port", "8080", "data/pieces.csv"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_EQ_FMT(8080, HTTP_PORT, "%d");
    ASSERT_STR_EQ("tcpserver", argv[1]);
    ASSERT_STR_EQ("data/pieces.csv", argv[2]);
    PASS();
}

/* Valeur absente (dernière position) : ignorée, HTTP_PORT reste à 0 (désactivée). */
TEST http_port_without_value_is_ignored(void)
{
    HTTP_PORT = 0;
    const char *argv[] = {"prog", "tcpserver", "--http-port"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ_FMT(0, HTTP_PORT, "%d");
    PASS();
}

/* Valeurs hors [1, 65535] ("abc", "0", "-1", "70000") : toutes ignorées,
   HTTP_PORT reste à 0 — jamais un port au hasard ou hors plage. */
TEST http_port_out_of_range_values_are_ignored(void)
{
    const char *bad_values[] = {"abc", "0", "-1", "70000"};
    for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); i++) {
        HTTP_PORT = 0;
        const char *argv[] = {"prog", "tcpserver", "--http-port", bad_values[i]};
        int argc = parse_cli_options(4, argv);

        ASSERT_EQ_FMT(2, argc, "%d");
        ASSERT_EQ_FMT(0, HTTP_PORT, "%d");
    }
    PASS();
}

/* --help / -h : position-indépendantes comme --stop-on-solution — retirées
   d'argv, help_requested positionné, arguments positionnels intacts. */
TEST help_flag_is_stripped_and_sets_global(void)
{
    const char *flags[] = {"--help", "-h"};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        help_requested = 0;
        const char *argv[] = {"prog", "tcpserver", flags[i], "8"};
        int argc = parse_cli_options(4, argv);

        ASSERT_EQ_FMT(3, argc, "%d");
        ASSERT_EQ_FMT(1, help_requested, "%d");
        ASSERT_STR_EQ("tcpserver", argv[1]);
        ASSERT_STR_EQ("8", argv[2]);
    }
    PASS();
}

/* Sans --help ni -h, le drapeau reste à 0 ("help" positionnel = mode, pas option). */
TEST help_flag_absent_leaves_global_untouched(void)
{
    help_requested = 0;
    const char *argv[] = {"prog", "help", "tcpserver"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_EQ_FMT(0, help_requested, "%d");
    PASS();
}

/* request_is_pause : renvoie la durée d'attente (µs) propre à chaque origine
   de pause, 0 sinon. Régression visée : REQUEST_ADMIN_PAUSE doit être reconnue
   comme une pause par les boucles chaudes (usleep + continue) au même titre
   que REQUEST_PAUSE, mais avec une durée bien plus longue (pas de contrainte
   de précision sur une pause manuelle/distante, à l'inverse de la régulation
   de débit de REQUEST_PAUSE qui doit rester fine). */
TEST request_is_pause_covers_both_pause_values(void)
{
    ASSERT_EQ_FMT(0, (int)request_is_pause(REQUEST_STOP), "%d");
    ASSERT_EQ_FMT(0, (int)request_is_pause(REQUEST_CONTINUE), "%d");
    ASSERT_EQ_FMT((int)PAUSE_POLL_SLEEP_US, (int)request_is_pause(REQUEST_PAUSE), "%d");
    ASSERT_EQ_FMT((int)ADMIN_PAUSE_POLL_SLEEP_US, (int)request_is_pause(REQUEST_ADMIN_PAUSE), "%d");
    ASSERT(request_is_pause(REQUEST_ADMIN_PAUSE) > request_is_pause(REQUEST_PAUSE));
    PASS();
}

/* request_keeps_running : vrai pour tout sauf REQUEST_STOP. */
TEST request_keeps_running_is_false_only_on_stop(void)
{
    ASSERT_EQ_FMT(0, request_keeps_running(REQUEST_STOP), "%d");
    ASSERT_EQ_FMT(1, request_keeps_running(REQUEST_CONTINUE), "%d");
    ASSERT_EQ_FMT(1, request_keeps_running(REQUEST_PAUSE), "%d");
    ASSERT_EQ_FMT(1, request_keeps_running(REQUEST_ADMIN_PAUSE), "%d");
    PASS();
}

SUITE(static_variables_suite)
{
    RUN_TEST(flag_absent_leaves_argv_and_flag_untouched);
    RUN_TEST(flag_at_end_is_stripped_and_sets_global);
    RUN_TEST(flag_in_the_middle_does_not_shift_positional_args);
    RUN_TEST(expand_level_strips_option_and_value_sets_global);
    RUN_TEST(expand_level_without_value_is_ignored);
    RUN_TEST(expand_level_negative_clamped_to_zero);
    RUN_TEST(expand_level_coexists_with_stop_on_solution);
    RUN_TEST(http_port_strips_option_and_value_sets_global);
    RUN_TEST(http_port_without_value_is_ignored);
    RUN_TEST(http_port_out_of_range_values_are_ignored);
    RUN_TEST(help_flag_is_stripped_and_sets_global);
    RUN_TEST(help_flag_absent_leaves_global_untouched);
    RUN_TEST(request_is_pause_covers_both_pause_values);
    RUN_TEST(request_keeps_running_is_false_only_on_stop);
}
