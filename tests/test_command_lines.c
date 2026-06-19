/*
 * Tests unitaires de command_lines.c — parsing et dispatch des commandes
 * interactives (do_command_line).
 *
 * On exerce les chemins sûrs : commande vide / inconnue (avec suggestion), et
 * quelques interprètes qui ne touchent qu'à des globaux (maxStockByThread,
 * prunerBatch, limit). Les commandes qui appellent exit() ou de lourdes
 * opérations de fichiers/réseau ne sont pas couvertes ici.
 *
 * send_command_to_childs est inerte ici : parent_pid (= 0 par défaut) != getpid(),
 * donc la garde interne empêche tout envoi (pas besoin de forkId).
 */
#include "greatest.h"
#include "../command_lines.h"
#include "../static_variables.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Exécute do_command_line en redirigeant stdout+stderr (les interprètes loguent). */
static int run_command_quiet(char *cmd)
{
    fflush(stdout); fflush(stderr);
    int s1 = dup(1), s2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    dup2(dn, 1); dup2(dn, 2); close(dn);

    int r = do_command_line(cmd);

    fflush(stdout); fflush(stderr);
    dup2(s1, 1); dup2(s2, 2);
    close(s1); close(s2);
    return r;
}

/* Entrées dégénérées : NULL, vide, espaces seuls -> 0, sans rien exécuter. */
TEST do_command_line_handles_empty_input(void)
{
    ASSERT_EQ_FMT(0, do_command_line(NULL), "%d");
    char empty[] = "";
    ASSERT_EQ_FMT(0, do_command_line(empty), "%d");
    char spaces[] = "    ";
    ASSERT_EQ_FMT(0, run_command_quiet(spaces), "%d");
    PASS();
}

/* Commande inconnue -> -1 (et suggestion loguée, ici silencieuse). */
TEST do_command_line_unknown_returns_error(void)
{
    char cmd[] = "definitelynotacommand";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

/* help : liste les commandes, retourne 0. */
TEST do_command_line_help_runs(void)
{
    char cmd[] = "help";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    PASS();
}

/* maxStockByThread <n> : fixe le global correspondant. */
TEST do_command_line_max_stock_sets_global(void)
{
    int saved = max_stock_by_thread;
    char cmd[] = "maxStockByThread 99";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(99, max_stock_by_thread, "%d");
    max_stock_by_thread = saved;
    PASS();
}

/* maxStockByThread sans argument -> -1 (erreur d'interprète). */
TEST do_command_line_max_stock_requires_arg(void)
{
    char cmd[] = "maxStockByThread";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

/* prunerBatch <n> : borné à [1, PRUNER_BATCH_MAX]. */
TEST do_command_line_pruner_batch_is_clamped(void)
{
    int saved = pruner_batch_size;

    char ok[] = "prunerBatch 8";
    ASSERT_EQ_FMT(0, run_command_quiet(ok), "%d");
    ASSERT_EQ_FMT(8, pruner_batch_size, "%d");

    char low[] = "prunerBatch 0"; /* < 1 -> ramené à 1 */
    ASSERT_EQ_FMT(0, run_command_quiet(low), "%d");
    ASSERT_EQ_FMT(1, pruner_batch_size, "%d");

    pruner_batch_size = saved;
    PASS();
}

SUITE(command_lines_suite)
{
    RUN_TEST(do_command_line_handles_empty_input);
    RUN_TEST(do_command_line_unknown_returns_error);
    RUN_TEST(do_command_line_help_runs);
    RUN_TEST(do_command_line_max_stock_sets_global);
    RUN_TEST(do_command_line_max_stock_requires_arg);
    RUN_TEST(do_command_line_pruner_batch_is_clamped);
}
