/*
 * Tests unitaires de etii_protocol.c (helpers d'envoi/réception du protocole
 * TCP client-serveur).
 *
 * Pas besoin d'un vrai serveur : socketpair(AF_UNIX, SOCK_STREAM) fournit deux
 * descripteurs connectés bout à bout, sur lesquels on rejoue les aller-retours.
 * Les échanges restent dans le tampon socket du noyau (mono-thread, sans
 * blocage tant que les volumes < taille du tampon).
 *
 * Seule dépendance de link : logger.c (déjà dans TEST_MODULES).
 */
#include "greatest.h"
#include "net/etii_protocol.h"

#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

/* Ouvre une paire de sockets connectés ; échoue le test si indisponible. */
#define MAKE_PAIR(sv)                                  \
    do {                                               \
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) FAILm("socketpair"); \
    } while (0)

/* send_instruction écrit 1 octet ; recv_instruction le relit à l'identique. */
TEST instruction_round_trip(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    ASSERT_EQ_FMT(1L, send_instruction(sv[0], INST_ADD), "%ld");
    ASSERT_EQ_FMT((int)INST_ADD, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* recv_instruction renvoie INST_END quand le pair a fermé (recv == 0). */
TEST recv_instruction_returns_end_on_peer_close(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    close(sv[1]); /* le pair ferme, aucune donnée en attente */
    ASSERT_EQ_FMT((int)INST_END, (int)recv_instruction(sv[0]), "%d");

    close(sv[0]);
    PASS();
}

/* send_all / recv_all transfèrent fidèlement un bloc multi-octets. */
TEST send_all_recv_all_round_trip(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    unsigned char src[1000], dst[1000];
    for (int i = 0; i < 1000; i++) src[i] = (unsigned char)(i * 7 + 3);

    ASSERT_EQ_FMT(1000L, send_all(sv[0], src, sizeof(src)), "%ld");
    ASSERT_EQ_FMT(1000L, recv_all(sv[1], dst, sizeof(dst)), "%ld");
    ASSERT_MEM_EQ(src, dst, sizeof(src));

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* recv_all rend le total partiel quand le pair ferme avant d'avoir tout envoyé. */
TEST recv_all_returns_partial_on_peer_close(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    unsigned char src[3] = { 10, 20, 30 };
    send_all(sv[0], src, sizeof(src));
    close(sv[0]); /* pair fermé après 3 octets */

    unsigned char dst[8] = { 0 };
    ASSERT_EQ_FMT(3L, recv_all(sv[1], dst, sizeof(dst)), "%ld"); /* 3 < 8 demandés */

    close(sv[1]);
    PASS();
}

/* is_connected == 1 lorsque le socket renvoie l'écho INST_TEST_CONNECTED. */
TEST is_connected_true_when_peer_echoes(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    /* Pré-charge sv[0] avec l'écho attendu (comme le ferait le serveur). */
    send_instruction(sv[1], INST_TEST_CONNECTED);

    ASSERT_EQ_FMT(1, is_connected(sv[0]), "%d");

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* is_connected == 0 lorsque le pair signale INST_END (et ferme sv[0] lui-même). */
TEST is_connected_false_on_end(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    send_instruction(sv[1], INST_END); /* le serveur annonce sa fin */

    ASSERT_EQ_FMT(0, is_connected(sv[0]), "%d");
    /* sv[0] est fermé par is_connected dans ce chemin. */
    close(sv[1]);
    PASS();
}

/* close_socket émet INST_END au pair avant de fermer : le pair le reçoit. */
TEST close_socket_sends_end_before_closing(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    close_socket(sv[0]); /* envoie INST_END puis ferme sv[0] */
    ASSERT_EQ_FMT((int)INST_END, (int)recv_instruction(sv[1]), "%d");

    close(sv[1]);
    PASS();
}

/* handshake_verdict distingue les 3 issues du contrôle de version. Régression
   du bug « faux Version not supported » : un timeout (INST_END) ou tout octet
   inattendu doit donner HANDSHAKE_RETRY (réessayer), JAMAIS un refus de version
   (qui, lui, arrête le client). Seul INST_UNSUPPORTED_VERSION est un vrai refus. */
TEST handshake_verdict_distinguishes_outcomes(void)
{
    ASSERT_EQ_FMT((int)HANDSHAKE_OK,
                  (int)handshake_verdict(INST_SUPPORTED_VERSION), "%d");
    ASSERT_EQ_FMT((int)HANDSHAKE_VERSION_REJECTED,
                  (int)handshake_verdict(INST_UNSUPPORTED_VERSION), "%d");
    /* Cœur du bug corrigé : timeout / déconnexion ⇒ réessayer, pas arrêter. */
    ASSERT_EQ_FMT((int)HANDSHAKE_RETRY, (int)handshake_verdict(INST_END), "%d");
    ASSERT_EQ_FMT((int)HANDSHAKE_RETRY, (int)handshake_verdict(INST_NULL), "%d");
    ASSERT_EQ_FMT((int)HANDSHAKE_RETRY, (int)handshake_verdict((int8_t)-1), "%d");
    ASSERT_EQ_FMT((int)HANDSHAKE_RETRY, (int)handshake_verdict((int8_t)42), "%d");
    PASS();
}

SUITE(etii_protocol_suite)
{
    /* Aucun envoi vers un pair fermé ici, mais on neutralise SIGPIPE par
       prudence pour que le runner ne meure jamais sur un send rompu. */
    signal(SIGPIPE, SIG_IGN);

    RUN_TEST(instruction_round_trip);
    RUN_TEST(recv_instruction_returns_end_on_peer_close);
    RUN_TEST(send_all_recv_all_round_trip);
    RUN_TEST(recv_all_returns_partial_on_peer_close);
    RUN_TEST(is_connected_true_when_peer_echoes);
    RUN_TEST(is_connected_false_on_end);
    RUN_TEST(close_socket_sends_end_before_closing);
    RUN_TEST(handshake_verdict_distinguishes_outcomes);
}
