#include "net/http_server.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "app/control_registry.h"
#include "app/known_clients_registry.h"
#include "app/etii_server.h"
#include "app/app_static_variables.h"
#include "core/datamanager.h"
#include "core/stock_spill.h"
#include "core/best_board.h"
#include "net/control_protocol.h"
#include "net/etii_protocol.h"
#include "net/tcpserver.h"
#include "ui/command_lines.h"
#include "ui/logger.h"

/// Délai (microsecondes) infligé sur un jeton PRÉSENT mais invalide, avant de
/// répondre 401 — anti-bruteforce minimal. Le serveur HTTP admin sert une
/// connexion à la fois (accept séquentiel, cf. doc du module) : ralentir
/// cette unique voie suffit à rendre un essai exhaustif de jetons peu
/// pratique, sans le moindre état à maintenir (pas de compteur, pas de
/// fenêtre glissante). Jamais infligé sur un jeton ABSENT : un client qui
/// n'essaie même pas de s'authentifier n'a rien à décourager.
#define HTTP_AUTH_FAIL_DELAY_US 200000

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
    out->stock_spilled_packets = stock_spill_total_packets();
    out->stock_spill_segments = stock_spill_total_segments();

    stock_rate_stats_t rate;
    datamanager_stock_rate_stats(&rate);
    out->stock_adds_last_1m = rate.adds_last_1m;
    out->stock_adds_last_1h = rate.adds_last_1h;
    out->stock_adds_last_1d = rate.adds_last_1d;
    out->stock_removes_last_1m = rate.removes_last_1m;
    out->stock_removes_last_1h = rate.removes_last_1h;
    out->stock_removes_last_1d = rate.removes_last_1d;
    out->stock_adds_checked_last_1m = rate.adds_checked_last_1m;
    out->stock_adds_checked_last_1h = rate.adds_checked_last_1h;
    out->stock_adds_checked_last_1d = rate.adds_checked_last_1d;
    out->stock_adds_unchecked_last_1m = rate.adds_unchecked_last_1m;
    out->stock_adds_unchecked_last_1h = rate.adds_unchecked_last_1h;
    out->stock_adds_unchecked_last_1d = rate.adds_unchecked_last_1d;
    out->stock_removes_checked_last_1m = rate.removes_checked_last_1m;
    out->stock_removes_checked_last_1h = rate.removes_checked_last_1h;
    out->stock_removes_checked_last_1d = rate.removes_checked_last_1d;
    out->stock_removes_unchecked_last_1m = rate.removes_unchecked_last_1m;
    out->stock_removes_unchecked_last_1h = rate.removes_unchecked_last_1h;
    out->stock_removes_unchecked_last_1d = rate.removes_unchecked_last_1d;

    // Métriques de besoin par rôle (PR2, docs/conception/pilotage_type_client.md).
    out->server_search_starved = server_search_starved;
    out->server_prune_starved = server_prune_starved;

    control_session_info_t sessions[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(sessions, MAX_CONTROL_SESSIONS);
    int nb_search = 0, nb_prune = 0;
    control_registry_count_roles(sessions, n, &nb_search, &nb_prune);
    out->nb_search_sessions = (unsigned long long)nb_search;
    out->nb_prune_sessions = (unsigned long long)nb_prune;

    unsigned long long unchecked_total = 0, checked_total = 0, analysed_total = 0;
    for (int f = 0; f < nb_file_possibility; f++) {
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

void http_stock_distribution_collect(http_stock_distribution_view_t *out)
{
    stock_distribution_t distribution;
    datamanager_stock_distribution(&distribution);

    memset(out, 0, sizeof(*out));
    for (int level = 0; level < STOCK_DISTRIBUTION_LEVELS; level++) {
        out->unchecked[level] = distribution.unchecked[level];
        out->checked[level] = distribution.checked[level];
        out->analysed[level] = distribution.analysed[level];
        out->unchecked_min_candidats_sum[level] = distribution.unchecked_min_candidats_sum[level];
        out->unchecked_min_candidats_known[level] = distribution.unchecked_min_candidats_known[level];
        out->checked_min_candidats_sum[level] = distribution.checked_min_candidats_sum[level];
        out->checked_min_candidats_known[level] = distribution.checked_min_candidats_known[level];
        out->analysed_min_candidats_sum[level] = distribution.analysed_min_candidats_sum[level];
        out->analysed_min_candidats_known[level] = distribution.analysed_min_candidats_known[level];
    }
    out->total_unchecked = distribution.total_unchecked;
    out->total_checked = distribution.total_checked;
    out->total_analysed = distribution.total_analysed;
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
    out->pruner_dfs_budget = pruner_dfs_budget;
    out->last_backup_duration_ms = server_last_backup_duration_ms;
    out->stock_ram_limit_mb = datamanager_packets_to_ram_mb(datamanager_ram_limit_packets());
    out->stock_ram_used_mb = datamanager_packets_to_ram_mb(datamanager_resident_packets());
}

/* Le memcpy champ-à-champ ci-dessous suppose ces tailles identiques (voir
 * commentaire dans http_codec.h sur la duplication délibérée des tailles) :
 * une divergence future doit casser la compilation, pas fuiter un octet. */
_Static_assert(sizeof(((http_client_info_t *)0)->label) == CLIENT_LABEL_MAX,
               "http_client_info_t.label doit matcher CLIENT_LABEL_MAX");
_Static_assert(sizeof(((http_client_info_t *)0)->machine_uid_hex) == 2 * MACHINE_UID_BYTES + 1,
               "http_client_info_t.machine_uid_hex doit matcher 2*MACHINE_UID_BYTES+1");
_Static_assert(sizeof(((http_client_info_t *)0)->client_uid_hex) == 2 * CLIENT_UID_BYTES + 1,
               "http_client_info_t.client_uid_hex doit matcher 2*CLIENT_UID_BYTES+1");
_Static_assert(sizeof(((http_client_info_t *)0)->peer_ip) == PEER_IP_MAX_LEN,
               "http_client_info_t.peer_ip doit matcher PEER_IP_MAX_LEN");
_Static_assert(sizeof(((http_known_client_info_t *)0)->machine_uid_hex) == 2 * MACHINE_UID_BYTES + 1,
               "http_known_client_info_t.machine_uid_hex doit matcher 2*MACHINE_UID_BYTES+1");
_Static_assert(sizeof(((http_known_client_info_t *)0)->label) == CLIENT_LABEL_MAX,
               "http_known_client_info_t.label doit matcher CLIENT_LABEL_MAX");
_Static_assert(sizeof(((http_known_client_info_t *)0)->peer_ip) == PEER_IP_MAX_LEN,
               "http_known_client_info_t.peer_ip doit matcher PEER_IP_MAX_LEN");

int http_clients_collect(http_client_info_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int cap = (max < MAX_CONTROL_SESSIONS) ? max : MAX_CONTROL_SESSIONS;
    int n = control_registry_snapshot(infos, cap);
    for (int i = 0; i < n; i++) {
        out[i].session_no = (unsigned long long)infos[i].session_no;
        out[i].pid = infos[i].pid;
        out[i].nb_forks = infos[i].nb_forks;
        out[i].mode = infos[i].mode;
        memcpy(out[i].label, infos[i].label, sizeof(out[i].label));
        memcpy(out[i].machine_uid_hex, infos[i].machine_uid_hex, sizeof(out[i].machine_uid_hex));
        memcpy(out[i].client_uid_hex, infos[i].client_uid_hex, sizeof(out[i].client_uid_hex));
        memcpy(out[i].peer_ip, infos[i].peer_ip, sizeof(out[i].peer_ip));
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

int http_known_clients_collect(http_known_client_info_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    known_client_info_t infos[MAX_KNOWN_CLIENTS];
    int cap = (max < MAX_KNOWN_CLIENTS) ? max : MAX_KNOWN_CLIENTS;
    int n = known_clients_registry_snapshot(infos, cap);
    for (int i = 0; i < n; i++) {
        memcpy(out[i].machine_uid_hex, infos[i].machine_uid_hex, sizeof(out[i].machine_uid_hex));
        memcpy(out[i].label, infos[i].label, sizeof(out[i].label));
        memcpy(out[i].peer_ip, infos[i].peer_ip, sizeof(out[i].peer_ip));
        out[i].mode = infos[i].mode;
        out[i].connected = infos[i].connected;
        out[i].nb_active_sessions = infos[i].nb_active_sessions;
        out[i].nb_active_search = infos[i].nb_active_search;
        out[i].nb_active_prune = infos[i].nb_active_prune;
        out[i].nb_connections_total = infos[i].nb_connections_total;
        out[i].first_seen = (long long)infos[i].first_seen;
        out[i].last_seen = (long long)infos[i].last_seen;
        out[i].total_pruner_checked = (unsigned long long)infos[i].total_pruner_checked;
        out[i].total_pruner_removed = (unsigned long long)infos[i].total_pruner_removed;
        out[i].best_max_result = (unsigned long long)infos[i].best_max_result;
        out[i].cumulative_uptime_seconds = (unsigned long long)infos[i].cumulative_uptime_seconds;
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
    out->min_candidats = board.min_candidats;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            decode_best_board_cell(board.grid[x][y], &out->grid[x][y]);
        }
    }
}

int http_token_load(const char *path, char *out, size_t out_size)
{
    if (path == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        log_error("--http-token-file : impossible d'accéder à \"%s\"\n", path);
        return -1;
    }
    // Permissions autres que propriétaire-seul refusées (mode & 0077 != 0) :
    // même exigence qu'une clé privée SSH. Vérifié AVANT l'ouverture, pour ne
    // jamais lire un jeton potentiellement exposé à d'autres comptes de la
    // machine, même en cas d'échec plus loin.
    if ((st.st_mode & 0077) != 0) {
        log_error("--http-token-file : permissions trop ouvertes sur \"%s\" "
                  "(attendu propriétaire seul, ex. chmod 600)\n", path);
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        log_error("--http-token-file : impossible d'ouvrir \"%s\" en lecture\n", path);
        return -1;
    }
    char line[HTTP_ADMIN_TOKEN_MAX + 2];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (got == NULL) {
        log_error("--http-token-file : \"%s\" est vide ou illisible\n", path);
        return -1;
    }

    // Trailing whitespace/retour à la ligne retiré (fichier généralement créé
    // par un éditeur ou `echo`, terminé par \n).
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
                        || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        log_error("--http-token-file : jeton vide dans \"%s\"\n", path);
        return -1;
    }
    if (len >= out_size) {
        log_error("--http-token-file : jeton trop long dans \"%s\" (max %zu octets)\n",
                  path, out_size - 1);
        return -1;
    }

    memcpy(out, line, len + 1);
    return (int)len;
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

/** @brief Formate et envoie une réponse 401 (en-tête WWW-Authenticate inclus). */
static void send_response_unauthorized(int socket_id, const char *json_body)
{
    char response[HTTP_RESPONSE_MAX];
    int written = http_response_format_unauthorized(response, sizeof(response), json_body);
    if (written > 0) {
        send_all(socket_id, response, (size_t)written);
    }
}

/**
 * @brief Traite POST /api/v1/command : extrait "command", décide de son
 *        autorisation, puis l'applique via `admin_apply_privileged_command`.
 *
 * **Toute commande de modification requiert un jeton Bearer valide** — seule
 * exception, `clientsWork` (`CTRL_CMD_READ_ONLY`), une consultation pure qui
 * ne change aucun état. La classification par `control_command_classify`
 * (`net/control_protocol.h`) réduit ici à deux drapeaux : `is_public`
 * (`CTRL_CMD_READ_ONLY`, jamais de jeton requis) et `needs_auth` (toute
 * commande reconnue mais PAS `is_public` — regroupe indifféremment les
 * commandes relayables à un client, `CTRL_CMD_WRITE_RELAYABLE` : `pause`,
 * `resume`, `limit`, `maxStockByThread`, `prunerBatch`,
 * `clientsCommand`/`clientsCmd`, `start`/`stopForks`/`configApply`/`config`/
 * `configSave`, et les commandes serveur-seulement, `CTRL_CMD_WRITE_SERVER_ONLY` :
 * `restore`/`backup`/`sortAsc`/`sortDesc`/`sortDescMulti`/`split`/`regroup` —
 * ces deux catégories exigent aujourd'hui exactement le même jeton, seule
 * leur relayabilité vers un client diffère, un axe sans effet sur cette
 * route). Le jeton n'est extrait/comparé QUE si `needs_auth` est vrai — une
 * commande purement lecture (`clientsWork`, ou une route `GET`) n'en a jamais
 * eu besoin. Sans `--http-token-file` configuré, toute commande de
 * modification reste donc inaccessible via cette API (401), quel que soit
 * l'en-tête fourni — seule la lecture (GET, `clientsWork`) fonctionne sans
 * jeton.
 */
static void handle_command_route(int socket_id, const http_request_t *req)
{
    char command[HTTP_COMMAND_MAX];
    int n = http_json_extract_string(req->body, req->body_len, "command", command, sizeof(command));
    if (n < 0) {
        send_response(socket_id, 400, "{\"error\":\"missing or malformed \\\"command\\\" field\"}");
        return;
    }

    control_command_class_t cmd_class = control_command_classify(command);
    int is_public = (cmd_class == CTRL_CMD_READ_ONLY);
    int needs_auth = (cmd_class == CTRL_CMD_WRITE_RELAYABLE || cmd_class == CTRL_CMD_WRITE_SERVER_ONLY);
    int has_configured_token = HTTP_ADMIN_TOKEN[0] != '\0';
    int token_present = 0;
    int token_valid = 0;

    if (needs_auth) {
        char token[HTTP_ADMIN_TOKEN_MAX];
        token_present = (http_extract_bearer_token(req->authorization, token, sizeof(token)) > 0);
        if (token_present && has_configured_token) {
            token_valid = http_token_equals_constant_time(token, HTTP_ADMIN_TOKEN, HTTP_ADMIN_TOKEN_MAX);
        }
    }

    http_cmd_auth_result_t decision = http_command_authorize(is_public, needs_auth, has_configured_token, token_valid);

    if (decision == HTTP_CMD_AUTH_FORBIDDEN) {
        send_response(socket_id, 403, "{\"error\":\"command not allowed\"}");
        return;
    }
    if (decision == HTTP_CMD_AUTH_UNAUTHORIZED) {
        if (token_present) {
            // Jeton PRÉSENT mais invalide (ou aucun jeton configuré côté
            // serveur) : anti-bruteforce avant de répondre. Jamais le jeton
            // lui-même dans les logs.
            usleep(HTTP_AUTH_FAIL_DELAY_US);
            log_error("commande \"%s\" (authentification requise) refusée via l'API HTTP admin : jeton invalide\n", command);
        } else {
            log_error("commande \"%s\" (authentification requise) refusée via l'API HTTP admin : jeton absent\n", command);
        }
        send_response_unauthorized(socket_id, "{\"error\":\"unauthorized\"}");
        return;
    }

    if (needs_auth) {
        log_info("commande \"%s\" (authentification requise) exécutée via l'API HTTP admin\n", command);
    }

    int result = admin_apply_privileged_command(command);
    if (result == ADMIN_CMD_OK) {
        send_response(socket_id, 200, "{\"result\":\"ok\"}");
    } else if (result == ADMIN_CMD_FORBIDDEN) {
        // Ne devrait plus se produire ici (décision déjà tranchée ci-dessus),
        // conservé par prudence défensive plutôt que par nécessité.
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
        case HTTP_ROUTE_KNOWN_CLIENTS: {
            http_known_client_info_t infos[MAX_KNOWN_CLIENTS];
            int n = http_known_clients_collect(infos, MAX_KNOWN_CLIENTS);
            if (http_json_format_known_clients(json, sizeof(json), infos, n) > 0) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
        case HTTP_ROUTE_STOCK_DISTRIBUTION: {
            http_stock_distribution_view_t view;
            http_stock_distribution_collect(&view);
            if (http_json_format_stock_distribution(json, sizeof(json), &view) > 0) {
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
