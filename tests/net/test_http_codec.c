/*
 * Tests unitaires de http_codec.c — parsing HTTP/1.1 minimal, routage,
 * formatage de réponse/JSON. Toutes les fonctions testées ici sont pures
 * (aucun socket, aucune allocation) : pas besoin de socketpair, contrairement
 * à test_control_protocol.c.
 */
#include "greatest.h"
#include "net/http_codec.h"

#include <string.h>

/* ---------- http_request_parse -------------------------------------------- */

TEST http_request_parse_valid_get(void)
{
    char req[] = "GET /api/v1/stats HTTP/1.1\r\nHost: localhost\r\n\r\n";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_OK, r, "%d");
    ASSERT_STR_EQ("GET", out.method);
    ASSERT_STR_EQ("/api/v1/stats", out.path);
    ASSERT_EQ_FMT(0, out.content_length, "%d");
    ASSERT_EQ(NULL, out.body);
    PASS();
}

TEST http_request_parse_valid_post_with_body(void)
{
    const char body[] = "{\"command\":\"pause\"}";
    char req[128];
    snprintf(req, sizeof(req), "POST /api/v1/command HTTP/1.1\r\nContent-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_OK, r, "%d");
    ASSERT_STR_EQ("POST", out.method);
    ASSERT_STR_EQ("/api/v1/command", out.path);
    ASSERT_EQ_FMT((int)strlen(body), out.content_length, "%d");
    ASSERT_EQ_FMT((int)strlen(body), out.body_len, "%d");
    ASSERT(out.body != NULL);
    ASSERT_EQ_FMT(0, memcmp(out.body, body, strlen(body)), "%d");
    PASS();
}

/* En-tête insensible à la casse : "content-length" en minuscules. */
TEST http_request_parse_header_case_insensitive(void)
{
    char req[] = "POST /api/v1/command HTTP/1.1\r\ncontent-length: 2\r\n\r\n{}";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_OK, r, "%d");
    ASSERT_EQ_FMT(2, out.content_length, "%d");
    PASS();
}

/* En-têtes pas encore terminés (pas de \r\n\r\n) : NEED_MORE, pas d'erreur. */
TEST http_request_parse_incomplete_headers_needs_more(void)
{
    char req[] = "GET /api/v1/stats HTTP/1.1\r\nHost: localhost\r\n";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_NEED_MORE, r, "%d");
    PASS();
}

/* Corps annoncé mais pas encore intégralement reçu, sous la limite : NEED_MORE. */
TEST http_request_parse_partial_body_needs_more(void)
{
    char req[] = "POST /api/v1/command HTTP/1.1\r\nContent-Length: 20\r\n\r\n{\"command\":\"pa";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_NEED_MORE, r, "%d");
    PASS();
}

/* Ligne de requête sans espace : impossible à parser -> BAD. */
TEST http_request_parse_garbage_request_line_is_bad(void)
{
    char req[] = "garbage\r\n\r\n";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_BAD, r, "%d");
    PASS();
}

/* Content-Length négatif ("-5") : rejeté par la validation chiffre-par-chiffre -> BAD. */
TEST http_request_parse_negative_content_length_is_bad(void)
{
    char req[] = "POST /x HTTP/1.1\r\nContent-Length: -5\r\n\r\n";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_BAD, r, "%d");
    PASS();
}

/* Content-Length dépassant HTTP_REQUEST_MAX : TOO_LARGE, sans tenter d'allouer/lire. */
TEST http_request_parse_huge_content_length_is_too_large(void)
{
    char req[256];
    snprintf(req, sizeof(req), "POST /x HTTP/1.1\r\nContent-Length: %d\r\n\r\n", HTTP_REQUEST_MAX + 1);
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_TOO_LARGE, r, "%d");
    PASS();
}

/* Requête déjà à la borne HTTP_REQUEST_MAX sans double CRLF trouvé : TOO_LARGE
   plutôt que NEED_MORE indéfiniment. */
TEST http_request_parse_oversized_without_terminator_is_too_large(void)
{
    char req[HTTP_REQUEST_MAX];
    memset(req, 'a', sizeof(req));
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, HTTP_REQUEST_MAX, &out);

    ASSERT_EQ_FMT(HTTP_PARSE_TOO_LARGE, r, "%d");
    PASS();
}

/* len négatif ou pointeurs NULL : BAD, jamais de déréférencement. */
TEST http_request_parse_rejects_invalid_arguments(void)
{
    http_request_t out;
    ASSERT_EQ_FMT(HTTP_PARSE_BAD, http_request_parse(NULL, 10, &out), "%d");
    ASSERT_EQ_FMT(HTTP_PARSE_BAD, http_request_parse("GET / HTTP/1.1\r\n\r\n", 10, NULL), "%d");
    ASSERT_EQ_FMT(HTTP_PARSE_BAD, http_request_parse("x", -1, &out), "%d");
    PASS();
}

/* ---------- http_route_resolve --------------------------------------------- */

TEST http_route_resolve_known_routes(void)
{
    ASSERT_EQ_FMT(HTTP_ROUTE_STATS, http_route_resolve("GET", "/api/v1/stats"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_STATUS, http_route_resolve("GET", "/api/v1/status"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_COMMAND, http_route_resolve("POST", "/api/v1/command"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_CLIENTS, http_route_resolve("GET", "/api/v1/clients"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_CLIENTS_STATS, http_route_resolve("POST", "/api/v1/clients/stats"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_KNOWN_CLIENTS, http_route_resolve("GET", "/api/v1/known-clients"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_STOCK_DISTRIBUTION, http_route_resolve("GET", "/api/v1/stock-distribution"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_COMMANDS, http_route_resolve("GET", "/api/v1/commands"), "%d");
    PASS();
}

TEST http_route_resolve_not_found(void)
{
    ASSERT_EQ_FMT(HTTP_ROUTE_NOT_FOUND, http_route_resolve("GET", "/unknown"), "%d");
    PASS();
}

TEST http_route_resolve_bad_method(void)
{
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("POST", "/api/v1/stats"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("GET", "/api/v1/command"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("POST", "/api/v1/clients"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("GET", "/api/v1/clients/stats"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("POST", "/api/v1/known-clients"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("POST", "/api/v1/stock-distribution"), "%d");
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("POST", "/api/v1/commands"), "%d");
    PASS();
}

/* ---------- http_response_format ------------------------------------------- */

TEST http_response_format_writes_expected_headers_and_body(void)
{
    char buf[256];
    int written = http_response_format(buf, sizeof(buf), 200, "{\"ok\":true}");

    ASSERT(written > 0);
    ASSERT(strstr(buf, "HTTP/1.1 200 OK\r\n") == buf);
    ASSERT(strstr(buf, "Content-Length: 11\r\n") != NULL);
    ASSERT(strstr(buf, "Connection: close\r\n") != NULL);
    ASSERT(strstr(buf, "\r\n\r\n{\"ok\":true}") != NULL);
    PASS();
}

TEST http_response_format_null_body_is_empty(void)
{
    char buf[256];
    int written = http_response_format(buf, sizeof(buf), 404, NULL);

    ASSERT(written > 0);
    ASSERT(strstr(buf, "404 Not Found") != NULL);
    ASSERT(strstr(buf, "Content-Length: 0\r\n") != NULL);
    PASS();
}

TEST http_response_format_buffer_too_small_fails(void)
{
    char buf[8];
    int written = http_response_format(buf, sizeof(buf), 200, "{}");
    ASSERT_EQ_FMT(-1, written, "%d");
    PASS();
}

/* ---------- http_json_extract_string --------------------------------------- */

TEST http_json_extract_string_nominal(void)
{
    char body[] = "{\"command\":\"limit 1000\"}";
    char out[64];
    int n = http_json_extract_string(body, (int32_t)strlen(body), "command", out, sizeof(out));

    ASSERT_EQ_FMT((int)strlen("limit 1000"), n, "%d");
    ASSERT_STR_EQ("limit 1000", out);
    PASS();
}

TEST http_json_extract_string_missing_key(void)
{
    char body[] = "{\"other\":\"value\"}";
    char out[64];
    int n = http_json_extract_string(body, (int32_t)strlen(body), "command", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* Échappement JSON ("\\\"") : rejeté plutôt que mal interprété. */
TEST http_json_extract_string_rejects_escapes(void)
{
    char body[] = "{\"command\":\"pa\\\"use\"}";
    char out[64];
    int n = http_json_extract_string(body, (int32_t)strlen(body), "command", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* Tampon de sortie trop petit pour la valeur -> rejeté (pas de troncature silencieuse). */
TEST http_json_extract_string_output_too_small(void)
{
    char body[] = "{\"command\":\"prunerBatch 12345\"}";
    char out[4];
    int n = http_json_extract_string(body, (int32_t)strlen(body), "command", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* ---------- http_json_format_stats / status --------------------------------- */

TEST http_json_format_stats_golden(void)
{
    http_stats_view_t view;
    memset(&view, 0, sizeof(view));
    view.shots_per_second = 100;
    view.possibility_stock = 10;
    view.checked_stock = 2;
    view.analysed_stock = 1;
    view.max_result = 42;
    view.active_threads = 3;
    view.pruner_checked = 5;
    view.pruner_removed = 4;
    view.stock_spilled_packets = 8;
    view.stock_spill_segments = 2;
    view.stock_adds_last_1m = 150;
    view.stock_adds_last_1h = 900;
    view.stock_adds_last_1d = 12000;
    view.stock_removes_last_1m = 125;
    view.stock_removes_last_1h = 720;
    view.stock_removes_last_1d = 9000;
    view.stock_adds_checked_last_1m = 30;
    view.stock_adds_unchecked_last_1m = 120;
    view.stock_removes_checked_last_1m = 100;
    view.stock_removes_unchecked_last_1m = 25;
    view.server_search_starved = 7;
    view.server_prune_starved = 91;
    view.nb_search_sessions = 4;
    view.nb_prune_sessions = 2;
    view.queue_unchecked[0] = 7;
    view.queue_checked[0] = 1;
    view.queue_analysed[0] = 0;

    char buf[4096];
    int n = http_json_format_stats(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"shots_per_second\":100") != NULL);
    ASSERT(strstr(buf, "\"max_result\":42") != NULL);
    ASSERT(strstr(buf, "\"active_threads\":3") != NULL);
    ASSERT(strstr(buf, "\"stock_spilled_packets\":8") != NULL);
    ASSERT(strstr(buf, "\"stock_spill_segments\":2") != NULL);
    ASSERT(strstr(buf, "\"stock_adds_last_1m\":150") != NULL);
    ASSERT(strstr(buf, "\"stock_adds_last_1h\":900") != NULL);
    ASSERT(strstr(buf, "\"stock_adds_last_1d\":12000") != NULL);
    ASSERT(strstr(buf, "\"stock_removes_last_1m\":125") != NULL);
    ASSERT(strstr(buf, "\"stock_removes_last_1h\":720") != NULL);
    ASSERT(strstr(buf, "\"stock_removes_last_1d\":9000") != NULL);
    ASSERT(strstr(buf, "\"stock_adds_checked_last_1m\":30") != NULL);
    ASSERT(strstr(buf, "\"stock_adds_unchecked_last_1m\":120") != NULL);
    ASSERT(strstr(buf, "\"stock_removes_checked_last_1m\":100") != NULL);
    ASSERT(strstr(buf, "\"stock_removes_unchecked_last_1m\":25") != NULL);
    ASSERT(strstr(buf, "\"server_search_starved\":7") != NULL);
    ASSERT(strstr(buf, "\"server_prune_starved\":91") != NULL);
    ASSERT(strstr(buf, "\"nb_search_sessions\":4") != NULL);
    ASSERT(strstr(buf, "\"nb_prune_sessions\":2") != NULL);
    ASSERT(strstr(buf, "\"queues\":[{\"file\":0,\"unchecked\":7,\"checked\":1,\"analysed\":0}") != NULL);
    ASSERT(strstr(buf, "]}") != NULL); /* tableau bien refermé */

    /* NB_FILE_POSSIBILITY_DEFAULT entrées attendues dans le tableau. */
    int file_count = 0;
    const char *cursor = buf;
    while ((cursor = strstr(cursor, "\"file\":")) != NULL) {
        file_count++;
        cursor += strlen("\"file\":");
    }
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY_DEFAULT, file_count, "%d");
    PASS();
}

TEST http_json_format_stats_buffer_too_small_fails(void)
{
    http_stats_view_t view;
    memset(&view, 0, sizeof(view));
    char buf[8];
    int n = http_json_format_stats(buf, sizeof(buf), &view);
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* ---------- http_json_format_stock_distribution ----------------------------- */

TEST http_json_format_stock_distribution_golden(void)
{
    http_stock_distribution_view_t view;
    memset(&view, 0, sizeof(view));
    view.unchecked[3] = 12;
    view.checked[3] = 4;
    view.analysed[3] = 1;
    view.unchecked[5] = 7;
    view.total_unchecked = 19;
    view.total_checked = 4;
    view.total_analysed = 1;
    // Seconde coordonnée : niveau 3 a des scores MRV connus (moyenne 2.00),
    // niveau 5 n'en a aucun (POSSIBILITY_MIN_CANDIDATS_UNKNOWN partout) : 0.00.
    view.unchecked_min_candidats_sum[3] = 24;
    view.unchecked_min_candidats_known[3] = 12;

    char buf[4096];
    int n = http_json_format_stock_distribution(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"total_unchecked\":19") != NULL);
    ASSERT(strstr(buf, "\"total_checked\":4") != NULL);
    ASSERT(strstr(buf, "\"total_analysed\":1") != NULL);
    ASSERT(strstr(buf, "\"levels\":[{\"alloc\":3,\"unchecked\":12,\"checked\":4,\"analysed\":1,"
                       "\"avg_min_candidats\":2.00},"
                       "{\"alloc\":5,\"unchecked\":7,\"checked\":0,\"analysed\":0,"
                       "\"avg_min_candidats\":0.00}]}") != NULL);
    PASS();
}

/* Les niveaux entièrement vides sont OMIS : c'est ce qui garde la réponse
 * petite malgré les 257 niveaux possibles (cf. doc de la fonction). */
TEST http_json_format_stock_distribution_skips_empty_levels(void)
{
    http_stock_distribution_view_t view;
    memset(&view, 0, sizeof(view));
    view.analysed[0] = 1;              /* seul le niveau 0 est peuplé... */
    view.unchecked[STOCK_DISTRIBUTION_LEVELS - 1] = 2; /* ...et le dernier */
    view.total_analysed = 1;
    view.total_unchecked = 2;

    char buf[4096];
    int n = http_json_format_stock_distribution(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    int level_count = 0;
    const char *cursor = buf;
    while ((cursor = strstr(cursor, "\"alloc\":")) != NULL) {
        level_count++;
        cursor += strlen("\"alloc\":");
    }
    ASSERT_EQ_FMT(2, level_count, "%d");
    PASS();
}

/* Stock vide -> tableau vide (jamais absent), totaux à zéro toujours présents. */
TEST http_json_format_stock_distribution_empty_is_empty_array(void)
{
    http_stock_distribution_view_t view;
    memset(&view, 0, sizeof(view));

    char buf[512];
    int n = http_json_format_stock_distribution(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"levels\":[]}") != NULL);
    ASSERT(strstr(buf, "\"total_unchecked\":0") != NULL);
    PASS();
}

/* Pire cas réaliste : TOUS les niveaux peuplés, compteurs à 9 chiffres (borne
 * pratique : un niveau ne peut pas contenir plus de possibilités que la RAM
 * n'en héberge). Doit tenir dans HTTP_RESPONSE_MAX, sinon la route répond 500. */
TEST http_json_format_stock_distribution_dense_fits_response_buffer(void)
{
    http_stock_distribution_view_t view;
    memset(&view, 0, sizeof(view));
    for (int i = 0; i < STOCK_DISTRIBUTION_LEVELS; i++) {
        view.unchecked[i] = 999999999ULL;
        view.checked[i] = 999999999ULL;
        view.analysed[i] = 999999999ULL;
    }

    static char buf[HTTP_RESPONSE_MAX];
    int n = http_json_format_stock_distribution(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT((size_t)n < sizeof(buf));
    PASS();
}

TEST http_json_format_stock_distribution_buffer_too_small_fails(void)
{
    http_stock_distribution_view_t view;
    memset(&view, 0, sizeof(view));
    view.unchecked[2] = 1;

    char buf[8];
    ASSERT_EQ_FMT(-1, http_json_format_stock_distribution(buf, sizeof(buf), &view), "%d");
    PASS();
}

TEST http_json_format_stock_distribution_null_args_fail(void)
{
    http_stock_distribution_view_t view;
    memset(&view, 0, sizeof(view));
    char buf[256];

    ASSERT_EQ_FMT(-1, http_json_format_stock_distribution(NULL, sizeof(buf), &view), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_stock_distribution(buf, 0, &view), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_stock_distribution(buf, sizeof(buf), NULL), "%d");
    PASS();
}

TEST http_json_format_status_golden(void)
{
    http_status_view_t view;
    view.state = "admin_pause";
    view.uptime_seconds = 3600;
    view.version = 9;
    view.limit = 1000;
    view.max_stock_by_thread = 500;
    view.pruner_batch = 64;
    view.pruner_dfs_budget = 10000;
    view.last_backup_duration_ms = 42;
    view.stock_ram_limit_mb = 2048;
    view.stock_ram_used_mb = 512;

    char buf[512];
    int n = http_json_format_status(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT_STR_EQ(
        "{\"state\":\"admin_pause\",\"uptime_seconds\":3600,\"version\":9,"
        "\"limit\":1000,\"max_stock_by_thread\":500,\"pruner_batch\":64,"
        "\"pruner_dfs_budget\":10000,\"last_backup_duration_ms\":42,"
        "\"stock_ram_limit_mb\":2048,\"stock_ram_used_mb\":512}",
        buf);
    PASS();
}

TEST http_json_format_status_buffer_too_small_fails(void)
{
    http_status_view_t view;
    view.state = "running";
    view.uptime_seconds = 1;
    view.version = 9;
    view.limit = 0;
    view.max_stock_by_thread = 0;
    view.pruner_batch = 0;

    char buf[8];
    int n = http_json_format_status(buf, sizeof(buf), &view);
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* ---------- http_json_format_clients ---------------------------------------- */

TEST http_json_format_clients_golden(void)
{
    http_client_info_t infos[2];
    memset(&infos, 0, sizeof(infos));
    infos[0].session_no = 1;
    infos[0].pid = 111;
    infos[0].nb_forks = 4;
    infos[0].mode = 0;
    /* infos[0].peer_ip/label/*_uid_hex laissés vides par le memset. */
    infos[0].last_activity = 1700000000;
    infos[0].has_stats = 0;
    infos[1].session_no = 2;
    infos[1].pid = 222;
    infos[1].nb_forks = 0;
    infos[1].mode = 2;
    strncpy(infos[1].label, "jetson-1", sizeof(infos[1].label) - 1);
    strncpy(infos[1].machine_uid_hex, "0102030405060708090a0b0c0d0e0f10", sizeof(infos[1].machine_uid_hex) - 1);
    strncpy(infos[1].client_uid_hex, "101112131415161718191a1b1c1d1e1f", sizeof(infos[1].client_uid_hex) - 1);
    strncpy(infos[1].peer_ip, "203.0.113.10", sizeof(infos[1].peer_ip) - 1);
    infos[1].last_activity = 1700000042;
    infos[1].has_stats = 1;
    infos[1].stats_shots_per_second = 12345;
    infos[1].stats_possibility_stock = 10;
    infos[1].stats_analysed_stock = 3;
    infos[1].stats_max_result = 200;
    infos[1].stats_pruner_checked = 7;
    infos[1].stats_pruner_removed = 2;
    infos[1].stats_pruner_cells_per_second = 55;
    infos[1].stats_time = 1700000040;

    char buf[768];
    int n = http_json_format_clients(buf, sizeof(buf), infos, 2);

    ASSERT(n > 0);
    ASSERT_STR_EQ(
        "{\"clients\":["
        "{\"session_no\":1,\"pid\":111,\"forks\":4,\"mode\":\"search\",\"label\":\"\","
        "\"machine_uid\":\"\",\"client_uid\":\"\",\"ip\":\"\",\"last_activity\":1700000000,\"stats\":null},"
        "{\"session_no\":2,\"pid\":222,\"forks\":0,\"mode\":\"gpu_pruner\",\"label\":\"jetson-1\","
        "\"machine_uid\":\"0102030405060708090a0b0c0d0e0f10\",\"client_uid\":\"101112131415161718191a1b1c1d1e1f\","
        "\"ip\":\"203.0.113.10\",\"last_activity\":1700000042,"
        "\"stats\":{\"shots_per_second\":12345,\"possibility_stock\":10,\"analysed_stock\":3,"
        "\"max_result\":200,\"pruner_checked\":7,\"pruner_removed\":2,"
        "\"pruner_cells_per_second\":55,\"stats_time\":1700000040}}"
        "]}",
        buf);
    PASS();
}

TEST http_json_format_clients_empty_is_empty_array(void)
{
    char buf[128];
    int n = http_json_format_clients(buf, sizeof(buf), NULL, 0);

    ASSERT(n > 0);
    ASSERT_STR_EQ("{\"clients\":[]}", buf);
    PASS();
}

TEST http_json_format_clients_unknown_mode_label(void)
{
    http_client_info_t info;
    memset(&info, 0, sizeof(info));
    info.pid = 1;
    info.nb_forks = 1;
    info.mode = 99;
    info.last_activity = 1;

    char buf[256];
    int n = http_json_format_clients(buf, sizeof(buf), &info, 1);

    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"mode\":\"unknown\"") != NULL);
    PASS();
}

TEST http_json_format_clients_buffer_too_small_fails(void)
{
    http_client_info_t info;
    memset(&info, 0, sizeof(info));
    info.pid = 1;
    info.nb_forks = 1;
    info.mode = 0;
    info.last_activity = 1;

    char buf[8];
    int n = http_json_format_clients(buf, sizeof(buf), &info, 1);
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* ---------- http_json_format_known_clients ----------------------------------- */

TEST http_json_format_known_clients_golden(void)
{
    http_known_client_info_t infos[2];
    memset(&infos, 0, sizeof(infos));
    strncpy(infos[0].machine_uid_hex, "0102030405060708090a0b0c0d0e0f10", sizeof(infos[0].machine_uid_hex) - 1);
    /* infos[0].label/peer_ip laissés vides par le memset. */
    infos[0].mode = 0;
    infos[0].connected = 0;
    infos[0].nb_active_sessions = 0;
    infos[0].nb_active_search = 0;
    infos[0].nb_active_prune = 0;
    infos[0].nb_connections_total = 2;
    infos[0].first_seen = 1700000000;
    infos[0].last_seen = 1700000100;
    infos[0].total_pruner_checked = 0;
    infos[0].total_pruner_removed = 0;
    infos[0].best_max_result = 0;
    infos[0].cumulative_uptime_seconds = 3600;

    strncpy(infos[1].machine_uid_hex, "aabbccddeeff00112233445566778899", sizeof(infos[1].machine_uid_hex) - 1);
    strncpy(infos[1].label, "jetson-1", sizeof(infos[1].label) - 1);
    strncpy(infos[1].peer_ip, "203.0.113.10", sizeof(infos[1].peer_ip) - 1);
    infos[1].mode = 1;
    infos[1].connected = 1;
    infos[1].nb_active_sessions = 1;
    infos[1].nb_active_search = 0;
    infos[1].nb_active_prune = 1;
    infos[1].nb_connections_total = 5;
    infos[1].first_seen = 1700000000;
    infos[1].last_seen = 1700000200;
    infos[1].total_pruner_checked = 900;
    infos[1].total_pruner_removed = 40;
    infos[1].best_max_result = 210;
    infos[1].cumulative_uptime_seconds = 7200;

    char buf[768];
    int n = http_json_format_known_clients(buf, sizeof(buf), infos, 2);

    ASSERT(n > 0);
    ASSERT_STR_EQ(
        "{\"known_clients\":["
        "{\"machine_uid\":\"0102030405060708090a0b0c0d0e0f10\",\"label\":\"\",\"ip\":\"\",\"mode\":\"search\","
        "\"connected\":false,\"active_sessions\":0,\"active_search\":0,\"active_prune\":0,\"connections_total\":2,"
        "\"first_seen\":1700000000,\"last_seen\":1700000100,"
        "\"total_pruner_checked\":0,\"total_pruner_removed\":0,"
        "\"best_max_result\":0,\"cumulative_uptime_seconds\":3600},"
        "{\"machine_uid\":\"aabbccddeeff00112233445566778899\",\"label\":\"jetson-1\",\"ip\":\"203.0.113.10\",\"mode\":\"pruner\","
        "\"connected\":true,\"active_sessions\":1,\"active_search\":0,\"active_prune\":1,\"connections_total\":5,"
        "\"first_seen\":1700000000,\"last_seen\":1700000200,"
        "\"total_pruner_checked\":900,\"total_pruner_removed\":40,"
        "\"best_max_result\":210,\"cumulative_uptime_seconds\":7200}"
        "]}",
        buf);
    PASS();
}

TEST http_json_format_known_clients_empty_is_empty_array(void)
{
    char buf[128];
    int n = http_json_format_known_clients(buf, sizeof(buf), NULL, 0);

    ASSERT(n > 0);
    ASSERT_STR_EQ("{\"known_clients\":[]}", buf);
    PASS();
}

TEST http_json_format_known_clients_buffer_too_small_fails(void)
{
    http_known_client_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.machine_uid_hex, "0102030405060708090a0b0c0d0e0f10", sizeof(info.machine_uid_hex) - 1);

    char buf[8];
    int n = http_json_format_known_clients(buf, sizeof(buf), &info, 1);
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* ---------- http_json_format_commands -------------------------------------- */

TEST http_json_format_commands_golden(void)
{
    http_command_info_t infos[2];
    memset(&infos, 0, sizeof(infos));

    strncpy(infos[0].name, "pause", sizeof(infos[0].name) - 1);
    infos[0].scope = CMD_SCOPE_COMMON;
    infos[0].remote_class = CTRL_CMD_WRITE_RELAYABLE;
    infos[0].requires_token = 1;
    infos[0].summary = "pose une pause administrative";
    infos[0].usage = NULL;

    strncpy(infos[1].name, "clientsWork", sizeof(infos[1].name) - 1);
    infos[1].scope = CMD_SCOPE_SERVER_ONLY;
    infos[1].remote_class = CTRL_CMD_READ_ONLY;
    infos[1].requires_token = 0;
    infos[1].summary = "consultation en lecture seule d'une session";
    infos[1].usage = "clientsWork <session_no|client_uid|label>";

    char buf[2048];
    int written = http_json_format_commands(buf, sizeof(buf), infos, 2);

    ASSERT(written > 0);
    ASSERT(strstr(buf, "{\"commands\":[") == buf);
    ASSERT(strstr(buf, "\"name\":\"pause\"") != NULL);
    ASSERT(strstr(buf, "\"scope\":\"common\"") != NULL);
    ASSERT(strstr(buf, "\"remote_class\":\"write_relayable\"") != NULL);
    ASSERT(strstr(buf, "\"requires_token\":true") != NULL);
    ASSERT(strstr(buf, "\"summary\":\"pose une pause administrative\"") != NULL);
    ASSERT(strstr(buf, "\"usage\":null") != NULL);
    ASSERT(strstr(buf, "\"name\":\"clientsWork\"") != NULL);
    ASSERT(strstr(buf, "\"scope\":\"server_only\"") != NULL);
    ASSERT(strstr(buf, "\"remote_class\":\"read_only\"") != NULL);
    ASSERT(strstr(buf, "\"requires_token\":false") != NULL);
    ASSERT(strstr(buf, "\"usage\":\"clientsWork <session_no|client_uid|label>\"") != NULL);
    PASS();
}

TEST http_json_format_commands_empty(void)
{
    char buf[64];
    int written = http_json_format_commands(buf, sizeof(buf), NULL, 0);
    ASSERT(written > 0);
    ASSERT_STR_EQ("{\"commands\":[]}", buf);
    PASS();
}

TEST http_json_format_commands_rejects_bad_args(void)
{
    http_command_info_t infos[1];
    memset(&infos, 0, sizeof(infos));
    char buf[64];
    ASSERT_EQ_FMT(-1, http_json_format_commands(NULL, sizeof(buf), infos, 1), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_commands(buf, 0, infos, 1), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_commands(buf, sizeof(buf), NULL, 1), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_commands(buf, sizeof(buf), infos, -1), "%d");
    PASS();
}

/* ---------- http_json_format_best_board ------------------------------------ */

TEST http_json_format_best_board_no_record_is_false(void)
{
    http_best_board_view_t view;
    memset(&view, 0, sizeof(view));
    view.has_board = 0;

    char buf[64];
    int n = http_json_format_best_board(buf, sizeof(buf), &view);
    ASSERT(n > 0);
    ASSERT_STR_EQ("{\"has_board\":false}", buf);
    PASS();
}

/* La grille expose la DESCRIPTION de la pièce (id, rotation, couleurs), pas
   l'indice brut encodé dans possibility_packet.grid — demande explicite : on
   doit pouvoir connaître l'id de la pièce, ses motifs et sa rotation sans
   avoir à décoder soi-même l'indexation interne. */
TEST http_json_format_best_board_golden(void)
{
    http_best_board_view_t view;
    memset(&view, 0, sizeof(view));
    view.has_board = 1;
    view.alloc = 187;
    view.min_candidats = 3;
    /* Toutes les cases sont vides par défaut (id=-1), comme le produit
       http_best_board_collect — un id=0 par défaut (memset) serait une pièce
       valide à tort et gonflerait artificiellement le JSON attendu. */
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            view.grid[x][y].id = -1;
        }
    }
    view.grid[0][0].id = 139;
    view.grid[0][0].rotation = 2;
    view.grid[0][0].top = 2;
    view.grid[0][0].right = 15;
    view.grid[0][0].bottom = 15;
    view.grid[0][0].left = 3;

    char buf[8192];
    int n = http_json_format_best_board(buf, sizeof(buf), &view);
    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"has_board\":true") != NULL);
    ASSERT(strstr(buf, "\"alloc\":187,\"min_candidats\":3") != NULL);
    ASSERT(strstr(buf, "{\"id\":139,\"rotation\":2,\"top\":2,\"right\":15,\"bottom\":15,\"left\":3}") != NULL);
    ASSERT(strstr(buf, "null") != NULL);
    ASSERT(strstr(buf, "]}") != NULL); /* grille bien refermée */
    PASS();
}

TEST http_json_format_best_board_buffer_too_small_fails(void)
{
    http_best_board_view_t view;
    memset(&view, 0, sizeof(view));
    view.has_board = 1;
    view.grid[0][0].id = 1;

    char buf[8];
    int n = http_json_format_best_board(buf, sizeof(buf), &view);
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

TEST http_json_format_best_board_null_args_fail(void)
{
    http_best_board_view_t view;
    memset(&view, 0, sizeof(view));
    char buf[64];
    ASSERT_EQ_FMT(-1, http_json_format_best_board(NULL, sizeof(buf), &view), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_best_board(buf, sizeof(buf), NULL), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_best_board(buf, 0, &view), "%d");
    PASS();
}

/* ---------- Authorization header (http_request_parse) ---------------------- */

TEST http_request_parse_captures_authorization_header(void)
{
    char req[] = "POST /api/v1/command HTTP/1.1\r\nAuthorization: Bearer abc123\r\nContent-Length: 2\r\n\r\n{}";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_OK, r, "%d");
    ASSERT_STR_EQ("Bearer abc123", out.authorization);
    PASS();
}

/* En-tête insensible à la casse ("authorization" en minuscules), comme content-length. */
TEST http_request_parse_authorization_header_case_insensitive(void)
{
    char req[] = "GET /api/v1/stats HTTP/1.1\r\nauthorization: Bearer xyz\r\n\r\n";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_OK, r, "%d");
    ASSERT_STR_EQ("Bearer xyz", out.authorization);
    PASS();
}

TEST http_request_parse_missing_authorization_is_empty_string(void)
{
    char req[] = "GET /api/v1/stats HTTP/1.1\r\nHost: x\r\n\r\n";
    http_request_t out;
    http_parse_result_t r = http_request_parse(req, (int32_t)strlen(req), &out);

    ASSERT_EQ_FMT(HTTP_PARSE_OK, r, "%d");
    ASSERT_STR_EQ("", out.authorization);
    PASS();
}

/* ---------- http_extract_bearer_token --------------------------------------- */

TEST http_extract_bearer_token_nominal(void)
{
    char out[64];
    int n = http_extract_bearer_token("Bearer abc123", out, sizeof(out));
    ASSERT_EQ_FMT((int)strlen("abc123"), n, "%d");
    ASSERT_STR_EQ("abc123", out);
    PASS();
}

/* Schéma insensible à la casse (RFC 7235), séparateur multi-espaces toléré. */
TEST http_extract_bearer_token_case_insensitive_scheme_and_extra_spaces(void)
{
    char out[64];
    int n = http_extract_bearer_token("bearer   abc123", out, sizeof(out));
    ASSERT_EQ_FMT((int)strlen("abc123"), n, "%d");
    ASSERT_STR_EQ("abc123", out);
    PASS();
}

TEST http_extract_bearer_token_missing_header_fails(void)
{
    char out[64];
    int n = http_extract_bearer_token(NULL, out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    ASSERT_STR_EQ("", out);
    PASS();
}

TEST http_extract_bearer_token_empty_header_fails(void)
{
    char out[64];
    int n = http_extract_bearer_token("", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

TEST http_extract_bearer_token_wrong_scheme_fails(void)
{
    char out[64];
    int n = http_extract_bearer_token("Basic abc123", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* "Bearerabc" : pas de séparateur après le schéma -> rejeté. */
TEST http_extract_bearer_token_no_separator_fails(void)
{
    char out[64];
    int n = http_extract_bearer_token("Bearerabc123", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

TEST http_extract_bearer_token_empty_token_fails(void)
{
    char out[64];
    int n = http_extract_bearer_token("Bearer ", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* Jeton trop long pour out : rejeté plutôt que tronqué silencieusement. */
TEST http_extract_bearer_token_output_too_small_fails(void)
{
    char out[4];
    int n = http_extract_bearer_token("Bearer abcdef", out, sizeof(out));
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

/* ---------- http_token_equals_constant_time --------------------------------- */

TEST http_token_equals_constant_time_equal_strings(void)
{
    ASSERT_EQ_FMT(1, http_token_equals_constant_time("secret123", "secret123", 64), "%d");
    PASS();
}

TEST http_token_equals_constant_time_different_length(void)
{
    ASSERT_EQ_FMT(0, http_token_equals_constant_time("short", "muchlonger", 64), "%d");
    PASS();
}

TEST http_token_equals_constant_time_same_length_different_content(void)
{
    ASSERT_EQ_FMT(0, http_token_equals_constant_time("aaaaaa", "aaaaab", 64), "%d");
    PASS();
}

TEST http_token_equals_constant_time_null_args_are_not_equal(void)
{
    ASSERT_EQ_FMT(0, http_token_equals_constant_time(NULL, "x", 64), "%d");
    ASSERT_EQ_FMT(0, http_token_equals_constant_time("x", NULL, 64), "%d");
    ASSERT_EQ_FMT(0, http_token_equals_constant_time("x", "x", 0), "%d");
    PASS();
}

/* ---------- http_command_authorize ------------------------------------------ */

TEST http_command_authorize_allowed_is_always_ok(void)
{
    /* is_allowed l'emporte, quel que soit l'état du jeton. */
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_OK, http_command_authorize(1, 0, 0, 0), "%d");
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_OK, http_command_authorize(1, 0, 1, 0), "%d");
    PASS();
}

TEST http_command_authorize_privileged_with_valid_token_is_ok(void)
{
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_OK, http_command_authorize(0, 1, 1, 1), "%d");
    PASS();
}

TEST http_command_authorize_privileged_without_configured_token_is_unauthorized(void)
{
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_UNAUTHORIZED, http_command_authorize(0, 1, 0, 0), "%d");
    PASS();
}

TEST http_command_authorize_privileged_with_invalid_token_is_unauthorized(void)
{
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_UNAUTHORIZED, http_command_authorize(0, 1, 1, 0), "%d");
    PASS();
}

TEST http_command_authorize_neither_is_forbidden(void)
{
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_FORBIDDEN, http_command_authorize(0, 0, 0, 0), "%d");
    ASSERT_EQ_FMT(HTTP_CMD_AUTH_FORBIDDEN, http_command_authorize(0, 0, 1, 1), "%d");
    PASS();
}

/* ---------- http_response_format_unauthorized -------------------------------- */

TEST http_response_format_unauthorized_includes_www_authenticate(void)
{
    char buf[256];
    int written = http_response_format_unauthorized(buf, sizeof(buf), "{\"error\":\"unauthorized\"}");

    ASSERT(written > 0);
    ASSERT(strstr(buf, "HTTP/1.1 401 Unauthorized\r\n") == buf);
    ASSERT(strstr(buf, "WWW-Authenticate: Bearer\r\n") != NULL);
    ASSERT(strstr(buf, "\r\n\r\n{\"error\":\"unauthorized\"}") != NULL);
    PASS();
}

TEST http_response_format_unauthorized_buffer_too_small_fails(void)
{
    char buf[8];
    int written = http_response_format_unauthorized(buf, sizeof(buf), "{}");
    ASSERT_EQ_FMT(-1, written, "%d");
    PASS();
}

TEST http_json_format_status_null_state_fails(void)
{
    http_status_view_t view;
    view.state = NULL;
    view.uptime_seconds = 1;
    view.version = 9;
    view.limit = 0;
    view.max_stock_by_thread = 0;
    view.pruner_batch = 0;

    char buf[512];
    int n = http_json_format_status(buf, sizeof(buf), &view);
    ASSERT_EQ_FMT(-1, n, "%d");
    PASS();
}

SUITE(http_codec_suite)
{
    RUN_TEST(http_request_parse_valid_get);
    RUN_TEST(http_request_parse_valid_post_with_body);
    RUN_TEST(http_request_parse_header_case_insensitive);
    RUN_TEST(http_request_parse_incomplete_headers_needs_more);
    RUN_TEST(http_request_parse_partial_body_needs_more);
    RUN_TEST(http_request_parse_garbage_request_line_is_bad);
    RUN_TEST(http_request_parse_negative_content_length_is_bad);
    RUN_TEST(http_request_parse_huge_content_length_is_too_large);
    RUN_TEST(http_request_parse_oversized_without_terminator_is_too_large);
    RUN_TEST(http_request_parse_rejects_invalid_arguments);

    RUN_TEST(http_json_format_best_board_no_record_is_false);
    RUN_TEST(http_json_format_best_board_golden);
    RUN_TEST(http_json_format_best_board_buffer_too_small_fails);
    RUN_TEST(http_json_format_best_board_null_args_fail);

    RUN_TEST(http_route_resolve_known_routes);
    RUN_TEST(http_route_resolve_not_found);
    RUN_TEST(http_route_resolve_bad_method);

    RUN_TEST(http_response_format_writes_expected_headers_and_body);
    RUN_TEST(http_response_format_null_body_is_empty);
    RUN_TEST(http_response_format_buffer_too_small_fails);

    RUN_TEST(http_json_extract_string_nominal);
    RUN_TEST(http_json_extract_string_missing_key);
    RUN_TEST(http_json_extract_string_rejects_escapes);
    RUN_TEST(http_json_extract_string_output_too_small);

    RUN_TEST(http_json_format_stats_golden);
    RUN_TEST(http_json_format_stats_buffer_too_small_fails);
    RUN_TEST(http_json_format_stock_distribution_golden);
    RUN_TEST(http_json_format_stock_distribution_skips_empty_levels);
    RUN_TEST(http_json_format_stock_distribution_empty_is_empty_array);
    RUN_TEST(http_json_format_stock_distribution_dense_fits_response_buffer);
    RUN_TEST(http_json_format_stock_distribution_buffer_too_small_fails);
    RUN_TEST(http_json_format_stock_distribution_null_args_fail);
    RUN_TEST(http_json_format_status_golden);
    RUN_TEST(http_json_format_status_buffer_too_small_fails);
    RUN_TEST(http_json_format_status_null_state_fails);

    RUN_TEST(http_json_format_clients_golden);
    RUN_TEST(http_json_format_clients_empty_is_empty_array);
    RUN_TEST(http_json_format_clients_unknown_mode_label);
    RUN_TEST(http_json_format_clients_buffer_too_small_fails);

    RUN_TEST(http_json_format_known_clients_golden);
    RUN_TEST(http_json_format_known_clients_empty_is_empty_array);
    RUN_TEST(http_json_format_known_clients_buffer_too_small_fails);

    RUN_TEST(http_json_format_commands_golden);
    RUN_TEST(http_json_format_commands_empty);
    RUN_TEST(http_json_format_commands_rejects_bad_args);

    RUN_TEST(http_request_parse_captures_authorization_header);
    RUN_TEST(http_request_parse_authorization_header_case_insensitive);
    RUN_TEST(http_request_parse_missing_authorization_is_empty_string);

    RUN_TEST(http_extract_bearer_token_nominal);
    RUN_TEST(http_extract_bearer_token_case_insensitive_scheme_and_extra_spaces);
    RUN_TEST(http_extract_bearer_token_missing_header_fails);
    RUN_TEST(http_extract_bearer_token_empty_header_fails);
    RUN_TEST(http_extract_bearer_token_wrong_scheme_fails);
    RUN_TEST(http_extract_bearer_token_no_separator_fails);
    RUN_TEST(http_extract_bearer_token_empty_token_fails);
    RUN_TEST(http_extract_bearer_token_output_too_small_fails);

    RUN_TEST(http_token_equals_constant_time_equal_strings);
    RUN_TEST(http_token_equals_constant_time_different_length);
    RUN_TEST(http_token_equals_constant_time_same_length_different_content);
    RUN_TEST(http_token_equals_constant_time_null_args_are_not_equal);

    RUN_TEST(http_command_authorize_allowed_is_always_ok);
    RUN_TEST(http_command_authorize_privileged_with_valid_token_is_ok);
    RUN_TEST(http_command_authorize_privileged_without_configured_token_is_unauthorized);
    RUN_TEST(http_command_authorize_privileged_with_invalid_token_is_unauthorized);
    RUN_TEST(http_command_authorize_neither_is_forbidden);

    RUN_TEST(http_response_format_unauthorized_includes_www_authenticate);
    RUN_TEST(http_response_format_unauthorized_buffer_too_small_fails);
}
