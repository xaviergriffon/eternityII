/*
 * Tests unitaires de static_variables.c — pour l'instant le parsing des options
 * globales de la ligne de commande (parse_cli_options).
 *
 * Régression visée : l'option --stop-on-solution doit être reconnue à n'importe
 * quelle position, positionner le drapeau global, et être RETIRÉE de argv sans
 * abîmer les arguments positionnels des modes (server/client/…). Une
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
    const char *argv[] = {"prog", "server", "1", "data/pieces16.csv"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(4, argc, "%d");
    ASSERT_EQ_FMT(0, stop_on_solution, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("1", argv[2]);
    ASSERT_STR_EQ("data/pieces16.csv", argv[3]);
    PASS();
}

TEST flag_at_end_is_stripped_and_sets_global(void)
{
    stop_on_solution = 0;
    const char *argv[] = {"prog", "server", "1", "data/pieces16.csv", "--stop-on-solution"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(4, argc, "%d");          /* l'option a été retirée */
    ASSERT_EQ_FMT(1, stop_on_solution, "%d");
    /* Les arguments positionnels restent intacts et dans l'ordre. */
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("1", argv[2]);
    ASSERT_STR_EQ("data/pieces16.csv", argv[3]);
    PASS();
}

TEST flag_in_the_middle_does_not_shift_positional_args(void)
{
    stop_on_solution = 0;
    const char *argv[] = {"prog", "client", "--stop-on-solution", "localhost", "2", "1000"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(5, argc, "%d");
    ASSERT_EQ_FMT(1, stop_on_solution, "%d");
    /* localhost/2/1000 doivent se retrouver compactés derrière le mode, sans
       trou laissé par l'option supprimée. */
    ASSERT_STR_EQ("client", argv[1]);
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
    const char *argv[] = {"prog", "server", "--expand-level", "4", "8", "data/pieces.csv"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(4, argc, "%d");              /* option + valeur retirées (6 → 4) */
    ASSERT_EQ_FMT(4, expand_min_level, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);               /* nb_threads non décalé */
    ASSERT_STR_EQ("data/pieces.csv", argv[3]);
    PASS();
}

/* Valeur absente (option en dernière position) : ignorée sans lire hors argv,
   expand_min_level reste à 0, l'option est tout de même consommée. */
TEST expand_level_without_value_is_ignored(void)
{
    expand_min_level = 0;
    const char *argv[] = {"prog", "server", "--expand-level"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");              /* seul le token option est retiré */
    ASSERT_EQ_FMT(0, expand_min_level, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    PASS();
}

/* Valeur négative : bornée à 0 (pas d'expansion), plutôt qu'un niveau absurde. */
TEST expand_level_negative_clamped_to_zero(void)
{
    expand_min_level = 7;                        /* valeur résiduelle à écraser */
    const char *argv[] = {"prog", "server", "--expand-level", "-3"};
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
    const char *argv[] = {"prog", "--expand-level", "3", "server", "8", "--stop-on-solution"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(3, argc, "%d");              /* 6 - 2 (expand+val) - 1 (stop) */
    ASSERT_EQ_FMT(3, expand_min_level, "%d");
    ASSERT_EQ_FMT(1, stop_on_solution, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);
    PASS();
}

/* --expand-max-stock <n> : option VALUÉE, même schéma que --expand-level, mais
   0/négatif/absent est IGNORÉ (garde la valeur courante) plutôt que bornée à 0
   — un plafond nul n'a pas de sens utile, contrairement à un niveau nul. */
TEST expand_max_stock_strips_option_and_value_sets_global(void)
{
    expand_max_stock = EXPAND_MAX_STOCK;
    const char *argv[] = {"prog", "server", "--expand-max-stock", "500000", "8"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");              /* option + valeur retirées (5 → 3) */
    ASSERT_EQ_FMT(500000, expand_max_stock, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);               /* argument positionnel non décalé */
    PASS();
}

/* Valeur absente : ignorée sans lire hors argv, expand_max_stock reste
   inchangé, l'option est tout de même consommée. */
TEST expand_max_stock_without_value_is_ignored(void)
{
    expand_max_stock = EXPAND_MAX_STOCK;
    const char *argv[] = {"prog", "server", "--expand-max-stock"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");              /* seul le token option est retiré */
    ASSERT_EQ_FMT(EXPAND_MAX_STOCK, expand_max_stock, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    PASS();
}

/* Valeur <= 0 : ignorée (garde la valeur déjà fixée), pas un plafond absurde
   qui arrêterait l'expansion avant même la première pièce placée. */
TEST expand_max_stock_non_positive_value_is_ignored(void)
{
    expand_max_stock = 12345;                    /* valeur résiduelle à préserver */
    const char *argv[] = {"prog", "server", "--expand-max-stock", "0"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ_FMT(12345, expand_max_stock, "%d");
    PASS();
}

/* --expand-max-levels <n> : option VALUÉE, même schéma que --expand-max-stock
   (0/négatif/absent ignoré, garde la valeur courante). */
TEST expand_max_levels_strips_option_and_value_sets_global(void)
{
    expand_max_levels = EXPAND_MAX_LEVELS;
    const char *argv[] = {"prog", "server", "--expand-max-levels", "8", "80"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");              /* option + valeur retirées (5 → 3) */
    ASSERT_EQ_FMT(8, expand_max_levels, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("80", argv[2]);              /* argument positionnel non décalé */
    PASS();
}

/* Valeur absente : ignorée sans lire hors argv, expand_max_levels reste
   inchangé, l'option est tout de même consommée. */
TEST expand_max_levels_without_value_is_ignored(void)
{
    expand_max_levels = EXPAND_MAX_LEVELS;
    const char *argv[] = {"prog", "server", "--expand-max-levels"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");              /* seul le token option est retiré */
    ASSERT_EQ_FMT(EXPAND_MAX_LEVELS, expand_max_levels, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    PASS();
}

/* Valeur <= 0 : ignorée (garde la valeur déjà fixée), un plafond nul
   empêcherait toute passe d'expansion. */
TEST expand_max_levels_non_positive_value_is_ignored(void)
{
    expand_max_levels = 6;                        /* valeur résiduelle à préserver */
    const char *argv[] = {"prog", "server", "--expand-max-levels", "-1"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ_FMT(6, expand_max_levels, "%d");
    PASS();
}

/* --stock-max-ram <mo> : option VALUÉE, même schéma que --expand-max-stock
   (0/négatif/absent ignoré, garde la valeur courante = 0 = illimité). */
TEST stock_max_ram_strips_option_and_value_sets_global(void)
{
    stock_max_ram_mb = 0;
    const char *argv[] = {"prog", "server", "--stock-max-ram", "2048", "8"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");              /* option + valeur retirées (5 → 3) */
    ASSERT_EQ_FMT(2048, stock_max_ram_mb, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);               /* argument positionnel non décalé */
    PASS();
}

/* Valeur absente : ignorée sans lire hors argv, stock_max_ram_mb reste
   inchangé, l'option est tout de même consommée. */
TEST stock_max_ram_without_value_is_ignored(void)
{
    stock_max_ram_mb = 0;
    const char *argv[] = {"prog", "server", "--stock-max-ram"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");              /* seul le token option est retiré */
    ASSERT_EQ_FMT(0, stock_max_ram_mb, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    PASS();
}

/* Valeur <= 0 fournie explicitement : ignorée (garde la valeur déjà fixée),
   même convention que --expand-max-stock -- pour DÉSACTIVER le plafond à
   chaud, la commande console `stockMaxRam 0` reste le bon outil (elle
   n'utilise pas ce chemin de parsing CLI). */
TEST stock_max_ram_non_positive_value_is_ignored(void)
{
    stock_max_ram_mb = 4096;                    /* valeur résiduelle à préserver */
    const char *argv[] = {"prog", "server", "--stock-max-ram", "-1"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ_FMT(4096, stock_max_ram_mb, "%d");
    PASS();
}

/* --stock-spill-dir <chemin> : option VALUÉE pointeur, même schéma que
   --http-token-file (jamais copiée, un pointeur argv remplace directement le
   défaut littéral). */
TEST stock_spill_dir_strips_option_and_value_sets_global(void)
{
    stock_spill_dir = "./eternityii-spill";
    const char *argv[] = {"prog", "server", "--stock-spill-dir", "/mnt/spill", "8"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_STR_EQ("/mnt/spill", stock_spill_dir);
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);
    PASS();
}

/* Valeur absente : ignorée sans lire hors argv, stock_spill_dir garde son
   défaut littéral, l'option est tout de même consommée. */
TEST stock_spill_dir_without_value_is_ignored(void)
{
    stock_spill_dir = "./eternityii-spill";
    const char *argv[] = {"prog", "server", "--stock-spill-dir"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_STR_EQ("./eternityii-spill", stock_spill_dir);
    ASSERT_STR_EQ("server", argv[1]);
    PASS();
}

/* --http-port <n> : option VALUÉE, même schéma que --expand-level. Valeur dans
   [1, 65535] : les deux tokens sont retirés d'argv, HTTP_PORT est fixé. */
TEST http_port_strips_option_and_value_sets_global(void)
{
    HTTP_PORT = 0;
    const char *argv[] = {"prog", "server", "--http-port", "8080", "data/pieces.csv"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_EQ_FMT(8080, HTTP_PORT, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("data/pieces.csv", argv[2]);
    PASS();
}

/* Valeur absente (dernière position) : ignorée, HTTP_PORT reste à 0 (désactivée). */
TEST http_port_without_value_is_ignored(void)
{
    HTTP_PORT = 0;
    const char *argv[] = {"prog", "server", "--http-port"};
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
        const char *argv[] = {"prog", "server", "--http-port", bad_values[i]};
        int argc = parse_cli_options(4, argv);

        ASSERT_EQ_FMT(2, argc, "%d");
        ASSERT_EQ_FMT(0, HTTP_PORT, "%d");
    }
    PASS();
}

/* --http-token-file <chemin> : option VALUÉE, même schéma que --http-port.
   Le chemin est mémorisé tel quel (pointeur dans argv), les deux tokens sont
   retirés d'argv. Aucune I/O ici (parse_cli_options reste pur) : le
   chargement réel est testé séparément (http_token_load, tests/net). */
TEST http_token_file_strips_option_and_value_sets_global(void)
{
    HTTP_TOKEN_FILE = NULL;
    const char *argv[] = {"prog", "server", "--http-token-file", "/etc/etii/token", "data/pieces.csv"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT(HTTP_TOKEN_FILE != NULL);
    ASSERT_STR_EQ("/etc/etii/token", HTTP_TOKEN_FILE);
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("data/pieces.csv", argv[2]);
    PASS();
}

/* Valeur absente (dernière position) : ignorée, HTTP_TOKEN_FILE reste NULL. */
TEST http_token_file_without_value_is_ignored(void)
{
    HTTP_TOKEN_FILE = NULL;
    const char *argv[] = {"prog", "server", "--http-token-file"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ(NULL, HTTP_TOKEN_FILE);
    PASS();
}

TEST name_option_strips_option_and_value_sets_global(void)
{
    client_label = NULL;
    const char *argv[] = {"prog", "client", "--name", "jetson-1", "localhost", "8"};
    int argc = parse_cli_options(6, argv);

    ASSERT_EQ_FMT(4, argc, "%d");
    ASSERT(client_label != NULL);
    ASSERT_STR_EQ("jetson-1", client_label);
    ASSERT_STR_EQ("client", argv[1]);
    ASSERT_STR_EQ("localhost", argv[2]);
    ASSERT_STR_EQ("8", argv[3]);
    PASS();
}

/* Valeur absente (dernière position) : ignorée, client_label reste NULL. */
TEST name_option_without_value_is_ignored(void)
{
    client_label = NULL;
    const char *argv[] = {"prog", "client", "--name"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_EQ(NULL, client_label);
    PASS();
}

TEST machine_uid_file_option_strips_option_and_value_sets_global(void)
{
    machine_uid_file_path = "./eternityii-machine_uid";
    const char *argv[] = {"prog", "client", "--machine-uid-file", "/etc/etii/machine_uid", "localhost"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_STR_EQ("/etc/etii/machine_uid", machine_uid_file_path);
    ASSERT_STR_EQ("client", argv[1]);
    ASSERT_STR_EQ("localhost", argv[2]);
    PASS();
}

/* Valeur absente : ignorée, la valeur (par défaut ou déjà en place) reste inchangée. */
TEST machine_uid_file_option_without_value_is_ignored(void)
{
    machine_uid_file_path = "./eternityii-machine_uid";
    const char *argv[] = {"prog", "client", "--machine-uid-file"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(2, argc, "%d");
    ASSERT_STR_EQ("./eternityii-machine_uid", machine_uid_file_path);
    PASS();
}

/* --gpu : position-indépendante — retirée d'argv, gpu_requested positionné,
   arguments positionnels du pruner intacts (l'interprétation CUDA/non-CUDA se
   fait dans main(), pas ici). */
TEST gpu_flag_is_stripped_and_sets_global(void)
{
    gpu_requested = 0;
    const char *argv[] = {"prog", "pruner", "--gpu", "localhost", "2"};
    int argc = parse_cli_options(5, argv);

    ASSERT_EQ_FMT(4, argc, "%d");
    ASSERT_EQ_FMT(1, gpu_requested, "%d");
    ASSERT_STR_EQ("pruner", argv[1]);
    ASSERT_STR_EQ("localhost", argv[2]);
    ASSERT_STR_EQ("2", argv[3]);
    PASS();
}

/* Sans --gpu, le drapeau reste à 0. */
TEST gpu_flag_absent_leaves_global_untouched(void)
{
    gpu_requested = 0;
    const char *argv[] = {"prog", "pruner", "localhost"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_EQ_FMT(0, gpu_requested, "%d");
    PASS();
}

/* --headless : position-indépendante — retirée d'argv, headless_mode
   positionné, arguments positionnels intacts. */
TEST headless_flag_is_stripped_and_sets_global(void)
{
    headless_mode = 0;
    const char *argv[] = {"prog", "server", "--headless", "8"};
    int argc = parse_cli_options(4, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_EQ_FMT(1, headless_mode, "%d");
    ASSERT_STR_EQ("server", argv[1]);
    ASSERT_STR_EQ("8", argv[2]);
    PASS();
}

/* Sans --headless, le drapeau reste à 0. */
TEST headless_flag_absent_leaves_global_untouched(void)
{
    headless_mode = 0;
    const char *argv[] = {"prog", "client", "localhost"};
    int argc = parse_cli_options(3, argv);

    ASSERT_EQ_FMT(3, argc, "%d");
    ASSERT_EQ_FMT(0, headless_mode, "%d");
    PASS();
}

/* --help / -h : position-indépendantes comme --stop-on-solution — retirées
   d'argv, help_requested positionné, arguments positionnels intacts. */
TEST help_flag_is_stripped_and_sets_global(void)
{
    const char *flags[] = {"--help", "-h"};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        help_requested = 0;
        const char *argv[] = {"prog", "server", flags[i], "8"};
        int argc = parse_cli_options(4, argv);

        ASSERT_EQ_FMT(3, argc, "%d");
        ASSERT_EQ_FMT(1, help_requested, "%d");
        ASSERT_STR_EQ("server", argv[1]);
        ASSERT_STR_EQ("8", argv[2]);
    }
    PASS();
}

/* Sans --help ni -h, le drapeau reste à 0 ("help" positionnel = mode, pas option). */
TEST help_flag_absent_leaves_global_untouched(void)
{
    help_requested = 0;
    const char *argv[] = {"prog", "help", "server"};
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

/* bench_parse_nodes_env : variable ETII_BENCH_NODES du banc de mesure
   (tests/bench/bench_search.sh). Absente/vide/non numérique -> 0 (banc
   désactivé) plutôt qu'un comportement surprenant sur une valeur malformée. */
TEST bench_parse_nodes_env_absent_or_empty_returns_zero(void)
{
    ASSERT_EQ_FMT(0ULL, bench_parse_nodes_env(NULL), "%llu");
    ASSERT_EQ_FMT(0ULL, bench_parse_nodes_env(""), "%llu");
    PASS();
}

TEST bench_parse_nodes_env_non_numeric_returns_zero(void)
{
    ASSERT_EQ_FMT(0ULL, bench_parse_nodes_env("abc"), "%llu");
    ASSERT_EQ_FMT(0ULL, bench_parse_nodes_env("--5"), "%llu");
    PASS();
}

TEST bench_parse_nodes_env_valid_decimal_is_parsed(void)
{
    ASSERT_EQ_FMT(2000000ULL, bench_parse_nodes_env("2000000"), "%llu");
    ASSERT_EQ_FMT(0ULL, bench_parse_nodes_env("0"), "%llu");
    PASS();
}

/* bench_should_stop : décision pure consommée par check_client_threads
   (src/app/etii_client.c). Cible 0 = banc désactivé : ne s'arrête jamais,
   quel que soit le nombre de nœuds déjà visités. */
TEST bench_should_stop_disabled_when_target_is_zero(void)
{
    ASSERT_EQ_FMT(0, bench_should_stop(0, 0), "%d");
    ASSERT_EQ_FMT(0, bench_should_stop(0, 1000000), "%d");
    PASS();
}

TEST bench_should_stop_false_while_below_target(void)
{
    ASSERT_EQ_FMT(0, bench_should_stop(1000, 999), "%d");
    PASS();
}

/* Egalité exacte ET dépassement doivent tous deux déclencher l'arrêt : le
   sondage périodique de check_client_threads observe rarement la valeur
   exacte de la cible. */
TEST bench_should_stop_true_at_or_above_target(void)
{
    ASSERT_EQ_FMT(1, bench_should_stop(1000, 1000), "%d");
    ASSERT_EQ_FMT(1, bench_should_stop(1000, 1500), "%d");
    PASS();
}

/* mrv_parse_env : sélecteur d'ordre de variable de la recherche (§4.7).
 * L'ordre DYNAMIQUE étant le défaut de production, l'absence de variable — ou
 * une valeur qu'on ne sait pas lire — doit rendre ce défaut, jamais 0 : une
 * faute de frappe dans l'environnement ne doit pas rétrograder silencieusement
 * le moteur de recherche. */
TEST mrv_parse_env_absent_or_unknown_returns_default(void)
{
    ASSERT_EQ_FMT(MRV_DEFAULT_ENABLED, mrv_parse_env(NULL), "%d");
    ASSERT_EQ_FMT(MRV_DEFAULT_ENABLED, mrv_parse_env(""), "%d");
    ASSERT_EQ_FMT(MRV_DEFAULT_ENABLED, mrv_parse_env("oui"), "%d");
    ASSERT_EQ_FMT(MRV_DEFAULT_ENABLED, mrv_parse_env("2"), "%d");
    PASS();
}

TEST mrv_parse_env_explicit_values_win(void)
{
    ASSERT_EQ_FMT(0, mrv_parse_env("0"), "%d"); /* ordre fixe */
    ASSERT_EQ_FMT(1, mrv_parse_env("1"), "%d"); /* ordre dynamique (MRV) */
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
    RUN_TEST(expand_max_stock_strips_option_and_value_sets_global);
    RUN_TEST(expand_max_stock_without_value_is_ignored);
    RUN_TEST(expand_max_stock_non_positive_value_is_ignored);
    RUN_TEST(expand_max_levels_strips_option_and_value_sets_global);
    RUN_TEST(expand_max_levels_without_value_is_ignored);
    RUN_TEST(expand_max_levels_non_positive_value_is_ignored);
    RUN_TEST(stock_max_ram_strips_option_and_value_sets_global);
    RUN_TEST(stock_max_ram_without_value_is_ignored);
    RUN_TEST(stock_max_ram_non_positive_value_is_ignored);
    RUN_TEST(stock_spill_dir_strips_option_and_value_sets_global);
    RUN_TEST(stock_spill_dir_without_value_is_ignored);
    RUN_TEST(http_port_strips_option_and_value_sets_global);
    RUN_TEST(http_port_without_value_is_ignored);
    RUN_TEST(http_port_out_of_range_values_are_ignored);
    RUN_TEST(http_token_file_strips_option_and_value_sets_global);
    RUN_TEST(http_token_file_without_value_is_ignored);
    RUN_TEST(name_option_strips_option_and_value_sets_global);
    RUN_TEST(name_option_without_value_is_ignored);
    RUN_TEST(machine_uid_file_option_strips_option_and_value_sets_global);
    RUN_TEST(machine_uid_file_option_without_value_is_ignored);
    RUN_TEST(gpu_flag_is_stripped_and_sets_global);
    RUN_TEST(gpu_flag_absent_leaves_global_untouched);
    RUN_TEST(headless_flag_is_stripped_and_sets_global);
    RUN_TEST(headless_flag_absent_leaves_global_untouched);
    RUN_TEST(help_flag_is_stripped_and_sets_global);
    RUN_TEST(help_flag_absent_leaves_global_untouched);
    RUN_TEST(request_is_pause_covers_both_pause_values);
    RUN_TEST(request_keeps_running_is_false_only_on_stop);
    RUN_TEST(mrv_parse_env_absent_or_unknown_returns_default);
    RUN_TEST(mrv_parse_env_explicit_values_win);
    RUN_TEST(bench_parse_nodes_env_absent_or_empty_returns_zero);
    RUN_TEST(bench_parse_nodes_env_non_numeric_returns_zero);
    RUN_TEST(bench_parse_nodes_env_valid_decimal_is_parsed);
    RUN_TEST(bench_should_stop_disabled_when_target_is_zero);
    RUN_TEST(bench_should_stop_false_while_below_target);
    RUN_TEST(bench_should_stop_true_at_or_above_target);
}
