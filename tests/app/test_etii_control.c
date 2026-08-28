/*
 * Tests unitaires du canal de contrôle côté client (src/app/etii_control.c).
 *
 * control_channel_handle_frame traite UNE trame déjà reçue : on l'exerce
 * directement par socketpair(AF_UNIX, SOCK_STREAM) sans passer par
 * ctrl_recv_frame côté "serveur" simulé — le test joue le rôle du serveur et
 * lit la réponse avec ctrl_recv_frame sur l'autre extrémité. SO_RCVTIMEO est
 * posé AVANT tout recv (leçon déjà tirée sur ce projet : un bug de cadrage ne
 * doit jamais bloquer le test indéfiniment).
 */
#include "greatest.h"
#include "app/etii_control.h"
#include "app/app_static_variables.h"
#include "app/fork_gate.h"
#include "net/control_protocol.h"
#include "net/etii_protocol.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define MAKE_PAIR(sv)                                                       \
    do {                                                                    \
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair"); \
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };             \
        setsockopt((sv)[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));      \
        setsockopt((sv)[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));      \
    } while (0)

/* CTRL_PING -> CTRL_ACK. */
TEST control_channel_ping_replies_ack(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    ASSERT_EQ_FMT(0, control_channel_handle_frame(sv[0], CTRL_PING, NULL, 0), "%d");

    void *payload = NULL;
    int32_t len = -1;
    int cmd = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT((int)CTRL_ACK, cmd, "%d");
    ASSERT_EQ_FMT(0, len, "%d");
    ASSERT_EQ(NULL, payload);

    close(sv[0]); close(sv[1]);
    PASS();
}

/* CTRL_GET_STATS -> CTRL_STATS décodable, agrégeant fork_statistics[]. */
TEST control_channel_get_stats_replies_aggregated_stats(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved_fs = fork_statistics;

    struct client_statistics fs[2];
    memset(fs, 0, sizeof fs);
    fs[0].shots_per_second = 100; fs[0].possibilities_in_stock = 10;
    fs[0].analyses_in_stock = 1;  fs[0].max_result = 20;
    fs[0].pruner_checked = 5;     fs[0].pruner_removed = 2;
    fs[0].pruner_cells_per_second = 30;
    fs[1].shots_per_second = 200; fs[1].possibilities_in_stock = 30;
    fs[1].analyses_in_stock = 3;  fs[1].max_result = 40;
    fs[1].pruner_checked = 7;     fs[1].pruner_removed = 1;
    fs[1].pruner_cells_per_second = 15;
    NB_THREADS = 2;
    fork_statistics = fs;
    max_result = 0;

    int sv[2];
    MAKE_PAIR(sv);

    int rc = control_channel_handle_frame(sv[0], CTRL_GET_STATS, NULL, 0);

    fork_statistics = saved_fs; NB_THREADS = saved_nb; max_result = saved_mr;

    ASSERT_EQ_FMT(0, rc, "%d");

    void *payload = NULL;
    int32_t len = -1;
    int cmd = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT((int)CTRL_STATS, cmd, "%d");
    ASSERT(payload != NULL);

    control_stats_t stats;
    memset(&stats, 0, sizeof stats);
    int decoded = control_stats_decode((const uint8_t *)payload, len, &stats);
    free(payload);
    ASSERT_EQ_FMT(0, decoded, "%d");

    ASSERT_EQ_FMT((unsigned long long)300, (unsigned long long)stats.shots_per_second, "%llu");
    ASSERT_EQ_FMT((unsigned long long)40, (unsigned long long)stats.possibility_stock, "%llu");
    ASSERT_EQ_FMT((unsigned long long)4, (unsigned long long)stats.analysed_stock, "%llu");
    ASSERT_EQ_FMT((unsigned long long)40, (unsigned long long)stats.max_result, "%llu");
    ASSERT_EQ_FMT((unsigned long long)12, (unsigned long long)stats.pruner_checked, "%llu");
    ASSERT_EQ_FMT((unsigned long long)3, (unsigned long long)stats.pruner_removed, "%llu");
    ASSERT_EQ_FMT((unsigned long long)45, (unsigned long long)stats.pruner_cells_per_second, "%llu");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* CTRL_COMMAND, commande autorisée ("limit") : exécutée, CTRL_RESULT >= 0. */
TEST control_channel_command_allowed_is_executed(void)
{
    unsigned long long saved = max_search_by_sec;

    int sv[2];
    MAKE_PAIR(sv);

    const char *cmd_line = "limit 4242";
    int rc = control_channel_handle_frame(sv[0], CTRL_COMMAND, cmd_line, (int32_t)strlen(cmd_line));
    ASSERT_EQ_FMT(0, rc, "%d");

    void *payload = NULL;
    int32_t len = -1;
    int cmd = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT((int)CTRL_RESULT, cmd, "%d");
    ASSERT(payload != NULL);
    ASSERT_EQ_FMT((int32_t)sizeof(int32_t), len, "%d");

    int32_t result = -999;
    memcpy(&result, payload, sizeof(result));
    free(payload);

    /* La commande a bien été exécutée : effet de bord observable. */
    ASSERT_EQ_FMT(4242ULL, max_search_by_sec, "%llu");
    ASSERT_EQ_FMT(0, result, "%d");

    max_search_by_sec = saved;
    close(sv[0]); close(sv[1]);
    PASS();
}

/* CTRL_COMMAND, commande interdite ("exit") : défense en profondeur -- REFUSÉE
   sans exécution (sinon exit_interpreter poserait REQUEST_STOP et enverrait
   des signaux aux enfants, désastreux à l'intérieur du runner de tests). */
TEST control_channel_command_forbidden_is_not_executed(void)
{
    int saved_request = request;

    int sv[2];
    MAKE_PAIR(sv);

    const char *cmd_line = "exit";
    int rc = control_channel_handle_frame(sv[0], CTRL_COMMAND, cmd_line, (int32_t)strlen(cmd_line));
    ASSERT_EQ_FMT(0, rc, "%d");

    void *payload = NULL;
    int32_t len = -1;
    int cmd = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT((int)CTRL_RESULT, cmd, "%d");
    ASSERT(payload != NULL);
    ASSERT_EQ_FMT((int32_t)sizeof(int32_t), len, "%d");

    int32_t result = 0;
    memcpy(&result, payload, sizeof(result));
    free(payload);

    /* Refusée : résultat négatif, ET la commande n'a PAS été exécutée --
       request est resté inchangé (exit_interpreter l'aurait mis à REQUEST_STOP). */
    ASSERT(result < 0);
    ASSERT_EQ_FMT(saved_request, request, "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Commande de trame inconnue : journalisée, ignorée, pas de réponse envoyée
   (donc rien à lire côté pair : on vérifie juste l'absence de crash / d'erreur
   de retour, et que la session n'est pas jugée perdue). */
TEST control_channel_unknown_frame_is_ignored(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    int rc = control_channel_handle_frame(sv[0], 200 /* cmd inconnue */, NULL, 0);
    ASSERT_EQ_FMT(0, rc, "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* control_channel_build_stats : agrégat pur, testable sans socket. */
TEST control_channel_build_stats_aggregates(void)
{
    int saved_nb = NB_THREADS;
    uint16_t saved_mr = max_result;
    struct client_statistics *saved_fs = fork_statistics;

    struct client_statistics fs[1];
    memset(fs, 0, sizeof fs);
    fs[0].shots_per_second = 42;
    fs[0].max_result = 7;
    NB_THREADS = 1;
    fork_statistics = fs;
    max_result = 99; /* record global supérieur au max des forks */

    control_stats_t stats;
    control_channel_build_stats(&stats);

    fork_statistics = saved_fs; NB_THREADS = saved_nb; max_result = saved_mr;

    ASSERT_EQ_FMT((unsigned long long)42, (unsigned long long)stats.shots_per_second, "%llu");
    /* Le record global (99) l'emporte sur le max_result du fork (7). */
    ASSERT_EQ_FMT((unsigned long long)99, (unsigned long long)stats.max_result, "%llu");
    PASS();
}

/* ==================== run_control_channel (fork_gate wiring) ================ */

/*
 * Régression réelle, rapportée en production : au boot, seul 1 fork sur 5
 * était créé, les 4 autres refusés par `orchestrator_spawn_forks` avec
 * "quiescence non atteinte" — laissant ces threads durablement à
 * shots/s=0, stock=0, analyse=0 (pas un bug d'affichage : les process
 * n'existent simplement jamais).
 *
 * Cause : `run_control_channel`'s boucle de service appelle
 * `fork_gate_checkpoint(gate_slot)` UNE FOIS avant chaque `ctrl_recv_frame`,
 * mais cet appel peut ensuite bloquer jusqu'à `tcp_timeout` secondes (10s par
 * défaut, SO_RCVTIMEO) en attendant la prochaine trame du SERVEUR — largement
 * au-delà du budget de `fork_gate_request_quiesce`
 * (`FORK_GATE_DEFAULT_TIMEOUT_MS`, 2s). Un premier `start` au boot pouvait
 * réussir par chance (le canal venait tout juste de se connecter, pas encore
 * entré dans une longue attente), mais TOUTE tentative de fork suivante dans
 * la même rafale (`orchestrator_spawn_forks` les tente en boucle serrée)
 * tombait presque systématiquement pendant cette fenêtre bloquante.
 *
 * Ce test reproduit le scénario avec un VRAI serveur TCP factice en
 * boucle locale : accepte la connexion, répond au handshake de version, puis
 * reste SILENCIEUX plus longtemps que le budget de quiescence — exactement
 * la situation qui bloquait `ctrl_recv_frame`. Si le correctif
 * (`fork_gate_mark_blocked` autour de l'appel bloquant, même patron que la
 * console autour de son `read()`) fonctionne, `fork_gate_request_quiesce`
 * doit réussir BIEN AVANT son propre budget, malgré ce silence prolongé.
 */
typedef struct {
    int listen_fd;
    int accepted_fd;
} mock_server_ctx_t;

static void *mock_slow_server_thread(void *arg)
{
    mock_server_ctx_t *ctx = (mock_server_ctx_t *)arg;
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept(ctx->listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        return NULL;
    }
    ctx->accepted_fd = fd;

    /* Handshake de version : lit INST_CHECK_VERSION + la version (int),
       répond INST_SUPPORTED_VERSION -- même trame que le vrai serveur. */
    int8_t inst = 0;
    if (recv(fd, &inst, sizeof(inst), 0) != (ssize_t)sizeof(inst)) {
        return NULL;
    }
    int version_recv = 0;
    if (recv(fd, &version_recv, sizeof(version_recv), 0) != (ssize_t)sizeof(version_recv)) {
        return NULL;
    }
    int8_t reply = INST_SUPPORTED_VERSION;
    if (send(fd, &reply, sizeof(reply), 0) != (ssize_t)sizeof(reply)) {
        return NULL;
    }

    /* Puis SILENCE prolongé -- aucun CTRL_PING -- jusqu'à ce que le test
       ferme la connexion (fin d'assertion) ou que le socket erreure. */
    char buf[8];
    recv(fd, buf, sizeof(buf), 0); /* débloqué par close() côté test */
    return NULL;
}

TEST run_control_channel_stays_quiescible_during_long_blocking_recv(void)
{
    fork_gate_reset();

    /* Sauvegarde des globales touchées. */
    volatile int saved_request = request;
    int saved_tcp_timeout = tcp_timeout;
    int saved_server_port = SERVER_PORT;

    request = REQUEST_CONTINUE;
    /* Assez long pour qu'un budget de quiescence de 1.5s soit ATTEINT très
       largement avant que ctrl_recv_frame n'ait la moindre chance de
       retourner de lui-même par timeout naturel -- sans quoi le test ne
       prouverait rien (un simple timeout naturel masquerait un correctif
       manquant). */
    tcp_timeout = 4;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(listen_fd >= 0);
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* port éphémère */
    ASSERT_EQ_FMT(0, bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), "%d");
    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ_FMT(0, getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len), "%d");
    ASSERT_EQ_FMT(0, listen(listen_fd, 1), "%d");

    SERVER_PORT = (int)ntohs(addr.sin_port);

    mock_server_ctx_t ctx = { .listen_fd = listen_fd, .accepted_fd = -1 };
    pthread_t server_thread;
    ASSERT_EQ_FMT(0, pthread_create(&server_thread, NULL, mock_slow_server_thread, &ctx), "%d");

    start_control_channel("127.0.0.1");

    /* Laisse le temps au thread réel de se connecter en boucle locale, faire
       le handshake, envoyer son hello, et entrer dans sa boucle de service
       (bloquée dans ctrl_recv_frame). Largement suffisant en local. */
    usleep(300000);

    fork_gate_result_t rc = fork_gate_request_quiesce(1500);

    fork_gate_release_quiesce();

    /* Nettoyage : débloque le thread réel (ferme la connexion -> son
       ctrl_recv_frame échoue -> reconnexion tentée -> checkpoint voit
       REQUEST_STOP -> sort) et le mock serveur. Le thread réel est DÉTACHÉ
       (start_control_channel), donc pas de pthread_join possible dessus --
       laisser un court délai suffit pour la suite de la suite de tests. */
    request = REQUEST_STOP;
    if (ctx.accepted_fd >= 0) {
        shutdown(ctx.accepted_fd, SHUT_RDWR);
        close(ctx.accepted_fd);
    }
    close(listen_fd);
    pthread_join(server_thread, NULL);
    usleep(50000);

    request = saved_request;
    tcp_timeout = saved_tcp_timeout;
    SERVER_PORT = saved_server_port;
    fork_gate_reset();

    ASSERT_EQ_FMT((int)FORK_GATE_QUIESCED, (int)rc, "%d");
    PASS();
}

SUITE(etii_control_suite)
{
    RUN_TEST(control_channel_ping_replies_ack);
    RUN_TEST(control_channel_get_stats_replies_aggregated_stats);
    RUN_TEST(control_channel_command_allowed_is_executed);
    RUN_TEST(control_channel_command_forbidden_is_not_executed);
    RUN_TEST(control_channel_unknown_frame_is_ignored);
    RUN_TEST(control_channel_build_stats_aggregates);
    RUN_TEST(run_control_channel_stays_quiescible_during_long_blocking_recv);
}
