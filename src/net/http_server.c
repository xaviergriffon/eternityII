#include "net/http_server.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "app/control_registry.h"
#include "app/etii_server.h"
#include "app/static_variables.h"
#include "core/datamanager.h"
#include "core/best_board.h"
#include "net/etii_protocol.h"
#include "net/tcpserver.h"
#include "ui/command_lines.h"
#include "ui/logger.h"

/// Longueur de la file d'attente de connexions du socket d'écoute admin
/// (usage occasionnel, pas besoin d'un backlog comparable au port de travail).
#define HTTP_ACCEPT_BACKLOG 4
/// Timeout d'E/S (secondes) posé sur chaque connexion acceptée : un client
/// muet ne doit jamais bloquer indéfiniment le thread accepteur (une seule
/// connexion servie à la fois, cf. doc du module).
#define HTTP_IO_TIMEOUT_SEC 5
/// Longueur maximale de la commande extraite du corps JSON de POST /api/v1/command.
#define HTTP_COMMAND_MAX 128

/// Descripteur du socket d'écoute admin, lu par le thread accepteur.
static int g_http_listen_fd = -1;
/// Instant de démarrage de l'API HTTP, pour `uptime_seconds` (0 tant que non démarrée).
static time_t g_http_start_time = 0;

static void *http_server_thread(void *arg);

int http_server_start(int port)
{
    int listen_fd = create_tcp_server_bound((uint32_t)INADDR_LOOPBACK, port, HTTP_ACCEPT_BACKLOG);
    if (listen_fd == -1) {
        log_error("http_server_start : impossible de démarrer l'API HTTP sur 127.0.0.1:%i\n", port);
        return -1;
    }

    pthread_attr_t thread_attributes;
    pthread_attr_init(&thread_attributes);
    pthread_attr_setdetachstate(&thread_attributes, PTHREAD_CREATE_DETACHED);

    g_http_listen_fd = listen_fd;
    g_http_start_time = time(NULL);

    pthread_t thread;
    if (0 != pthread_create(&thread, &thread_attributes, http_server_thread, NULL)) {
        log_error("http_server_start : problème avec pthread_create()\n");
        pthread_attr_destroy(&thread_attributes);
        close(listen_fd);
        g_http_listen_fd = -1;
        return -1;
    }
    pthread_attr_destroy(&thread_attributes);

    log_event("API HTTP démarrée sur 127.0.0.1:%i", port);
    return 0;
}

void http_stats_collect(http_stats_view_t *out)
{
    memset(out, 0, sizeof(*out));

    out->shots_per_second = server_shots_per_second;
    out->max_result = max_result;
    out->active_threads = (unsigned long long)server_active_client_count();
    out->pruner_checked = pruner_checked;
    out->pruner_removed = pruner_removed;

    unsigned long long unchecked_total = 0, checked_total = 0, analysed_total = 0;
    for (int f = 0; f < NB_FILE_POSSIBILITY; f++) {
        unsigned long long u = file_size(f);
        unsigned long long c = file_checked_size(f);
        unsigned long long a = file_analysed_size(f);
        out->queue_unchecked[f] = u;
        out->queue_checked[f] = c;
        out->queue_analysed[f] = a;
        unchecked_total += u;
        checked_total += c;
        analysed_total += a;
    }
    out->possibility_stock = unchecked_total;
    out->checked_stock = checked_total;
    out->analysed_stock = analysed_total;
}

static const char *state_label(int r)
{
    switch (r) {
        case REQUEST_CONTINUE:    return "running";
        case REQUEST_PAUSE:       return "regulation_pause";
        case REQUEST_ADMIN_PAUSE: return "admin_pause";
        case REQUEST_STOP:        return "stopping";
        default:                  return "unknown";
    }
}

void http_status_collect(http_status_view_t *out)
{
    out->state = state_label(request);
    out->uptime_seconds = (g_http_start_time > 0) ? (long)(time(NULL) - g_http_start_time) : 0;
    out->version = VERSION;
    out->limit = max_search_by_sec;
    out->max_stock_by_thread = max_stock_by_thread;
    out->pruner_batch = pruner_batch_size;
}

int http_clients_collect(http_client_info_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int cap = (max < MAX_CONTROL_SESSIONS) ? max : MAX_CONTROL_SESSIONS;
    int n = control_registry_snapshot(infos, cap);
    for (int i = 0; i < n; i++) {
        out[i].pid = infos[i].pid;
        out[i].nb_forks = infos[i].nb_forks;
        out[i].mode = infos[i].mode;
        out[i].last_activity = (long long)infos[i].last_activity;
        out[i].has_stats = infos[i].has_stats;
        out[i].stats_shots_per_second = (unsigned long long)infos[i].stats.shots_per_second;
        out[i].stats_possibility_stock = (unsigned long long)infos[i].stats.possibility_stock;
        out[i].stats_analysed_stock = (unsigned long long)infos[i].stats.analysed_stock;
        out[i].stats_max_result = (unsigned long long)infos[i].stats.max_result;
        out[i].stats_pruner_checked = (unsigned long long)infos[i].stats.pruner_checked;
        out[i].stats_pruner_removed = (unsigned long long)infos[i].stats.pruner_removed;
        out[i].stats_pruner_cells_per_second = (unsigned long long)infos[i].stats.pruner_cells_per_second;
        out[i].stats_time = (long long)infos[i].stats_time;
    }
    return n;
}

/**
 * @brief Décode `board.grid[x][y]` (indice `id + ETERN_PARTS*rotation`, cf.
 * `id_for_rotated_part`) en description de pièce, via `g_server_rotate_parts`
 * (`src/app/etii_server.c`). Même décodage que `save_solution_csv`
 * (`src/core/possibility.c`), avec le même repli sans couleurs si la table
 * n'est pas (encore) disponible.
 */
static void decode_best_board_cell(int16_t idx, http_best_board_cell_t *out)
{
    if (idx < 0) {
        out->id = -1;
        return;
    }
    if (g_server_rotate_parts != NULL && idx < g_server_rotate_parts->size) {
        const struct part *p = &g_server_rotate_parts->parts[idx];
        out->id = p->id;
        out->rotation = p->rotation;
        out->top = p->top;
        out->right = p->right;
        out->bottom = p->bottom;
        out->left = p->left;
        return;
    }
    // Repli : table indisponible (ex. entre le démarrage et la fin de
    // rotate_all_parts), pas de couleurs. Même formule que save_solution_csv.
    out->id = (int16_t)(idx % ETERN_PARTS);
    out->rotation = (int8_t)(idx / ETERN_PARTS);
    out->top = -1;
    out->right = -1;
    out->bottom = -1;
    out->left = -1;
}

void http_best_board_collect(http_best_board_view_t *out)
{
    memset(out, 0, sizeof(*out));
    struct possibility_packet board;
    uint16_t alloc = 0;
    out->has_board = best_board_get(&g_server_best_board, &board, &alloc) ? 1 : 0;
    if (!out->has_board) {
        return;
    }
    out->alloc = alloc;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            decode_best_board_cell(board.grid[x][y], &out->grid[x][y]);
        }
    }
}

/** @brief Formate et envoie une réponse ; ignore un échec d'envoi (client déjà parti). */
static void send_response(int socket_id, int status, const char *json_body)
{
    char response[HTTP_RESPONSE_MAX];
    int written = http_response_format(response, sizeof(response), status, json_body);
    if (written > 0) {
        send_all(socket_id, response, (size_t)written);
    }
}

/** @brief Traite POST /api/v1/command : extrait "command", l'applique via admin_apply_remote_command. */
static void handle_command_route(int socket_id, const http_request_t *req)
{
    char command[HTTP_COMMAND_MAX];
    int n = http_json_extract_string(req->body, req->body_len, "command", command, sizeof(command));
    if (n < 0) {
        send_response(socket_id, 400, "{\"error\":\"missing or malformed \\\"command\\\" field\"}");
        return;
    }

    int result = admin_apply_remote_command(command);
    if (result == ADMIN_CMD_OK) {
        send_response(socket_id, 200, "{\"result\":\"ok\"}");
    } else if (result == ADMIN_CMD_FORBIDDEN) {
        send_response(socket_id, 403, "{\"error\":\"command not allowed\"}");
    } else {
        send_response(socket_id, 400, "{\"error\":\"missing or invalid argument\"}");
    }
}

int handle_http_connection(int socket_id)
{
    char request_buf[HTTP_REQUEST_MAX];
    int32_t total = 0;
    http_request_t req;
    http_parse_result_t parse_result;

    do {
        ssize_t n = recv(socket_id, request_buf + total, sizeof(request_buf) - (size_t)total, 0);
        if (n <= 0) {
            return -1; /* connexion fermée par le pair, ou timeout d'E/S */
        }
        total += (int32_t)n;
        parse_result = http_request_parse(request_buf, total, &req);
    } while (parse_result == HTTP_PARSE_NEED_MORE && total < HTTP_REQUEST_MAX);

    if (parse_result == HTTP_PARSE_BAD) {
        send_response(socket_id, 400, "{\"error\":\"bad request\"}");
        return 0;
    }
    if (parse_result != HTTP_PARSE_OK) {
        send_response(socket_id, 413, "{\"error\":\"payload too large\"}");
        return 0;
    }

    char json[HTTP_RESPONSE_MAX];
    http_route_t route = http_route_resolve(req.method, req.path);
    switch (route) {
        case HTTP_ROUTE_STATS: {
            http_stats_view_t view;
            http_stats_collect(&view);
            if (http_json_format_stats(json, sizeof(json), &view) > 0) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
        case HTTP_ROUTE_STATUS: {
            http_status_view_t view;
            http_status_collect(&view);
            if (http_json_format_status(json, sizeof(json), &view) > 0) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
        case HTTP_ROUTE_COMMAND:
            handle_command_route(socket_id, &req);
            break;
        case HTTP_ROUTE_CLIENTS: {
            http_client_info_t infos[MAX_CONTROL_SESSIONS];
            int n = http_clients_collect(infos, MAX_CONTROL_SESSIONS);
            if (http_json_format_clients(json, sizeof(json), infos, n) > 0) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
        case HTTP_ROUTE_CLIENTS_STATS: {
            int n = control_registry_broadcast_get_stats();
            int written = snprintf(json, sizeof(json), "{\"result\":\"ok\",\"requested\":%d}", n);
            if (written > 0 && (size_t)written < sizeof(json)) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
        case HTTP_ROUTE_BEST_BOARD: {
            http_best_board_view_t view;
            http_best_board_collect(&view);
            if (http_json_format_best_board(json, sizeof(json), &view) > 0) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
        case HTTP_ROUTE_NOT_FOUND:
            send_response(socket_id, 404, "{\"error\":\"not found\"}");
            break;
        case HTTP_ROUTE_BAD_METHOD:
        default:
            send_response(socket_id, 405, "{\"error\":\"method not allowed\"}");
            break;
    }
    return 0;
}

static void *http_server_thread(void *arg)
{
    (void)arg;
    int listen_fd = g_http_listen_fd;

    while (request != REQUEST_STOP) {
        int client_fd = accept(listen_fd, NULL, 0);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            log_errno("http_server_thread : erreur sur accept() => ");
            break;
        }

        struct timeval tv = { .tv_sec = HTTP_IO_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handle_http_connection(client_fd);
        close(client_fd);
    }

    close(listen_fd);
    return NULL;
}
