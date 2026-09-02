/*
 * Tests unitaires de server_config.c :
 * parsing/écriture de la configuration serveur clé=valeur, et son application
 * aux globales avec la priorité CLI > fichier > défauts (cf. client_config,
 * son pendant côté client).
 */
#include "greatest.h"
#include "app/server_config.h"
#include "app/app_static_variables.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ---------------------------- server_config_parse_line -------------------- */

TEST parse_line_blank_and_comment_are_ignored(void)
{
    server_config_t cfg;
    server_config_init(&cfg);

    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_IGNORED, server_config_parse_line("\n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_IGNORED, server_config_parse_line("", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_IGNORED, server_config_parse_line("   \n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_IGNORED, server_config_parse_line("# commentaire\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_nb_threads, "%d");
    PASS();
}

TEST parse_line_without_equals_is_unknown(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_UNKNOWN_KEY, server_config_parse_line("nb_threads 4\n", &cfg), "%d");
    PASS();
}

TEST parse_line_unknown_key_is_reported(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_UNKNOWN_KEY, server_config_parse_line("bogus_key = 1\n", &cfg), "%d");
    PASS();
}

TEST parse_line_nb_threads_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("nb_threads = 40\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_nb_threads, "%d");
    ASSERT_EQ_FMT(40, cfg.nb_threads, "%d");
    PASS();
}

TEST parse_line_nb_threads_zero_or_negative_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("nb_threads = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("nb_threads = -1\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_nb_threads, "%d");
    PASS();
}

TEST parse_line_nb_threads_non_numeric_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("nb_threads = abc\n", &cfg), "%d");
    PASS();
}

TEST parse_line_parts_file_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("parts_file = data/pieces.csv\n", &cfg), "%d");
    ASSERT_STR_EQ("data/pieces.csv", cfg.parts_file);
    server_config_free(&cfg);
    PASS();
}

TEST parse_line_parts_file_empty_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("parts_file =\n", &cfg), "%d");
    PASS();
}

/* expand_level : 0 est une valeur VALIDE (expansion désactivée), comme côté CLI. */
TEST parse_line_expand_level_zero_is_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("expand_level = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_expand_level, "%d");
    ASSERT_EQ_FMT(0, cfg.expand_level, "%d");
    PASS();
}

TEST parse_line_expand_level_negative_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("expand_level = -1\n", &cfg), "%d");
    PASS();
}

TEST parse_line_http_port_in_range_is_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("http_port = 8080\n", &cfg), "%d");
    ASSERT_EQ_FMT(8080, cfg.http_port, "%d");
    PASS();
}

TEST parse_line_http_port_out_of_range_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("http_port = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("http_port = 70000\n", &cfg), "%d");
    PASS();
}

TEST parse_line_http_token_file_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("http_token_file = ./token\n", &cfg), "%d");
    ASSERT_STR_EQ("./token", cfg.http_token_file);
    server_config_free(&cfg);
    PASS();
}

TEST parse_line_stock_spill_dir_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("stock_spill_dir = /var/spill\n", &cfg), "%d");
    ASSERT_STR_EQ("/var/spill", cfg.stock_spill_dir);
    server_config_free(&cfg);
    PASS();
}

TEST parse_line_boolean_keys_accept_zero_and_one(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("auto_roles = 1\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.auto_roles, "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("stop_on_solution = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.stop_on_solution, "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("headless = 1\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.headless, "%d");
    PASS();
}

TEST parse_line_boolean_keys_reject_other_values(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("auto_roles = 2\n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("headless = yes\n", &cfg), "%d");
    PASS();
}

TEST parse_line_sort_enabled_accepts_zero_and_one(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("sort_enabled = 1\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_sort_enabled, "%d");
    ASSERT_EQ_FMT(1, cfg.sort_enabled, "%d");
    PASS();
}

TEST parse_line_sort_enabled_rejects_other_values(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("sort_enabled = 2\n", &cfg), "%d");
    PASS();
}

TEST parse_line_sort_interval_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("sort_interval = 120\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_sort_interval, "%d");
    ASSERT_EQ_FMT(120, cfg.sort_interval, "%d");
    PASS();
}

TEST parse_line_sort_interval_zero_or_negative_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("sort_interval = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("sort_interval = -1\n", &cfg), "%d");
    PASS();
}

TEST parse_line_sort_direction_accepts_asc_and_desc(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("sort_direction = desc\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_sort_direction, "%d");
    ASSERT_EQ_FMT(SORT_DIRECTION_DESC, cfg.sort_direction, "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("sort_direction = asc\n", &cfg), "%d");
    ASSERT_EQ_FMT(SORT_DIRECTION_ASC, cfg.sort_direction, "%d");
    PASS();
}

TEST parse_line_sort_direction_rejects_other_values(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("sort_direction = bogus\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_sort_direction, "%d");
    PASS();
}

TEST parse_line_sort_lock_attempts_valid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_SET, server_config_parse_line("sort_lock_attempts = 10\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_sort_lock_attempts, "%d");
    ASSERT_EQ_FMT(10, cfg.sort_lock_attempts, "%d");
    PASS();
}

TEST parse_line_sort_lock_attempts_zero_or_negative_is_invalid(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("sort_lock_attempts = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(SERVER_CONFIG_LINE_INVALID_VALUE, server_config_parse_line("sort_lock_attempts = -1\n", &cfg), "%d");
    PASS();
}

/* ------------------------------ server_config_load ------------------------- */

TEST load_missing_file_is_absent_not_an_error(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_ABSENT, server_config_load("/tmp/does_not_exist_server_conf_xyz", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_nb_threads, "%d");
    PASS();
}

TEST load_null_path_is_absent(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_ABSENT, server_config_load(NULL, &cfg), "%d");
    PASS();
}

TEST load_valid_file_sets_all_keys(void)
{
    char path[] = "/tmp/etii_server_config_valid_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    ASSERT(f != NULL);
    fputs("nb_threads        = 40\n", f);
    fputs("parts_file        = data/pieces.csv\n", f);
    fputs("expand_level      = 3\n", f);
    fputs("expand_max_stock  = 5000\n", f);
    fputs("expand_max_levels = 2\n", f);
    fputs("http_port         = 8080\n", f);
    fputs("http_token_file   = ./token\n", f);
    fputs("stock_files       = 20\n", f);
    fputs("stock_max_ram     = 512\n", f);
    fputs("stock_spill_dir   = /var/spill\n", f);
    fputs("rebalance_budget  = 2000\n", f);
    fputs("tcp_timeout       = 30\n", f);
    fputs("auto_roles        = 1\n", f);
    fputs("stop_on_solution  = 1\n", f);
    fputs("headless          = 1\n", f);
    fputs("sort_enabled      = 1\n", f);
    fputs("sort_interval     = 90\n", f);
    fputs("sort_direction    = desc\n", f);
    fputs("sort_lock_attempts = 10\n", f);
    fclose(f);

    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LOADED, server_config_load(path, &cfg), "%d");

    ASSERT_EQ_FMT(40, cfg.nb_threads, "%d");
    ASSERT_STR_EQ("data/pieces.csv", cfg.parts_file);
    ASSERT_EQ_FMT(3, cfg.expand_level, "%d");
    ASSERT_EQ_FMT(5000, cfg.expand_max_stock, "%d");
    ASSERT_EQ_FMT(2, cfg.expand_max_levels, "%d");
    ASSERT_EQ_FMT(8080, cfg.http_port, "%d");
    ASSERT_STR_EQ("./token", cfg.http_token_file);
    ASSERT_EQ_FMT(20, cfg.stock_files, "%d");
    ASSERT_EQ_FMT(512, cfg.stock_max_ram, "%d");
    ASSERT_STR_EQ("/var/spill", cfg.stock_spill_dir);
    ASSERT_EQ_FMT(2000, cfg.rebalance_budget, "%d");
    ASSERT_EQ_FMT(30, cfg.tcp_timeout, "%d");
    ASSERT_EQ_FMT(1, cfg.auto_roles, "%d");
    ASSERT_EQ_FMT(1, cfg.stop_on_solution, "%d");
    ASSERT_EQ_FMT(1, cfg.headless, "%d");
    ASSERT_EQ_FMT(1, cfg.sort_enabled, "%d");
    ASSERT_EQ_FMT(90, cfg.sort_interval, "%d");
    ASSERT_EQ_FMT(SORT_DIRECTION_DESC, cfg.sort_direction, "%d");
    ASSERT_EQ_FMT(10, cfg.sort_lock_attempts, "%d");

    server_config_free(&cfg);
    unlink(path);
    PASS();
}

TEST load_tolerates_invalid_lines_and_keeps_parsing(void)
{
    char path[] = "/tmp/etii_server_config_tolerant_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    ASSERT(f != NULL);
    fputs("bogus_key = 1\n", f);
    fputs("nb_threads = -3\n", f);
    fputs("# un commentaire\n", f);
    fputs("\n", f);
    fputs("nb_threads = 6\n", f);
    fclose(f);

    server_config_t cfg;
    server_config_init(&cfg);
    ASSERT_EQ_FMT(SERVER_CONFIG_LOADED, server_config_load(path, &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_nb_threads, "%d");
    ASSERT_EQ_FMT(6, cfg.nb_threads, "%d");

    server_config_free(&cfg);
    unlink(path);
    PASS();
}

/* ------------------------------ server_config_format ------------------------ */

TEST format_empty_config_produces_empty_string(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    char buf[256];
    ASSERT_EQ_FMT(0, server_config_format(&cfg, buf, sizeof(buf)), "%d");
    ASSERT_STR_EQ("", buf);
    PASS();
}

TEST format_includes_only_present_keys(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_nb_threads = 1;
    cfg.nb_threads = 40;
    cfg.has_tcp_timeout = 1;
    cfg.tcp_timeout = 30;

    char buf[256];
    int n = server_config_format(&cfg, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(strstr(buf, "nb_threads") != NULL);
    ASSERT(strstr(buf, "tcp_timeout") != NULL);
    ASSERT(strstr(buf, "http_port") == NULL);
    ASSERT(strstr(buf, "parts_file") == NULL);
    PASS();
}

TEST format_includes_sort_direction_as_text(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_sort_enabled = 1;
    cfg.sort_enabled = 1;
    cfg.has_sort_interval = 1;
    cfg.sort_interval = 90;
    cfg.has_sort_direction = 1;
    cfg.sort_direction = SORT_DIRECTION_DESC;
    cfg.has_sort_lock_attempts = 1;
    cfg.sort_lock_attempts = 10;

    char buf[256];
    int n = server_config_format(&cfg, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(strstr(buf, "sort_enabled       = 1") != NULL);
    ASSERT(strstr(buf, "sort_interval      = 90") != NULL);
    ASSERT(strstr(buf, "sort_direction     = desc") != NULL);
    ASSERT(strstr(buf, "sort_lock_attempts = 10") != NULL);
    PASS();
}

TEST format_truncates_safely_on_small_buffer(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_nb_threads = 1;
    cfg.nb_threads = 40;
    cfg.has_parts_file = 1;
    cfg.parts_file = strdup("data/pieces.csv");
    cfg.has_stock_spill_dir = 1;
    cfg.stock_spill_dir = strdup("/var/spill");

    char buf[8];
    int n = server_config_format(&cfg, buf, sizeof(buf));
    ASSERT_EQ_FMT(-1, n, "%d");
    ASSERT_EQ_FMT('\0', buf[sizeof(buf) - 1], "%d");

    server_config_free(&cfg);
    PASS();
}

/* ------------------------------- server_config_save ------------------------- */

TEST save_load_round_trip_preserves_values(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_nb_threads = 1;
    cfg.nb_threads = 24;
    cfg.has_http_port = 1;
    cfg.http_port = 9090;
    cfg.has_stock_spill_dir = 1;
    cfg.stock_spill_dir = strdup("/data/spill");

    char path[] = "/tmp/etii_server_config_save_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, server_config_save(path, &cfg), "%d");
    server_config_free(&cfg);

    server_config_t loaded;
    server_config_init(&loaded);
    ASSERT_EQ_FMT(SERVER_CONFIG_LOADED, server_config_load(path, &loaded), "%d");
    ASSERT_EQ_FMT(24, loaded.nb_threads, "%d");
    ASSERT_EQ_FMT(9090, loaded.http_port, "%d");
    ASSERT_STR_EQ("/data/spill", loaded.stock_spill_dir);

    server_config_free(&loaded);
    unlink(path);
    PASS();
}

TEST save_does_not_leave_tmp_file_behind(void)
{
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_nb_threads = 1;
    cfg.nb_threads = 10;

    char path[] = "/tmp/etii_server_config_notmp_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, server_config_save(path, &cfg), "%d");
    server_config_free(&cfg);

    char tmp_path[sizeof(path) + 4];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    ASSERT(access(tmp_path, F_OK) != 0);

    unlink(path);
    PASS();
}

/* ---------------------------- server_config_apply_* ------------------------- */

/* HTTP_PORT/stock_files_requested/stock_max_ram_mb/auto_roles_requested sont
   des globales partagées avec le reste de la suite (etii_server_suite
   notamment) -- restaurées à leur défaut après coup pour ne pas polluer les
   tests suivants. */
TEST apply_pre_dispatch_uses_file_value_when_global_is_default(void)
{
    HTTP_PORT = 0;
    stock_files_requested = 0;
    stock_max_ram_mb = 0;
    auto_roles_requested = 0;

    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_http_port = 1;
    cfg.http_port = 8080;
    cfg.has_stock_files = 1;
    cfg.stock_files = 20;
    cfg.has_stock_max_ram = 1;
    cfg.stock_max_ram = 512;
    cfg.has_auto_roles = 1;
    cfg.auto_roles = 1;

    server_config_apply_pre_dispatch(&cfg);

    ASSERT_EQ_FMT(8080, HTTP_PORT, "%d");
    ASSERT_EQ_FMT(20, stock_files_requested, "%d");
    ASSERT_EQ_FMT(512, stock_max_ram_mb, "%d");
    ASSERT_EQ_FMT(1, auto_roles_requested, "%d");

    HTTP_PORT = 0;
    stock_files_requested = 0;
    stock_max_ram_mb = 0;
    auto_roles_requested = 0;
    PASS();
}

TEST apply_pre_dispatch_leaves_cli_value_untouched_when_already_provided(void)
{
    HTTP_PORT = 1234;
    stock_files_requested = 5;

    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_http_port = 1;
    cfg.http_port = 8080;
    cfg.has_stock_files = 1;
    cfg.stock_files = 20;

    server_config_apply_pre_dispatch(&cfg);

    ASSERT_EQ_FMT(1234, HTTP_PORT, "%d");
    ASSERT_EQ_FMT(5, stock_files_requested, "%d");

    HTTP_PORT = 0;
    stock_files_requested = 0;
    PASS();
}

TEST apply_pre_dispatch_expand_max_stock_uses_file_value_only_at_default(void)
{
    expand_max_stock = EXPAND_MAX_STOCK;

    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_expand_max_stock = 1;
    cfg.expand_max_stock = 5000;

    server_config_apply_pre_dispatch(&cfg);
    ASSERT_EQ_FMT(5000, expand_max_stock, "%d");

    /* Une valeur CLI déjà différente du défaut n'est jamais écrasée. */
    expand_max_stock = 777;
    server_config_apply_pre_dispatch(&cfg);
    ASSERT_EQ_FMT(777, expand_max_stock, "%d");

    expand_max_stock = EXPAND_MAX_STOCK;
    PASS();
}

/* server_sort_enabled/server_sort_interval/server_sort_direction sont des
   globales partagées avec le reste de la suite -- restaurées à leur défaut
   après coup pour ne pas polluer les tests suivants. */
TEST apply_pre_dispatch_sort_options_use_file_value_when_global_is_default(void)
{
    server_sort_enabled = 0;
    server_sort_interval = SORT_PERIODIC_INTERVAL_DEFAULT;
    server_sort_direction = SORT_DIRECTION_ASC;
    server_sort_lock_attempts = SORT_LOCK_ATTEMPTS_DEFAULT;

    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_sort_enabled = 1;
    cfg.sort_enabled = 1;
    cfg.has_sort_interval = 1;
    cfg.sort_interval = 120;
    cfg.has_sort_direction = 1;
    cfg.sort_direction = SORT_DIRECTION_DESC;
    cfg.has_sort_lock_attempts = 1;
    cfg.sort_lock_attempts = 10;

    server_config_apply_pre_dispatch(&cfg);

    ASSERT_EQ_FMT(1, server_sort_enabled, "%d");
    ASSERT_EQ_FMT(120, server_sort_interval, "%d");
    ASSERT_EQ_FMT(SORT_DIRECTION_DESC, server_sort_direction, "%d");
    ASSERT_EQ_FMT(10, server_sort_lock_attempts, "%d");

    server_sort_enabled = 0;
    server_sort_interval = SORT_PERIODIC_INTERVAL_DEFAULT;
    server_sort_direction = SORT_DIRECTION_ASC;
    server_sort_lock_attempts = SORT_LOCK_ATTEMPTS_DEFAULT;
    PASS();
}

TEST apply_pre_dispatch_sort_interval_leaves_cli_value_untouched_when_already_provided(void)
{
    server_sort_interval = 45;

    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_sort_interval = 1;
    cfg.sort_interval = 120;

    server_config_apply_pre_dispatch(&cfg);
    ASSERT_EQ_FMT(45, server_sort_interval, "%d");

    server_sort_interval = SORT_PERIODIC_INTERVAL_DEFAULT;
    PASS();
}

TEST apply_to_globals_uses_file_nb_threads_when_cli_did_not_provide_it(void)
{
    NB_THREADS = 80;
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_nb_threads = 1;
    cfg.nb_threads = 40;

    server_config_apply_to_globals(&cfg, /*cli_gave_nb_threads=*/0, /*cli_gave_parts_file=*/1);
    ASSERT_EQ_FMT(40, NB_THREADS, "%d");
    PASS();
}

TEST apply_to_globals_leaves_cli_nb_threads_untouched_when_already_provided(void)
{
    NB_THREADS = 12;
    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_nb_threads = 1;
    cfg.nb_threads = 40;

    server_config_apply_to_globals(&cfg, /*cli_gave_nb_threads=*/1, /*cli_gave_parts_file=*/1);
    ASSERT_EQ_FMT(12, NB_THREADS, "%d");
    PASS();
}

/* parts_files est un global partagé avec le reste de la suite (etii_client_suite
   notamment) -- restaurer sa valeur d'origine après coup pour ne pas polluer
   les tests suivants. */
TEST apply_to_globals_uses_file_parts_file_when_cli_did_not_provide_it(void)
{
    char *original = parts_files;

    server_config_t cfg;
    server_config_init(&cfg);
    cfg.has_parts_file = 1;
    cfg.parts_file = strdup("from-file.csv");

    server_config_apply_to_globals(&cfg, /*cli_gave_nb_threads=*/1, /*cli_gave_parts_file=*/0);
    ASSERT_STR_EQ("from-file.csv", parts_files);

    free(parts_files);
    parts_files = original;
    PASS();
}

SUITE(server_config_suite)
{
    RUN_TEST(parse_line_blank_and_comment_are_ignored);
    RUN_TEST(parse_line_without_equals_is_unknown);
    RUN_TEST(parse_line_unknown_key_is_reported);
    RUN_TEST(parse_line_nb_threads_valid);
    RUN_TEST(parse_line_nb_threads_zero_or_negative_is_invalid);
    RUN_TEST(parse_line_nb_threads_non_numeric_is_invalid);
    RUN_TEST(parse_line_parts_file_valid);
    RUN_TEST(parse_line_parts_file_empty_is_invalid);
    RUN_TEST(parse_line_expand_level_zero_is_valid);
    RUN_TEST(parse_line_expand_level_negative_is_invalid);
    RUN_TEST(parse_line_http_port_in_range_is_valid);
    RUN_TEST(parse_line_http_port_out_of_range_is_invalid);
    RUN_TEST(parse_line_http_token_file_valid);
    RUN_TEST(parse_line_stock_spill_dir_valid);
    RUN_TEST(parse_line_boolean_keys_accept_zero_and_one);
    RUN_TEST(parse_line_boolean_keys_reject_other_values);
    RUN_TEST(parse_line_sort_enabled_accepts_zero_and_one);
    RUN_TEST(parse_line_sort_enabled_rejects_other_values);
    RUN_TEST(parse_line_sort_interval_valid);
    RUN_TEST(parse_line_sort_interval_zero_or_negative_is_invalid);
    RUN_TEST(parse_line_sort_direction_accepts_asc_and_desc);
    RUN_TEST(parse_line_sort_direction_rejects_other_values);
    RUN_TEST(parse_line_sort_lock_attempts_valid);
    RUN_TEST(parse_line_sort_lock_attempts_zero_or_negative_is_invalid);

    RUN_TEST(load_missing_file_is_absent_not_an_error);
    RUN_TEST(load_null_path_is_absent);
    RUN_TEST(load_valid_file_sets_all_keys);
    RUN_TEST(load_tolerates_invalid_lines_and_keeps_parsing);

    RUN_TEST(format_empty_config_produces_empty_string);
    RUN_TEST(format_includes_only_present_keys);
    RUN_TEST(format_includes_sort_direction_as_text);
    RUN_TEST(format_truncates_safely_on_small_buffer);

    RUN_TEST(save_load_round_trip_preserves_values);
    RUN_TEST(save_does_not_leave_tmp_file_behind);

    RUN_TEST(apply_pre_dispatch_uses_file_value_when_global_is_default);
    RUN_TEST(apply_pre_dispatch_leaves_cli_value_untouched_when_already_provided);
    RUN_TEST(apply_pre_dispatch_expand_max_stock_uses_file_value_only_at_default);
    RUN_TEST(apply_pre_dispatch_sort_options_use_file_value_when_global_is_default);
    RUN_TEST(apply_pre_dispatch_sort_interval_leaves_cli_value_untouched_when_already_provided);
    RUN_TEST(apply_to_globals_uses_file_nb_threads_when_cli_did_not_provide_it);
    RUN_TEST(apply_to_globals_leaves_cli_nb_threads_untouched_when_already_provided);
    RUN_TEST(apply_to_globals_uses_file_parts_file_when_cli_did_not_provide_it);
}
