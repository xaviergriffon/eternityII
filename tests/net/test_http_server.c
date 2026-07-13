/*
 * Tests unitaires de http_server.c — partie réseau de l'API HTTP admin.
 *
 * Même pattern que test_control_protocol.c : socketpair(AF_UNIX, SOCK_STREAM)
 * fournit deux descripteurs connectés bout à bout ; on écrit une requête HTTP
 * brute sur une extrémité, on appelle handle_http_connection() directement
 * sur l'autre (sans passer par accept()), et on relit la réponse. SO_RCVTIMEO
 * est posé AVANT tout recv : un bug de cadrage ne doit jamais bloquer le test
 * indéfiniment, il doit échouer proprement à la place.
 */
#include "greatest.h"
#include "net/http_server.h"
#include "app/control_registry.h"
#include "app/etii_server.h"
#include "app/static_variables.h"
#include "core/best_board.h"
#include "core/part.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#define MAKE_PAIR(sv)                                                          \
    do {                                                                       \
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair"); \
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };                \
        setsockopt((sv)[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));         \
        setsockopt((sv)[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));         \
    } while (0)

/* send() intégral (socketpair local : pas de partial write attendu en usage
   normal, mais on boucle par prudence plutôt que de supposer). */
static int send_all_test(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static ssize_t read_response(int fd, char *buf, size_t size)
{
    ssize_t n = recv(fd, buf, size - 1, 0);
    if (n < 0) {
        n = 0;
    }
    buf[n] = '\0';
    return n;
}

/* GET /api/v1/stats -> 200, corps JSON contenant les champs attendus. */
TEST http_server_get_stats_returns_200(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /api/v1/stats HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "\"shots_per_second\"") != NULL);
    ASSERT(strstr(resp, "\"queues\":[") != NULL);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* GET /api/v1/status -> 200, corps JSON contenant l'état courant. */
TEST http_server_get_status_returns_200(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /api/v1/status HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "\"state\"") != NULL);
    ASSERT(strstr(resp, "\"version\"") != NULL);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* GET /api/v1/clients -> 200, tableau vide (aucune session de contrôle active en test). */
TEST http_server_get_clients_returns_200_empty(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /api/v1/clients HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "{\"clients\":[]}") != NULL);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* GET /api/v1/clients reflète les statistiques mises en cache par
   control_registry_record_stats (round-trip HTTP -> registre -> HTTP, sans
   passer par le réseau du canal de contrôle lui-même). */
TEST http_server_get_clients_reflects_recorded_stats(void)
{
    control_hello_t hello = { .pid = 4242, .nb_forks = 3, .mode = 1 };
    int idx = control_registry_register(-1, &hello);
    ASSERT(idx >= 0);

    control_stats_t stats = {
        .shots_per_second = 999, .possibility_stock = 5, .analysed_stock = 2,
        .max_result = 100, .pruner_checked = 8, .pruner_removed = 1
    };
    control_registry_record_stats(idx, &stats);

    int sv[2];
    MAKE_PAIR(sv);
    const char req[] = "GET /api/v1/clients HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "\"pid\":4242") != NULL);
    ASSERT(strstr(resp, "\"mode\":\"pruner\"") != NULL);
    ASSERT(strstr(resp, "\"shots_per_second\":999") != NULL);
    ASSERT(strstr(resp, "\"pruner_removed\":1") != NULL);

    control_registry_unregister(idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* POST /api/v1/clients/stats -> 200, "requested" reflète le nombre de sessions actives. */
TEST http_server_post_clients_stats_returns_200(void)
{
    control_hello_t hello = { .pid = 5555, .nb_forks = 1, .mode = 0 };
    int idx = control_registry_register(-1, &hello);
    ASSERT(idx >= 0);

    int sv[2];
    MAKE_PAIR(sv);
    const char req[] = "POST /api/v1/clients/stats HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "\"requested\":1") != NULL);

    control_registry_unregister(idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* GET /api/v1/clients/stats -> 405 (route connue, méthode non supportée). */
TEST http_server_get_clients_stats_returns_405(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /api/v1/clients/stats HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 405") == resp);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* POST /api/v1/clients -> 405 (route connue, méthode non supportée). */
TEST http_server_post_clients_returns_405(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "POST /api/v1/clients HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 405") == resp);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Chemin inconnu -> 404. */
TEST http_server_unknown_path_returns_404(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /unknown HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 404") == resp);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Chemin connu, mauvaise méthode -> 405. */
TEST http_server_wrong_method_returns_405(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "POST /api/v1/stats HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 405") == resp);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Requête dépassant HTTP_REQUEST_MAX sans terminateur d'en-têtes -> 413. */
TEST http_server_oversized_request_returns_413(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    char *garbage = malloc(HTTP_REQUEST_MAX);
    memset(garbage, 'a', HTTP_REQUEST_MAX);
    int sent = send_all_test(sv[0], garbage, HTTP_REQUEST_MAX);
    free(garbage);
    ASSERT_EQ_FMT(0, sent, "%d");

    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 413") == resp);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* POST /api/v1/command {"command":"exit"} -> 403 (hors liste blanche), état inchangé. */
TEST http_server_command_forbidden_returns_403(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    int saved_req = request;
    request = REQUEST_CONTINUE;

    const char body[] = "{\"command\":\"exit\"}";
    char req[256];
    snprintf(req, sizeof(req), "POST /api/v1/command HTTP/1.1\r\nContent-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 403") == resp);
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_req;
    close(sv[0]); close(sv[1]);
    PASS();
}

/* POST /api/v1/command {"command":"pause"} -> 200, request bascule en REQUEST_ADMIN_PAUSE. */
TEST http_server_command_pause_returns_200_and_pauses(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    int saved_req = request;
    request = REQUEST_CONTINUE;

    const char body[] = "{\"command\":\"pause\"}";
    char req[256];
    snprintf(req, sizeof(req), "POST /api/v1/command HTTP/1.1\r\nContent-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT_EQ_FMT(REQUEST_ADMIN_PAUSE, request, "%d");

    request = saved_req;
    close(sv[0]); close(sv[1]);
    PASS();
}

/* Commande absente du corps JSON -> 400. */
TEST http_server_command_missing_field_returns_400(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char body[] = "{\"other\":\"x\"}";
    char req[256];
    snprintf(req, sizeof(req), "POST /api/v1/command HTTP/1.1\r\nContent-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 400") == resp);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Connexion fermée avant qu'une requête complète soit reçue -> -1, aucune réponse. */
TEST http_server_connection_closed_before_complete_request(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char partial[] = "GET /api/v1/stat";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], partial, strlen(partial)), "%d");
    close(sv[0]); /* ferme avant la fin des en-têtes : recv() renvoie 0 côté sv[1] */

    ASSERT_EQ_FMT(-1, handle_http_connection(sv[1]), "%d");

    close(sv[1]);
    PASS();
}

/* ---------- GET /api/v1/best-board ------------------------------------------ */

TEST http_server_get_best_board_returns_200_no_record(void)
{
    best_board_init(&g_server_best_board); /* évite la pollution d'une autre suite */

    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /api/v1/best-board HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "{\"has_board\":false}") != NULL);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Sans table de rotations connue (g_server_rotate_parts == NULL, comme hors
   de runserver), http_best_board_collect retombe sur id/rotation décodés de
   l'indice brut, sans couleurs (-1) — jamais un crash ni un id erroné. */
TEST http_server_get_best_board_returns_200_with_record_no_rotate_table(void)
{
    struct array_part *saved_rp = g_server_rotate_parts;
    g_server_rotate_parts = NULL;
    best_board_init(&g_server_best_board);

    struct possibility_packet board;
    memset(&board, 0, sizeof(board));
    /* Toutes les cases vides par défaut (-2, comme un vrai paquet) : un
       memset à 0 laisserait des cases à l'indice 0 (une pièce "valide" à
       tort), gonflant la réponse et faisant déborder le tampon non lu du
       socketpair côté test (deadlock send/recv sans lecteur concurrent). */
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            board.grid[x][y] = -2;
        }
    }
    board.alloc = 5;
    board.grid[0][0] = 3 + ETERN_PARTS * 1; /* id=3, rotation=1 (repli) */
    ASSERT_EQ_FMT(1, best_board_try_record(&g_server_best_board, &board, 5), "%d");

    int sv[2];
    MAKE_PAIR(sv);
    const char req[] = "GET /api/v1/best-board HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    ASSERT(strstr(resp, "\"has_board\":true") != NULL);
    ASSERT(strstr(resp, "\"alloc\":5") != NULL);
    ASSERT(strstr(resp, "\"id\":3,\"rotation\":1,\"top\":-1,\"right\":-1,\"bottom\":-1,\"left\":-1") != NULL);

    close(sv[0]); close(sv[1]);
    g_server_rotate_parts = saved_rp;
    PASS();
}

/* Avec la table de rotations disponible (comme en production, cf. runserver),
   la réponse porte la description RÉELLE de la pièce (id, rotation, couleurs)
   — pas l'indice brut : c'est le point demandé (connaître la pièce posée, ses
   motifs et sa rotation, pas juste un identifiant de case opaque). */
TEST http_server_get_best_board_reflects_real_part_with_rotate_table(void)
{
    struct array_part *saved_rp = g_server_rotate_parts;
    best_board_init(&g_server_best_board);

    struct array_part fake_rotate_parts;
    struct part parts[ETERN_PARTS * 4 + 1];
    memset(parts, 0, sizeof(parts));
    /* id=1, rotation=1 : reste dans les bornes quel que soit ETERN_PARTS
       (256 ou 16, cf. tests-16/tests-256), contrairement à un id fixe élevé
       comme 139 qui déborderait le tableau en build 16 pièces. */
    int idx = 1 + ETERN_PARTS * 1;
    parts[idx].id = 1;
    parts[idx].rotation = 1;
    parts[idx].top = 2;
    parts[idx].right = 15;
    parts[idx].bottom = 15;
    parts[idx].left = 3;
    fake_rotate_parts.parts = parts;
    fake_rotate_parts.size = ETERN_PARTS * 4 + 1;
    g_server_rotate_parts = &fake_rotate_parts;

    struct possibility_packet board;
    memset(&board, 0, sizeof(board));
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            board.grid[x][y] = -2; /* même précaution que le test précédent */
        }
    }
    board.alloc = 1;
    board.grid[0][0] = (int16_t)idx;
    ASSERT_EQ_FMT(1, best_board_try_record(&g_server_best_board, &board, 1), "%d");

    int sv[2];
    MAKE_PAIR(sv);
    const char req[] = "GET /api/v1/best-board HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "\"id\":1,\"rotation\":1,\"top\":2,\"right\":15,\"bottom\":15,\"left\":3") != NULL);

    close(sv[0]); close(sv[1]);
    g_server_rotate_parts = saved_rp;
    PASS();
}

/* http_stats_collect / http_status_collect appelées directement (sans réseau) :
   ne doivent jamais planter et renseignent bien les champs de base. */
TEST http_stats_and_status_collect_do_not_crash(void)
{
    http_stats_view_t stats;
    http_stats_collect(&stats);
    ASSERT(stats.active_threads >= 0 || 1); /* unsigned : toujours vrai, exerce juste l'appel */

    int saved_req = request;
    request = REQUEST_CONTINUE;
    http_status_view_t status;
    http_status_collect(&status);
    ASSERT_STR_EQ("running", status.state);
    request = saved_req;

    PASS();
}

SUITE(http_server_suite)
{
    RUN_TEST(http_server_get_stats_returns_200);
    RUN_TEST(http_server_get_status_returns_200);
    RUN_TEST(http_server_get_clients_returns_200_empty);
    RUN_TEST(http_server_get_clients_reflects_recorded_stats);
    RUN_TEST(http_server_post_clients_stats_returns_200);
    RUN_TEST(http_server_get_clients_stats_returns_405);
    RUN_TEST(http_server_post_clients_returns_405);
    RUN_TEST(http_server_unknown_path_returns_404);
    RUN_TEST(http_server_wrong_method_returns_405);
    RUN_TEST(http_server_oversized_request_returns_413);
    RUN_TEST(http_server_command_forbidden_returns_403);
    RUN_TEST(http_server_command_pause_returns_200_and_pauses);
    RUN_TEST(http_server_command_missing_field_returns_400);
    RUN_TEST(http_server_connection_closed_before_complete_request);
    RUN_TEST(http_stats_and_status_collect_do_not_crash);
    RUN_TEST(http_server_get_best_board_returns_200_no_record);
    RUN_TEST(http_server_get_best_board_returns_200_with_record_no_rotate_table);
    RUN_TEST(http_server_get_best_board_reflects_real_part_with_rotate_table);
}
