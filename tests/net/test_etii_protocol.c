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
#include <fcntl.h>

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

/* poll_server_hunger : envoie INST_NEED_WORK et lit la faim (int32) répondue
 * par le serveur. Réponse pré-chargée sur le pair (comme les tests is_connected). */
TEST poll_server_hunger_reads_reply(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    int32_t reply = 42;
    ASSERT_EQ((long)sizeof(reply), send_all(sv[1], &reply, sizeof(reply)));

    ASSERT_EQ_FMT(42, poll_server_hunger(sv[0]), "%d");
    /* Le pair a bien reçu l'instruction de sonde. */
    ASSERT_EQ_FMT((int)INST_NEED_WORK, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]);
    close(sv[1]);
    PASS();
}

/* poll_server_hunger == -1 quand le pair est fermé (le socket est alors fermé
 * par la sonde elle-même, comme is_connected). */
TEST poll_server_hunger_fails_on_peer_close(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    close(sv[1]); /* pair fermé -> send ou recv échoue */

    ASSERT_EQ_FMT(-1, poll_server_hunger(sv[0]), "%d");
    /* sv[0] fermé par poll_server_hunger dans ce chemin. */
    PASS();
}

/* poll_server_hunger == -1 sur une réponse négative (protocole corrompu). */
TEST poll_server_hunger_rejects_negative_reply(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    int32_t reply = -7;
    ASSERT_EQ((long)sizeof(reply), send_all(sv[1], &reply, sizeof(reply)));

    ASSERT_EQ_FMT(-1, poll_server_hunger(sv[0]), "%d");
    /* sv[0] fermé par la sonde. */
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

/* send_instruction : le send échoue quand le pair est fermé (EPIPE, SIGPIPE ignoré).
 * Couvre la branche « result <= 0 » de send_instruction (lignes 70-71). */
TEST send_instruction_error_on_broken_socket(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    close(sv[1]); /* plus de lecteur côté sv[0] */

    long r = send_instruction(sv[0], INST_ADD);
    ASSERT(r <= 0); /* EPIPE avec SIGPIPE ignoré */

    close(sv[0]);
    PASS();
}

/* is_connected : le send échoue quand le pair est fermé → retourne 0.
 * Couvre le chemin « send fails » (lignes 143-150 : shutdown + close + return 0).
 * is_connected ferme sv[0] lui-même dans ce chemin. */
TEST is_connected_false_when_send_fails(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    close(sv[1]); /* send_instruction dans is_connected va échouer */

    ASSERT_EQ_FMT(0, is_connected(sv[0]), "%d");
    /* sv[0] est fermé par is_connected — pas de double close. */
    PASS();
}

/* is_connected : le pair répond un octet qui n'est ni INST_END ni
 * INST_TEST_CONNECTED → retourne 0 ET ferme le socket, comme les trois
 * autres branches d'échec (send/recv/INST_END). Régression : cette branche
 * était la SEULE des quatre à ne pas fermer le socket — fuite de fd côté
 * appelant, et côté serveur une session orpheline qui ne se termine que par
 * son propre timeout, pendant lequel `requeue_last_sent_possibility` peut
 * remettre en jeu un travail que ce client fait toujours (cf.
 * requeue_last_sent_possibility dans etii_server.c). Couvre lignes
 * « wrong instruction » de is_connected.
 *
 * Mécanisme mono-thread : on pré-charge le tampon de sv[0] avec INST_ADD via sv[1]
 * avant l'appel.  is_connected(sv[0]) envoie INST_TEST_CONNECTED (sv[1] l'absorbe),
 * lit INST_ADD depuis son tampon → mauvaise instruction → return 0. */
TEST is_connected_false_on_wrong_instruction(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    /* Pré-charge sv[0] avec une mauvaise instruction. */
    send_instruction(sv[1], INST_ADD);

    ASSERT_EQ_FMT(0, is_connected(sv[0]), "%d");
    /* sv[0] est fermé par is_connected — pas de double close (comme les
     * trois autres branches d'échec). */
    close(sv[1]);
    PASS();
}

/* send_all : retourne -1 quand le pair est fermé (EPIPE dès le premier send).
 * Couvre ligne 126 (branche « s < 0 && errno != EINTR → return -1 »). */
TEST send_all_error_on_broken_socket(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    close(sv[1]); /* ferme le lecteur — tout send sur sv[0] donnera EPIPE */

    unsigned char buf[64] = { 0 };
    long r = send_all(sv[0], buf, sizeof(buf));
    ASSERT_EQ_FMT(-1L, r, "%ld");

    close(sv[0]);
    PASS();
}

/* close_socket : close() interne échoue (fd déjà fermé → EBADF) → branche
 * log_error (lignes 194-197). Le send_instruction/shutdown préalables sur le fd
 * mort sont sans effet ; seul le close() final décide de la branche. */
TEST close_socket_logs_error_on_bad_fd(void)
{
    int sv[2];
    MAKE_PAIR(sv);
    close(sv[0]);                       /* fd désormais invalide */
    ASSERT(fcntl(sv[0], F_GETFD) == -1); /* précondition : sv[0] est bien fermé */

    close_socket(sv[0]); /* close() interne renvoie -1 → exécute log_error */

    close(sv[1]);
    PASS();
}

SUITE(etii_protocol_suite)
{
    /* SIGPIPE ignoré dans toute la suite : les tests avec pair fermé font des
       send() qui génèreraient SIGPIPE → on veut -1/EPIPE, pas un signal. */
    signal(SIGPIPE, SIG_IGN);

    RUN_TEST(instruction_round_trip);
    RUN_TEST(recv_instruction_returns_end_on_peer_close);
    RUN_TEST(send_all_recv_all_round_trip);
    RUN_TEST(recv_all_returns_partial_on_peer_close);
    RUN_TEST(is_connected_true_when_peer_echoes);
    RUN_TEST(is_connected_false_on_end);
    RUN_TEST(close_socket_sends_end_before_closing);
    RUN_TEST(poll_server_hunger_reads_reply);
    RUN_TEST(poll_server_hunger_fails_on_peer_close);
    RUN_TEST(poll_server_hunger_rejects_negative_reply);
    RUN_TEST(handshake_verdict_distinguishes_outcomes);
    RUN_TEST(send_instruction_error_on_broken_socket);
    RUN_TEST(is_connected_false_when_send_fails);
    RUN_TEST(is_connected_false_on_wrong_instruction);
    RUN_TEST(send_all_error_on_broken_socket);
    RUN_TEST(close_socket_logs_error_on_bad_fd);
}
