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
#include "../part.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Non déclarée dans datamanager.h (helper interne non statique). */
unsigned long long count_combinations(unsigned long long x);

/* Coupe temporairement stdout/stderr (fonctions verbeuses : tri, statistiques). */
static int g_fd1 = -1, g_fd2 = -1;
static void silence_std(void)
{
    fflush(stdout); fflush(stderr);
    g_fd1 = dup(1); g_fd2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    dup2(dn, 1); dup2(dn, 2); close(dn);
}
static void restore_std(void)
{
    fflush(stdout); fflush(stderr);
    dup2(g_fd1, 1); dup2(g_fd2, 2);
    close(g_fd1); close(g_fd2);
}

/* Vide entièrement les pools locaux (vérifié + non vérifié). */
static void drain_datamanager(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000);
        free_array_possibility_packet(r);
    }
}

/* Vide aussi le pool « analysed » (réinjecté dans le stock puis drainé). */
static void drain_all(void)
{
    silence_std();
    restock_analysed();
    restore_std();
    drain_datamanager();
}

/* Somme des tailles du pool analysed sur toutes les files. */
static unsigned long long analysed_total(void)
{
    unsigned long long s = 0;
    for (int f = 0; f < 10; f++) s += file_analysed_size(f);
    return s;
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

/* --------------------------------------------------------------------------
 * Pool « checked » : possibilités vérifiées par un pruner (checked == 1)
 * ------------------------------------------------------------------------ */

TEST checked_possibility_goes_to_checked_pool(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 6;
    pk.checked = 1; /* routé vers file_possibility_checked */
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    ASSERT_EQ_FMT(1ULL, file_checked_size(0), "%llu");
    ASSERT_EQ_FMT(0ULL, file_size(0), "%llu");      /* pas dans le pool non vérifié */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");      /* mais compté dans le total */

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Pool « analysed » : add / file_analysed_size / restock
 * ------------------------------------------------------------------------ */

TEST analysed_add_and_restock(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 5;
    add_possibility_analysed(&pk, 0); /* file analysed 0 */

    ASSERT_EQ_FMT(1ULL, file_analysed_size(0), "%llu");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu"); /* le pool analysed n'est pas dans datas_size */

    /* restock : remet la possibilité dans le stock principal */
    silence_std();
    restock_analysed();
    restore_std();
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    drain_all();
    PASS();
}

/* backup_analysed + restore_analysed : round-trip du pool analysed. */
TEST analysed_backup_restore_round_trip(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    for (int i = 0; i < 3; i++) {
        pk.alloc = (uint16_t)(i + 1);
        add_possibility_analysed(&pk, 0);
    }
    ASSERT_EQ_FMT(3ULL, analysed_total(), "%llu");

    char path[] = "/tmp/etii_back_an_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    silence_std();
    ASSERT_EQ_FMT(0, backup_analysed(path), "%d");
    restock_analysed();
    restore_std();
    drain_datamanager();
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    silence_std();
    int rc = restore_analysed(path);
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(3ULL, analysed_total(), "%llu"); /* les 3 sont revenues */

    unlink(path);
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * Tri / statistiques : exécution sans erreur, total préservé (sortie musellée)
 * ------------------------------------------------------------------------ */

TEST sort_preserves_count(void)
{
    drain_all();
    int allocs[] = { 5, 2, 8, 1, 6 };
    add_packets(allocs, 5);

    silence_std();
    int a = sort_ascending();
    int d = sort_descending();
    restore_std();

    ASSERT_EQ_FMT(0, a, "%d");
    ASSERT_EQ_FMT(0, d, "%d");
    ASSERT_EQ_FMT(5ULL, datas_size(), "%llu"); /* total inchangé par le tri */

    drain_all();
    PASS();
}

TEST statistic_and_print_run(void)
{
    drain_all();
    int allocs[] = { 3, 3 };
    add_packets(allocs, 2);

    silence_std();
    statistic_datas();
    printdatamanager();
    print_all_file_analysed();
    restore_std();

    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * count_combinations : nombre de paires (x*(x-1)/2)
 * ------------------------------------------------------------------------ */

TEST count_combinations_is_triangular(void)
{
    ASSERT_EQ_FMT(0ULL, count_combinations(0), "%llu");
    ASSERT_EQ_FMT(0ULL, count_combinations(1), "%llu");
    ASSERT_EQ_FMT(1ULL, count_combinations(2), "%llu");
    ASSERT_EQ_FMT(6ULL, count_combinations(4), "%llu");
    ASSERT_EQ_FMT(10ULL, count_combinations(5), "%llu");
    PASS();
}

/* --------------------------------------------------------------------------
 * get_last_possibility_tocheck : extraction côté pruner (pool non vérifié)
 * ------------------------------------------------------------------------ */

TEST get_tocheck_drains_unchecked_pool(void)
{
    drain_all();
    int allocs[] = { 3, 4 };
    add_packets(allocs, 2); /* non vérifiées -> pool historique */

    array_possibility_packet *r = get_last_possibility_tocheck(10);
    ASSERT(r != NULL);
    ASSERT_EQ_FMT(2, r->size, "%d");
    free_array_possibility_packet(r);
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * remove_possibility_analysed : retrait ciblé dans le pool analysed
 * ------------------------------------------------------------------------ */

TEST remove_analysed_finds_then_misses(void)
{
    drain_all();
    struct possibility_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.alloc = 5;
    add_possibility_analysed(&pk, 0);
    ASSERT_EQ_FMT(1ULL, analysed_total(), "%llu");

    /* trouvée et retirée -> 0 */
    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pk, 0), "%d");
    ASSERT_EQ_FMT(0ULL, analysed_total(), "%llu");

    /* deuxième passage : plus rien à retirer -> 1 */
    ASSERT_EQ_FMT(1, remove_possibility_analysed(&pk, 0), "%d");

    drain_all();
    PASS();
}

/* --------------------------------------------------------------------------
 * remove_possibilities_with_no_next : élagage des impasses du stock
 * ------------------------------------------------------------------------ */

TEST remove_no_next_prunes_dead_packets(void)
{
    drain_all();
    /* map sans pièce « tout bord 0 » : une case (0,0) vide reste sans candidat. */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof(pks));
    /* pks[0] : grille pleine (tout à 0) -> a une suite, conservée */
    /* pks[1] : trou sur la 1re case du parcours, clé (0,0,0,0) sans candidat -> impasse */
    pks[1].grid[dirx[0]][diry[0]] = -2;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    remove_possibilities_with_no_next(map, &rp);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* l'impasse a été retirée */

    free_bigarray(map);
    drain_all();
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
    RUN_TEST(checked_possibility_goes_to_checked_pool);
    RUN_TEST(analysed_add_and_restock);
    RUN_TEST(analysed_backup_restore_round_trip);
    RUN_TEST(sort_preserves_count);
    RUN_TEST(statistic_and_print_run);
    RUN_TEST(count_combinations_is_triangular);
    RUN_TEST(get_tocheck_drains_unchecked_pool);
    RUN_TEST(remove_analysed_finds_then_misses);
    RUN_TEST(remove_no_next_prunes_dead_packets);
}
