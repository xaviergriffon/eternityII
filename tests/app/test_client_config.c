/*
 * Tests unitaires de client_config.c :
 * parsing/écriture de la configuration client clé=valeur, et son application
 * aux globales avec la priorité CLI > fichier > défauts.
 */
#include "greatest.h"
#include "app/client_config.h"
#include "app/static_variables.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ---------------------------- client_config_parse_line -------------------- */

TEST parse_line_blank_and_comment_are_ignored(void)
{
    client_config_t cfg;
    client_config_init(&cfg);

    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_IGNORED, client_config_parse_line("\n", &cfg), "%d");
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_IGNORED, client_config_parse_line("", &cfg), "%d");
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_IGNORED, client_config_parse_line("   \n", &cfg), "%d");
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_IGNORED, client_config_parse_line("# commentaire\n", &cfg), "%d");
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_IGNORED, client_config_parse_line("   # commentaire\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_nb_forks, "%d");
    PASS();
}

TEST parse_line_without_equals_is_unknown(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_UNKNOWN_KEY, client_config_parse_line("nb_forks 4\n", &cfg), "%d");
    PASS();
}

TEST parse_line_unknown_key_is_reported(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_UNKNOWN_KEY, client_config_parse_line("bogus_key = 1\n", &cfg), "%d");
    PASS();
}

TEST parse_line_nb_forks_valid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET, client_config_parse_line("nb_forks = 4\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_nb_forks, "%d");
    ASSERT_EQ_FMT(4, cfg.nb_forks, "%d");
    PASS();
}

/* Espaces autour du '=', et commentaire en fin de ligne après la valeur. */
TEST parse_line_nb_forks_with_spacing_and_inline_comment(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET,
                  client_config_parse_line("  nb_forks   =   8   # redémarrage requis\n", &cfg), "%d");
    ASSERT_EQ_FMT(8, cfg.nb_forks, "%d");
    PASS();
}

TEST parse_line_nb_forks_zero_or_negative_is_invalid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE, client_config_parse_line("nb_forks = 0\n", &cfg), "%d");
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE, client_config_parse_line("nb_forks = -1\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_nb_forks, "%d");
    PASS();
}

TEST parse_line_nb_forks_non_numeric_is_invalid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE, client_config_parse_line("nb_forks = abc\n", &cfg), "%d");
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE, client_config_parse_line("nb_forks = 4abc\n", &cfg), "%d");
    PASS();
}

TEST parse_line_server_host_valid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET,
                  client_config_parse_line("server_host = 192.168.1.10\n", &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_server_host, "%d");
    ASSERT_STR_EQ("192.168.1.10", cfg.server_host);
    client_config_free(&cfg);
    PASS();
}

TEST parse_line_server_host_empty_is_invalid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE, client_config_parse_line("server_host =\n", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_server_host, "%d");
    PASS();
}

/* La dernière occurrence d'une clé chaîne l'emporte, sans fuite (l'ancienne
   copie est libérée avant d'être remplacée). */
TEST parse_line_server_host_last_occurrence_wins(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    client_config_parse_line("server_host = first.example\n", &cfg);
    client_config_parse_line("server_host = second.example\n", &cfg);
    ASSERT_STR_EQ("second.example", cfg.server_host);
    client_config_free(&cfg);
    PASS();
}

TEST parse_line_parts_file_valid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET,
                  client_config_parse_line("parts_file = data/pieces.csv\n", &cfg), "%d");
    ASSERT_STR_EQ("data/pieces.csv", cfg.parts_file);
    client_config_free(&cfg);
    PASS();
}

TEST parse_line_max_stock_by_thread_valid_and_invalid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET,
                  client_config_parse_line("max_stock_by_thread = 200\n", &cfg), "%d");
    ASSERT_EQ_FMT(200, cfg.max_stock_by_thread, "%d");

    client_config_t cfg2;
    client_config_init(&cfg2);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE,
                  client_config_parse_line("max_stock_by_thread = -5\n", &cfg2), "%d");
    PASS();
}

TEST parse_line_limit_valid_and_invalid(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET, client_config_parse_line("limit = 5000\n", &cfg), "%d");
    ASSERT_EQ_FMT(5000, (int)cfg.limit, "%d");

    client_config_t cfg2;
    client_config_init(&cfg2);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_INVALID_VALUE, client_config_parse_line("limit = -1\n", &cfg2), "%d");
    PASS();
}

/* pruner_batch est borné via pruner_batch_clamp, jamais rejeté une fois numérique. */
TEST parse_line_pruner_batch_is_clamped_not_rejected(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET, client_config_parse_line("pruner_batch = 999999999\n", &cfg), "%d");
    ASSERT_EQ_FMT(PRUNER_BATCH_MAX, cfg.pruner_batch, "%d");

    client_config_t cfg2;
    client_config_init(&cfg2);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LINE_SET, client_config_parse_line("pruner_batch = 0\n", &cfg2), "%d");
    ASSERT_EQ_FMT(1, cfg2.pruner_batch, "%d");
    PASS();
}

/* ------------------------------- client_config_load ------------------------ */

TEST load_missing_file_is_absent_not_an_error(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_ABSENT,
                  client_config_load("/tmp/etii_no_such_client_config_zzz_999", &cfg), "%d");
    ASSERT_EQ_FMT(0, cfg.has_nb_forks, "%d");
    PASS();
}

TEST load_null_path_is_absent(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_ABSENT, client_config_load(NULL, &cfg), "%d");
    PASS();
}

TEST load_valid_file_sets_all_keys(void)
{
    char path[] = "/tmp/etii_client_config_load_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    ASSERT(f != NULL);
    fputs("nb_forks            = 4\n", f);
    fputs("server_host         = 192.168.1.10\n", f);
    fputs("parts_file          = data/pieces.csv\n", f);
    fputs("max_stock_by_thread = 200\n", f);
    fputs("limit               = 0\n", f);
    fputs("pruner_batch        = 500\n", f);
    fclose(f);

    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LOADED, client_config_load(path, &cfg), "%d");

    ASSERT_EQ_FMT(1, cfg.has_nb_forks, "%d");
    ASSERT_EQ_FMT(4, cfg.nb_forks, "%d");
    ASSERT_STR_EQ("192.168.1.10", cfg.server_host);
    ASSERT_STR_EQ("data/pieces.csv", cfg.parts_file);
    ASSERT_EQ_FMT(200, cfg.max_stock_by_thread, "%d");
    ASSERT_EQ_FMT(1, cfg.has_limit, "%d");
    ASSERT_EQ_FMT(0, (int)cfg.limit, "%d");
    ASSERT_EQ_FMT(500, cfg.pruner_batch, "%d");

    client_config_free(&cfg);
    unlink(path);
    PASS();
}

/* Une ligne invalide/inconnue est ignorée, mais le chargement continue et les
   lignes valides restantes sont bien appliquées. */
TEST load_tolerates_invalid_lines_and_keeps_parsing(void)
{
    char path[] = "/tmp/etii_client_config_tolerant_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    FILE *f = fdopen(fd, "w");
    ASSERT(f != NULL);
    fputs("bogus_key = 1\n", f);
    fputs("nb_forks = -3\n", f);
    fputs("# un commentaire\n", f);
    fputs("\n", f);
    fputs("nb_forks = 6\n", f);
    fclose(f);

    client_config_t cfg;
    client_config_init(&cfg);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LOADED, client_config_load(path, &cfg), "%d");
    ASSERT_EQ_FMT(1, cfg.has_nb_forks, "%d");
    ASSERT_EQ_FMT(6, cfg.nb_forks, "%d");

    client_config_free(&cfg);
    unlink(path);
    PASS();
}

/* ------------------------------ client_config_format ------------------------ */

TEST format_empty_config_produces_empty_string(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    char buf[256];
    ASSERT_EQ_FMT(0, client_config_format(&cfg, buf, sizeof(buf)), "%d");
    ASSERT_STR_EQ("", buf);
    PASS();
}

TEST format_includes_only_present_keys(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_nb_forks = 1;
    cfg.nb_forks = 4;
    cfg.has_limit = 1;
    cfg.limit = 100;

    char buf[256];
    int n = client_config_format(&cfg, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(strstr(buf, "nb_forks") != NULL);
    ASSERT(strstr(buf, "limit") != NULL);
    ASSERT(strstr(buf, "server_host") == NULL);
    ASSERT(strstr(buf, "parts_file") == NULL);
    PASS();
}

TEST format_truncates_safely_on_small_buffer(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_nb_forks = 1;
    cfg.nb_forks = 4;
    cfg.has_server_host = 1;
    cfg.server_host = strdup("a.example.com");
    cfg.has_parts_file = 1;
    cfg.parts_file = strdup("data/pieces.csv");

    char buf[8];
    int n = client_config_format(&cfg, buf, sizeof(buf));
    ASSERT_EQ_FMT(-1, n, "%d");
    /* Toujours terminé par '\0' malgré la troncature. */
    ASSERT_EQ_FMT('\0', buf[sizeof(buf) - 1], "%d");

    client_config_free(&cfg);
    PASS();
}

/* ------------------------------- client_config_save ------------------------- */

TEST save_load_round_trip_preserves_values(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_nb_forks = 1;
    cfg.nb_forks = 3;
    cfg.has_server_host = 1;
    cfg.server_host = strdup("etii-server");
    cfg.has_pruner_batch = 1;
    cfg.pruner_batch = 250;

    char path[] = "/tmp/etii_client_config_save_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, client_config_save(path, &cfg), "%d");
    client_config_free(&cfg);

    client_config_t loaded;
    client_config_init(&loaded);
    ASSERT_EQ_FMT(CLIENT_CONFIG_LOADED, client_config_load(path, &loaded), "%d");
    ASSERT_EQ_FMT(3, loaded.nb_forks, "%d");
    ASSERT_STR_EQ("etii-server", loaded.server_host);
    ASSERT_EQ_FMT(250, loaded.pruner_batch, "%d");

    client_config_free(&loaded);
    unlink(path);
    PASS();
}

/* Le fichier temporaire ".tmp" ne doit jamais rester après un save réussi. */
TEST save_does_not_leave_tmp_file_behind(void)
{
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_nb_forks = 1;
    cfg.nb_forks = 1;

    char path[] = "/tmp/etii_client_config_notmp_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, client_config_save(path, &cfg), "%d");

    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "r");
    ASSERT(f == NULL);

    unlink(path);
    PASS();
}

/* ------------------------------ apply_to_globals ---------------------------- */

TEST apply_uses_file_value_when_cli_did_not_provide_it(void)
{
    NB_THREADS = 1;
    pruner_mode = 0;
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_nb_forks = 1;
    cfg.nb_forks = 7;

    /* argc == 2 : "prog client" seulement, aucun nb_forks positionnel fourni. */
    client_config_apply_to_globals(&cfg, 2, NULL);
    ASSERT_EQ_FMT(7, NB_THREADS, "%d");
    PASS();
}

TEST apply_leaves_cli_value_untouched_when_already_provided(void)
{
    NB_THREADS = 42;
    pruner_mode = 0;
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_nb_forks = 1;
    cfg.nb_forks = 7;

    /* argc >= 4 : "prog client host 42" -> nb_forks déjà fourni par la CLI. */
    client_config_apply_to_globals(&cfg, 4, NULL);
    ASSERT_EQ_FMT(42, NB_THREADS, "%d");
    PASS();
}

TEST apply_sets_server_host_only_when_cli_did_not_provide_it(void)
{
    pruner_mode = 0;
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_server_host = 1;
    cfg.server_host = strdup("from-file.example");

    const char *server_host = "localhost";
    /* argc == 2 : aucun hôte positionnel fourni par la CLI. */
    client_config_apply_to_globals(&cfg, 2, &server_host);
    ASSERT_STR_EQ("from-file.example", server_host);

    const char *server_host2 = "cli-host.example";
    /* argc >= 3 : l'hôte a été fourni par la CLI, le fichier ne doit rien changer. */
    client_config_apply_to_globals(&cfg, 3, &server_host2);
    ASSERT_STR_EQ("cli-host.example", server_host2);

    client_config_free(&cfg);
    PASS();
}

TEST apply_parts_file_threshold_differs_for_pruner_mode(void)
{
    char *saved_parts_files = parts_files;

    pruner_mode = 1;
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_parts_file = 1;
    cfg.parts_file = strdup("from-file.csv");

    /* Pruner : argc >= 5 signifie que la CLI a déjà fourni parts_file. */
    parts_files = (char *)"unchanged.csv";
    client_config_apply_to_globals(&cfg, 5, NULL);
    ASSERT_STR_EQ("unchanged.csv", parts_files);

    parts_files = (char *)"unchanged.csv";
    client_config_apply_to_globals(&cfg, 4, NULL);
    ASSERT_STR_EQ("from-file.csv", parts_files);

    client_config_free(&cfg);
    pruner_mode = 0;
    parts_files = saved_parts_files;
    PASS();
}

TEST apply_limit_always_applies_when_present(void)
{
    max_search_by_sec = 999;
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_limit = 1;
    cfg.limit = 0;

    client_config_apply_to_globals(&cfg, 10, NULL);
    ASSERT_EQ_FMT(0, (int)max_search_by_sec, "%d");
    PASS();
}

TEST apply_pruner_batch_only_applies_in_pruner_mode(void)
{
    pruner_mode = 0;
    pruner_batch_size = 111;
    client_config_t cfg;
    client_config_init(&cfg);
    cfg.has_pruner_batch = 1;
    cfg.pruner_batch = 222;

    client_config_apply_to_globals(&cfg, 2, NULL);
    ASSERT_EQ_FMT(111, pruner_batch_size, "%d");

    pruner_mode = 1;
    client_config_apply_to_globals(&cfg, 2, NULL);
    ASSERT_EQ_FMT(222, pruner_batch_size, "%d");

    pruner_mode = 0;
    PASS();
}

/* ---------------------------- capture_effective ----------------------------- */

TEST capture_effective_reads_current_globals(void)
{
    NB_THREADS = 5;
    char *saved_parts_files = parts_files;
    parts_files = (char *)"effective.csv";
    max_stock_by_thread = 321;
    max_search_by_sec = 654;
    pruner_batch_size = 789;

    client_config_t cfg;
    client_config_capture_effective(&cfg, "srv.example");

    ASSERT_EQ_FMT(1, cfg.has_nb_forks, "%d");
    ASSERT_EQ_FMT(5, cfg.nb_forks, "%d");
    ASSERT_EQ_FMT(1, cfg.has_server_host, "%d");
    ASSERT_STR_EQ("srv.example", cfg.server_host);
    ASSERT_STR_EQ("effective.csv", cfg.parts_file);
    ASSERT_EQ_FMT(321, cfg.max_stock_by_thread, "%d");
    ASSERT_EQ_FMT(654, (int)cfg.limit, "%d");
    ASSERT_EQ_FMT(789, cfg.pruner_batch, "%d");

    client_config_free(&cfg);
    parts_files = saved_parts_files;
    PASS();
}

/* NULL/vide : server_host absent du résultat (mode serveur/test). */
TEST capture_effective_omits_server_host_when_absent(void)
{
    client_config_t cfg;
    client_config_capture_effective(&cfg, NULL);
    ASSERT_EQ_FMT(0, cfg.has_server_host, "%d");

    client_config_t cfg2;
    client_config_capture_effective(&cfg2, "");
    ASSERT_EQ_FMT(0, cfg2.has_server_host, "%d");
    PASS();
}

/* ------------------------------- diff ----------------------------------- */

/* Rien de stagé : toujours HOT_ONLY, même avec un current bien rempli. */
TEST diff_nothing_staged_is_hot_only(void)
{
    client_config_t current;
    client_config_init(&current);
    current.has_nb_forks = 1;
    current.nb_forks = 4;

    client_config_t staged;
    client_config_init(&staged);

    ASSERT_EQ_FMT((int)CLIENT_CONFIG_DIFF_HOT_ONLY, (int)client_config_diff(&current, &staged), "%d");
    PASS();
}

/* Seules des clés à chaud stagées (peu importe si elles diffèrent du
   current) : HOT_ONLY. */
TEST diff_only_hot_keys_staged_is_hot_only(void)
{
    client_config_t current;
    client_config_init(&current);
    current.has_max_stock_by_thread = 1;
    current.max_stock_by_thread = 100;

    client_config_t staged;
    client_config_init(&staged);
    staged.has_max_stock_by_thread = 1;
    staged.max_stock_by_thread = 200;
    staged.has_limit = 1;
    staged.limit = 5000;
    staged.has_pruner_batch = 1;
    staged.pruner_batch = 64;

    ASSERT_EQ_FMT((int)CLIENT_CONFIG_DIFF_HOT_ONLY, (int)client_config_diff(&current, &staged), "%d");
    PASS();
}

/* nb_forks stagé différent du current : NEEDS_RESTART. */
TEST diff_nb_forks_changed_needs_restart(void)
{
    client_config_t current;
    client_config_init(&current);
    current.has_nb_forks = 1;
    current.nb_forks = 4;

    client_config_t staged;
    client_config_init(&staged);
    staged.has_nb_forks = 1;
    staged.nb_forks = 8;

    ASSERT_EQ_FMT((int)CLIENT_CONFIG_DIFF_NEEDS_RESTART, (int)client_config_diff(&current, &staged), "%d");
    PASS();
}

/* nb_forks stagé mais IDENTIQUE au current : HOT_ONLY — une valeur stagée
   qui ne change rien ne doit pas coûter un redémarrage à l'opérateur, cf. la
   doc de client_config_diff (client_config.h). */
TEST diff_nb_forks_staged_same_value_is_hot_only(void)
{
    client_config_t current;
    client_config_init(&current);
    current.has_nb_forks = 1;
    current.nb_forks = 4;

    client_config_t staged;
    client_config_init(&staged);
    staged.has_nb_forks = 1;
    staged.nb_forks = 4;

    ASSERT_EQ_FMT((int)CLIENT_CONFIG_DIFF_HOT_ONLY, (int)client_config_diff(&current, &staged), "%d");
    PASS();
}

/* server_host stagé et différent (chaînes) : NEEDS_RESTART. */
TEST diff_server_host_changed_needs_restart(void)
{
    client_config_t current;
    client_config_init(&current);
    current.has_server_host = 1;
    current.server_host = strdup("old.example");

    client_config_t staged;
    client_config_init(&staged);
    staged.has_server_host = 1;
    staged.server_host = strdup("new.example");

    ASSERT_EQ_FMT((int)CLIENT_CONFIG_DIFF_NEEDS_RESTART, (int)client_config_diff(&current, &staged), "%d");

    client_config_free(&current);
    client_config_free(&staged);
    PASS();
}

/* parts_file stagé, current absent (pas encore capturé) : NEEDS_RESTART. */
TEST diff_parts_file_staged_with_no_current_needs_restart(void)
{
    client_config_t current;
    client_config_init(&current);

    client_config_t staged;
    client_config_init(&staged);
    staged.has_parts_file = 1;
    staged.parts_file = strdup("data/pieces16.csv");

    ASSERT_EQ_FMT((int)CLIENT_CONFIG_DIFF_NEEDS_RESTART, (int)client_config_diff(&current, &staged), "%d");

    client_config_free(&staged);
    PASS();
}

SUITE(client_config_suite)
{
    RUN_TEST(parse_line_blank_and_comment_are_ignored);
    RUN_TEST(parse_line_without_equals_is_unknown);
    RUN_TEST(parse_line_unknown_key_is_reported);
    RUN_TEST(parse_line_nb_forks_valid);
    RUN_TEST(parse_line_nb_forks_with_spacing_and_inline_comment);
    RUN_TEST(parse_line_nb_forks_zero_or_negative_is_invalid);
    RUN_TEST(parse_line_nb_forks_non_numeric_is_invalid);
    RUN_TEST(parse_line_server_host_valid);
    RUN_TEST(parse_line_server_host_empty_is_invalid);
    RUN_TEST(parse_line_server_host_last_occurrence_wins);
    RUN_TEST(parse_line_parts_file_valid);
    RUN_TEST(parse_line_max_stock_by_thread_valid_and_invalid);
    RUN_TEST(parse_line_limit_valid_and_invalid);
    RUN_TEST(parse_line_pruner_batch_is_clamped_not_rejected);

    RUN_TEST(load_missing_file_is_absent_not_an_error);
    RUN_TEST(load_null_path_is_absent);
    RUN_TEST(load_valid_file_sets_all_keys);
    RUN_TEST(load_tolerates_invalid_lines_and_keeps_parsing);

    RUN_TEST(format_empty_config_produces_empty_string);
    RUN_TEST(format_includes_only_present_keys);
    RUN_TEST(format_truncates_safely_on_small_buffer);

    RUN_TEST(save_load_round_trip_preserves_values);
    RUN_TEST(save_does_not_leave_tmp_file_behind);

    RUN_TEST(apply_uses_file_value_when_cli_did_not_provide_it);
    RUN_TEST(apply_leaves_cli_value_untouched_when_already_provided);
    RUN_TEST(apply_sets_server_host_only_when_cli_did_not_provide_it);
    RUN_TEST(apply_parts_file_threshold_differs_for_pruner_mode);
    RUN_TEST(apply_limit_always_applies_when_present);
    RUN_TEST(apply_pruner_batch_only_applies_in_pruner_mode);

    RUN_TEST(capture_effective_reads_current_globals);
    RUN_TEST(capture_effective_omits_server_host_when_absent);

    RUN_TEST(diff_nothing_staged_is_hot_only);
    RUN_TEST(diff_only_hot_keys_staged_is_hot_only);
    RUN_TEST(diff_nb_forks_changed_needs_restart);
    RUN_TEST(diff_nb_forks_staged_same_value_is_hot_only);
    RUN_TEST(diff_server_host_changed_needs_restart);
    RUN_TEST(diff_parts_file_staged_with_no_current_needs_restart);
}
