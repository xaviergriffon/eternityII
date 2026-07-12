#include "net/http_server.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "app/etii_server.h"
#include "app/static_variables.h"
#include "core/datamanager.h"
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
