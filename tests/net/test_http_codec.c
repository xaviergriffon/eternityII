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
    view.queue_unchecked[0] = 7;
    view.queue_checked[0] = 1;
    view.queue_analysed[0] = 0;

    char buf[4096];
    int n = http_json_format_stats(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT(strstr(buf, "\"shots_per_second\":100") != NULL);
    ASSERT(strstr(buf, "\"max_result\":42") != NULL);
    ASSERT(strstr(buf, "\"active_threads\":3") != NULL);
    ASSERT(strstr(buf, "\"queues\":[{\"file\":0,\"unchecked\":7,\"checked\":1,\"analysed\":0}") != NULL);
    ASSERT(strstr(buf, "]}") != NULL); /* tableau bien refermé */

    /* NB_FILE_POSSIBILITY entrées attendues dans le tableau. */
    int file_count = 0;
    const char *cursor = buf;
    while ((cursor = strstr(cursor, "\"file\":")) != NULL) {
        file_count++;
        cursor += strlen("\"file\":");
    }
    ASSERT_EQ_FMT(NB_FILE_POSSIBILITY, file_count, "%d");
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

TEST http_json_format_status_golden(void)
{
    http_status_view_t view;
    view.state = "admin_pause";
    view.uptime_seconds = 3600;
    view.version = 9;
    view.limit = 1000;
    view.max_stock_by_thread = 500;
    view.pruner_batch = 64;

    char buf[512];
    int n = http_json_format_status(buf, sizeof(buf), &view);

    ASSERT(n > 0);
    ASSERT_STR_EQ(
        "{\"state\":\"admin_pause\",\"uptime_seconds\":3600,\"version\":9,"
        "\"limit\":1000,\"max_stock_by_thread\":500,\"pruner_batch\":64}",
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
    RUN_TEST(http_json_format_status_golden);
    RUN_TEST(http_json_format_status_buffer_too_small_fails);
    RUN_TEST(http_json_format_status_null_state_fails);
}
