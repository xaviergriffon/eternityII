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
#include "app/control_registry.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/best_board.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* exit_interpreter n'est pas exposé dans command_lines.h (appelé uniquement via
 * la table de dispatch de do_command_line). */
int exit_interpreter(void);

/* lock_all_file / unlock_all_file ne sont pas dans datamanager.h mais sont
 * accessibles via liaison directe (datamanager.c est toujours dans TEST_MODULES).
 * Utilisées pour passer maintenance=1 sans deadlock (backup() vérifie le flag
 * avant d'essayer de prendre les mutex). */
extern void lock_all_file(void);
extern void unlock_all_file(void);

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

/* help <commande> / help <catégorie> : sujets connus -> 0 ; inconnu -> -1
   (avec ou sans suggestion de typo). */
TEST do_command_line_help_with_topic(void)
{
    char cmd_command[] = "help limit";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd_command), "%d");

    char cmd_alias[] = "help quit"; /* alias résolu vers exit */
    ASSERT_EQ_FMT(0, run_command_quiet(cmd_alias), "%d");

    char cmd_category[] = "help stock";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd_category), "%d");

    char cmd_near[] = "help limti"; /* proche de "limit" -> suggestion émise */
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd_near), "%d");

    char cmd_far[] = "help zzzzzzzzz"; /* trop loin -> aucune suggestion */
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd_far), "%d");
    PASS();
}

/* clear (alias cls) : efface l'écran via clear_console — no-op ici (sortie
   redirigée -> non-tty -> early-return) — et retourne 0. L'alias résout vers
   l'entrée canonique et l'aide documente le raccourci Ctrl-L. */
TEST do_command_line_clear_runs(void)
{
    char cmd[] = "clear";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");

    char alias[] = "cls";
    ASSERT_EQ_FMT(0, run_command_quiet(alias), "%d");
    ASSERT_STR_EQ("clear", command_canonical_name("cls"));

    char out[16384];
    ASSERT_EQ_FMT(0, help_format_topic("clear", out, sizeof out), "%d");
    ASSERT(strstr(out, "Ctrl-L") != NULL);
    ASSERT(strstr(out, "scrollback") != NULL);
    PASS();
}

/* help_format_general (pure) : contient les titres de catégories, l'usage des
   commandes à arguments et les alias des entrées canoniques. */
TEST help_format_general_lists_categories_and_aliases(void)
{
    char out[16384];
    ASSERT_EQ_FMT(0, help_format_general(out, sizeof out), "%d");
    ASSERT(strstr(out, "Recherche & régulation") != NULL);
    ASSERT(strstr(out, "Pilotage des clients (serveur)") != NULL);
    ASSERT(strstr(out, "limit <n>") != NULL);            /* usage affiché, pas le nom nu */
    ASSERT(strstr(out, "(alias : quit)") != NULL);       /* alias rattaché à exit */
    ASSERT(strstr(out, "[serveur]") != NULL);            /* marqueur des commandes serveur */
    PASS();
}

/* help_format_topic (pure) : détail d'une commande (usage, portée, propagation,
   complément), section d'une catégorie, -1 sur sujet inconnu. */
TEST help_format_topic_command_and_category(void)
{
    char out[16384];

    ASSERT_EQ_FMT(0, help_format_topic("expand", out, sizeof out), "%d");
    ASSERT(strstr(out, "expand <niveau>") != NULL);
    ASSERT(strstr(out, "propagation") != NULL);
    ASSERT(strstr(out, "EXPAND_MAX_LEVELS") != NULL);    /* le complément est affiché */

    ASSERT_EQ_FMT(0, help_format_topic("clients", out, sizeof out), "%d");
    ASSERT(strstr(out, "serveur uniquement") != NULL);

    /* mot-clé de catégorie : n'affiche que sa section */
    ASSERT_EQ_FMT(0, help_format_topic("sauvegarde", out, sizeof out), "%d");
    ASSERT(strstr(out, "backup") != NULL);
    ASSERT(strstr(out, "Recherche & régulation") == NULL);

    ASSERT_EQ_FMT(-1, help_format_topic("nonexistent", out, sizeof out), "%d");
    ASSERT_EQ_FMT(-1, help_format_topic(NULL, out, sizeof out), "%d");
    PASS();
}

/*
 * config/configSave sont masquées côté SERVEUR (ni listées dans l'aide, ni
 * exécutables) : voir command_is_client_only, command_lines.c. Exécutées
 * côté client/pruner (server=0), elles fonctionnent normalement -- elles
 * agiraient sinon sur les globales du SERVEUR (NB_THREADS y désigne le pool
 * de connexions, pas un nombre de forks), ce qui serait trompeur plutôt
 * qu'un no-op inoffensif comme les commandes `*(serveur)*` à l'inverse.
 */
TEST help_hides_config_commands_on_server_only(void)
{
    int saved_server = server;
    char out[16384];

    server = 0;
    ASSERT_EQ_FMT(0, help_format_topic("config", out, sizeof out), "%d");
    ASSERT_EQ_FMT(0, help_format_general(out, sizeof out), "%d");
    ASSERT(strstr(out, "config") != NULL);

    server = 1;
    ASSERT_EQ_FMT(-1, help_format_topic("config", out, sizeof out), "%d");
    ASSERT_EQ_FMT(-1, help_format_topic("configSave", out, sizeof out), "%d");
    ASSERT_EQ_FMT(0, help_format_general(out, sizeof out), "%d");
    ASSERT(strstr(out, "config") == NULL);
    ASSERT(strstr(out, "configSave") == NULL);

    server = saved_server;
    PASS();
}

TEST do_command_line_config_is_masked_on_server_only(void)
{
    int saved_server = server;

    server = 0;
    char cmd_client[] = "config";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd_client), "%d");

    server = 1;
    char cmd_server[] = "config";
    /* Masquée : traitée exactement comme une commande inconnue. */
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd_server), "%d");

    server = saved_server;
    PASS();
}

/* configSave écrit réellement un fichier : client_config_file_path est
   redirigé vers un chemin temporaire pour ne rien laisser dans le dépôt, et
   pour distinguer "masquée, rien écrit" de "exécutée, fichier ré-écrit". */
TEST do_command_line_config_save_is_masked_on_server_only(void)
{
    int saved_server = server;
    const char *saved_path = client_config_file_path;

    char path[] = "/tmp/etii_cmd_configsave_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    client_config_file_path = path;

    server = 0;
    char cmd_client[] = "configSave";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd_client), "%d");
    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    fclose(f);

    unlink(path);
    server = 1;
    char cmd_server[] = "configSave";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd_server), "%d");
    FILE *f2 = fopen(path, "r");
    ASSERT(f2 == NULL); /* masquée : rien n'a été écrit */

    client_config_file_path = saved_path;
    server = saved_server;
    PASS();
}

/* command_canonical_name (pure) : les noms canoniques sont les formes camelCase
   complètes, les noms historiques abrégés sont des alias ; casse ignorée, NULL sûr. */
TEST command_canonical_name_resolves_aliases_and_case(void)
{
    ASSERT_STR_EQ("exit", command_canonical_name("quit"));
    ASSERT_STR_EQ("help", command_canonical_name("?"));
    ASSERT_STR_EQ("statistic", command_canonical_name("stats"));
    ASSERT_STR_EQ("sortAsc", command_canonical_name("sorta"));
    ASSERT_STR_EQ("sortDesc", command_canonical_name("sortd"));
    ASSERT_STR_EQ("sortDescMulti", command_canonical_name("sortdm"));
    ASSERT_STR_EQ("removeNoNext", command_canonical_name("rmnonext"));
    ASSERT_STR_EQ("removeNoNext", command_canonical_name("prune"));
    ASSERT_STR_EQ("clientsCommand", command_canonical_name("clientsCmd"));
    /* Les anciens noms tout-minuscule restent couverts par la casse ignorée. */
    ASSERT_STR_EQ("printFile", command_canonical_name("printfile"));
    ASSERT_STR_EQ("checkDatas", command_canonical_name("checkdatas"));
    ASSERT_STR_EQ("loadJson", command_canonical_name("loadjson"));
    ASSERT_STR_EQ("maxStockByThread", command_canonical_name("MAXSTOCKBYTHREAD"));
    ASSERT_STR_EQ("limit", command_canonical_name("limit"));
    ASSERT_EQ(NULL, command_canonical_name("nonexistent"));
    ASSERT_EQ(NULL, command_canonical_name(NULL));
    PASS();
}

/* do_command_line : la casse est ignorée et un alias exécute bien l'entrée
   canonique (LIMIT/sortAsc fixent et trient comme limit/sorta). */
TEST do_command_line_case_insensitive_and_alias_dispatch(void)
{
    unsigned long long saved = max_search_by_sec;
    char upper[] = "LIMIT 1234";
    ASSERT_EQ_FMT(0, run_command_quiet(upper), "%d");
    ASSERT_EQ_FMT(1234ULL, max_search_by_sec, "%llu");
    max_search_by_sec = saved;

    char alias[] = "sorta"; /* alias historique de sortAsc, stock vide : tri sans effet */
    ASSERT_EQ_FMT(0, run_command_quiet(alias), "%d");
    PASS();
}

/* expand sans argument : CMD_ERR_USAGE -> rappel d'usage automatique, -1 rendu. */
TEST do_command_line_expand_requires_arg(void)
{
    char cmd[] = "expand";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
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

/* leaseDuration <n> : fixe analysed_lease_seconds (PR7). Pas de bornage
 * (contrairement à prunerBatch) : <= 0 est une valeur légitime (désactive le
 * bail, cf. commentaire de static_variables.h), donc acceptée telle quelle. */
TEST do_command_line_lease_duration_sets_global(void)
{
    int saved = analysed_lease_seconds;

    char ok[] = "leaseDuration 42";
    ASSERT_EQ_FMT(0, run_command_quiet(ok), "%d");
    ASSERT_EQ_FMT(42, analysed_lease_seconds, "%d");

    char disable[] = "leaseDuration 0";
    ASSERT_EQ_FMT(0, run_command_quiet(disable), "%d");
    ASSERT_EQ_FMT(0, analysed_lease_seconds, "%d");

    analysed_lease_seconds = saved;
    PASS();
}

/* leaseDuration sans argument -> -1 (erreur d'interprète), global inchangé. */
TEST do_command_line_lease_duration_requires_arg(void)
{
    int saved = analysed_lease_seconds;
    char cmd[] = "leaseDuration";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(saved, analysed_lease_seconds, "%d");
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

/* ---------- Export console vers fichier (P5) ------------------------------
 * `print [fichier]` / `printFile <n> [fichier]` / `printAnalysed [fichier]`
 * écrivent le dump JSON dans un fichier au lieu de la console quand
 * l'argument optionnel est fourni. */

/* print <fichier> : exporte tout le data manager, contenu JSON présent. */
TEST do_command_line_print_exports_to_file(void)
{
    dm_drain();
    int allocs[] = { 21, 34 };
    dm_add(allocs, 2);

    char path[] = "/tmp/etii_cmd_print_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "print %s", path);
    int r = run_command_quiet(cmd);

    FILE *f = fopen(path, "r");
    ASSERT(f != NULL);
    char buf[8192] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    (void)n;

    dm_drain();
    ASSERT_EQ_FMT(0, r, "%d");
    ASSERT(strstr(buf, "\"alloc\": 21") != NULL);
    ASSERT(strstr(buf, "\"alloc\": 34") != NULL);
    PASS();
}

/* printFile <n> sans [fichier] : comportement console inchangé (0, pas de
   fichier créé). Couvre la non-régression du chemin historique. */
TEST do_command_line_printfile_without_path_still_prints_to_console(void)
{
    dm_drain();
    char cmd[] = "printfile 0";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    dm_drain();
    PASS();
}

/* printFile <n> <fichier> : exporte la file <n> au format JSON. */
TEST do_command_line_printfile_exports_to_file(void)
{
    dm_drain();
    int allocs[] = { 12, 13, 14 };
    dm_add(allocs, 3);

    char path[] = "/tmp/etii_cmd_printfile_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    /* Peu importe la file effectivement peuplée par add_possibility (routage
       interne non déterministe depuis les tests) : on exporte les 10 files
       et on vérifie qu'AU MOINS UN export contient le contenu attendu. */
    int found = 0;
    for (int fp = 0; fp < 10 && !found; fp++) {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "printfile %d %s", fp, path);
        int r = run_command_quiet(cmd);
        ASSERT_EQ_FMT(0, r, "%d");

        FILE *f = fopen(path, "r");
        ASSERT(f != NULL);
        char buf[8192] = {0};
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        (void)n;
        if (strstr(buf, "\"alloc\": 12") != NULL) {
            found = 1;
        }
    }
    unlink(path);
    dm_drain();
    ASSERT(found);
    PASS();
}

/* printAnalysed <fichier> en mode SERVEUR (server=1) : pas de forks de
   recherche -> le chemin est utilisé TEL QUEL, sans suffixe de pid. */
TEST do_command_line_printanalysed_exports_without_pid_suffix_on_server(void)
{
    dm_drain();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 91;
    add_possibility_analysed(&pk, 0);

    int saved_server = server;
    server = 1;

    char path[] = "/tmp/etii_cmd_pa_srv_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "printanalysed %s", path);
    int r = run_command_quiet(cmd);
    server = saved_server;

    FILE *f = fopen(path, "r");
    ASSERT(f != NULL); /* le chemin exact existe : pas de suffixe côté serveur */
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    unlink(path);
    (void)n;

    dm_drain();
    ASSERT_EQ_FMT(0, r, "%d");
    ASSERT(strstr(buf, "\"alloc\": 91") != NULL);
    PASS();
}

/*
 * printAnalysed <fichier> en mode CLIENT (server=0) : cette commande a
 * send_to_childs=1 (voir command_lines.c) — le TEXTE de la commande, argument
 * fichier compris, est rejoué tel quel par chaque process (parent + forks de
 * recherche, cf. send_command_to_childs). Sans précaution, tous écriraient
 * dans LE MÊME fichier en concurrence. printanalysed_interpreter suffixe donc
 * le chemin par le pid courant (même convention que backup_interpreter) :
 * on vérifie que le fichier suffixé existe ET que le chemin NU (sans
 * suffixe) n'a PAS été créé.
 */
TEST do_command_line_printanalysed_exports_with_pid_suffix_on_client(void)
{
    dm_drain();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 92;
    add_possibility_analysed(&pk, 0);

    int saved_server = server;
    server = 0;

    char path[] = "/tmp/etii_cmd_pa_cli_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    unlink(path); /* on ne veut que le nom, pas le fichier vide créé par mkstemp */

    char cmd[160];
    snprintf(cmd, sizeof cmd, "printanalysed %s", path);
    int r = run_command_quiet(cmd);
    server = saved_server;

    char expected_suffixed[192];
    snprintf(expected_suffixed, sizeof expected_suffixed, "%s_%i", path, (int)getpid());

    int bare_exists = access(path, F_OK) == 0;
    int suffixed_exists = access(expected_suffixed, F_OK) == 0;

    FILE *f = suffixed_exists ? fopen(expected_suffixed, "r") : NULL;
    char buf[4096] = {0};
    if (f != NULL) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        (void)n;
    }
    unlink(path);
    unlink(expected_suffixed);

    dm_drain();
    ASSERT_EQ_FMT(0, r, "%d");
    ASSERT_FALSEm("le chemin nu ne doit PAS être créé en mode client", bare_exists);
    ASSERT(suffixed_exists);
    ASSERT(strstr(buf, "\"alloc\": 92") != NULL);
    PASS();
}

/* Les tests « répertoire non inscriptible » ci-dessous reposent sur le fait
 * qu'un chmod(dir, 0444) interdise réellement d'y créer un fichier. root
 * outrepasse les bits de permission (CAP_DAC_OVERRIDE sous Linux) : fopen() en
 * écriture y réussit malgré le 0444, l'échec attendu ne se produit jamais et
 * l'assertion tombe. C'est exactement le cas sous `make test-docker`, dont le
 * conteneur tourne en root ; la CI GitHub, elle, tourne sous l'utilisateur
 * `runner` et exécute donc bien ces tests. On les saute explicitement quand le
 * postulat de la permission ne tient pas — même logique que le SKIPm sur un
 * chmod() non supporté. */
#define SKIP_IF_ROOT()                                                        \
    do {                                                                      \
        if (geteuid() == 0)                                                   \
            SKIPm("root outrepasse les permissions : chmod 0444 sans effet"); \
    } while (0)

/* print vers un répertoire non inscriptible : fopen échoue -> -1, sans crash. */
TEST do_command_line_print_fails_on_unwritable_dir(void)
{
    SKIP_IF_ROOT();

    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_prw_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    if (chmod(dir, 0444) != 0) {
        chdir(saved_cwd); rmdir(dir);
        SKIPm("chmod non supporté sur cet environnement");
    }

    char cmd[] = "print ./out.json";
    int r = run_command_quiet(cmd);

    chmod(dir, 0755);
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);

    ASSERT_EQ_FMT(-1, r, "%d");
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

    // Régression : backup_interpreter oubliait d'écrire ce fichier (best_board_save
    // n'était branché que sur l'autobackup périodique et l'arrêt sur solution,
    // jamais sur la commande console `backup`) — restore échouait ensuite avec
    // "aucun plateau record connu" faute de fichier. On force un enregistrement
    // pour que le round-trip ait quelque chose à sauvegarder/restaurer.
    best_board_init(&g_server_best_board);
    struct possibility_packet board;
    memset(&board, 0, sizeof(board));
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            board.grid[x][y] = -2;
        }
    }
    best_board_try_record(&g_server_best_board, &board, 4);

    char backup[] = "backup";
    int r_backup = run_command_quiet(backup);
    int back_exists = access("./eternityII.back", F_OK) == 0;
    int an_exists   = access("./eternityII-in_analyse.back", F_OK) == 0;
    int bb_exists   = access("./eternityII-best_board.back", F_OK) == 0;

    dm_drain();
    unsigned long long after_drain = datas_size();
    best_board_init(&g_server_best_board); /* efface l'enregistrement en mémoire */
    char restore[] = "restore";
    int r_restore = run_command_quiet(restore);
    unsigned long long after_restore = datas_size();
    uint16_t restored_alloc = best_board_result(&g_server_best_board);

    char import[] = "import";
    int r_import = run_command_quiet(import);
    unsigned long long after_import = datas_size();

    /* --- nettoyage avant toute assertion --- */
    dm_drain();
    best_board_init(&g_server_best_board);
    unlink("./eternityII.back");
    unlink("./eternityII-in_analyse.back");
    unlink("./eternityII-best_board.back");
    if (chdir(saved_cwd) != 0) { /* best-effort : rien de mieux à faire ici */ }
    rmdir(dir);
    server = saved_server;

    ASSERT_EQ_FMT(0, r_backup, "%d");
    ASSERT(back_exists);
    ASSERT(an_exists);
    ASSERT(bb_exists);
    ASSERT_EQ_FMT(0ULL, after_drain, "%llu");
    ASSERT_EQ_FMT(0, r_restore, "%d");
    ASSERT_EQ_FMT(4ULL, after_restore, "%llu");  /* les 4 possibilités sont revenues */
    ASSERT_EQ_FMT(4, (int)restored_alloc, "%d"); /* le plateau record est revenu aussi */
    ASSERT_EQ_FMT(0, r_import, "%d");
    ASSERT_EQ_FMT(8ULL, after_import, "%llu");   /* import ajoute par-dessus -> 4 + 4 */
    PASS();
}

/* Régression : après un restore, max_result (score affiché en console/HTTP)
 * doit refléter le meilleur plateau connu (g_server_best_board), pas
 * seulement la profondeur maximale du stock de possibilités réimporté. Ici le
 * stock ne contient que des allocs <= 2, mais le plateau record sauvegardé a
 * alloc=9 -> max_result doit remonter à 9 après restore, pas rester à 2. */
TEST do_command_line_restore_syncs_max_result_from_best_board(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_bkr_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    int saved_server = server;
    server = 1;
    uint16_t saved_max_result = max_result;

    // file_possibility_analysed est un pool global partagé avec les autres tests
    // de la suite (dm_drain ne vide que le stock principal) : on le rapatrie dans
    // le stock puis on vide le tout, pour ne pas hériter d'un alloc résiduel
    // (ex. do_command_line_printanalysed_exports_with_pid_suffix_on_client, alloc=92)
    // qui fausserait le résultat attendu du restore ci-dessous.
    char restock[] = "restockAnalysed";
    run_command_quiet(restock);
    dm_drain();
    int allocs[] = { 1, 2 };
    dm_add(allocs, 2);

    best_board_init(&g_server_best_board);
    struct possibility_packet board;
    memset(&board, 0, sizeof(board));
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            board.grid[x][y] = -2;
        }
    }
    best_board_try_record(&g_server_best_board, &board, 9);

    char backup[] = "backup";
    int r_backup = run_command_quiet(backup);

    dm_drain();
    max_result = 0; /* simule un redémarrage : compteur en mémoire remis à zéro */
    best_board_init(&g_server_best_board);
    char restore[] = "restore";
    int r_restore = run_command_quiet(restore);
    uint16_t result_after_restore = max_result;

    dm_drain();
    best_board_init(&g_server_best_board);
    unlink("./eternityII.back");
    unlink("./eternityII-in_analyse.back");
    unlink("./eternityII-best_board.back");
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);
    server = saved_server;
    max_result = saved_max_result;

    ASSERT_EQ_FMT(0, r_backup, "%d");
    ASSERT_EQ_FMT(0, r_restore, "%d");
    ASSERT_EQ_FMT(9, (int)result_after_restore, "%d");
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

    char expected_file[64], expected_analyse[64], expected_best_board[64];
    snprintf(expected_file, sizeof expected_file, "./eternityII.back_%i", (int)getpid());
    snprintf(expected_analyse, sizeof expected_analyse, "./eternityII-in_analyse.back_%i", (int)getpid());
    snprintf(expected_best_board, sizeof expected_best_board, "./eternityII-best_board.back_%i", (int)getpid());
    int file_exists = access(expected_file, F_OK) == 0;
    int analyse_exists = access(expected_analyse, F_OK) == 0;
    int best_board_exists = access(expected_best_board, F_OK) == 0;

    dm_drain();
    unlink(expected_file);
    unlink(expected_analyse);
    unlink(expected_best_board);
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);
    server = saved_server;

    ASSERT_EQ_FMT(0, r_backup, "%d");
    ASSERT(file_exists);
    ASSERT(analyse_exists);
    ASSERT(best_board_exists);
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

/* check : ternaire « lastcheck != NULL » (L166) et « report_copy != NULL » (L170)
 * jamais vrais dans les tests précédents — on publie un rapport non-NULL. */
TEST do_command_line_check_with_non_null_lastcheck(void)
{
    pthread_mutex_lock(&lastcheck_mutex);
    char *saved = lastcheck;
    lastcheck = strdup("rapport test");
    pthread_mutex_unlock(&lastcheck_mutex);

    char cmd[] = "check";
    int r = run_command_quiet(cmd);

    pthread_mutex_lock(&lastcheck_mutex);
    free(lastcheck);
    lastcheck = saved;
    pthread_mutex_unlock(&lastcheck_mutex);

    ASSERT_EQ_FMT(0, r, "%d");
    PASS();
}

/* backup avec maintenance active : backup() retourne BACKUP_SKIPPED_MAINTENANCE
 * (L190 branche vraie, L196 branche vraie). lock_all_file() pose maintenance=1
 * sans deadlock car backup() vérifie le flag avant toute prise de mutex. */
TEST do_command_line_backup_skipped_when_maintenance(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_bkm_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    int saved_server = server;
    server = 1;

    lock_all_file();
    char cmd[] = "backup";
    int r = run_command_quiet(cmd);
    unlock_all_file();

    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);
    server = saved_server;

    ASSERT_EQ_FMT(0, r, "%d"); /* backup_interpreter retourne toujours 0 */
    PASS();
}

/* backup vers un répertoire non inscriptible : fopen échoue → BACKUP_ERROR
 * (L192 branche vraie, L198 branche vraie). */
TEST do_command_line_backup_fails_on_unwritable_dir(void)
{
    /* Sous root, backup() réussirait : la branche BACKUP_ERROR visée ne serait
     * pas prise (et les .back écrits feraient échouer le rmdir final). Le
     * résultat asserté vaut 0 dans les deux cas, donc le test ne rougirait pas
     * — raison de plus pour le sauter explicitement plutôt que de le laisser
     * passer à vide. */
    SKIP_IF_ROOT();

    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_bkw_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    if (chmod(dir, 0444) != 0) {
        chdir(saved_cwd); rmdir(dir);
        SKIPm("chmod non supporté sur cet environnement");
    }
    int saved_server = server;
    server = 1;

    char cmd[] = "backup";
    int r = run_command_quiet(cmd);

    chmod(dir, 0755);
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);
    server = saved_server;

    ASSERT_EQ_FMT(0, r, "%d"); /* backup_interpreter retourne toujours 0 */
    PASS();
}

/* restore avec request != REQUEST_CONTINUE : la branche de pause (L285 fausse)
 * et la remise en route (L299 fausse) ne sont jamais prises. En passant
 * request=REQUEST_STOP avant l'appel, on couvre les deux branches fausses.
 * On utilise un fichier inexistant → restore() renvoie -1 → L291 branche vraie. */
TEST do_command_line_restore_when_not_running(void)
{
    int saved_req = request;
    request = REQUEST_STOP; /* != REQUEST_CONTINUE */

    char cmd[] = "restore /tmp/etii_nonexistent_restore_file.back";
    int r = run_command_quiet(cmd);

    request = saved_req;

    /* restore() échoue sur fichier absent -> résultat non nul */
    ASSERT_EQ_FMT(-1, r, "%d");
    PASS();
}

/* restore avec fichier inexistant et request=REQUEST_CONTINUE : couvre L291 T
 * (restore échoue) dans le chemin "pause / reprise". */
TEST do_command_line_restore_fails_on_missing_file(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;

    char cmd[] = "restore /tmp/etii_nonexistent_restore_file.back";
    int r = run_command_quiet(cmd);

    request = saved_req;

    ASSERT_EQ_FMT(-1, r, "%d");
    PASS();
}

/* exit_interpreter mode client avec childrens_pid != NULL (L215 branche vraie).
 * On remplit le tableau avec un pid mort (> 0, pour L219 T) et un zéro (L219 F).
 * Le pid mort fait échouer kill(pid, 0) → remaining reste 0 → boucle s'arrête
 * → exit(EXIT_SUCCESS). Exécuté dans un fork pour ne pas tuer le runner. */
static void fork_exit_client_with_children_array(void)
{
    server = 0;
    parent_pid = getpid();

    /* Crée un fils qui termine immédiatement pour obtenir un pid mort. */
    pid_t dead_child = fork();
    if (dead_child == 0) {
        _exit(0);
    }
    waitpid(dead_child, NULL, 0); /* récolte → pid maintenant disparu */

    pid_t pids[2] = { dead_child, 0 }; /* > 0 puis <= 0 : couvre L219 T et F */
    childrens_pid = pids;
    NB_THREADS = 2;

    exit_interpreter();
    _exit(99); /* non atteint */
}

TEST exit_interpreter_client_with_children_array_exits(void)
{
    int code = run_in_fork(fork_exit_client_with_children_array, NULL);
    ASSERT_EQ_FMT(0, code, "%d");
    PASS();
}

/* ---------- admin_pause_transition (pure) -------------------------------- */
/*
 * Fonction pure extraite de pause_interpreter/resume_interpreter : couvre
 * toutes les combinaisons état courant x demande (pause/resume), sans passer
 * par le global `request` ni par le dispatch de commande.
 */

/* Demande de pause (want_pause=1) : CONTINUE et PAUSE basculent vers
   ADMIN_PAUSE ; ADMIN_PAUSE et STOP sont inchangés (no-op). */
TEST admin_pause_transition_pause_request(void)
{
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, admin_pause_transition(REQUEST_CONTINUE, 1), "%d");
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, admin_pause_transition(REQUEST_PAUSE, 1), "%d");
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, admin_pause_transition(REQUEST_ADMIN_PAUSE, 1), "%d");
    ASSERT_EQ_FMT(REQUEST_STOP, admin_pause_transition(REQUEST_STOP, 1), "%d");
    PASS();
}

/* Demande de reprise (want_pause=0) : seul ADMIN_PAUSE bascule vers CONTINUE ;
   REQUEST_PAUSE (régulation de débit) n'est PAS de son ressort -> inchangé. */
TEST admin_pause_transition_resume_request(void)
{
    ASSERT_EQ_FMT(REQUEST_CONTINUE, admin_pause_transition(REQUEST_ADMIN_PAUSE, 0), "%d");
    ASSERT_EQ_FMT(REQUEST_CONTINUE, admin_pause_transition(REQUEST_CONTINUE, 0), "%d");
    ASSERT_EQ_FMT(REQUEST_PAUSE, admin_pause_transition(REQUEST_PAUSE, 0), "%d");
    ASSERT_EQ_FMT(REQUEST_STOP, admin_pause_transition(REQUEST_STOP, 0), "%d");
    PASS();
}

/* ---------- pause / resume interpreters (globals) ------------------------ */

/* `pause` depuis REQUEST_CONTINUE : bascule en pause admin. */
TEST do_command_line_pause_sets_admin_pause(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;

    char cmd[] = "pause";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, request, "%d");

    request = saved_req;
    PASS();
}

/* `pause` depuis REQUEST_ADMIN_PAUSE : no-op (reste en pause admin). */
TEST do_command_line_pause_is_idempotent(void)
{
    int saved_req = request;
    request = REQUEST_ADMIN_PAUSE;

    char cmd[] = "pause";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, request, "%d");

    request = saved_req;
    PASS();
}

/* `pause` pendant un arrêt en cours (REQUEST_STOP) : n'interfère pas. */
TEST do_command_line_pause_noop_on_stop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    char cmd[] = "pause";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(REQUEST_STOP, request, "%d");

    request = saved_req;
    PASS();
}

/* `resume` depuis REQUEST_ADMIN_PAUSE : reprise (-> REQUEST_CONTINUE). */
TEST do_command_line_resume_clears_admin_pause(void)
{
    int saved_req = request;
    request = REQUEST_ADMIN_PAUSE;

    char cmd[] = "resume";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    PASS();
}

/* `resume` en dehors d'une pause admin (ex. REQUEST_PAUSE, régulation de débit)
   : no-op, ne doit surtout pas interférer avec le régulateur. */
TEST do_command_line_resume_noop_outside_admin_pause(void)
{
    int saved_req = request;
    request = REQUEST_PAUSE;

    char cmd[] = "resume";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");
    ASSERT_EQ_FMT(REQUEST_PAUSE, request, "%d");

    request = saved_req;
    PASS();
}

/* `pause`/`resume` console diffusent aussi CTRL_COMMAND aux sessions de
 * contrôle actives (fusion de l'ancien `clientsPause`/`clientsResume`) : le
 * serveur ne lance jamais sa propre recherche (`request` n'y est jamais
 * consulté), donc une pause purement locale y serait un no-op ; c'est cette
 * diffusion qui rend `pause` utile côté serveur. */
TEST do_command_line_pause_broadcasts_to_control_sessions(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;

    control_hello_t h = { .pid = 999, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);

    char cmd[] = "pause";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");

    uint8_t out_cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx, &out_cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)out_cmd);
    ASSERT_STR_EQ("pause", line);
    ASSERT_EQ(1, control_registry_desired_pause_state());

    /* Nettoyage : remet le registre dans un état neutre pour les tests suivants. */
    char resume_cmd[] = "resume";
    run_command_quiet(resume_cmd);
    control_registry_unregister(idx);
    request = saved_req;
    PASS();
}

/* ---------- clientsCommand --to (adressage, PR3) --------------------------
 *
 * `clientsCommand [--to <cible>] <ligne...>` : sans --to, diffusion à toutes
 * les sessions (comportement historique, déjà couvert plus haut) ; avec
 * --to <session_no|client_uid|label>, n'atteint QUE la session désignée, en
 * repassant par la MÊME liste blanche (`control_command_allowed`).
 */

TEST do_command_line_clientscommand_to_reaches_only_targeted_session_by_label(void)
{
    control_hello_t alpha = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0, .label = "alpha" } };
    control_hello_t beta = { .pid = 2, .nb_forks = 1, .identity = { .mode = 0, .label = "beta" } };
    int idx_alpha = control_registry_register(1, "203.0.113.1", &alpha);
    int idx_beta = control_registry_register(2, "203.0.113.2", &beta);
    ASSERT(idx_alpha >= 0);
    ASSERT(idx_beta >= 0);

    char cmd[] = "clientsCommand --to beta limit 500";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");

    uint8_t out_cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx_beta, &out_cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)out_cmd);
    ASSERT_STR_EQ("limit 500", line);
    /* "alpha" n'a rien reçu : le timeout doit expirer. */
    ASSERT_EQ(1, control_registry_wait_command(idx_alpha, &out_cmd, NULL, 0, 100));

    control_registry_unregister(idx_alpha);
    control_registry_unregister(idx_beta);
    PASS();
}

TEST do_command_line_clientscommand_to_reaches_only_targeted_session_by_session_no(void)
{
    control_hello_t h = { .pid = 3, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);

    control_session_info_t infos[1];
    ASSERT_EQ(1, control_registry_snapshot(infos, 1));
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "clientsCommand --to %llu pause",
             (unsigned long long)infos[0].session_no);

    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");

    uint8_t out_cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx, &out_cmd, line, sizeof(line), 200));
    ASSERT_STR_EQ("pause", line);

    control_registry_unregister(idx);
    PASS();
}

TEST do_command_line_clientscommand_to_still_enforces_whitelist(void)
{
    control_hello_t h = { .pid = 4, .nb_forks = 1, .identity = { .mode = 0, .label = "gamma" } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);

    /* "exit" reste hors liste blanche même avec une cible valide : le
       ciblage n'élargit jamais le jeu de commandes autorisées. */
    char cmd[] = "clientsCommand --to gamma exit";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");

    uint8_t out_cmd = 0;
    ASSERT_EQ(1, control_registry_wait_command(idx, &out_cmd, NULL, 0, 100)); /* rien reçu */

    control_registry_unregister(idx);
    PASS();
}

TEST do_command_line_clientscommand_to_unknown_target_rejected(void)
{
    char cmd[] = "clientsCommand --to no-such-client pause";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

TEST do_command_line_clientscommand_to_missing_command_is_usage_error(void)
{
    char cmd[] = "clientsCommand --to alpha";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

/* ---------- clientsWork (attribution des analyses en cours, PR6) ----------
 *
 * Consultation « que travaille X ? » : la cible est résolue exactement comme
 * `clientsCommand --to` (même refus inconnu/ambigu), mais rien n'est envoyé
 * au client -- seule l'attribution déjà enregistrée côté serveur (table
 * latérale de datamanager.c, adossée à analysed_index) est lue.
 */

TEST do_command_line_clientswork_missing_target_is_usage_error(void)
{
    char cmd[] = "clientsWork";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

TEST do_command_line_clientswork_unknown_target_rejected(void)
{
    char cmd[] = "clientsWork no-such-client";
    ASSERT_EQ_FMT(-1, run_command_quiet(cmd), "%d");
    PASS();
}

TEST do_command_line_clientswork_reports_nothing_owned(void)
{
    control_hello_t h = { .pid = 5, .nb_forks = 1, .identity = { .mode = 0, .label = "delta" } };
    int idx = control_registry_register(1, "203.0.113.30", &h);
    ASSERT(idx >= 0);

    char cmd[] = "clientsWork delta";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");

    control_registry_unregister(idx);
    PASS();
}

TEST do_command_line_clientswork_reports_owned_attribution(void)
{
    control_hello_t h = { .pid = 6, .nb_forks = 1, .identity = { .mode = 0, .label = "epsilon" } };
    for (int i = 0; i < CLIENT_UID_BYTES; i++) {
        h.identity.client_uid[i] = (uint8_t)(0x60 + i);
    }
    int idx = control_registry_register(1, "203.0.113.31", &h);
    ASSERT(idx >= 0);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 55;
    add_possibility_analysed_owned(&pk, -1, h.identity.client_uid);

    char cmd[] = "clientsWork epsilon";
    ASSERT_EQ_FMT(0, run_command_quiet(cmd), "%d");

    /* La commande ne consomme rien : l'attribution reste lisible ensuite. */
    unsigned long long count = 0;
    int max_alloc = -1;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(h.identity.client_uid, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");
    ASSERT_EQ_FMT(55, max_alloc, "%d");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, -1), "%d");
    control_registry_unregister(idx);
    PASS();
}

/* ---------- pruner_batch_clamp (pure) ------------------------------------ */
/*
 * Fonction pure extraite de pruner_batch_interpreter : bornes [1, PRUNER_BATCH_MAX],
 * réutilisée par admin_apply_remote_command.
 */
TEST pruner_batch_clamp_bounds(void)
{
    ASSERT_EQ_FMT(1, pruner_batch_clamp(0), "%d");
    ASSERT_EQ_FMT(1, pruner_batch_clamp(-5), "%d");
    ASSERT_EQ_FMT(8, pruner_batch_clamp(8), "%d");
    ASSERT_EQ_FMT(PRUNER_BATCH_MAX, pruner_batch_clamp(999999999), "%d");
    ASSERT_EQ_FMT(PRUNER_BATCH_MAX, pruner_batch_clamp(PRUNER_BATCH_MAX), "%d");
    PASS();
}

/* ---------- admin_apply_remote_command ------------------------------------ */
/*
 * Chemin d'exécution réentrant (strtok_r) destiné à un appelant concurrent
 * (ex. thread HTTP) — jamais do_command_line/strtok. On vérifie : les 5
 * commandes whitelistées appliquent bien le bon effet, une commande hors
 * liste blanche est rejetée AVANT toute tokenisation, les arguments manquants
 * sont détectés, et un appel au milieu d'une séquence strtok() externe ne la
 * perturbe pas (preuve de la réentrance).
 */

TEST admin_apply_remote_command_pause_resume(void)
{
    int saved_req = request;

    request = REQUEST_CONTINUE;
    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("pause"), "%d");
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, request, "%d");

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("resume"), "%d");
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    PASS();
}

/* Bug rapporté : une commande "pause"/"resume" passée via l'API HTTP admin
 * (POST /api/v1/command -> admin_apply_remote_command, cf. http_server.c) ne
 * mettait à jour que l'état LOCAL du serveur (jamais consulté par sa propre
 * recherche, qu'il ne lance pas) sans jamais atteindre les clients connectés
 * — contrairement à la même commande tapée sur la console (pause_interpreter/
 * resume_interpreter, qui diffusent via control_registry_broadcast_command).
 * Ce test verrouille la parité entre les deux chemins d'entrée. */
TEST admin_apply_remote_command_pause_broadcasts_to_control_sessions(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;

    control_hello_t h = { .pid = 998, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("pause"), "%d");

    uint8_t out_cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx, &out_cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)out_cmd);
    ASSERT_STR_EQ("pause", line);
    ASSERT_EQ(1, control_registry_desired_pause_state());

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("resume"), "%d");
    ASSERT_EQ(0, control_registry_wait_command(idx, &out_cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)out_cmd);
    ASSERT_STR_EQ("resume", line);
    ASSERT_EQ(0, control_registry_desired_pause_state());

    control_registry_unregister(idx);
    request = saved_req;
    PASS();
}

TEST admin_apply_remote_command_limit_sets_global(void)
{
    unsigned long long saved = max_search_by_sec;

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("limit 500"), "%d");
    ASSERT_EQ_FMT(500ULL, max_search_by_sec, "%llu");

    max_search_by_sec = saved;
    PASS();
}

TEST admin_apply_remote_command_max_stock_sets_global(void)
{
    int saved = max_stock_by_thread;

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("maxStockByThread 42"), "%d");
    ASSERT_EQ_FMT(42, max_stock_by_thread, "%d");

    max_stock_by_thread = saved;
    PASS();
}

TEST admin_apply_remote_command_pruner_batch_is_clamped(void)
{
    int saved = pruner_batch_size;

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("prunerBatch 999999999"), "%d");
    ASSERT_EQ_FMT(PRUNER_BATCH_MAX, pruner_batch_size, "%d");

    pruner_batch_size = saved;
    PASS();
}

TEST admin_apply_remote_command_rejects_forbidden(void)
{
    int saved_req = request;
    request = REQUEST_CONTINUE;

    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("exit"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("restore"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("import"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command(NULL), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command(""), "%d");
    /* état inchangé : rien n'a été tokenisé ni appliqué */
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    PASS();
}

TEST admin_apply_remote_command_bad_args(void)
{
    unsigned long long saved = max_search_by_sec;

    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("limit"), "%d");
    ASSERT_EQ_FMT(saved, max_search_by_sec, "%llu");

    max_search_by_sec = saved;
    PASS();
}

/* Réentrance : un appel au milieu d'une séquence strtok() externe ne doit pas
   perturber le curseur global de cette séquence (do_command_line en dépend). */
TEST admin_apply_remote_command_does_not_disturb_external_strtok(void)
{
    unsigned long long saved = max_search_by_sec;

    char outer[] = "alpha beta gamma";
    char *first = strtok(outer, " ");
    ASSERT(first != NULL);
    ASSERT_STR_EQ("alpha", first);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("limit 77"), "%d");
    ASSERT_EQ_FMT(77ULL, max_search_by_sec, "%llu");

    char *second = strtok(NULL, " ");
    ASSERT(second != NULL);
    ASSERT_STR_EQ("beta", second);

    max_search_by_sec = saved;
    PASS();
}

/* ---------- admin_apply_remote_command : clientsCommand/clientsCmd/clientsWork
 *
 * Mêmes commandes que la console (clients_cmd_interpreter/clients_work_interpreter),
 * mais réentrantes (strtok_r) pour être appelables depuis l'API HTTP admin
 * (POST /api/v1/command) sans jamais toucher au curseur global de strtok.
 */

TEST admin_apply_remote_command_clientscommand_broadcasts(void)
{
    control_hello_t h = { .pid = 100, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.40", &h);
    ASSERT(idx >= 0);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("clientsCommand pause"), "%d");

    uint8_t out_cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx, &out_cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)out_cmd);
    ASSERT_STR_EQ("pause", line);

    /* L'alias "clientsCmd" applique exactement le même comportement. */
    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("clientsCmd resume"), "%d");
    ASSERT_EQ(0, control_registry_wait_command(idx, &out_cmd, line, sizeof(line), 200));
    ASSERT_STR_EQ("resume", line);

    control_registry_unregister(idx);
    PASS();
}

TEST admin_apply_remote_command_clientscommand_to_targets_one_session(void)
{
    control_hello_t alpha = { .pid = 101, .nb_forks = 1, .identity = { .mode = 0, .label = "alpha" } };
    control_hello_t beta = { .pid = 102, .nb_forks = 1, .identity = { .mode = 0, .label = "beta" } };
    int idx_alpha = control_registry_register(1, "203.0.113.41", &alpha);
    int idx_beta = control_registry_register(2, "203.0.113.42", &beta);
    ASSERT(idx_alpha >= 0);
    ASSERT(idx_beta >= 0);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("clientsCommand --to beta limit 500"), "%d");

    uint8_t out_cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx_beta, &out_cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)out_cmd);
    ASSERT_STR_EQ("limit 500", line);
    /* "alpha" n'a rien reçu. */
    ASSERT_EQ(1, control_registry_wait_command(idx_alpha, &out_cmd, NULL, 0, 100));

    control_registry_unregister(idx_alpha);
    control_registry_unregister(idx_beta);
    PASS();
}

TEST admin_apply_remote_command_clientscommand_to_still_enforces_whitelist(void)
{
    control_hello_t h = { .pid = 103, .nb_forks = 1, .identity = { .mode = 0, .label = "gamma" } };
    int idx = control_registry_register(1, "203.0.113.43", &h);
    ASSERT(idx >= 0);

    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("clientsCommand --to gamma exit"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("clientsCommand exit"), "%d");

    uint8_t out_cmd = 0;
    ASSERT_EQ(1, control_registry_wait_command(idx, &out_cmd, NULL, 0, 100)); /* rien reçu */

    control_registry_unregister(idx);
    PASS();
}

TEST admin_apply_remote_command_clientscommand_to_unknown_target_is_bad_args(void)
{
    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("clientsCommand --to no-such-client pause"), "%d");
    PASS();
}

TEST admin_apply_remote_command_clientscommand_missing_args(void)
{
    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("clientsCommand"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("clientsCommand --to alpha"), "%d");
    PASS();
}

TEST admin_apply_remote_command_clientswork_missing_target_is_bad_args(void)
{
    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("clientsWork"), "%d");
    PASS();
}

TEST admin_apply_remote_command_clientswork_unknown_target_is_bad_args(void)
{
    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("clientsWork no-such-client"), "%d");
    PASS();
}

TEST admin_apply_remote_command_clientswork_reports_owned_attribution(void)
{
    control_hello_t h = { .pid = 104, .nb_forks = 1, .identity = { .mode = 0, .label = "zeta" } };
    for (int i = 0; i < CLIENT_UID_BYTES; i++) {
        h.identity.client_uid[i] = (uint8_t)(0x70 + i);
    }
    int idx = control_registry_register(1, "203.0.113.44", &h);
    ASSERT(idx >= 0);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 77;
    add_possibility_analysed_owned(&pk, -1, h.identity.client_uid);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_remote_command("clientsWork zeta"), "%d");

    /* La commande ne consomme rien : l'attribution reste lisible ensuite. */
    unsigned long long count = 0;
    int max_alloc = -1;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(h.identity.client_uid, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");
    ASSERT_EQ_FMT(77, max_alloc, "%d");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, -1), "%d");
    control_registry_unregister(idx);
    PASS();
}

/* Réentrance : un appel au milieu d'une séquence strtok() externe ne doit pas
   perturber le curseur global de cette séquence. */
TEST admin_apply_remote_command_clientscommand_does_not_disturb_external_strtok(void)
{
    char outer[] = "alpha beta gamma";
    char *first = strtok(outer, " ");
    ASSERT(first != NULL);
    ASSERT_STR_EQ("alpha", first);

    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_remote_command("clientsWork no-such-client"), "%d");

    char *second = strtok(NULL, " ");
    ASSERT(second != NULL);
    ASSERT_STR_EQ("beta", second);
    PASS();
}

/* ---------- admin_apply_privileged_command -------------------------------- */
/*
 * Variante de admin_apply_remote_command destinée exclusivement à l'appelant
 * HTTP APRÈS authentification (src/net/http_server.c) : accepte EN PLUS
 * restore/backup (control_command_privileged). Les commandes standard
 * (pause/limit/...) restent déléguées à admin_apply_remote_command sans
 * changement de comportement, testé séparément ci-dessus.
 */

/* Commandes standard : comportement strictement identique à admin_apply_remote_command. */
TEST admin_apply_privileged_command_still_handles_standard_commands(void)
{
    unsigned long long saved = max_search_by_sec;

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("limit 321"), "%d");
    ASSERT_EQ_FMT(321ULL, max_search_by_sec, "%llu");
    ASSERT_EQ_FMT(ADMIN_CMD_BAD_ARGS, admin_apply_privileged_command("limit"), "%d");

    max_search_by_sec = saved;
    PASS();
}

/* Commandes hors des DEUX listes blanches : toujours refusées. */
TEST admin_apply_privileged_command_rejects_others(void)
{
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_privileged_command("exit"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_privileged_command("import"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_privileged_command(NULL), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_privileged_command(""), "%d");
    PASS();
}

/* backup/restore round-trip via admin_apply_privileged_command, dans un
   répertoire temporaire isolé (même technique que
   do_command_line_backup_restore_import_round_trip) : la commande console
   restore_interpreter tokenise via le strtok GLOBAL, mais admin_apply_privileged_command
   passe par restore_apply (strtok_r) sans jamais toucher à ce curseur — ce
   test vérifie le résultat fonctionnel, pas l'implémentation. */
TEST admin_apply_privileged_command_backup_restore_round_trip(void)
{
    char saved_cwd[4096];
    const char *got = getcwd(saved_cwd, sizeof saved_cwd);
    char tmpl[] = "/tmp/etii_priv_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (got == NULL || dir == NULL || chdir(dir) != 0) {
        if (dir != NULL) rmdir(dir);
        FAILm("setup du répertoire temporaire impossible");
    }
    int saved_server = server;
    server = 1;

    dm_drain();
    int allocs[] = { 1, 2, 3 };
    dm_add(allocs, 3);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("backup"), "%d");
    int back_exists = access("./eternityII.back", F_OK) == 0;

    dm_drain();
    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("restore"), "%d");
    unsigned long long after_restore = datas_size();

    /* --- nettoyage avant toute assertion --- */
    dm_drain();
    unlink("./eternityII.back");
    unlink("./eternityII-in_analyse.back");
    unlink("./eternityII-best_board.back");
    if (chdir(saved_cwd) != 0) { /* best-effort */ }
    rmdir(dir);
    server = saved_server;

    ASSERT(back_exists);
    ASSERT(after_restore > 0);
    PASS();
}

/* sortAsc/sortDesc/sortDescMulti/split/regroup : privilégiées comme
   restore/backup (control_command_privileged) -- refusées via le chemin
   standard non authentifié, acceptées et appliquées via le chemin privilégié. */
TEST admin_apply_remote_command_rejects_sort_group_split(void)
{
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("sortAsc"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("sortDesc"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("sortDescMulti"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("split"), "%d");
    ASSERT_EQ_FMT(ADMIN_CMD_FORBIDDEN, admin_apply_remote_command("regroup"), "%d");
    PASS();
}

TEST admin_apply_privileged_command_split_regroup_round_trip(void)
{
    dm_drain();
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    dm_add(allocs, 10);

    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* tout dans le pool 0 au depart */

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("split"), "%d");
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT(file_size(0) < 10); /* reparti sur plusieurs files */

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("regroup"), "%d");
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* re-consolide dans le pool 0 */

    dm_drain();
    PASS();
}

TEST admin_apply_privileged_command_sorts_run(void)
{
    dm_drain();
    int allocs[3] = { 3, 1, 2 };
    dm_add(allocs, 3);

    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("sortAsc"), "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("sortDesc"), "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(ADMIN_CMD_OK, admin_apply_privileged_command("sortDescMulti"), "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");

    dm_drain();
    PASS();
}

SUITE(command_lines_suite)
{
    RUN_TEST(do_command_line_handles_empty_input);
    RUN_TEST(do_command_line_unknown_returns_error);
    RUN_TEST(do_command_line_help_runs);
    RUN_TEST(do_command_line_help_with_topic);
    RUN_TEST(do_command_line_clear_runs);
    RUN_TEST(help_format_general_lists_categories_and_aliases);
    RUN_TEST(help_format_topic_command_and_category);
    RUN_TEST(help_hides_config_commands_on_server_only);
    RUN_TEST(do_command_line_config_is_masked_on_server_only);
    RUN_TEST(do_command_line_config_save_is_masked_on_server_only);
    RUN_TEST(command_canonical_name_resolves_aliases_and_case);
    RUN_TEST(do_command_line_case_insensitive_and_alias_dispatch);
    RUN_TEST(do_command_line_expand_requires_arg);
    RUN_TEST(do_command_line_max_stock_sets_global);
    RUN_TEST(do_command_line_max_stock_requires_arg);
    RUN_TEST(do_command_line_pruner_batch_is_clamped);
    RUN_TEST(do_command_line_pruner_batch_requires_arg);
    RUN_TEST(do_command_line_lease_duration_sets_global);
    RUN_TEST(do_command_line_lease_duration_requires_arg);
    RUN_TEST(do_command_line_limit_sets_global);
    RUN_TEST(do_command_line_limit_requires_arg);
    RUN_TEST(do_command_line_unknown_command_typo_suggestion);
    RUN_TEST(do_command_line_sort_runs);
    RUN_TEST(do_command_line_sortdm_runs);
    RUN_TEST(do_command_line_split_regroup_runs);
    RUN_TEST(do_command_line_inspect_commands_run);
    RUN_TEST(do_command_line_printfile_requires_arg);
    RUN_TEST(do_command_line_print_exports_to_file);
    RUN_TEST(do_command_line_printfile_without_path_still_prints_to_console);
    RUN_TEST(do_command_line_printfile_exports_to_file);
    RUN_TEST(do_command_line_printanalysed_exports_without_pid_suffix_on_server);
    RUN_TEST(do_command_line_printanalysed_exports_with_pid_suffix_on_client);
    RUN_TEST(do_command_line_print_fails_on_unwritable_dir);
    RUN_TEST(do_command_line_checkfiles_runs);
    RUN_TEST(do_command_line_checkfile_requires_arg);
    RUN_TEST(do_command_line_checkdirections_runs);
    RUN_TEST(do_command_line_checkdatas_runs);
    RUN_TEST(do_command_line_checkduplicate_runs);
    RUN_TEST(do_command_line_check_runs);
    RUN_TEST(do_command_line_loadjson_runs);
    RUN_TEST(do_command_line_rmnonext_runs);
    RUN_TEST(do_command_line_backup_restore_import_round_trip);
    RUN_TEST(do_command_line_restore_syncs_max_result_from_best_board);

    RUN_TEST(exit_interpreter_server_mode_exits_immediately);
    RUN_TEST(exit_interpreter_client_parent_no_children_exits);
    RUN_TEST(exit_interpreter_client_non_parent_returns_zero);

    RUN_TEST(do_command_line_backup_client_mode_appends_pid);
    RUN_TEST(do_command_line_restore_with_one_argument);
    RUN_TEST(do_command_line_restore_with_two_arguments);

    RUN_TEST(do_command_line_check_with_non_null_lastcheck);
    RUN_TEST(do_command_line_backup_skipped_when_maintenance);
    RUN_TEST(do_command_line_backup_fails_on_unwritable_dir);
    RUN_TEST(do_command_line_restore_when_not_running);
    RUN_TEST(do_command_line_restore_fails_on_missing_file);
    RUN_TEST(exit_interpreter_client_with_children_array_exits);

    RUN_TEST(admin_pause_transition_pause_request);
    RUN_TEST(admin_pause_transition_resume_request);
    RUN_TEST(do_command_line_pause_sets_admin_pause);
    RUN_TEST(do_command_line_pause_is_idempotent);
    RUN_TEST(do_command_line_pause_noop_on_stop);
    RUN_TEST(do_command_line_resume_clears_admin_pause);
    RUN_TEST(do_command_line_resume_noop_outside_admin_pause);
    RUN_TEST(do_command_line_pause_broadcasts_to_control_sessions);

    RUN_TEST(do_command_line_clientscommand_to_reaches_only_targeted_session_by_label);
    RUN_TEST(do_command_line_clientscommand_to_reaches_only_targeted_session_by_session_no);
    RUN_TEST(do_command_line_clientscommand_to_still_enforces_whitelist);
    RUN_TEST(do_command_line_clientscommand_to_unknown_target_rejected);
    RUN_TEST(do_command_line_clientscommand_to_missing_command_is_usage_error);

    RUN_TEST(do_command_line_clientswork_missing_target_is_usage_error);
    RUN_TEST(do_command_line_clientswork_unknown_target_rejected);
    RUN_TEST(do_command_line_clientswork_reports_nothing_owned);
    RUN_TEST(do_command_line_clientswork_reports_owned_attribution);

    RUN_TEST(pruner_batch_clamp_bounds);
    RUN_TEST(admin_apply_remote_command_pause_resume);
    RUN_TEST(admin_apply_remote_command_pause_broadcasts_to_control_sessions);
    RUN_TEST(admin_apply_remote_command_limit_sets_global);
    RUN_TEST(admin_apply_remote_command_max_stock_sets_global);
    RUN_TEST(admin_apply_remote_command_pruner_batch_is_clamped);
    RUN_TEST(admin_apply_remote_command_rejects_forbidden);
    RUN_TEST(admin_apply_remote_command_bad_args);
    RUN_TEST(admin_apply_remote_command_does_not_disturb_external_strtok);

    RUN_TEST(admin_apply_remote_command_clientscommand_broadcasts);
    RUN_TEST(admin_apply_remote_command_clientscommand_to_targets_one_session);
    RUN_TEST(admin_apply_remote_command_clientscommand_to_still_enforces_whitelist);
    RUN_TEST(admin_apply_remote_command_clientscommand_to_unknown_target_is_bad_args);
    RUN_TEST(admin_apply_remote_command_clientscommand_missing_args);
    RUN_TEST(admin_apply_remote_command_clientswork_missing_target_is_bad_args);
    RUN_TEST(admin_apply_remote_command_clientswork_unknown_target_is_bad_args);
    RUN_TEST(admin_apply_remote_command_clientswork_reports_owned_attribution);
    RUN_TEST(admin_apply_remote_command_clientscommand_does_not_disturb_external_strtok);

    RUN_TEST(admin_apply_privileged_command_still_handles_standard_commands);
    RUN_TEST(admin_apply_privileged_command_rejects_others);
    RUN_TEST(admin_apply_privileged_command_backup_restore_round_trip);
    RUN_TEST(admin_apply_remote_command_rejects_sort_group_split);
    RUN_TEST(admin_apply_privileged_command_split_regroup_round_trip);
    RUN_TEST(admin_apply_privileged_command_sorts_run);
}
