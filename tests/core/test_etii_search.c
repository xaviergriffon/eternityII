/*
 * Tests unitaires de etii_search.c.
 *
 * Ce fichier inclut directement l'unité de compilation `core/etii_search.c`
 * (et non son seul en-tête). Deux raisons :
 *   1. Il expose les fonctions globales mais non déclarées dans etii_search.h
 *      (checkAndDelegatePossibilitiesIfNeeded[_with_big_table]).
 *   2. Surtout, il rend testables les helpers `static` de la boucle chaude de
 *      backtracking (bt_init_constraints, bt_propagate_place/undo,
 *      bt_count_pending, bt_forward_check) — autrement inatteignables.
 *
 * CONSÉQUENCE BUILD : comme cette TU définit ici tous les symboles de
 * etii_search.c, ce module est RETIRÉ de TEST_MODULES dans le Makefile (sinon
 * doubles définitions au link). Ce fichier de test est donc l'unique fournisseur
 * des symboles de etii_search pour tout le binaire de test.
 *
 * Le reste du module (autosearch/autoprune/search_packet_backtracking) reste
 * constitué de boucles de threads, hors périmètre unitaire.
 */
#include "greatest.h"

/* L'unité de compilation complète : helpers static + fonctions globales. */
#include "core/etii_search.c"

#include "app/etii_client.h"       /* client_possibility_t */
#include "core/lifo.h"             /* File, big_table */
#include "core/datamanager.h"      /* datas_size, get_last_possibility */
#include "core/possibility.h"
#include "app/static_variables.h"  /* max_stock_by_thread */

#include <stdlib.h>
#include <string.h>

/* ======================================================================
 * checkAndDelegatePossibilitiesIfNeeded(_with_big_table)
 *
 * Délègue au serveur les possibilités au-delà de max_stock_by_thread. En
 * l'absence de serveur (server_ip == NULL), add_possibility route vers le
 * datamanager local : on passe client = NULL et on observe le stock local.
 * ====================================================================== */

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

/* ======================================================================
 * Helpers de backtracking (static dans etii_search.c).
 *
 * Fixtures construites à la main, indépendantes de pieces.csv et de
 * ETERN_PARTS : un plateau vide (grid == -2 partout), quelques pièces de
 * rotation aux faces connues, et — pour le forward-check — une map_big_array
 * minuscule fabriquée de toutes pièces.
 * ====================================================================== */

#define ALL_FACE 5  /* sentinelle « toute face » arbitraire pour les tests de cache */

static int key_eq(const key_part *a, const key_part *b)
{
    return a->k1 == b->k1 && a->k2 == b->k2 && a->k3 == b->k3 && a->k4 == b->k4;
}

/* Plateau vide : toutes les cases « non remplies » (-2), aucune pièce utilisée. */
static void make_empty_board(struct possibility_packet *b)
{
    memset(b, 0, sizeof(*b));
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            b->grid[x][y] = -2;
}

/* Tableau de rotations factice : index i -> pièce aux faces distinctes connues. */
static struct array_part *make_parts(void)
{
    static struct part parts[8];
    static struct array_part ap;
    for (int i = 0; i < 8; i++) {
        parts[i].id = (int16_t)(i + 1);
        parts[i].top    = (int8_t)(10 * i + 1);
        parts[i].right  = (int8_t)(10 * i + 2);
        parts[i].bottom = (int8_t)(10 * i + 3);
        parts[i].left   = (int8_t)(10 * i + 4);
        parts[i].rotation = 0;
    }
    ap.size = 8;
    ap.parts = parts;
    return &ap;
}

/* bt_init_constraints : clés de coin et d'intérieur sur un plateau vide. */
TEST bt_init_constraints_empty_board(void)
{
    struct array_part *all = make_parts();
    struct possibility_packet board;
    make_empty_board(&board);

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, all, ALL_FACE);

    /* Coin (0,0) : bord en haut/gauche (0), voisins droite/bas vides (all_face). */
    key_part corner = { 0, ALL_FACE, ALL_FACE, 0 };
    ASSERT(key_eq(&C[0][0], &corner));

    /* Intérieur (1,1) : les 4 voisins existent et sont vides -> all_face partout. */
    key_part inside = { ALL_FACE, ALL_FACE, ALL_FACE, ALL_FACE };
    ASSERT(key_eq(&C[1][1], &inside));

    PASS();
}

/*
 * Invariant central de la boucle chaude : maintenir le cache incrémentalement
 * (bt_propagate_place) donne le MÊME cache qu'un recalcul complet
 * (bt_init_constraints) sur le plateau résultant ; bt_propagate_undo le ramène
 * exactement à l'état du plateau vide.
 */
TEST bt_propagate_matches_full_recompute(void)
{
    struct array_part *all = make_parts();
    const int idx = 6;            /* pièce posée : all->parts[6] */
    const int cx = 1, cy = 1;     /* case intérieure valable en 4x4 comme en 16x16 */

    /* Référence : init complet sur plateau vide. */
    struct possibility_packet empty;
    make_empty_board(&empty);
    key_part Cempty[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(Cempty, &empty, all, ALL_FACE);

    /* Référence : init complet sur plateau avec la pièce posée en (cx,cy). */
    struct possibility_packet placed;
    make_empty_board(&placed);
    placed.grid[cx][cy] = (int16_t)idx;
    key_part Cfull[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(Cfull, &placed, all, ALL_FACE);

    /* Incrémental : partir du vide puis propager le placement. */
    key_part C[ETERN_SIZE][ETERN_SIZE];
    memcpy(C, Cempty, sizeof(C));
    bt_propagate_place(C, cx, cy, &all->parts[idx]);

    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            ASSERT(key_eq(&C[x][y], &Cfull[x][y]));

    /* Annulation : on doit retrouver exactement le cache du plateau vide. */
    bt_propagate_undo(C, cx, cy, ALL_FACE);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            ASSERT(key_eq(&C[x][y], &Cempty[x][y]));

    PASS();
}

/*
 * bt_count_pending : compte, pour chaque niveau de la pile, les candidats
 * restants (indice >= next_s) dont la pièce est libre dans l'état « racine ».
 * Pile : niveau 0 = [1,2,3] pièce 1 posée (next_s=1) ; niveau 1 = [4,5] pièce 4
 * posée (next_s=1). Plateau au feuillage : pièces 1 et 4 marquées utilisées.
 */
TEST bt_count_pending_counts_remaining_free(void)
{
    struct part cand_a[3] = {
        { .id = 1 }, { .id = 2 }, { .id = 3 }
    };
    struct part cand_b[2] = {
        { .id = 4 }, { .id = 5 }
    };
    struct array_part list_a = { .size = 3, .parts = cand_a };
    struct array_part list_b = { .size = 2, .parts = cand_b };

    bt_level stack[2];
    stack[0].search = &list_a; stack[0].next_s = 1; stack[0].placed_pos = 0; /* id 1 */
    stack[1].search = &list_b; stack[1].next_s = 1; stack[1].placed_pos = 3; /* id 4 */

    struct possibility_packet board;
    make_empty_board(&board);
    set_face_used(board.b_faceused, 0, 1); /* pièce 1 (chemin) */
    set_face_used(board.b_faceused, 3, 1); /* pièce 4 (chemin) */

    /* niveau 0 : id 2,3 libres -> 2 ; niveau 1 : id 5 libre -> 1. Total 3. */
    ASSERT_EQ_FMT(3ULL, bt_count_pending(&board, stack, 1), "%llu");

    /* Avec la pièce 2 utilisée à la racine (hors chemin) : elle est exclue. */
    set_face_used(board.b_faceused, 1, 1); /* pièce 2 */
    ASSERT_EQ_FMT(2ULL, bt_count_pending(&board, stack, 1), "%llu");

    PASS();
}

#if FORWARD_CHECK_K > 0
/* Construit une map_big_array minuscule (sizearray=3, all_face=2) dont TOUTES les
 * entrées pointent vers `cand`. Indépendante de la traversée dirx/diry : peu
 * importe la case inspectée, le lookup renvoie toujours la même liste. */
static map_big_array *make_uniform_map(struct array_part *cand)
{
    static map_big_array map;
    static struct array_part flat[3 * 3 * 3 * 3];
    map.sizearray  = 3;
    map.sizearrayM = 2;  /* == all_face passé à bt_init_constraints */
    map.arena = NULL;
    map.flat = flat;
    for (int i = 0; i < 3 * 3 * 3 * 3; i++) flat[i] = *cand;
    return &map;
}

/* bt_forward_check : 1 si chaque case vide de la fenêtre a un candidat libre,
 * 0 si une case est morte (aucun candidat / tous utilisés). */
TEST bt_forward_check_detects_dead_cells(void)
{
    struct possibility_packet board;
    make_empty_board(&board);

    /* all_face DOIT valoir map->sizearrayM (=2) pour que les clés indexent flat. */
    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_parts(), 2);

    /* (a) un candidat libre (pièce 9) pour toute clé -> toutes les cases vivent. */
    struct part p_free[1] = { { .id = 9 } };
    struct array_part cand_free = { .size = 1, .parts = p_free };
    map_big_array *map = make_uniform_map(&cand_free);
    ASSERT_EQ_FMT(1, bt_forward_check(C, &board, map, 0), "%d");

    /* (b) le seul candidat (pièce 9) est déjà utilisé -> case morte -> 0. */
    set_face_used(board.b_faceused, 8, 1); /* pièce 9 */
    ASSERT_EQ_FMT(0, bt_forward_check(C, &board, map, 0), "%d");
    set_face_used(board.b_faceused, 8, 0);

    /* (c) aucun candidat pour aucune clé -> case morte -> 0. */
    struct array_part cand_empty = { .size = 0, .parts = NULL };
    map_big_array *map_empty = make_uniform_map(&cand_empty);
    ASSERT_EQ_FMT(0, bt_forward_check(C, &board, map_empty, 0), "%d");

    PASS();
}
#endif /* FORWARD_CHECK_K > 0 */

SUITE(etii_search_suite)
{
    RUN_TEST(delegate_noop_below_threshold);
    RUN_TEST(delegate_moves_excess_to_local_pool);
    RUN_TEST(delegate_big_table_moves_excess);
    RUN_TEST(bt_init_constraints_empty_board);
    RUN_TEST(bt_propagate_matches_full_recompute);
    RUN_TEST(bt_count_pending_counts_remaining_free);
#if FORWARD_CHECK_K > 0
    RUN_TEST(bt_forward_check_detects_dead_cells);
#endif
}
