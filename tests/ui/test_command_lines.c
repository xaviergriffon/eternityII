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
#include "ui/command_lines.h"
#include "app/static_variables.h"
#include "core/datamanager.h"
#include "core/possibility.h"

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

/* Vide entièrement le pool local (vérifié + non vérifié) entre les tests :
   l'état datamanager est global et partagé avec les autres suites. */
static void dm_drain(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000);
        free_array_possibility_packet(r);
    }
}

/* Insère n possibilités non vérifiées d'allocs donnés dans le stock local. */
static void dm_add(const int *allocs, int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc(n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        arr.possibilities[i].alloc = (uint16_t)allocs[i];
        arr.possibilities[i].checked = 0;
    }
    add_possibility(NULL, &arr); /* server_ip == NULL -> stock local */
    free(arr.possibilities);
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

/* limit <n> : fixe le débit maximum de recherche par seconde (global). */
TEST do_command_line_limit_sets_global(void)
{
    unsigned long long saved = max_search_by_sec;
    char cmd[] = "limit 4242";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(4242ULL, max_search_by_sec, "%llu");
    max_search_by_sec = saved;
    PASS();
}

/* limit sans argument -> -1 (erreur d'interprète). */
TEST do_command_line_limit_requires_arg(void)
{
    char cmd[] = "limit";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

/* ==========================================================================
 * Interprètes adossés au datamanager : dispatchés via do_command_line() sur un
 * petit stock construit à la main. On vérifie le code retour et la préservation
 * du total quand l'opération ne fait que réorganiser les files.
 * ========================================================================== */

/* sorta / sortd : tri du stock -> 0, total préservé. */
TEST do_command_line_sort_runs(void)
{
    dm_drain();
    int allocs[] = { 5, 2, 8, 1, 6 };
    dm_add(allocs, 5);

    char sorta[] = "sorta";
    ASSERT_EQ_FMT(0, run_command_quiet(sorta), "%d");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");

    char sortd[] = "sortd";
    ASSERT_EQ_FMT(0, run_command_quiet(sortd), "%d");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");

    /* sortd <n> : tri d'une seule file (sort_d_mono). */
    char sortd0[] = "sortd 0";
    ASSERT_EQ_FMT(0, run_command_quiet(sortd0), "%d");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu");

    dm_drain();
    PASS();
}

/* sortdm : tri multi-thread -> 0, total préservé. */
TEST do_command_line_sortdm_runs(void)
{
    dm_drain();
    int allocs[] = { 5, 2, 8, 1, 6, 3 };
    dm_add(allocs, 6);

    char cmd[] = "sortdm";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(6ULL, datas_size(), "%llu");

    dm_drain();
    PASS();
}

/* split puis regroup : redistribution puis consolidation -> 0, total préservé. */
TEST do_command_line_split_regroup_runs(void)
{
    dm_drain();
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    dm_add(allocs, 10);

    char split[] = "split";
    ASSERT_EQ_FMT(0, run_command_quiet(split), "%d");
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");

    char regroup[] = "regroup";
    ASSERT_EQ_FMT(0, run_command_quiet(regroup), "%d");
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu");

    dm_drain();
    PASS();
}

/* print / statistic / min / printfile / printanalysed / restockanalysed :
   commandes d'affichage / consultation -> 0. */
TEST do_command_line_inspect_commands_run(void)
{
    dm_drain();
    int allocs[] = { 7, 3, 9 };
    dm_add(allocs, 3);

    char print[] = "print";
    ASSERT_EQ_FMT(0, run_command_quiet(print), "%d");

    char statistic[] = "statistic";
    ASSERT_EQ_FMT(0, run_command_quiet(statistic), "%d");

    char min[] = "min";
    ASSERT_EQ_FMT(0, run_command_quiet(min), "%d");

    char printfile[] = "printfile 0";
    ASSERT_EQ_FMT(0, run_command_quiet(printfile), "%d");

    char printanalysed[] = "printanalysed";
    ASSERT_EQ_FMT(0, run_command_quiet(printanalysed), "%d");

    char restockanalysed[] = "restockanalysed";
    ASSERT_EQ_FMT(0, run_command_quiet(restockanalysed), "%d");

    dm_drain();
    PASS();
}

/* printfile sans argument -> -1 (erreur d'interprète). */
TEST do_command_line_printfile_requires_arg(void)
{
    char cmd[] = "printfile";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

/* checkfiles / checkfile <n> : intégrité des files -> 0 sur un stock cohérent. */
TEST do_command_line_checkfiles_runs(void)
{
    dm_drain();
    int allocs[] = { 1, 2, 3 };
    dm_add(allocs, 3);

    char checkfiles[] = "checkfiles";
    ASSERT_EQ_FMT(0, run_command_quiet(checkfiles), "%d");

    char checkfile[] = "checkfile 0";
    ASSERT_EQ_FMT(0, run_command_quiet(checkfile), "%d");

    dm_drain();
    PASS();
}

/* checkfile sans argument -> -1 (erreur d'interprète). */
TEST do_command_line_checkfile_requires_arg(void)
{
    char cmd[] = "checkfile";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

/* checkdirections : cohérence du tableau de traversée -> 0. */
TEST do_command_line_checkdirections_runs(void)
{
    char cmd[] = "checkdirections";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    PASS();
}

/* checkdatas : valide le stock contre le CSV (présent à la racine du dépôt).
   Stock vide -> 0. */
TEST do_command_line_checkdatas_runs(void)
{
    dm_drain();
    char cmd[] = "checkdatas";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    dm_drain();
    PASS();
}

/* checkduplicate : détection de doublons -> 0 sur un stock vide.
 * Sûr depuis le correctif du blocage de check_duplicate sur petit stock
 * (la jointure ne porte que sur les threads réellement lancés). */
TEST do_command_line_checkduplicate_runs(void)
{
    dm_drain();
    char cmd[] = "checkduplicate";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    dm_drain();
    PASS();
}

/* check : réaffiche le dernier rapport `lastcheck` en place -> 0. lastcheck peut
   être NULL (jamais calculé en test), le ternaire de l'interprète le gère. */
TEST do_command_line_check_runs(void)
{
    char cmd[] = "check";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    PASS();
}

/* loadjson : importe la possibilité depuis la chaîne JSON codée en dur du module
   (via import_json) -> 0, et exactement 1 possibilité au stock local. */
TEST do_command_line_loadjson_runs(void)
{
    dm_drain();
    char cmd[] = "loadjson";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    dm_drain();
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
    RUN_TEST(do_command_line_limit_sets_global);
    RUN_TEST(do_command_line_limit_requires_arg);
    RUN_TEST(do_command_line_sort_runs);
    RUN_TEST(do_command_line_sortdm_runs);
    RUN_TEST(do_command_line_split_regroup_runs);
    RUN_TEST(do_command_line_inspect_commands_run);
    RUN_TEST(do_command_line_printfile_requires_arg);
    RUN_TEST(do_command_line_checkfiles_runs);
    RUN_TEST(do_command_line_checkfile_requires_arg);
    RUN_TEST(do_command_line_checkdirections_runs);
    RUN_TEST(do_command_line_checkdatas_runs);
    RUN_TEST(do_command_line_checkduplicate_runs);
    RUN_TEST(do_command_line_check_runs);
    RUN_TEST(do_command_line_loadjson_runs);
}
