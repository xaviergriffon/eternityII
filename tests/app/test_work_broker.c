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
#include "app/app_static_variables.h"
#include "core/core_static_variables.h"

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


/* ---------- comptabilité des offres (invariant d'acquittement A4) ---------- */

/* Le règlement avance dès qu'une offre est entièrement disposée. */
TEST acc_settles_when_offer_fully_disposed(void)
{
    work_broker_offer_acc_t ring[WORK_BROKER_OFFER_WINDOW];
    memset(ring, 0, sizeof ring);

    ASSERT_EQ_FMT(0, work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW, 1u, 2), "%d");
    ASSERT_EQ_FMT(0u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 0u), "%u");

    ASSERT_EQ_FMT(0, work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 1u), "%d");
    ASSERT_EQ_FMT(0u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 0u), "%u");

    ASSERT_EQ_FMT(0, work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 1u), "%d");
    ASSERT_EQ_FMT(1u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 0u), "%u");
    PASS();
}

/* LE test de l'invariant : une offre plus récente entièrement disposée ne doit
 * PAS faire sauter par-dessus une offre plus ancienne encore en vol. Régler 2
 * alors que l'offre 1 circule encore laisserait le fils acquitter une racine
 * dont du travail n'est ni durable ni terminé — la branche serait perdue si le
 * client mourait ensuite. */
TEST acc_never_settles_past_an_outstanding_offer(void)
{
    work_broker_offer_acc_t ring[WORK_BROKER_OFFER_WINDOW];
    memset(ring, 0, sizeof ring);
    work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW, 1u, 1);
    work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW, 2u, 1);

    /* L'offre 2 est disposée la première (un fils l'a prouvée morte vite). */
    work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 2u);
    ASSERT_EQ_FMT(0u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 0u), "%u");

    /* L'offre 1 se dispose à son tour : les DEUX se règlent d'un coup. */
    work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 1u);
    ASSERT_EQ_FMT(2u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 0u), "%u");
    PASS();
}

/* Anneau plein : l'ajout échoue, pour que l'appelant REFUSE l'offre entière
 * plutôt que de n'en suivre qu'une partie (règlement alors incohérent). */
TEST acc_add_refuses_when_ring_is_full(void)
{
    work_broker_offer_acc_t ring[WORK_BROKER_OFFER_WINDOW];
    memset(ring, 0, sizeof ring);
    for (int i = 0; i < WORK_BROKER_OFFER_WINDOW; i++) {
        ASSERT_EQ_FMT(0, work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW,
                                             (uint32_t)(i + 1), 1), "%d");
    }
    ASSERT_EQ_FMT(-1, work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW, 99u, 1), "%d");

    /* Un règlement libère une place. */
    work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 1u);
    ASSERT_EQ_FMT(1u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 0u), "%u");
    ASSERT_EQ_FMT(0, work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW, 99u, 1), "%d");
    PASS();
}

/* Décompter une offre inconnue (message périmé) est signalé, jamais imputé à
 * une autre offre. */
TEST acc_dispose_reports_unknown_offer(void)
{
    work_broker_offer_acc_t ring[WORK_BROKER_OFFER_WINDOW];
    memset(ring, 0, sizeof ring);
    work_broker_acc_add(ring, WORK_BROKER_OFFER_WINDOW, 5u, 1);
    ASSERT_EQ_FMT(-1, work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 4u), "%d");
    /* L'offre 5 est intacte : partant de 4, le règlement ne bouge pas. */
    ASSERT_EQ_FMT(4u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 4u), "%u");
    /* Une fois vraiment disposée, elle se règle. */
    ASSERT_EQ_FMT(0, work_broker_acc_dispose(ring, WORK_BROKER_OFFER_WINDOW, 5u), "%d");
    ASSERT_EQ_FMT(5u, work_broker_acc_settle(ring, WORK_BROKER_OFFER_WINDOW, 4u), "%u");
    PASS();
}

/* ---------- cadrage du couple (origin_slot, origin_seq) ---------- */

TEST tag_encode_decode_roundtrip(void)
{
    uint8_t buf[IPC_WORK_TAG_SIZE];
    ASSERT_EQ_FMT(IPC_WORK_TAG_SIZE, work_broker_tag_encode(3, 77u, buf, sizeof buf), "%d");
    int32_t slot = -1; uint32_t seq = 0;
    ASSERT_EQ_FMT(0, work_broker_tag_decode(buf, sizeof buf, &slot, &seq), "%d");
    ASSERT_EQ_FMT(3, slot, "%d");
    ASSERT_EQ_FMT(77u, seq, "%u");

    uint8_t small[IPC_WORK_TAG_SIZE - 1];
    ASSERT_EQ_FMT(-1, work_broker_tag_encode(3, 77u, small, sizeof small), "%d");
    ASSERT_EQ_FMT(-1, work_broker_tag_decode(buf, IPC_WORK_TAG_SIZE - 1, &slot, &seq), "%d");
    PASS();
}

/* ---------- redistribution ---------- */

/* Une demande servie retire la possibilité du tampon : elle est alors chez le
 * fils, plus dans la file — et n'est donc pas poussée au serveur en double. */
TEST request_takes_one_packet_out_of_the_buffer(void)
{
    work_broker_parent_reset();
    struct possibility_packet pkts[2];
    for (int i = 0; i < 2; i++) fill_packet(&pkts[i], 0x50 + i);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + 2 * sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, pkts, 2, buf, sizeof buf);
    work_broker_on_offer(0, buf, (size_t)n);
    ASSERT_EQ_FMT(2ULL, work_broker_pending_packets(), "%llu");

    /* Le fils 1 réclame. L'envoi du GRANT échoue (aucun forkId câblé dans ce
       test), donc la possibilité est REMISE en tampon — c'est précisément le
       comportement voulu : une attribution que le fils n'a jamais reçue ne
       doit pas disparaître du tampon. */
    work_broker_on_request(1);
    ASSERT_EQ_FMT(2ULL, work_broker_pending_packets(), "%llu");

    work_broker_parent_reset();
    PASS();
}

/* Un slot hors bornes ne sert rien et ne touche pas au tampon. */
TEST request_from_unknown_slot_is_ignored(void)
{
    work_broker_parent_reset();
    struct possibility_packet pkt;
    fill_packet(&pkt, 3);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, &pkt, 1, buf, sizeof buf);
    work_broker_on_offer(0, buf, (size_t)n);

    work_broker_on_request(-1);
    ASSERT_EQ_FMT(1ULL, work_broker_pending_packets(), "%llu");
    work_broker_parent_reset();
    PASS();
}

/* Un DONE qui ne correspond à aucune attribution en cours est ignoré :
 * décompter sur la foi d'un message périmé retirerait une unité à une offre
 * encore en vol, et réglerait donc un fils trop tôt. */
TEST done_without_matching_grant_is_ignored(void)
{
    work_broker_parent_reset();
    struct possibility_packet pkt;
    fill_packet(&pkt, 4);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + sizeof(struct possibility_packet)];
    int32_t n = work_broker_offer_encode(1u, &pkt, 1, buf, sizeof buf);
    work_broker_on_offer(0, buf, (size_t)n);

    uint8_t tag[IPC_WORK_TAG_SIZE];
    work_broker_tag_encode(0, 1u, tag, sizeof tag);
    work_broker_on_done(1, tag, sizeof tag);   /* le fils 1 n'a rien reçu */
    work_broker_on_done(1, tag, IPC_WORK_TAG_SIZE - 1); /* trop court */

    /* Le tampon n'a pas bougé : l'offre reste à disposer. */
    ASSERT_EQ_FMT(1ULL, work_broker_pending_packets(), "%llu");
    work_broker_parent_reset();
    PASS();
}

/* Une offre qui déborde la fenêtre est refusée en BLOC : ne suivre qu'une
 * partie de ses paquets rendrait le règlement de ce fils incohérent. */
TEST offer_beyond_window_is_refused_whole(void)
{
    work_broker_parent_reset();
    struct possibility_packet pkt;
    fill_packet(&pkt, 6);
    uint8_t buf[IPC_WORK_OFFER_HEADER_SIZE + sizeof(struct possibility_packet)];

    for (int i = 1; i <= WORK_BROKER_OFFER_WINDOW; i++) {
        int32_t n = work_broker_offer_encode((uint32_t)i, &pkt, 1, buf, sizeof buf);
        work_broker_on_offer(0, buf, (size_t)n);
    }
    ASSERT_EQ_FMT((unsigned long long)WORK_BROKER_OFFER_WINDOW,
                  work_broker_pending_packets(), "%llu");

    int32_t n = work_broker_offer_encode(WORK_BROKER_OFFER_WINDOW + 1u, &pkt, 1, buf, sizeof buf);
    work_broker_on_offer(0, buf, (size_t)n);
    ASSERT_EQ_FMT((unsigned long long)WORK_BROKER_OFFER_WINDOW,
                  work_broker_pending_packets(), "%llu");

    work_broker_parent_reset();
    PASS();
}


/* Périmètre A5 : un fork PRUNER garde sa connexion même sous --local-dispatch.
 * Le courtier ne sert que des racines de recherche ; couper la connexion d'un
 * pruner le priverait de travail sans rien lui donner en échange. */
TEST exclusive_mode_never_applies_to_pruner_forks(void)
{
    int saved_opt = local_dispatch_enabled;
    int saved_mode = pruner_mode;

    local_dispatch_enabled = 0; pruner_mode = 0;
    ASSERT_EQ_FMT(0, work_broker_child_is_exclusive(), "%d");
    local_dispatch_enabled = 1; pruner_mode = 0;
    ASSERT_EQ_FMT(1, work_broker_child_is_exclusive(), "%d");
    local_dispatch_enabled = 1; pruner_mode = 1;
    ASSERT_EQ_FMT(0, work_broker_child_is_exclusive(), "%d");

    local_dispatch_enabled = saved_opt;
    pruner_mode = saved_mode;
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
    RUN_TEST(exclusive_mode_never_applies_to_pruner_forks);
    RUN_TEST(acc_settles_when_offer_fully_disposed);
    RUN_TEST(acc_never_settles_past_an_outstanding_offer);
    RUN_TEST(acc_add_refuses_when_ring_is_full);
    RUN_TEST(acc_dispose_reports_unknown_offer);
    RUN_TEST(tag_encode_decode_roundtrip);
    RUN_TEST(request_takes_one_packet_out_of_the_buffer);
    RUN_TEST(request_from_unknown_slot_is_ignored);
    RUN_TEST(done_without_matching_grant_is_ignored);
    RUN_TEST(offer_beyond_window_is_refused_whole);
}
