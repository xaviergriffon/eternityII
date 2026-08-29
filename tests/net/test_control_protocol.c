/*
 * Tests unitaires de control_protocol.c (codec du futur canal de contrôle).
 *
 * Comme test_etii_protocol.c : socketpair(AF_UNIX, SOCK_STREAM) fournit deux
 * descripteurs connectés bout à bout, sur lesquels on rejoue les trames.
 * SO_RCVTIMEO est posé AVANT tout recv sur chaque extrémité : un bug de
 * cadrage dans le codec ne doit jamais bloquer le test indéfiniment (leçon
 * déjà tirée sur ce projet), il doit échouer proprement à la place.
 *
 * Seule dépendance de link : etii_protocol.c (send_all/recv_all) + logger.c.
 */
#include "greatest.h"
#include "net/control_protocol.h"
#include "net/etii_protocol.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

/* Ouvre une paire de sockets connectés avec un timeout de réception, échoue
 * le test si indisponible. */
#define MAKE_PAIR(sv)                                                       \
    do {                                                                    \
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair"); \
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300000 };             \
        setsockopt((sv)[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));      \
        setsockopt((sv)[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));      \
    } while (0)

/* Round-trip complet, payload NULL/0. */
TEST ctrl_frame_round_trip_empty_payload(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    ASSERT_EQ_FMT(0, ctrl_send_frame(sv[0], CTRL_PING, NULL, 0), "%d");

    void *payload = NULL;
    int32_t len = -1;
    int cmd = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT((int)CTRL_PING, cmd, "%d");
    ASSERT_EQ_FMT(0, len, "%d");
    ASSERT_EQ(NULL, payload);

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* Round-trip complet, payload non vide. */
TEST ctrl_frame_round_trip_with_payload(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char *msg = "pause";
    ASSERT_EQ_FMT(0, ctrl_send_frame(sv[0], CTRL_COMMAND, msg, (int32_t)strlen(msg)), "%d");

    void *payload = NULL;
    int32_t len = -1;
    int cmd = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT((int)CTRL_COMMAND, cmd, "%d");
    ASSERT_EQ_FMT((int32_t)strlen(msg), len, "%d");
    ASSERT(payload != NULL);
    ASSERT_MEM_EQ(msg, payload, (size_t)len);

    free(payload);
    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* Trame tronquée : le pair ferme après un envoi partiel -> -1, pas de blocage. */
TEST ctrl_frame_truncated_returns_error(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    uint8_t cmd = CTRL_STATS;
    /* Envoie juste le cmd, sans le len ni le payload annoncé. */
    ASSERT_EQ_FMT(1L, send_all(sv[0], &cmd, sizeof(cmd)), "%ld");
    close(sv[0]); /* ferme avant d'envoyer le reste */

    void *payload = NULL;
    int32_t len = -1;
    int r = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT(-1, r, "%d");
    ASSERT_EQ(NULL, payload);

    close(sv[1]);
    PASS();
}

/* len hors borne (négatif) construit manuellement -> rejeté sans allocation. */
TEST ctrl_frame_rejects_negative_len(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    uint8_t cmd = CTRL_RESULT;
    int32_t bad_len = -42;
    ASSERT_EQ_FMT(1L, send_all(sv[0], &cmd, sizeof(cmd)), "%ld");
    ASSERT_EQ_FMT((long)sizeof(bad_len), send_all(sv[0], &bad_len, sizeof(bad_len)), "%ld");

    void *payload = NULL;
    int32_t len = 0;
    int r = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT(-1, r, "%d");
    ASSERT_EQ(NULL, payload);

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* len hors borne (> CTRL_PAYLOAD_MAX) -> rejeté sans allocation absurde. */
TEST ctrl_frame_rejects_too_large_len(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    uint8_t cmd = CTRL_RESULT;
    int32_t bad_len = CTRL_PAYLOAD_MAX + 1;
    ASSERT_EQ_FMT(1L, send_all(sv[0], &cmd, sizeof(cmd)), "%ld");
    ASSERT_EQ_FMT((long)sizeof(bad_len), send_all(sv[0], &bad_len, sizeof(bad_len)), "%ld");

    void *payload = NULL;
    int32_t len = 0;
    int r = ctrl_recv_frame(sv[1], &payload, &len);
    ASSERT_EQ_FMT(-1, r, "%d");
    ASSERT_EQ(NULL, payload);

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* ctrl_send_frame rejette lui aussi une longueur hors borne, sans I/O. */
TEST ctrl_send_frame_rejects_out_of_range_len(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    char buf[4] = { 0 };
    ASSERT_EQ_FMT(-1, ctrl_send_frame(sv[0], CTRL_PING, buf, -1), "%d");
    ASSERT_EQ_FMT(-1, ctrl_send_frame(sv[0], CTRL_PING, buf, CTRL_PAYLOAD_MAX + 1), "%d");

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* control_hello_encode/decode : aller-retour fidèle, y compris l'identité
   étendue (v12) — machine_uid/client_uid/fork_seq/mode/label. */
TEST control_hello_round_trip(void)
{
    control_hello_t hello = { .pid = 4242, .nb_forks = 8 };
    for (int i = 0; i < MACHINE_UID_BYTES; i++) {
        hello.identity.machine_uid[i] = (uint8_t)(i + 1);
    }
    for (int i = 0; i < CLIENT_UID_BYTES; i++) {
        hello.identity.client_uid[i] = (uint8_t)(0x50 + i);
    }
    hello.identity.fork_seq = -1;
    hello.identity.mode = 1;
    strncpy(hello.identity.label, "jetson-1", CLIENT_LABEL_MAX - 1);
    hello.identity.label[CLIENT_LABEL_MAX - 1] = '\0';

    uint8_t buf[CONTROL_HELLO_WIRE_MAX_SIZE];
    int32_t wlen = control_hello_encode(&hello, buf, sizeof(buf));
    ASSERT_EQ_FMT((int32_t)(CONTROL_HELLO_WIRE_MIN_SIZE + strlen("jetson-1")), wlen, "%d");

    control_hello_t out;
    memset(&out, 0xAA, sizeof(out));
    ASSERT_EQ_FMT(0, control_hello_decode(buf, wlen, &out), "%d");
    ASSERT_EQ_FMT(hello.pid, out.pid, "%d");
    ASSERT_EQ_FMT(hello.nb_forks, out.nb_forks, "%d");
    ASSERT_MEM_EQ(hello.identity.machine_uid, out.identity.machine_uid, MACHINE_UID_BYTES);
    ASSERT_MEM_EQ(hello.identity.client_uid, out.identity.client_uid, CLIENT_UID_BYTES);
    ASSERT_EQ_FMT(hello.identity.fork_seq, out.identity.fork_seq, "%d");
    ASSERT_EQ_FMT((int)hello.identity.mode, (int)out.identity.mode, "%d");
    ASSERT_STR_EQ("jetson-1", out.identity.label);
    PASS();
}

/* control_hello_encode : bufsize trop petit -> -1 proprement. */
TEST control_hello_encode_rejects_buffer_too_small(void)
{
    control_hello_t hello = { .pid = 1, .nb_forks = 1 };
    uint8_t buf[CONTROL_HELLO_WIRE_MIN_SIZE - 1];
    ASSERT_EQ_FMT(-1, control_hello_encode(&hello, buf, sizeof(buf)), "%d");
    PASS();
}

/* control_hello_decode : buffer trop court -> -1 proprement. */
TEST control_hello_decode_rejects_short_buffer(void)
{
    uint8_t buf[CONTROL_HELLO_WIRE_MIN_SIZE] = { 0 };
    control_hello_t out;
    ASSERT_EQ_FMT(-1, control_hello_decode(buf, CONTROL_HELLO_WIRE_MIN_SIZE - 1, &out), "%d");
    PASS();
}

/* control_stats_encode/decode : aller-retour fidèle. */
TEST control_stats_round_trip(void)
{
    control_stats_t stats = {
        .shots_per_second = 123456789ULL,
        .possibility_stock = 42ULL,
        .analysed_stock = 7ULL,
        .max_result = 250ULL,
        .pruner_checked = 999ULL,
        .pruner_removed = 111ULL,
        .pruner_cells_per_second = 555ULL,
    };
    uint8_t buf[CONTROL_STATS_WIRE_SIZE];

    ASSERT_EQ_FMT((int32_t)CONTROL_STATS_WIRE_SIZE, control_stats_encode(&stats, buf), "%d");

    control_stats_t out;
    memset(&out, 0xAA, sizeof(out));
    ASSERT_EQ_FMT(0, control_stats_decode(buf, sizeof(buf), &out), "%d");
    ASSERT_EQ_FMT((int)stats.shots_per_second, (int)out.shots_per_second, "%d");
    ASSERT_EQ_FMT((int)stats.possibility_stock, (int)out.possibility_stock, "%d");
    ASSERT_EQ_FMT((int)stats.analysed_stock, (int)out.analysed_stock, "%d");
    ASSERT_EQ_FMT((int)stats.max_result, (int)out.max_result, "%d");
    ASSERT_EQ_FMT((int)stats.pruner_checked, (int)out.pruner_checked, "%d");
    ASSERT_EQ_FMT((int)stats.pruner_removed, (int)out.pruner_removed, "%d");
    ASSERT_EQ_FMT((int)stats.pruner_cells_per_second, (int)out.pruner_cells_per_second, "%d");
    PASS();
}

/* control_stats_decode : buffer trop court -> -1 proprement. */
TEST control_stats_decode_rejects_short_buffer(void)
{
    uint8_t buf[CONTROL_STATS_WIRE_SIZE] = { 0 };
    control_stats_t out;
    ASSERT_EQ_FMT(-1, control_stats_decode(buf, CONTROL_STATS_WIRE_SIZE - 1, &out), "%d");
    PASS();
}

/* control_command_allowed : liste blanche. */
TEST control_command_allowed_accepts_whitelist(void)
{
    ASSERT_EQ_FMT(1, control_command_allowed("pause"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("resume"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("limit"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("maxStockByThread"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("prunerBatch"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("prunerDfsBudget"), "%d");
    /* Avec argument : seul le premier mot compte. */
    ASSERT_EQ_FMT(1, control_command_allowed("limit 100"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("maxStockByThread 5000"), "%d");
    PASS();
}

/* Commandes de cycle de vie des fils : rejoignent la liste blanche pour être
   pilotables à distance (canal de contrôle, API HTTP admin). */
TEST control_command_allowed_accepts_lifecycle_commands(void)
{
    ASSERT_EQ_FMT(1, control_command_allowed("start"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("stopForks"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("configApply"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("config"), "%d");
    ASSERT_EQ_FMT(1, control_command_allowed("configSave"), "%d");
    /* Avec argument : seul le premier mot compte. */
    ASSERT_EQ_FMT(1, control_command_allowed("config nb_forks 2"), "%d");
    PASS();
}

/* clientsRoles (PR3, docs/conception/pilotage_type_client.md) : rejoint la
   liste blanche relayable, même famille que clientsCommand/clientsCmd -- un
   seul point à toucher (control_command_classify) pour que le contrôle
   d'accès HTTP (needs_auth) et la vérification client-side (etii_control.c)
   la reconnaissent. */
TEST control_command_allowed_accepts_clients_roles(void)
{
    ASSERT_EQ_FMT(1, control_command_allowed("clientsRoles"), "%d");
    /* Avec argument : seul le premier mot compte. */
    ASSERT_EQ_FMT(1, control_command_allowed("clientsRoles --to jetson-1 2"), "%d");
    ASSERT_EQ_FMT(0, control_command_privileged("clientsRoles"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("clientsRoles"), "%d");
    PASS();
}

TEST control_command_allowed_rejects_others(void)
{
    ASSERT_EQ_FMT(0, control_command_allowed("exit"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed("restore"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed("import"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed("rmnonext"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed(""), "%d");
    /* Préfixe partiel non whitelisté (ex: "pauses" != "pause"). */
    ASSERT_EQ_FMT(0, control_command_allowed("pauses"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed("pau"), "%d");
    PASS();
}

/* NULL géré explicitement sans crash. */
TEST control_command_allowed_handles_null(void)
{
    ASSERT_EQ_FMT(0, control_command_allowed(NULL), "%d");
    PASS();
}

/* control_command_privileged : liste blanche disjointe de control_command_allowed
   (restore/backup uniquement, jamais via le canal de contrôle binaire). */
TEST control_command_privileged_accepts_whitelist(void)
{
    ASSERT_EQ_FMT(1, control_command_privileged("restore"), "%d");
    ASSERT_EQ_FMT(1, control_command_privileged("backup"), "%d");
    /* Avec argument : seul le premier mot compte. */
    ASSERT_EQ_FMT(1, control_command_privileged("restore fichier.back"), "%d");
    PASS();
}

TEST control_command_privileged_rejects_others(void)
{
    ASSERT_EQ_FMT(0, control_command_privileged("exit"), "%d");
    ASSERT_EQ_FMT(0, control_command_privileged("import"), "%d");
    ASSERT_EQ_FMT(0, control_command_privileged("pause"), "%d");
    ASSERT_EQ_FMT(0, control_command_privileged(""), "%d");
    /* Préfixe partiel non whitelisté. */
    ASSERT_EQ_FMT(0, control_command_privileged("restored"), "%d");
    PASS();
}

TEST control_command_privileged_handles_null(void)
{
    ASSERT_EQ_FMT(0, control_command_privileged(NULL), "%d");
    PASS();
}

/* stockMaxRam (PR1, --stock-max-ram) : même famille que rebalance/sortAsc --
 * privilégiée (jeton Bearer requis côté API HTTP admin), jamais relayable via
 * le canal de contrôle binaire. */
TEST control_command_privileged_accepts_stock_max_ram(void)
{
    ASSERT_EQ_FMT(1, control_command_privileged("stockMaxRam"), "%d");
    ASSERT_EQ_FMT(1, control_command_privileged("stockMaxRam 100"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed("stockMaxRam"), "%d"); /* jamais dans l'autre liste */
    PASS();
}

/* spill (PR2, débordement sur disque) : même famille que stockMaxRam --
 * privilégiée, jamais relayable via le canal de contrôle binaire. */
TEST control_command_privileged_accepts_spill(void)
{
    ASSERT_EQ_FMT(1, control_command_privileged("spill"), "%d");
    ASSERT_EQ_FMT(1, control_command_privileged("spill 10"), "%d");
    ASSERT_EQ_FMT(0, control_command_allowed("spill"), "%d");
    PASS();
}

/* Les deux listes blanches ne se recoupent jamais. */
TEST control_command_allowed_and_privileged_are_disjoint(void)
{
    static const char *const allowed_names[] = {
        "pause", "resume", "limit", "maxStockByThread", "prunerBatch", "prunerDfsBudget",
        "clientsCommand", "clientsCmd", "clientsRoles", "clientsWork",
        "start", "stopForks", "configApply", "config", "configSave"
    };
    static const char *const privileged_names[] = { "restore", "backup" };

    for (size_t i = 0; i < sizeof(allowed_names) / sizeof(allowed_names[0]); i++) {
        ASSERT_EQ_FMT(0, control_command_privileged(allowed_names[i]), "%d");
    }
    for (size_t i = 0; i < sizeof(privileged_names) / sizeof(privileged_names[0]); i++) {
        ASSERT_EQ_FMT(0, control_command_allowed(privileged_names[i]), "%d");
    }
    PASS();
}

/* control_command_read_only : n'identifie QUE "clientsWork" parmi les
   commandes de control_command_allowed -- utilisé exclusivement par l'API
   HTTP admin pour décider si l'authentification est requise (voir
   handle_command_route, src/net/http_server.c). */
TEST control_command_read_only_accepts_only_clientswork(void)
{
    ASSERT_EQ_FMT(1, control_command_read_only("clientsWork"), "%d");
    /* Avec argument : seul le premier mot compte. */
    ASSERT_EQ_FMT(1, control_command_read_only("clientsWork beta"), "%d");
    PASS();
}

TEST control_command_read_only_rejects_modifying_standard_commands(void)
{
    ASSERT_EQ_FMT(0, control_command_read_only("pause"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("resume"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("limit"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("maxStockByThread"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("prunerBatch"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("prunerDfsBudget"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("clientsCommand"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("clientsCmd"), "%d");
    /* Les commandes de cycle de vie des fils modifient toutes un état
       (local au minimum) -- aucune n'est un pur read. */
    ASSERT_EQ_FMT(0, control_command_read_only("start"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("stopForks"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("configApply"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("config"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("configSave"), "%d");
    PASS();
}

TEST control_command_read_only_rejects_others(void)
{
    ASSERT_EQ_FMT(0, control_command_read_only("exit"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only("restore"), "%d");
    ASSERT_EQ_FMT(0, control_command_read_only(""), "%d");
    /* Préfixe partiel non whitelisté. */
    ASSERT_EQ_FMT(0, control_command_read_only("clientsWorker"), "%d");
    PASS();
}

TEST control_command_read_only_handles_null(void)
{
    ASSERT_EQ_FMT(0, control_command_read_only(NULL), "%d");
    PASS();
}

SUITE(control_protocol_suite)
{
    signal(SIGPIPE, SIG_IGN);

    RUN_TEST(ctrl_frame_round_trip_empty_payload);
    RUN_TEST(ctrl_frame_round_trip_with_payload);
    RUN_TEST(ctrl_frame_truncated_returns_error);
    RUN_TEST(ctrl_frame_rejects_negative_len);
    RUN_TEST(ctrl_frame_rejects_too_large_len);
    RUN_TEST(ctrl_send_frame_rejects_out_of_range_len);
    RUN_TEST(control_hello_round_trip);
    RUN_TEST(control_hello_encode_rejects_buffer_too_small);
    RUN_TEST(control_hello_decode_rejects_short_buffer);
    RUN_TEST(control_stats_round_trip);
    RUN_TEST(control_stats_decode_rejects_short_buffer);
    RUN_TEST(control_command_allowed_accepts_whitelist);
    RUN_TEST(control_command_allowed_accepts_lifecycle_commands);
    RUN_TEST(control_command_allowed_accepts_clients_roles);
    RUN_TEST(control_command_allowed_rejects_others);
    RUN_TEST(control_command_allowed_handles_null);
    RUN_TEST(control_command_privileged_accepts_whitelist);
    RUN_TEST(control_command_privileged_rejects_others);
    RUN_TEST(control_command_privileged_handles_null);
    RUN_TEST(control_command_privileged_accepts_stock_max_ram);
    RUN_TEST(control_command_privileged_accepts_spill);
    RUN_TEST(control_command_allowed_and_privileged_are_disjoint);
    RUN_TEST(control_command_read_only_accepts_only_clientswork);
    RUN_TEST(control_command_read_only_rejects_modifying_standard_commands);
    RUN_TEST(control_command_read_only_rejects_others);
    RUN_TEST(control_command_read_only_handles_null);
}
