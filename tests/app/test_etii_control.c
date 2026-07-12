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
#include "app/static_variables.h"
#include "net/control_protocol.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

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
    fs[1].shots_per_second = 200; fs[1].possibilities_in_stock = 30;
    fs[1].analyses_in_stock = 3;  fs[1].max_result = 40;
    fs[1].pruner_checked = 7;     fs[1].pruner_removed = 1;
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

SUITE(etii_control_suite)
{
    RUN_TEST(control_channel_ping_replies_ack);
    RUN_TEST(control_channel_get_stats_replies_aggregated_stats);
    RUN_TEST(control_channel_command_allowed_is_executed);
    RUN_TEST(control_channel_command_forbidden_is_not_executed);
    RUN_TEST(control_channel_unknown_frame_is_ignored);
    RUN_TEST(control_channel_build_stats_aggregates);
}
