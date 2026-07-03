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
#include "fork_assert.h"
#include "ui/command_lines.h"
#include "app/static_variables.h"
#include "core/datamanager.h"
#include "core/possibility.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

/* exit_interpreter n'est pas exposé dans command_lines.h (appelé uniquement via
 * la table de dispatch de do_command_line). */
int exit_interpreter(void);

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

    char high[] = "prunerBatch 999999999"; /* > PRUNER_BATCH_MAX -> plafonné */
    ASSERT_EQ_FMT(0, run_command_quiet(high), "%d");
    ASSERT_EQ_FMT(PRUNER_BATCH_MAX, pruner_batch_size, "%d");

    pruner_batch_size = saved;
    PASS();
}

/* prunerBatch sans argument -> -1 (erreur d'interprète), stock inchangé. */
TEST do_command_line_pruner_batch_requires_arg(void)
{
    int saved = pruner_batch_size;
    char cmd[] = "prunerBatch";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(saved, pruner_batch_size, "%d");
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

/* Commande inconnue : les deux issues du diagnostic de typo. « bakup » est à
   distance 1 de « backup » (<= seuil) -> branche AVEC suggestion (closest_command
   != NULL) ; « zzzzzzzzz » n'est proche d'aucune commande -> branche SANS
   suggestion. Les deux renvoient -1. */
TEST do_command_line_unknown_command_typo_suggestion(void)
{
    char near[] = "bakup";      /* proche de "backup" -> suggestion émise */
    ASSERT_EQ_FMT(-1, run_command_quiet(near), "%d");

    char far[] = "zzzzzzzzz";   /* trop loin -> aucune suggestion */
    ASSERT_EQ_FMT(-1, run_command_quiet(far), "%d");
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

/* rmnonext : relit le CSV des pièces (parts_files), reconstruit la map et purge
   les possibilités sans continuation valide -> 0. parts_files pointe par défaut
   sur le CSV adapté au build (pieces.csv en 256, pieces16.csv en 16), résolu
   relativement à la racine du dépôt (CWD de `make test`). Si le fichier est
   introuvable (binaire lancé hors racine), on saute proprement plutôt que
   d'échouer sur l'environnement. Stock vide : la purge ne fait que parcourir des
   files vides, mais tout le corps de l'interprète est exercé. */
TEST do_command_line_rmnonext_runs(void)
{
    if (access(parts_files, R_OK) != 0) {
        SKIPm("CSV des pièces introuvable (lancer depuis la racine du dépôt)");
    }
    dm_drain();
    char cmd[] = "rmnonext";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    dm_drain();
    PASS();
}

/* backup / restore / import : ces interprètes écrivent/lisent DEF_FILE relatif
   ("./eternityII.back") -> on travaille dans un répertoire temporaire dédié, et
   server = 1 rend les noms déterministes (pas de suffixe _<pid> côté client).
   Round-trip : backup d'un stock, restore après vidage (total restauré), puis
   import par-dessus (total doublé). Le nettoyage (chdir retour, unlink, rmdir,
   restauration des globales) précède toute assertion : un échec ne doit jamais
   laisser le CWD dans le répertoire temporaire et casser les suites suivantes. */
TEST do_command_line_backup_restore_import_round_trip(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_bk_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    int saved_server = server;
    server = 1;

    dm_drain();
    int allocs[] = { 1, 2, 3, 4 };
    dm_add(allocs, 4);

    char backup[] = "backup";
    int r_backup = run_command_quiet(backup);
    int back_exists = access("./eternityII.back", F_OK) == 0;
    int an_exists   = access("./eternityII-in_analyse.back", F_OK) == 0;

    dm_drain();
    unsigned long long after_drain = datas_size();
    char restore[] = "restore";
    int r_restore = run_command_quiet(restore);
    unsigned long long after_restore = datas_size();

    char import[] = "import";
    int r_import = run_command_quiet(import);
    unsigned long long after_import = datas_size();

    /* --- nettoyage avant toute assertion --- */
    dm_drain();
    unlink("./eternityII.back");
    unlink("./eternityII-in_analyse.back");
    if (chdir(saved_cwd) != 0) { /* best-effort : rien de mieux à faire ici */ }
    rmdir(dir);
    server = saved_server;

    ASSERT_EQ_FMT(0, r_backup, "%d");
    ASSERT(back_exists);
    ASSERT(an_exists);
    ASSERT_EQ_FMT(0ULL, after_drain, "%llu");
    ASSERT_EQ_FMT(0, r_restore, "%d");
    ASSERT_EQ_FMT(4ULL, after_restore, "%llu");  /* les 4 possibilités sont revenues */
    ASSERT_EQ_FMT(0, r_import, "%d");
    ASSERT_EQ_FMT(8ULL, after_import, "%llu");   /* import ajoute par-dessus -> 4 + 4 */
    PASS();
}

/* ---------- exit_interpreter --------------------------------------------- */
/*
 * exit_interpreter appelle exit() dans deux cas (mode serveur ; mode client
 * "parent" du process courant) : exécutés dans un fork (fork_assert.h) pour ne
 * pas tuer le runner. Le globals utilisées (server/parent_pid/childrens_pid)
 * ne sont positionnées QU'À L'INTÉRIEUR de la fonction forkée : fork() copie la
 * mémoire du parent, donc rien à restaurer côté parent après coup.
 */

/* Mode serveur : exit(EXIT_SUCCESS) immédiat, sans toucher aux enfants. */
static void fork_exit_server_mode(void)
{
    server = 1;
    exit_interpreter();
    _exit(99); /* non atteint */
}

TEST exit_interpreter_server_mode_exits_immediately(void)
{
    int code = run_in_fork(fork_exit_server_mode, NULL);
    ASSERT_EQ_FMT(0, code, "%d");
    PASS();
}

/* Mode client, process "parent" du point de vue courant, aucun enfant suivi
 * (childrens_pid == NULL) : le tour d'attente est immédiat, puis exit(EXIT_SUCCESS). */
static void fork_exit_client_parent_no_children(void)
{
    server = 0;
    parent_pid = getpid();
    childrens_pid = NULL;
    exit_interpreter();
    _exit(99); /* non atteint */
}

TEST exit_interpreter_client_parent_no_children_exits(void)
{
    int code = run_in_fork(fork_exit_client_parent_no_children, NULL);
    ASSERT_EQ_FMT(0, code, "%d");
    PASS();
}

/* Mode client, mais PAS le process "parent" (cas d'un thread de recherche
 * forké) : ne fait rien de plus que positionner REQUEST_STOP et renvoyer 0,
 * sans jamais appeler exit() -> testable directement, sans fork. */
TEST exit_interpreter_client_non_parent_returns_zero(void)
{
    int saved_server = server;
    pid_t saved_parent = parent_pid;
    int saved_req = request;

    server = 0;
    parent_pid = getpid() + 999999; /* garanti différent du pid courant */

    int r = exit_interpreter();

    ASSERT_EQ_FMT(0, r, "%d");
    ASSERT_EQ_FMT(REQUEST_STOP, request, "%d");

    server = saved_server;
    parent_pid = saved_parent;
    request = saved_req;
    PASS();
}

/* ---------- backup_interpreter (mode client) ----------------------------- */
/*
 * En mode client (server == 0), backup_interpreter suffixe les deux noms de
 * fichier avec le pid courant (pas de collision entre plusieurs process client
 * sur la même machine). Round-trip similaire à la version serveur ci-dessus,
 * dans un répertoire temporaire dédié.
 */
TEST do_command_line_backup_client_mode_appends_pid(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_bkc_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    int saved_server = server;
    server = 0;

    dm_drain();
    int allocs[] = { 1, 2 };
    dm_add(allocs, 2);

    char backup[] = "backup";
    int r_backup = run_command_quiet(backup);

    char expected_file[64], expected_analyse[64];
    snprintf(expected_file, sizeof expected_file, "./eternityII.back_%i", (int)getpid());
    snprintf(expected_analyse, sizeof expected_analyse, "./eternityII-in_analyse.back_%i", (int)getpid());
    int file_exists = access(expected_file, F_OK) == 0;
    int analyse_exists = access(expected_analyse, F_OK) == 0;

    dm_drain();
    unlink(expected_file);
    unlink(expected_analyse);
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);
    server = saved_server;

    ASSERT_EQ_FMT(0, r_backup, "%d");
    ASSERT(file_exists);
    ASSERT(analyse_exists);
    PASS();
}

/* ---------- restore_interpreter avec arguments --------------------------- */
/*
 * restore [fichier [fichier_analyse]] : sans argument, restore_interpreter
 * utilise les noms par défaut (déjà couvert par le round-trip serveur
 * ci-dessus) ; avec 1 ou 2 arguments explicites, il restaure depuis des
 * fichiers arbitraires — jamais exercé jusqu'ici.
 */

/* 1 argument : fichier de stock explicite, fichier d'analyse par défaut
 * (absent -> restore_analysed échoue, mais le résultat global reste -1 sans
 * perdre le stock déjà restauré depuis le fichier explicite). */
TEST do_command_line_restore_with_one_argument(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_r1_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }

    dm_drain();
    int allocs[] = { 5, 6, 7 };
    dm_add(allocs, 3);
    ASSERT_EQ_FMT(0, backup("./custom.back"), "%d");

    dm_drain();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    char cmd[] = "restore ./custom.back";
    int r = run_command_quiet(cmd);
    unsigned long long after = datas_size();

    dm_drain();
    unlink("./custom.back");
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);

    ASSERT_EQ_FMT(3ULL, after, "%llu"); /* le stock explicite est bien revenu */
    (void)r; /* le code de retour dépend aussi de restore_analysed (fichier par défaut absent) */
    PASS();
}

/* 2 arguments : fichier de stock ET fichier d'analyse explicites -> les deux
 * restaurations réussissent, résultat 0. */
TEST do_command_line_restore_with_two_arguments(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_r2_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }

    dm_drain();
    int allocs[] = { 8, 9 };
    dm_add(allocs, 2);
    ASSERT_EQ_FMT(0, backup("./custom.back"), "%d");
    ASSERT_EQ_FMT(0, backup_analysed("./custom_analysed.back"), "%d");

    dm_drain();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    char cmd[] = "restore ./custom.back ./custom_analysed.back";
    int r = run_command_quiet(cmd);
    unsigned long long after = datas_size();

    dm_drain();
    unlink("./custom.back");
    unlink("./custom_analysed.back");
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);

    ASSERT_EQ_FMT(0, r, "%d");
    ASSERT_EQ_FMT(2ULL, after, "%llu");
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
    RUN_TEST(do_command_line_pruner_batch_requires_arg);
    RUN_TEST(do_command_line_limit_sets_global);
    RUN_TEST(do_command_line_limit_requires_arg);
    RUN_TEST(do_command_line_unknown_command_typo_suggestion);
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
    RUN_TEST(do_command_line_rmnonext_runs);
    RUN_TEST(do_command_line_backup_restore_import_round_trip);

    RUN_TEST(exit_interpreter_server_mode_exits_immediately);
    RUN_TEST(exit_interpreter_client_parent_no_children_exits);
    RUN_TEST(exit_interpreter_client_non_parent_returns_zero);

    RUN_TEST(do_command_line_backup_client_mode_appends_pid);
    RUN_TEST(do_command_line_restore_with_one_argument);
    RUN_TEST(do_command_line_restore_with_two_arguments);
}
