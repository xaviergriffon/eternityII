/*
 * Tests unitaires des fonctions pures extraites de etii_server.c.
 *
 * Fonctions couvertes :
 *   - clamp_pruner_batch     : borne la taille d'un lot pruner dans [1, PRUNER_BATCH_MAX]
 *   - find_free_thread_slot  : premier slot exist!=0 && socket_id==-1
 *   - find_empty_thread_slot : premier slot exist==0
 *   - get_active_threads     : compte les slots connectés (socket_id != -1) sur NB_THREADS
 *   - build_file_queues_table: tableau de stats par file (corps extrait de la
 *                              boucle check_server)
 */
#include "greatest.h"
#include "app/etii_server.h"
#include "core/datamanager.h"
#include "core/possibility.h"

#include <string.h>
#include <stdlib.h>

/* Vide le pool local du datamanager (état global partagé entre suites). */
static void dm_drain_all(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000);
        free_array_possibility_packet(r);
    }
}

/* ---------- clamp_pruner_batch ------------------------------------------- */

TEST clamp_nominal_value_unchanged(void)
{
    ASSERT_EQ(10, clamp_pruner_batch(10));
    PASS();
}

TEST clamp_zero_becomes_one(void)
{
    ASSERT_EQ(1, clamp_pruner_batch(0));
    PASS();
}

TEST clamp_negative_becomes_one(void)
{
    ASSERT_EQ(1, clamp_pruner_batch(-5));
    PASS();
}

TEST clamp_exactly_max_unchanged(void)
{
    ASSERT_EQ(PRUNER_BATCH_MAX, clamp_pruner_batch(PRUNER_BATCH_MAX));
    PASS();
}

TEST clamp_above_max_capped(void)
{
    ASSERT_EQ(PRUNER_BATCH_MAX, clamp_pruner_batch(PRUNER_BATCH_MAX + 1));
    ASSERT_EQ(PRUNER_BATCH_MAX, clamp_pruner_batch(999999));
    PASS();
}

TEST clamp_one_unchanged(void)
{
    ASSERT_EQ(1, clamp_pruner_batch(1));
    PASS();
}

/* ---------- find_free_thread_slot ---------------------------------------- */

/* Construit un tableau minimal de client_t avec seulement exist et socket_id */
static client_t make_slot(int exist, int socket_id) {
    client_t s;
    memset(&s, 0, sizeof s);
    s.exist     = exist;
    s.socket_id = socket_id;
    return s;
}

TEST free_slot_none_available(void)
{
    client_t slots[] = {
        make_slot(0, -1),  /* exist==0 : pas libre */
        make_slot(1, 5),   /* occupé   : socket != -1 */
        make_slot(1, 7),
    };
    ASSERT_EQ(-1, find_free_thread_slot(slots, 3));
    PASS();
}

TEST free_slot_first(void)
{
    client_t slots[] = {
        make_slot(1, -1),  /* libre */
        make_slot(1, 5),
    };
    ASSERT_EQ(0, find_free_thread_slot(slots, 2));
    PASS();
}

TEST free_slot_middle(void)
{
    client_t slots[] = {
        make_slot(1, 3),
        make_slot(1, -1),  /* libre */
        make_slot(1, 7),
    };
    ASSERT_EQ(1, find_free_thread_slot(slots, 3));
    PASS();
}

TEST free_slot_zero_nb(void)
{
    client_t slots[1] = { make_slot(1, -1) };
    ASSERT_EQ(-1, find_free_thread_slot(slots, 0));
    PASS();
}

/* ---------- find_empty_thread_slot --------------------------------------- */

TEST empty_slot_none(void)
{
    client_t slots[] = { make_slot(1, -1), make_slot(1, 5) };
    ASSERT_EQ(-1, find_empty_thread_slot(slots, 2));
    PASS();
}

TEST empty_slot_first(void)
{
    client_t slots[] = { make_slot(0, -1), make_slot(1, -1) };
    ASSERT_EQ(0, find_empty_thread_slot(slots, 2));
    PASS();
}

TEST empty_slot_last(void)
{
    client_t slots[] = {
        make_slot(1, -1),
        make_slot(1, 5),
        make_slot(0, -1),  /* vide */
    };
    ASSERT_EQ(2, find_empty_thread_slot(slots, 3));
    PASS();
}

TEST empty_slot_zero_nb(void)
{
    client_t slots[1] = { make_slot(0, -1) };
    ASSERT_EQ(-1, find_empty_thread_slot(slots, 0));
    PASS();
}

/* ---------- get_active_threads ------------------------------------------- */

/* get_active_threads ne prend pas de paramètre de taille : il boucle sur le
   global NB_THREADS. On le fixe à la taille de la fixture le temps du test
   (sauvegarde/restauration AVANT toute assertion : NB_THREADS est partagé avec
   les autres suites, défaut 10). */

TEST active_threads_null_is_zero(void)
{
    ASSERT_EQ(0, get_active_threads(NULL));
    PASS();
}

TEST active_threads_counts_connected_slots(void)
{
    int saved = NB_THREADS;
    client_t slots[] = {
        make_slot(1, -1),  /* libre              -> non compté */
        make_slot(1, 5),   /* connecté           -> compté     */
        make_slot(0, 8),   /* socket != -1 (exist ignoré)      -> compté     */
        make_slot(1, -1),  /* libre              -> non compté */
        make_slot(1, 42),  /* connecté           -> compté     */
    };
    NB_THREADS = 5;
    int active = get_active_threads(slots);
    NB_THREADS = saved;
    ASSERT_EQ(3, active);  /* seuls socket_id != -1 comptent */
    PASS();
}

TEST active_threads_none_connected_is_zero(void)
{
    int saved = NB_THREADS;
    client_t slots[] = { make_slot(1, -1), make_slot(0, -1), make_slot(1, -1) };
    NB_THREADS = 3;
    int active = get_active_threads(slots);
    NB_THREADS = saved;
    ASSERT_EQ(0, active);
    PASS();
}

TEST active_threads_all_connected(void)
{
    int saved = NB_THREADS;
    client_t slots[] = { make_slot(1, 1), make_slot(1, 2), make_slot(1, 3), make_slot(1, 4) };
    NB_THREADS = 4;
    int active = get_active_threads(slots);
    NB_THREADS = saved;
    ASSERT_EQ(4, active);
    PASS();
}

/* ---------- build_file_queues_table -------------------------------------- */

/* Sur un stock vide, tous les totaux valent 0 et le tableau est bien structuré
   (en-tête + ligne Total). Exerce la boucle complète sur NB_FILE_POSSIBILITY. */
TEST file_queues_table_empty_is_all_zero(void)
{
    dm_drain_all();
    unsigned long long u = 9, c = 9, a = 9;   /* sentinelles non nulles */
    char *t = build_file_queues_table(&u, &c, &a);
    int header = (strstr(t, "File queues") != NULL);
    int total  = (strstr(t, "Total|") != NULL);
    free(t);
    ASSERT(header);
    ASSERT(total);
    ASSERT_EQ_FMT(0ULL, u, "%llu");
    ASSERT_EQ_FMT(0ULL, c, "%llu");
    ASSERT_EQ_FMT(0ULL, a, "%llu");
    PASS();
}

/* Le total « unchecked » reflète le stock non vérifié réellement présent. */
TEST file_queues_table_reflects_unchecked_stock(void)
{
    dm_drain_all();
    struct possibility_packet pks[3];
    memset(pks, 0, sizeof pks);
    for (int i = 0; i < 3; i++) pks[i].alloc = (uint16_t)(i + 1);
    array_possibility_packet arr = { .size = 3, .possibilities = pks };
    add_possibility(NULL, &arr);

    unsigned long long u = 0, c = 0, a = 0;
    char *t = build_file_queues_table(&u, &c, &a);
    free(t);
    dm_drain_all();

    ASSERT_EQ_FMT(3ULL, u, "%llu");  /* 3 possibilités non vérifiées */
    ASSERT_EQ_FMT(0ULL, c, "%llu");
    ASSERT_EQ_FMT(0ULL, a, "%llu");
    PASS();
}

/* ---------- requeue_last_sent_possibility -------------------------------- */

/* NULL : no-op, ne touche pas le stock. */
TEST requeue_null_is_noop(void)
{
    dm_drain_all();
    requeue_last_sent_possibility(NULL);
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");
    PASS();
}

/* Possibilité encore « en analyse » (le client ne l'a pas acquittée) :
   à la déconnexion, elle est retirée de l'« en analyse » et rendue au stock. */
TEST requeue_unacked_returns_to_stock(void)
{
    dm_drain_all();

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 7;
    add_possibility_analysed(&pkt, -1);          /* le serveur l'avait servie */

    array_possibility_packet sent = { .size = 1, .possibilities = &pkt };
    requeue_last_sent_possibility(&sent);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");   /* rendue au stock */
    dm_drain_all();
    PASS();
}

/* Possibilité déjà acquittée (absente de file_analysed) : non réinjectée,
   sinon le travail déjà terminé serait dupliqué. */
TEST requeue_acked_is_skipped(void)
{
    dm_drain_all();

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 9;
    /* jamais ajoutée à file_analysed : simule un client ayant déjà acquitté */

    array_possibility_packet sent = { .size = 1, .possibilities = &pkt };
    requeue_last_sent_possibility(&sent);

    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* rien rendu */
    PASS();
}

/* Lot mixte : seules les possibilités encore en analyse reviennent. */
TEST requeue_mixed_batch_returns_only_unacked(void)
{
    dm_drain_all();

    struct possibility_packet pkts[3];
    memset(pkts, 0, sizeof pkts);
    for (int i = 0; i < 3; i++) pkts[i].alloc = (uint16_t)(i + 1);
    /* Seules pkts[0] et pkts[2] sont « en analyse » (pkts[1] déjà acquittée). */
    add_possibility_analysed(&pkts[0], -1);
    add_possibility_analysed(&pkts[2], -1);

    array_possibility_packet sent = { .size = 3, .possibilities = pkts };
    requeue_last_sent_possibility(&sent);

    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    dm_drain_all();
    PASS();
}

/* ---------- suite --------------------------------------------------------- */

SUITE(etii_server_suite)
{
    RUN_TEST(clamp_nominal_value_unchanged);
    RUN_TEST(clamp_zero_becomes_one);
    RUN_TEST(clamp_negative_becomes_one);
    RUN_TEST(clamp_exactly_max_unchanged);
    RUN_TEST(clamp_above_max_capped);
    RUN_TEST(clamp_one_unchanged);

    RUN_TEST(free_slot_none_available);
    RUN_TEST(free_slot_first);
    RUN_TEST(free_slot_middle);
    RUN_TEST(free_slot_zero_nb);

    RUN_TEST(empty_slot_none);
    RUN_TEST(empty_slot_first);
    RUN_TEST(empty_slot_last);
    RUN_TEST(empty_slot_zero_nb);

    RUN_TEST(active_threads_null_is_zero);
    RUN_TEST(active_threads_counts_connected_slots);
    RUN_TEST(active_threads_none_connected_is_zero);
    RUN_TEST(active_threads_all_connected);

    RUN_TEST(file_queues_table_empty_is_all_zero);
    RUN_TEST(file_queues_table_reflects_unchecked_stock);

    RUN_TEST(requeue_null_is_noop);
    RUN_TEST(requeue_unacked_returns_to_stock);
    RUN_TEST(requeue_acked_is_skipped);
    RUN_TEST(requeue_mixed_batch_returns_only_unacked);
}
