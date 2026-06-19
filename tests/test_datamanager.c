/*
 * Tests unitaires de datamanager.c — opérations sur les files de possibilités
 * et round-trip backup/restore.
 *
 * datamanager.c gère un état global (files statiques mutex-protégées). En
 * mono-thread, pthread_mutex_trylock réussit toujours ; on n'exerce donc QUE la
 * logique de structure de données et de sérialisation, pas la concurrence ni
 * les chemins réseau (server_ip reste NULL -> add/get travaillent en local ;
 * create_tcp_client est stubbé).
 *
 * Indépendance des tests : chaque test commence par drain_datamanager() qui
 * vide les files (l'état global est partagé entre tests).
 */
#include "greatest.h"
#include "../datamanager.h"
#include "../possibility.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Vide entièrement les pools locaux (vérifié + non vérifié). */
static void drain_datamanager(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000);
        free_array_possibility_packet(r);
    }
}

/* Ajoute n possibilités non vérifiées (checked = 0) d'allocs donnés. */
static void add_packets(const int *allocs, int n)
{
    array_possibility_packet arr;
    arr.size = n;
    arr.possibilities = calloc(n, sizeof(struct possibility_packet));
    for (int i = 0; i < n; i++) {
        arr.possibilities[i].alloc = (uint16_t)allocs[i];
        arr.possibilities[i].checked = 0;
    }
    add_possibility(NULL, &arr); /* server_ip == NULL -> put_to_local */
    free(arr.possibilities);
}

/* --------------------------------------------------------------------------
 * set_server_ip / get_server_ip
 * ------------------------------------------------------------------------ */

TEST server_ip_round_trip(void)
{
    set_server_ip("192.168.1.42");
    char *ip = get_server_ip();
    ASSERT(ip != NULL);
    ASSERT_STR_EQ("192.168.1.42", ip);
    free(ip);

    /* Remise à NULL : get renvoie NULL. */
    set_server_ip(NULL);
    ASSERT_EQ(NULL, get_server_ip());

    /* Chaîne vide traitée comme « pas d'IP ». */
    set_server_ip("");
    ASSERT_EQ(NULL, get_server_ip());
    PASS();
}

/* --------------------------------------------------------------------------
 * add_possibility / datas_size / file_size
 * ------------------------------------------------------------------------ */

TEST add_increases_datas_size(void)
{
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    int allocs[] = { 3, 5, 7 };
    add_packets(allocs, 3);

    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");
    /* Toutes les possibilités non vérifiées atterrissent dans le pool 0. */
    ASSERT_EQ_FMT(3ULL, file_size(0), "%llu");

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * get_last_possibility : extraction (LIFO local)
 * ------------------------------------------------------------------------ */

TEST get_last_possibility_drains_pool(void)
{
    drain_datamanager();
    int allocs[] = { 2, 4 };
    add_packets(allocs, 2);

    array_possibility_packet *r = get_last_possibility(NULL, 10);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(2, r->size, "%d");
    free_array_possibility_packet(r);

    /* Le pool est vidé. */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* Sur un pool vide, get renvoie un tableau de taille 0. */
    array_possibility_packet *empty = get_last_possibility(NULL, 10);
    ASSERT_EQ_FMT(0, empty->size, "%d");
    free_array_possibility_packet(empty);
    PASS();
}

/* --------------------------------------------------------------------------
 * search_min_datas : plus petit alloc présent (0 si vide)
 * ------------------------------------------------------------------------ */

TEST search_min_datas_finds_minimum(void)
{
    drain_datamanager();
    ASSERT_EQ_FMT(0, search_min_datas(), "%d"); /* aucun élément */

    int allocs[] = { 9, 4, 7 };
    add_packets(allocs, 3);
    ASSERT_EQ_FMT(4, search_min_datas(), "%d"); /* le minimum */

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * backup / restore : round-trip sur fichier temporaire
 * ------------------------------------------------------------------------ */

TEST backup_then_restore_preserves_count(void)
{
    drain_datamanager();
    int allocs[] = { 1, 2, 3, 4 };
    add_packets(allocs, 4);

    char path[] = "/tmp/etii_back_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, backup(path), "%d");

    /* On vide le stock courant, puis on restaure depuis le fichier. */
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    ASSERT_EQ_FMT(0, restore(path), "%d");
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu"); /* les 4 possibilités sont revenues */

    unlink(path);
    drain_datamanager();
    PASS();
}

/* restore sur un fichier inexistant échoue (-1) sans toucher au stock. */
TEST restore_missing_file_returns_error(void)
{
    drain_datamanager();
    int allocs[] = { 5 };
    add_packets(allocs, 1);

    ASSERT_EQ_FMT(-1, restore("/tmp/etii_no_such_back_zzz_999"), "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* stock préservé */

    drain_datamanager();
    PASS();
}

/* --------------------------------------------------------------------------
 * split_datas / regroup_datas : redistribution puis consolidation
 * ------------------------------------------------------------------------ */

TEST split_then_regroup_preserves_count(void)
{
    drain_datamanager();
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);

    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* tout dans le pool 0 au départ */

    split_datas();
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu"); /* total préservé */
    ASSERT(file_size(0) < 10);                  /* réparti sur plusieurs files */

    regroup_datas();
    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* re-consolidé dans le pool 0 */

    drain_datamanager();
    PASS();
}

SUITE(datamanager_suite)
{
    RUN_TEST(server_ip_round_trip);
    RUN_TEST(add_increases_datas_size);
    RUN_TEST(get_last_possibility_drains_pool);
    RUN_TEST(search_min_datas_finds_minimum);
    RUN_TEST(backup_then_restore_preserves_count);
    RUN_TEST(restore_missing_file_returns_error);
    RUN_TEST(split_then_regroup_preserves_count);
}
