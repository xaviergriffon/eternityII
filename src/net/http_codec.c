#include "net/http_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Recherche `needle` (longueur `needle_len`) dans `hay` (longueur `hay_len`). */
static const char *find_bytes(const char *hay, int32_t hay_len, const char *needle, size_t needle_len)
{
    if (needle_len == 0 || hay_len < 0 || (size_t)hay_len < needle_len) {
        return NULL;
    }
    int32_t last = hay_len - (int32_t)needle_len;
    for (int32_t i = 0; i <= last; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static int ascii_lower(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 'a';
    }
    return c;
}

/** @brief Compare le début d'une ligne d'en-tête (`line`, `line_len` octets) à `name`, insensible à la casse. */
static int header_name_matches(const char *line, size_t line_len, const char *name)
{
    size_t name_len = strlen(name);
    if (line_len < name_len) {
        return 0;
    }
    for (size_t i = 0; i < name_len; i++) {
        if (ascii_lower((unsigned char)line[i]) != ascii_lower((unsigned char)name[i])) {
            return 0;
        }
    }
    return 1;
}

http_parse_result_t http_request_parse(const char *buf, int32_t len, http_request_t *out)
{
    if (buf == NULL || out == NULL || len < 0) {
        return HTTP_PARSE_BAD;
    }
    if (len > HTTP_REQUEST_MAX) {
        return HTTP_PARSE_TOO_LARGE;
    }

    const char *headers_end = find_bytes(buf, len, "\r\n\r\n", 4);
    if (headers_end == NULL) {
        return (len >= HTTP_REQUEST_MAX) ? HTTP_PARSE_TOO_LARGE : HTTP_PARSE_NEED_MORE;
    }

    const char *request_line_end = find_bytes(buf, (int32_t)(headers_end - buf), "\r\n", 2);
    if (request_line_end == NULL) {
        return HTTP_PARSE_BAD;
    }

    const char *sp1 = memchr(buf, ' ', (size_t)(request_line_end - buf));
    if (sp1 == NULL) {
        return HTTP_PARSE_BAD;
    }
    size_t method_len = (size_t)(sp1 - buf);
    if (method_len == 0 || method_len >= HTTP_METHOD_MAX) {
        return HTTP_PARSE_BAD;
    }

    const char *path_start = sp1 + 1;
    const char *sp2 = memchr(path_start, ' ', (size_t)(request_line_end - path_start));
    if (sp2 == NULL) {
        return HTTP_PARSE_BAD;
    }
    size_t path_len = (size_t)(sp2 - path_start);
    if (path_len == 0 || path_len >= HTTP_PATH_MAX) {
        return HTTP_PARSE_BAD;
    }

    const char *version_start = sp2 + 1;
    size_t version_len = (size_t)(request_line_end - version_start);
    static const char http_prefix[] = "HTTP/";
    if (version_len < strlen(http_prefix) || memcmp(version_start, http_prefix, strlen(http_prefix)) != 0) {
        return HTTP_PARSE_BAD;
    }

    http_request_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    memcpy(parsed.method, buf, method_len);
    parsed.method[method_len] = '\0';
    memcpy(parsed.path, path_start, path_len);
    parsed.path[path_len] = '\0';

    int32_t content_length = 0;
    const char *header_cursor = request_line_end + 2;
    while (header_cursor < headers_end) {
        const char *header_line_end = find_bytes(header_cursor, (int32_t)(headers_end - header_cursor), "\r\n", 2);
        if (header_line_end == NULL) {
            header_line_end = headers_end;
        }
        size_t header_line_len = (size_t)(header_line_end - header_cursor);
        if (header_name_matches(header_cursor, header_line_len, "authorization")) {
            const char *colon = memchr(header_cursor, ':', header_line_len);
            if (colon != NULL) {
                const char *value = colon + 1;
                const char *value_end = header_cursor + header_line_len;
                while (value < value_end && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                size_t value_len = (value < value_end) ? (size_t)(value_end - value) : 0;
                /* Trop long pour parsed.authorization : laissé vide (traité
                   comme absent) plutôt que tronqué silencieusement — cf. doc
                   du champ dans http_codec.h. */
                if (value_len > 0 && value_len < sizeof(parsed.authorization)) {
                    memcpy(parsed.authorization, value, value_len);
                    parsed.authorization[value_len] = '\0';
                }
            }
        } else if (header_name_matches(header_cursor, header_line_len, "content-length")) {
            const char *colon = memchr(header_cursor, ':', header_line_len);
            if (colon == NULL) {
                return HTTP_PARSE_BAD;
            }
            const char *value = colon + 1;
            const char *value_end = header_cursor + header_line_len;
            while (value < value_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            size_t value_len = (value < value_end) ? (size_t)(value_end - value) : 0;
            char value_buf[16];
            if (value_len == 0 || value_len >= sizeof(value_buf)) {
                return HTTP_PARSE_BAD;
            }
            for (size_t i = 0; i < value_len; i++) {
                if (value[i] < '0' || value[i] > '9') {
                    return HTTP_PARSE_BAD;
                }
            }
            memcpy(value_buf, value, value_len);
            value_buf[value_len] = '\0';
            long parsed_len = strtol(value_buf, NULL, 10);
            if (parsed_len < 0 || parsed_len > HTTP_REQUEST_MAX) {
                return HTTP_PARSE_TOO_LARGE;
            }
            content_length = (int32_t)parsed_len;
        }
        header_cursor = header_line_end + 2;
    }

    const char *body_start = headers_end + 4;
    int32_t available_body = (int32_t)(len - (body_start - buf));
    if (available_body < 0) {
        available_body = 0;
    }

    if (content_length > 0) {
        if (available_body < content_length) {
            return (len >= HTTP_REQUEST_MAX) ? HTTP_PARSE_TOO_LARGE : HTTP_PARSE_NEED_MORE;
        }
        parsed.body = body_start;
        parsed.body_len = content_length;
    } else {
        parsed.body = NULL;
        parsed.body_len = 0;
    }
    parsed.content_length = content_length;

    *out = parsed;
    return HTTP_PARSE_OK;
}

http_route_t http_route_resolve(const char *method, const char *path)
{
    if (method == NULL || path == NULL) {
        return HTTP_ROUTE_NOT_FOUND;
    }
    if (strcmp(path, "/api/v1/stats") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_STATS : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/status") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_STATUS : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/command") == 0) {
        return (strcmp(method, "POST") == 0) ? HTTP_ROUTE_COMMAND : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/clients") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_CLIENTS : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/clients/stats") == 0) {
        return (strcmp(method, "POST") == 0) ? HTTP_ROUTE_CLIENTS_STATS : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/best-board") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_BEST_BOARD : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/known-clients") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_KNOWN_CLIENTS : HTTP_ROUTE_BAD_METHOD;
    }
    if (strcmp(path, "/api/v1/stock-distribution") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_STOCK_DISTRIBUTION : HTTP_ROUTE_BAD_METHOD;
    }
    return HTTP_ROUTE_NOT_FOUND;
}

static const char *reason_phrase(int status)
{
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default:  return "Error";
    }
}

int http_response_format(char *buf, size_t size, int status, const char *json_body)
{
    if (buf == NULL || size == 0) {
        return -1;
    }
    const char *body = (json_body != NULL) ? json_body : "";
    size_t body_len = strlen(body);
    int written = snprintf(buf, size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, reason_phrase(status), body_len, body);
    if (written < 0 || (size_t)written >= size) {
        return -1;
    }
    return written;
}

int http_response_format_unauthorized(char *buf, size_t size, const char *json_body)
{
    if (buf == NULL || size == 0) {
        return -1;
    }
    const char *body = (json_body != NULL) ? json_body : "";
    size_t body_len = strlen(body);
    int written = snprintf(buf, size,
        "HTTP/1.1 401 %s\r\n"
        "WWW-Authenticate: Bearer\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        reason_phrase(401), body_len, body);
    if (written < 0 || (size_t)written >= size) {
        return -1;
    }
    return written;
}

static int ascii_lower_char(char c)
{
    return ascii_lower((unsigned char)c);
}

int http_extract_bearer_token(const char *authorization_header, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (authorization_header == NULL) {
        return -1;
    }

    static const char scheme[] = "Bearer";
    size_t scheme_len = strlen(scheme);
    size_t header_len = strlen(authorization_header);
    if (header_len <= scheme_len) {
        return -1;
    }
    for (size_t i = 0; i < scheme_len; i++) {
        if (ascii_lower_char(authorization_header[i]) != ascii_lower_char(scheme[i])) {
            return -1;
        }
    }

    const char *cursor = authorization_header + scheme_len;
    if (*cursor != ' ' && *cursor != '\t') {
        return -1; /* pas de séparateur : "Bearerxyz" n'est pas un schéma Bearer */
    }
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    size_t token_len = strlen(cursor);
    if (token_len == 0 || token_len >= out_size) {
        return -1;
    }
    memcpy(out, cursor, token_len);
    out[token_len] = '\0';
    return (int)token_len;
}

int http_token_equals_constant_time(const char *a, const char *b, size_t max_len)
{
    if (a == NULL || b == NULL || max_len == 0) {
        return 0;
    }
    size_t len_a = strnlen(a, max_len);
    size_t len_b = strnlen(b, max_len);
    unsigned char diff = (unsigned char)(len_a != len_b);
    for (size_t i = 0; i < max_len; i++) {
        unsigned char ca = (i < len_a) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < len_b) ? (unsigned char)b[i] : 0;
        diff = (unsigned char)(diff | (ca ^ cb));
    }
    return diff == 0;
}

http_cmd_auth_result_t http_command_authorize(int is_public, int needs_auth, int has_configured_token, int token_valid)
{
    if (is_public) {
        return HTTP_CMD_AUTH_OK;
    }
    if (needs_auth) {
        return (has_configured_token && token_valid) ? HTTP_CMD_AUTH_OK : HTTP_CMD_AUTH_UNAUTHORIZED;
    }
    return HTTP_CMD_AUTH_FORBIDDEN;
}

int http_json_extract_string(const char *body, int32_t len, const char *key, char *out, size_t out_size)
{
    if (body == NULL || len < 0 || key == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    char pattern[64];
    int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern)) {
        return -1;
    }

    const char *key_pos = find_bytes(body, len, pattern, (size_t)pattern_len);
    if (key_pos == NULL) {
        return -1;
    }

    const char *cursor = key_pos + pattern_len;
    const char *end = body + len;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    if (cursor >= end || *cursor != ':') {
        return -1;
    }
    cursor++;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    if (cursor >= end || *cursor != '"') {
        return -1;
    }
    cursor++;

    size_t written = 0;
    while (cursor < end && *cursor != '"') {
        if (*cursor == '\\') {
            /* Échappements JSON non supportés : rejeté plutôt que mal interprété
               (cf. doc dans http_codec.h). */
            return -1;
        }
        if (written + 1 >= out_size) {
            return -1;
        }
        out[written++] = *cursor;
        cursor++;
    }
    if (cursor >= end || *cursor != '"') {
        return -1; /* chaîne non terminée avant la fin du corps */
    }
    out[written] = '\0';
    return (int)written;
}

int http_json_format_stats(char *buf, size_t size, const http_stats_view_t *view)
{
    if (buf == NULL || size == 0 || view == NULL) {
        return -1;
    }

    size_t offset = 0;
    int written = snprintf(buf + offset, size - offset,
        "{"
        "\"shots_per_second\":%llu,"
        "\"possibility_stock\":%llu,"
        "\"checked_stock\":%llu,"
        "\"analysed_stock\":%llu,"
        "\"max_result\":%llu,"
        "\"active_threads\":%llu,"
        "\"pruner_checked\":%llu,"
        "\"pruner_removed\":%llu,"
        "\"stock_spilled_packets\":%llu,"
        "\"stock_spill_segments\":%llu,"
        "\"stock_adds_per_sec_1m\":%.4f,"
        "\"stock_adds_per_sec_1h\":%.4f,"
        "\"stock_adds_per_sec_1d\":%.4f,"
        "\"stock_removes_per_sec_1m\":%.4f,"
        "\"stock_removes_per_sec_1h\":%.4f,"
        "\"stock_removes_per_sec_1d\":%.4f,"
        "\"queues\":[",
        view->shots_per_second, view->possibility_stock, view->checked_stock,
        view->analysed_stock, view->max_result, view->active_threads,
        view->pruner_checked, view->pruner_removed,
        view->stock_spilled_packets, view->stock_spill_segments,
        view->stock_adds_per_sec_1m, view->stock_adds_per_sec_1h, view->stock_adds_per_sec_1d,
        view->stock_removes_per_sec_1m, view->stock_removes_per_sec_1h, view->stock_removes_per_sec_1d);
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    for (int i = 0; i < nb_file_possibility; i++) {
        written = snprintf(buf + offset, size - offset,
            "%s{\"file\":%d,\"unchecked\":%llu,\"checked\":%llu,\"analysed\":%llu}",
            (i == 0) ? "" : ",", i,
            view->queue_unchecked[i], view->queue_checked[i], view->queue_analysed[i]);
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;
    }

    written = snprintf(buf + offset, size - offset, "]}");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    return (int)offset;
}

int http_json_format_stock_distribution(char *buf, size_t size, const http_stock_distribution_view_t *view)
{
    if (buf == NULL || size == 0 || view == NULL) {
        return -1;
    }

    size_t offset = 0;
    int written = snprintf(buf + offset, size - offset,
        "{"
        "\"total_unchecked\":%llu,"
        "\"total_checked\":%llu,"
        "\"total_analysed\":%llu,"
        "\"levels\":[",
        view->total_unchecked, view->total_checked, view->total_analysed);
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    // Niveaux vides omis (cf. doc de la fonction) : le tableau est creux par
    // nature, seuls quelques `alloc` sont peuplés sur un serveur réel.
    int emitted = 0;
    for (int level = 0; level < STOCK_DISTRIBUTION_LEVELS; level++) {
        if (view->unchecked[level] == 0 && view->checked[level] == 0 && view->analysed[level] == 0) {
            continue;
        }
        // Seconde coordonnée (score MRV moyen), les trois pools combinés : 0.0
        // signifie « non mesuré » (aucun `known`), jamais une vraie moyenne —
        // un score réel stocké vaut toujours >= 0 candidat pour une case
        // contrainte, mais un sous-arbre à 0 candidat est mort et n'est
        // jamais matérialisé, donc 0.0 en sortie est sans ambiguïté possible
        // avec une moyenne réelle. Combiné (pas un champ par pool) pour
        // garder `HTTP_RESPONSE_MAX` large sur un histogramme à beaucoup de
        // niveaux peuplés — le détail par pool reste dans `stock_distribution_t`
        // pour un consommateur interne au process (console `statistic`).
        unsigned long long known = view->unchecked_min_candidats_known[level]
            + view->checked_min_candidats_known[level] + view->analysed_min_candidats_known[level];
        double avg_min_candidats = known > 0
            ? (double)(view->unchecked_min_candidats_sum[level] + view->checked_min_candidats_sum[level]
                       + view->analysed_min_candidats_sum[level]) / (double)known
            : 0.0;
        written = snprintf(buf + offset, size - offset,
            "%s{\"alloc\":%d,\"unchecked\":%llu,\"checked\":%llu,\"analysed\":%llu,"
            "\"avg_min_candidats\":%.2f}",
            (emitted == 0) ? "" : ",", level,
            view->unchecked[level], view->checked[level], view->analysed[level],
            avg_min_candidats);
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;
        emitted++;
    }

    written = snprintf(buf + offset, size - offset, "]}");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    return (int)offset;
}

/** @brief Libellé du mode d'un client de contrôle (0/1/2, cf. control_hello_t.mode). */
static const char *client_mode_label(uint8_t mode)
{
    switch (mode) {
        case 0:  return "search";
        case 1:  return "pruner";
        case 2:  return "gpu_pruner";
        default: return "unknown";
    }
}

/**
 * @brief Écrit dans `out` une chaîne JSON valide (guillemets inclus) de
 *        `in`, échappant `"`/`\` et remplaçant les caractères de contrôle
 *        par un espace. Nécessaire car `label` (option CLI `--name`) est une
 *        donnée DÉCLARÉE par le client — jamais vérifiée — contrairement à
 *        `peer_ip` ou aux champs hexadécimaux (`machine_uid_hex`/`client_uid_hex`,
 *        toujours `[0-9a-f]`) : l'embarquer tel quel casserait la structure
 *        JSON de la réponse dès qu'il contient un `"` ou un `\`.
 *
 * @param in       Chaîne source, NUL-terminée.
 * @param out      Tampon destination (toujours NUL-terminé en sortie).
 * @param out_size Taille de `out` — tronque proprement (jamais de guillemet
 *                 fermant manquant) si trop petit.
 */
static void json_escape_label(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (out_size < 3) {
        out[0] = '\0';
        return;
    }
    size_t o = 0;
    out[o++] = '"';
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_size) { /* \c + guillemet fermant + NUL */
                break;
            }
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 2 >= out_size) {
                break;
            }
            out[o++] = ' ';
        } else {
            if (o + 2 >= out_size) {
                break;
            }
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
}

int http_json_format_clients(char *buf, size_t size, const http_client_info_t *infos, int count)
{
    if (buf == NULL || size == 0 || (infos == NULL && count > 0) || count < 0) {
        return -1;
    }

    size_t offset = 0;
    int written = snprintf(buf + offset, size - offset, "{\"clients\":[");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    for (int i = 0; i < count; i++) {
        char label_json[2 * HTTP_CLIENT_LABEL_MAX + 3];
        json_escape_label(infos[i].label, label_json, sizeof(label_json));

        written = snprintf(buf + offset, size - offset,
            "%s{\"session_no\":%llu,\"pid\":%d,\"forks\":%d,\"mode\":\"%s\",\"label\":%s,"
            "\"machine_uid\":\"%s\",\"client_uid\":\"%s\",\"ip\":\"%s\",\"last_activity\":%lld,\"stats\":",
            (i == 0) ? "" : ",", infos[i].session_no, infos[i].pid, infos[i].nb_forks,
            client_mode_label(infos[i].mode), label_json,
            infos[i].machine_uid_hex, infos[i].client_uid_hex,
            infos[i].peer_ip, infos[i].last_activity);
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;

        if (infos[i].has_stats) {
            written = snprintf(buf + offset, size - offset,
                "{\"shots_per_second\":%llu,\"possibility_stock\":%llu,\"analysed_stock\":%llu,"
                "\"max_result\":%llu,\"pruner_checked\":%llu,\"pruner_removed\":%llu,"
                "\"pruner_cells_per_second\":%llu,\"stats_time\":%lld}",
                infos[i].stats_shots_per_second, infos[i].stats_possibility_stock,
                infos[i].stats_analysed_stock, infos[i].stats_max_result,
                infos[i].stats_pruner_checked, infos[i].stats_pruner_removed,
                infos[i].stats_pruner_cells_per_second, infos[i].stats_time);
        } else {
            written = snprintf(buf + offset, size - offset, "null");
        }
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;

        written = snprintf(buf + offset, size - offset, "}");
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;
    }

    written = snprintf(buf + offset, size - offset, "]}");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    return (int)offset;
}

int http_json_format_known_clients(char *buf, size_t size, const http_known_client_info_t *infos, int count)
{
    if (buf == NULL || size == 0 || (infos == NULL && count > 0) || count < 0) {
        return -1;
    }

    size_t offset = 0;
    int written = snprintf(buf + offset, size - offset, "{\"known_clients\":[");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    for (int i = 0; i < count; i++) {
        char label_json[2 * HTTP_CLIENT_LABEL_MAX + 3];
        json_escape_label(infos[i].label, label_json, sizeof(label_json));

        written = snprintf(buf + offset, size - offset,
            "%s{\"machine_uid\":\"%s\",\"label\":%s,\"ip\":\"%s\",\"mode\":\"%s\","
            "\"connected\":%s,\"active_sessions\":%d,\"connections_total\":%d,"
            "\"first_seen\":%lld,\"last_seen\":%lld,"
            "\"total_pruner_checked\":%llu,\"total_pruner_removed\":%llu,"
            "\"best_max_result\":%llu,\"cumulative_uptime_seconds\":%llu}",
            (i == 0) ? "" : ",", infos[i].machine_uid_hex, label_json, infos[i].peer_ip,
            client_mode_label(infos[i].mode), infos[i].connected ? "true" : "false",
            infos[i].nb_active_sessions, infos[i].nb_connections_total,
            infos[i].first_seen, infos[i].last_seen,
            infos[i].total_pruner_checked, infos[i].total_pruner_removed,
            infos[i].best_max_result, infos[i].cumulative_uptime_seconds);
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;
    }

    written = snprintf(buf + offset, size - offset, "]}");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    return (int)offset;
}

int http_json_format_status(char *buf, size_t size, const http_status_view_t *view)
{
    if (buf == NULL || size == 0 || view == NULL || view->state == NULL) {
        return -1;
    }
    int written = snprintf(buf, size,
        "{"
        "\"state\":\"%s\","
        "\"uptime_seconds\":%ld,"
        "\"version\":%d,"
        "\"limit\":%llu,"
        "\"max_stock_by_thread\":%d,"
        "\"pruner_batch\":%d,"
        "\"pruner_dfs_budget\":%d,"
        "\"last_backup_duration_ms\":%llu,"
        "\"stock_ram_limit_mb\":%llu,"
        "\"stock_ram_used_mb\":%llu"
        "}",
        view->state, view->uptime_seconds, view->version, view->limit,
        view->max_stock_by_thread, view->pruner_batch, view->pruner_dfs_budget,
        view->last_backup_duration_ms, view->stock_ram_limit_mb, view->stock_ram_used_mb);
    if (written < 0 || (size_t)written >= size) {
        return -1;
    }
    return written;
}

int http_json_format_best_board(char *buf, size_t size, const http_best_board_view_t *view)
{
    if (buf == NULL || size == 0 || view == NULL) {
        return -1;
    }

    if (!view->has_board) {
        int written = snprintf(buf, size, "{\"has_board\":false}");
        if (written < 0 || (size_t)written >= size) {
            return -1;
        }
        return written;
    }

    size_t offset = 0;
    int written = snprintf(buf + offset, size - offset,
        "{\"has_board\":true,\"alloc\":%u,\"min_candidats\":%d,\"grid\":[",
        view->alloc, view->min_candidats);
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    for (int y = 0; y < ETERN_SIZE; y++) {
        written = snprintf(buf + offset, size - offset, "%s[", (y == 0) ? "" : ",");
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;

        for (int x = 0; x < ETERN_SIZE; x++) {
            const http_best_board_cell_t *cell = &view->grid[x][y];
            if (cell->id < 0) {
                written = snprintf(buf + offset, size - offset, "%snull", (x == 0) ? "" : ",");
            } else {
                written = snprintf(buf + offset, size - offset,
                    "%s{\"id\":%d,\"rotation\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d,\"left\":%d}",
                    (x == 0) ? "" : ",", cell->id, cell->rotation,
                    cell->top, cell->right, cell->bottom, cell->left);
            }
            if (written < 0 || (size_t)written >= size - offset) {
                return -1;
            }
            offset += (size_t)written;
        }

        written = snprintf(buf + offset, size - offset, "]");
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;
    }

    written = snprintf(buf + offset, size - offset, "]}");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    return (int)offset;
}
