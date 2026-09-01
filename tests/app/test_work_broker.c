/*
 * Tests unitaires de work_broker.c (courtier de travail du process parent).
 *
 * Le cadrage des offres, la fenêtre de contrôle de flux et le verrou
 * d'acquittement sont des fonctions pures : ils s'exercent sans socket ni
 * serveur. Le tampon du parent s'exerce lui aussi hors réseau, `work_broker_on_offer`
 * n'étant qu'un décodage suivi d'un empilement.
 */
#include "greatest.h"
#include "app/work_broker.h"
#include "net/ipc_protocol.h"
#include "net/local_socket.h"
#include "core/possibility.h"

#include <stdlib.h>
#include <string.h>

/* Remplit un paquet d'un motif reconnaissable, octets nuls compris. */
static void fill_packet(struct possibility_packet *p, int marker)
{
    memset(p, 0, sizeof *p);
    ((unsigned char *)p)[0] = (unsigned char)marker;
    ((unsigned char *)p)[sizeof *p - 1] = (unsigned char)(marker ^ 0xff);
}

/* ---------- cadrage des offres ---------- */

/* Aller-retour encode/decode : seq, nombre et contenu binaire préservés. */
TEST offer_encode_decode_roundtrip(void)
{
    struct possibility_packet pkts[3];
    for (int i = 0; i < 3; i++) fill_packet(&pkts[i], 0x10 + i);

    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + 3 * sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(4242u, pkts, 3, buf, sizeof buf);
    ASSERT_EQ_FMT((long)sizeof buf, (long)n, "%ld");

    uint32_t seq = 0;
    const struct possibility_packet *out = NULL;
    int count = -1;
    ASSERT_EQ_FMT(0, work_broker_offer_decode(buf, (size_t)n, &seq, &out, &count), "%d");
    ASSERT_EQ_FMT(4242u, seq, "%u");
    ASSERT_EQ_FMT(3, count, "%d");
    ASSERT_EQ_FMT(0, memcmp(out, pkts, sizeof pkts), "%d");
    PASS();
}

/* Une offre vide est légale (0 paquet) : seul l'en-tête voyage. */
TEST offer_encode_accepts_zero_packets(void)
{
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE];
    ASSERT_EQ_FMT((long)IPC_WORK_OFFER_HEADER_SIZE,
                  (long)work_broker_offer_encode(1u, NULL, 0, buf, sizeof buf), "%ld");
    int count = -1;
    ASSERT_EQ_FMT(0, work_broker_offer_decode(buf, sizeof buf, NULL, NULL, &count), "%d");
    ASSERT_EQ_FMT(0, count, "%d");
    PASS();
}

/* Tampon trop court : refus, jamais d'écriture partielle. */
TEST offer_encode_refuses_short_buffer(void)
{
    struct possibility_packet pkt;
    fill_packet(&pkt, 7);
    uint8_t small[IPC_WORK_OFFER_HEADER_SIZE + 4];
    memset(small, 0xAA, sizeof small);
    ASSERT_EQ_FMT(-1, (int)work_broker_offer_encode(1u, &pkt, 1, small, sizeof small), "%d");
    for (size_t i = 0; i < sizeof small; i++) {
        ASSERT_EQ_FMT(0xAA, (int)small[i], "%d");
    }
    PASS();
}

/* Décodage défensif : une longueur qui ne correspond pas EXACTEMENT au `count`
 * annoncé est rejetée. Accepter un datagramme plus court ferait lire au-delà du
 * tampon reçu — le décodeur est le seul rempart, la charge venant d'un
 * datagramme dont rien ne garantit la cohérence. */
TEST offer_decode_rejects_malformed(void)
{
    struct possibility_packet pkts[2];
    for (int i = 0; i < 2; i++) fill_packet(&pkts[i], i);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + 2 * sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, pkts, 2, buf, sizeof buf);
    ASSERT(n > 0);

    /* trop court pour l'en-tête */
    ASSERT_EQ_FMT(-1, work_broker_offer_decode(buf, 3, NULL, NULL, NULL), "%d");
    /* en-tête seul, mais count == 2 annoncé */
    ASSERT_EQ_FMT(-1, work_broker_offer_decode(buf, IPC_WORK_OFFER_HEADER_SIZE, NULL, NULL, NULL), "%d");
    /* un octet de moins que le compte annoncé */
    ASSERT_EQ_FMT(-1, work_broker_offer_decode(buf, (size_t)n - 1, NULL, NULL, NULL), "%d");
    /* NULL */
    ASSERT_EQ_FMT(-1, work_broker_offer_decode(NULL, (size_t)n, NULL, NULL, NULL), "%d");

    /* count négatif fabriqué à la main */
    int32_t bad = -1;
    memcpy(buf + 4, &bad, sizeof bad);
    ASSERT_EQ_FMT(-1, work_broker_offer_decode(buf, (size_t)n, NULL, NULL, NULL), "%d");
    PASS();
}

/* Une offre tient dans un datagramme : la borne annoncée est cohérente avec
 * ipc_max_datagram(), et au moins un paquet passe sur toute configuration. */
TEST offer_max_packets_fits_one_datagram(void)
{
    size_t per = ipc_work_offer_max_packets();
    ASSERT(per >= 1);
    size_t frame = 1 + IPC_WORK_OFFER_HEADER_SIZE + per * sizeof(struct possibility_packet);
    ASSERT(frame <= ipc_max_datagram());
    /* un paquet de plus déborderait : la borne est bien la plus grande possible */
    ASSERT(frame + sizeof(struct possibility_packet) > ipc_max_datagram());
    PASS();
}

/* ---------- fenêtre de contrôle de flux ---------- */

TEST window_allows_up_to_the_bound(void)
{
    ASSERT_EQ_FMT(1, work_broker_window_allows(0, 0, 8), "%d");   /* rien en vol */
    ASSERT_EQ_FMT(1, work_broker_window_allows(7, 0, 8), "%d");   /* 7 en vol < 8 */
    ASSERT_EQ_FMT(0, work_broker_window_allows(8, 0, 8), "%d");   /* fenêtre pleine */
    ASSERT_EQ_FMT(1, work_broker_window_allows(8, 1, 8), "%d");   /* un règlement libère */
    PASS();
}

/* Arithmétique non signée : la fenêtre reste correcte au rebouclage de seq
 * (2^32 offres). Une comparaison signée y renverrait le mauvais verdict. */
TEST window_survives_seq_wraparound(void)
{
    uint32_t settled = 0xFFFFFFFEu;
    ASSERT_EQ_FMT(1, work_broker_window_allows(0xFFFFFFFFu, settled, 8), "%d"); /* 1 en vol */
    ASSERT_EQ_FMT(1, work_broker_window_allows(3u, settled, 8), "%d");          /* 5 en vol */
    ASSERT_EQ_FMT(0, work_broker_window_allows(6u, settled, 8), "%d");          /* 8 en vol */
    PASS();
}

/* ---------- verrou d'acquittement (côté fils) ---------- */

/* Sans offre en vol, l'acquittement est autorisé — le comportement historique
 * est donc strictement repris tant que rien n'est cédé au courtier. */
TEST ack_allowed_when_nothing_offered(void)
{
    work_broker_child_reset();
    ASSERT_EQ_FMT(1, work_broker_ack_allowed(), "%d");
    PASS();
}

/* Un règlement ne recule jamais : un datagramme réordonné annonçant un `seq`
 * plus ancien ne doit pas rouvrir une fenêtre déjà refermée — un fils
 * acquitterait alors une racine dont du travail est encore en transit. */
TEST settled_advance_never_moves_backwards(void)
{
    ASSERT_EQ_FMT(10u, work_broker_settled_advance(0u, 10u), "%u");  /* avance */
    ASSERT_EQ_FMT(10u, work_broker_settled_advance(10u, 4u), "%u");  /* recul refusé */
    ASSERT_EQ_FMT(10u, work_broker_settled_advance(10u, 10u), "%u"); /* rejeu */
    /* Au rebouclage : 2 est plus RÉCENT que 0xFFFFFFFE, pas plus ancien. */
    ASSERT_EQ_FMT(2u, work_broker_settled_advance(0xFFFFFFFEu, 2u), "%u");
    ASSERT_EQ_FMT(0xFFFFFFFEu, work_broker_settled_advance(0xFFFFFFFEu, 0xFFFFFFF0u), "%u");
    PASS();
}

/* Charge utile tronquée : ignorée, jamais interprétée sur des octets absents. */
TEST settled_ignores_short_payload(void)
{
    work_broker_child_reset();
    uint8_t two[2] = { 0xff, 0xff };
    work_broker_child_on_settled(two, sizeof two);
    work_broker_child_on_settled(NULL, 4);
    ASSERT_EQ_FMT(1, work_broker_ack_allowed(), "%d");
    PASS();
}

/* ---------- tampon du parent ---------- */

/* Une offre bien formée empile exactement ses paquets. */
TEST on_offer_queues_packets(void)
{
    work_broker_parent_reset();
    ASSERT_EQ_FMT(0ULL, work_broker_pending_packets(), "%llu");

    struct possibility_packet pkts[2];
    for (int i = 0; i < 2; i++) fill_packet(&pkts[i], 0x30 + i);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + 2 * sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, pkts, 2, buf, sizeof buf);
    ASSERT(n > 0);

    work_broker_on_offer(0, buf, (size_t)n);
    ASSERT_EQ_FMT(2ULL, work_broker_pending_packets(), "%llu");

    work_broker_on_offer(1, buf, (size_t)n);
    ASSERT_EQ_FMT(4ULL, work_broker_pending_packets(), "%llu");

    work_broker_parent_reset();
    ASSERT_EQ_FMT(0ULL, work_broker_pending_packets(), "%llu");
    PASS();
}

/* Expéditeur non attribuable ou offre mal formée : rien n'est empilé. Un
 * paquet qu'on ne sait pas rattacher à un fils ne serait jamais réglable —
 * le fils resterait bloqué sur un acquittement qui n'arrive pas. */
TEST on_offer_drops_unknown_slot_and_malformed(void)
{
    work_broker_parent_reset();
    struct possibility_packet pkt;
    fill_packet(&pkt, 9);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, &pkt, 1, buf, sizeof buf);
    ASSERT(n > 0);

    work_broker_on_offer(-1, buf, (size_t)n);          /* find_fork_index a échoué */
    ASSERT_EQ_FMT(0ULL, work_broker_pending_packets(), "%llu");

    work_broker_on_offer(0, buf, (size_t)n - 1);       /* longueur incohérente */
    ASSERT_EQ_FMT(0ULL, work_broker_pending_packets(), "%llu");

    work_broker_parent_reset();
    PASS();
}

/* Sans courtier démarré (broker_client NULL), un tour de relais ne fait rien
 * et ne touche pas au tampon : le module est inerte tant que --local-dispatch
 * n'a pas armé le parent. */
TEST relay_step_is_inert_without_broker(void)
{
    work_broker_parent_reset();
    struct possibility_packet pkt;
    fill_packet(&pkt, 5);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, &pkt, 1, buf, sizeof buf);
    work_broker_on_offer(0, buf, (size_t)n);

    ASSERT_EQ_FMT(0, work_broker_relay_step(), "%d");
    ASSERT_EQ_FMT(1ULL, work_broker_pending_packets(), "%llu");

    work_broker_parent_reset();
    PASS();
}

SUITE(work_broker_suite)
{
    RUN_TEST(offer_encode_decode_roundtrip);
    RUN_TEST(offer_encode_accepts_zero_packets);
    RUN_TEST(offer_encode_refuses_short_buffer);
    RUN_TEST(offer_decode_rejects_malformed);
    RUN_TEST(offer_max_packets_fits_one_datagram);
    RUN_TEST(window_allows_up_to_the_bound);
    RUN_TEST(window_survives_seq_wraparound);
    RUN_TEST(ack_allowed_when_nothing_offered);
    RUN_TEST(settled_advance_never_moves_backwards);
    RUN_TEST(settled_ignores_short_payload);
    RUN_TEST(on_offer_queues_packets);
    RUN_TEST(on_offer_drops_unknown_slot_and_malformed);
    RUN_TEST(relay_step_is_inert_without_broker);
}
