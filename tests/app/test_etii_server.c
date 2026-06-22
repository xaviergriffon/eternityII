/*
 * Tests unitaires des fonctions pures extraites de etii_server.c.
 *
 * Fonctions couvertes :
 *   - clamp_pruner_batch     : borne la taille d'un lot pruner dans [1, PRUNER_BATCH_MAX]
 *   - find_free_thread_slot  : premier slot exist!=0 && socket_id==-1
 *   - find_empty_thread_slot : premier slot exist==0
 */
#include "greatest.h"
#include "app/etii_server.h"

#include <string.h>

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
}
