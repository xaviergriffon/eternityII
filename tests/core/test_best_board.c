/*
 * Tests unitaires de best_board.c (mémorisation de la représentation du
 * plateau au meilleur résultat, cf. AGENTS.md — statistiques).
 *
 * Seule dépendance de link : aucune (best_board.c n'a besoin que de pthread).
 */
#include "greatest.h"
#include "core/best_board.h"

#include <string.h>
#include <unistd.h>
#include <stdlib.h>

/* Construit un possibility_packet minimal, distinguable par une valeur
 * arbitraire dans grid[0][0] : le contenu réel des pièces n'a aucune
 * importance pour ce module, qui ne fait que copier/comparer `alloc`. */
static void make_board(struct possibility_packet *out, int16_t marker, uint16_t alloc)
{
    memset(out, 0, sizeof(*out));
    out->grid[0][0] = marker;
    out->alloc = alloc;
}

TEST fresh_instance_has_no_record(void)
{
    best_board_t bb;
    best_board_init(&bb);

    ASSERT_EQ_FMT(0, best_board_get(&bb, NULL, NULL), "%d");
    ASSERT_EQ_FMT((int)0, (int)best_board_result(&bb), "%d");
    PASS();
}

TEST first_record_is_always_accepted(void)
{
    best_board_t bb;
    best_board_init(&bb);

    struct possibility_packet board;
    make_board(&board, 42, 5);
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &board, 5), "%d");

    struct possibility_packet out;
    uint16_t alloc = 0;
    ASSERT_EQ_FMT(1, best_board_get(&bb, &out, &alloc), "%d");
    ASSERT_EQ_FMT(5, (int)alloc, "%d");
    ASSERT_EQ_FMT(5, (int)out.alloc, "%d");
    ASSERT_EQ_FMT(42, (int)out.grid[0][0], "%d");
    PASS();
}

/* Demande explicite : on ne garde QUE la première représentation qui dépasse
 * strictement le record — un nouveau plateau à égalité n'écrase jamais le
 * précédent. */
TEST equal_alloc_does_not_overwrite(void)
{
    best_board_t bb;
    best_board_init(&bb);

    struct possibility_packet first;
    make_board(&first, 1, 10);
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &first, 10), "%d");

    struct possibility_packet second;
    make_board(&second, 2, 10);
    ASSERT_EQ_FMT(0, best_board_try_record(&bb, &second, 10), "%d");

    struct possibility_packet out;
    ASSERT_EQ_FMT(1, best_board_get(&bb, &out, NULL), "%d");
    ASSERT_EQ_FMT(1, (int)out.grid[0][0], "%d"); /* le premier est toujours là */
    PASS();
}

TEST lower_alloc_does_not_overwrite(void)
{
    best_board_t bb;
    best_board_init(&bb);

    struct possibility_packet first;
    make_board(&first, 1, 10);
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &first, 10), "%d");

    struct possibility_packet second;
    make_board(&second, 2, 3);
    ASSERT_EQ_FMT(0, best_board_try_record(&bb, &second, 3), "%d");

    ASSERT_EQ_FMT(10, (int)best_board_result(&bb), "%d");
    PASS();
}

TEST strictly_greater_alloc_overwrites(void)
{
    best_board_t bb;
    best_board_init(&bb);

    struct possibility_packet first;
    make_board(&first, 1, 10);
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &first, 10), "%d");

    struct possibility_packet second;
    make_board(&second, 2, 11);
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &second, 11), "%d");

    struct possibility_packet out;
    uint16_t alloc = 0;
    ASSERT_EQ_FMT(1, best_board_get(&bb, &out, &alloc), "%d");
    ASSERT_EQ_FMT(11, (int)alloc, "%d");
    ASSERT_EQ_FMT(2, (int)out.grid[0][0], "%d");
    PASS();
}

/* Le champ alloc STOCKÉ suit le paramètre `alloc`, pas board->alloc : certains
 * sites d'appel (cases pré-remplies du backtracking) passent un plateau dont
 * `alloc` ne reflète pas encore la profondeur atteinte. */
TEST stored_alloc_follows_parameter_not_board_field(void)
{
    best_board_t bb;
    best_board_init(&bb);

    struct possibility_packet board;
    make_board(&board, 7, 999); /* board.alloc volontairement incohérent */
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &board, 6), "%d");

    struct possibility_packet out;
    ASSERT_EQ_FMT(1, best_board_get(&bb, &out, NULL), "%d");
    ASSERT_EQ_FMT(6, (int)out.alloc, "%d");
    PASS();
}

TEST try_record_null_args_are_noop(void)
{
    best_board_t bb;
    best_board_init(&bb);
    struct possibility_packet board;
    make_board(&board, 1, 5);

    ASSERT_EQ_FMT(0, best_board_try_record(NULL, &board, 5), "%d");
    ASSERT_EQ_FMT(0, best_board_try_record(&bb, NULL, 5), "%d");
    ASSERT_EQ_FMT(0, best_board_get(NULL, NULL, NULL), "%d");
    ASSERT_EQ_FMT(0, (int)best_board_result(NULL), "%d");
    PASS();
}

TEST save_load_round_trip_preserves_record(void)
{
    best_board_t bb;
    best_board_init(&bb);
    struct possibility_packet board;
    make_board(&board, 77, 123);
    ASSERT_EQ_FMT(1, best_board_try_record(&bb, &board, 123), "%d");

    char path[] = "/tmp/etii_best_board_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, best_board_save(&bb, path), "%d");

    best_board_t loaded;
    best_board_init(&loaded);
    ASSERT_EQ_FMT(0, best_board_load(&loaded, path), "%d");

    struct possibility_packet out;
    uint16_t alloc = 0;
    ASSERT_EQ_FMT(1, best_board_get(&loaded, &out, &alloc), "%d");
    ASSERT_EQ_FMT(123, (int)alloc, "%d");
    ASSERT_EQ_FMT(77, (int)out.grid[0][0], "%d");

    unlink(path);
    PASS();
}

/* Une instance sans enregistrement se sauvegarde/recharge comme "aucun
 * plateau" — pas d'erreur, `has_board` (via best_board_get) reste à 0. */
TEST save_load_round_trip_preserves_absence_of_record(void)
{
    best_board_t bb;
    best_board_init(&bb);

    char path[] = "/tmp/etii_best_board_empty_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    ASSERT_EQ_FMT(0, best_board_save(&bb, path), "%d");

    best_board_t loaded;
    best_board_init(&loaded);
    /* Pré-remplit `loaded` pour vérifier que le load écrase bien vers "vide". */
    struct possibility_packet stale;
    make_board(&stale, 9, 50);
    best_board_try_record(&loaded, &stale, 50);

    ASSERT_EQ_FMT(0, best_board_load(&loaded, path), "%d");
    ASSERT_EQ_FMT(0, best_board_get(&loaded, NULL, NULL), "%d");

    unlink(path);
    PASS();
}

TEST load_missing_file_returns_error(void)
{
    best_board_t bb;
    best_board_init(&bb);
    ASSERT_EQ_FMT(-1, best_board_load(&bb, "/tmp/etii_no_such_best_board_zzz_999"), "%d");
    PASS();
}

SUITE(best_board_suite)
{
    RUN_TEST(fresh_instance_has_no_record);
    RUN_TEST(first_record_is_always_accepted);
    RUN_TEST(equal_alloc_does_not_overwrite);
    RUN_TEST(lower_alloc_does_not_overwrite);
    RUN_TEST(strictly_greater_alloc_overwrites);
    RUN_TEST(stored_alloc_follows_parameter_not_board_field);
    RUN_TEST(try_record_null_args_are_noop);
    RUN_TEST(save_load_round_trip_preserves_record);
    RUN_TEST(save_load_round_trip_preserves_absence_of_record);
    RUN_TEST(load_missing_file_returns_error);
}
