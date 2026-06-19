/*
 * Tests unitaires des helpers non-threadés de etii_search.c.
 *
 * checkAndDelegatePossibilitiesIfNeeded(_with_big_table) délègue au serveur les
 * possibilités au-delà de max_stock_by_thread. En l'absence de serveur
 * (server_ip == NULL), add_possibility route vers le datamanager local : on
 * peut donc passer client = NULL et observer le stock local.
 *
 * Ces fonctions sont globales mais non déclarées dans etii_search.h : prototypes
 * locaux ci-dessous. Le reste du module (autosearch/autoprune) est constitué de
 * boucles de threads, hors périmètre unitaire.
 */
#include "greatest.h"
#include "app/etii_client.h"   /* client_possibility_t */
#include "core/lifo.h"          /* File, big_table */
#include "core/datamanager.h"   /* datas_size, get_last_possibility */
#include "core/possibility.h"
#include "app/static_variables.h" /* max_stock_by_thread */

#include <stdlib.h>
#include <string.h>

void checkAndDelegatePossibilitiesIfNeeded(client_possibility_t *client_possibility, File *db);
void checkAndDelegatePossibilitiesIfNeeded_with_big_table(client_possibility_t *client_possibility, big_table *bt);

static void drain_local(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000);
        free_array_possibility_packet(r);
    }
}

/* Stock sous le seuil : aucune délégation. */
TEST delegate_noop_below_threshold(void)
{
    drain_local();
    max_stock_by_thread = 10;

    File db;
    init_file_with_cache(&db, 0, sizeof(struct possibility_packet));
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    for (int i = 0; i < 3; i++) { pk.alloc = (uint16_t)i; put(&db, &pk); }

    checkAndDelegatePossibilitiesIfNeeded(NULL, &db);

    ASSERT_EQ_FMT(3ULL, (unsigned long long)db.size, "%llu"); /* inchangé */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");                /* rien délégué */

    while (db.size > 0) { struct possibility_packet t; scroll(&db, &t); }
    PASS();
}

/* Stock au-dessus du seuil : délègue max_stock_by_thread possibilités au local. */
TEST delegate_moves_excess_to_local_pool(void)
{
    drain_local();
    max_stock_by_thread = 2;

    File db;
    init_file_with_cache(&db, 0, sizeof(struct possibility_packet));
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    for (int i = 0; i < 5; i++) { pk.alloc = (uint16_t)i; put(&db, &pk); }

    checkAndDelegatePossibilitiesIfNeeded(NULL, &db);

    /* remains = 5 - 2 = 3 : la file est ramenée à 3, 2 possibilités déléguées. */
    ASSERT_EQ_FMT(3ULL, (unsigned long long)db.size, "%llu");
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    while (db.size > 0) { struct possibility_packet t; scroll(&db, &t); }
    drain_local();
    PASS();
}

/* Variante big_table : même logique de délégation. */
TEST delegate_big_table_moves_excess(void)
{
    drain_local();
    max_stock_by_thread = 2;

    big_table bt;
    init_big_table(&bt, 4, sizeof(struct possibility_packet));
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    for (int i = 0; i < 5; i++) { pk.alloc = (uint16_t)i; put_big_table(&bt, &pk); }

    checkAndDelegatePossibilitiesIfNeeded_with_big_table(NULL, &bt);

    ASSERT_EQ_FMT(3ULL, (unsigned long long)bt.size, "%llu");
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    clear_big_table(&bt);
    drain_local();
    PASS();
}

SUITE(etii_search_suite)
{
    RUN_TEST(delegate_noop_below_threshold);
    RUN_TEST(delegate_moves_excess_to_local_pool);
    RUN_TEST(delegate_big_table_moves_excess);
}
