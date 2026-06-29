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
#include "core/part.h"             /* prepare_map_part, rotate_all_parts */
#include "core/readdata.h"         /* read_parts */
#include "app/static_variables.h"  /* max_stock_by_thread, request, counters… */
#include "fork_assert.h"           /* run_in_fork (chemins qui exit()) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

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
    init_file(&db, sizeof(struct possibility_packet));
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
    init_file(&db, sizeof(struct possibility_packet));
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

/* ======================================================================
 * Délégation / flush / matérialisation de la pile de décisions.
 *
 * bt_materialize_pending, bt_delegate_if_needed et bt_flush_pending sont la
 * frontière « format paquet » du backtracking in-place. On les exerce sans
 * serveur (server_ip == NULL → add_possibility route vers le datamanager
 * local) ni puzzle réel : un client minimal pointe sur une map UNIFORME
 * (chaque clé -> mêmes candidats libres, comme make_uniform_map) si bien que le
 * forward-checking passe toujours et que la matérialisation est déterministe,
 * indépendamment de ETERN_PARTS.
 *
 * Fixture : pile à 2 niveaux le long du parcours dirx/diry depuis la racine
 *   niveau 0 (case dirx[0]) : candidats [1,2,3], pièce 1 posée, next_s=1
 *   niveau 1 (case dirx[1]) : candidats [4,5],   pièce 4 posée, next_s=1
 * -> 3 frères non explorés (pièces 2,3 au niveau 0 ; pièce 5 au niveau 1).
 * ====================================================================== */

/* counters/lastfilesize sont des pointeurs alloués par init_counters() (main.c,
 * absent du binaire de test) : on les alloue ici pour l'indice compteur 0. */
static void ensure_counters(void)
{
    if (counters == NULL)     counters     = calloc(NB_THREADS, sizeof(*counters));
    if (lastfilesize == NULL) lastfilesize = calloc(NB_THREADS, sizeof(*lastfilesize));
}

/* idParts comme dans autosearch : idParts[p][r] = p + ETERN_PARTS*r. */
static void fill_idparts(int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    for (int p = 0; p <= ETERN_PARTS; p++) {
        int base = p;
        for (int r = 0; r < PART_SIZES; r++) { idParts[p][r] = (int16_t)base; base += ETERN_PARTS; }
    }
}

/* Pièces aux faces PETITES (toutes dans {0,1,2}) : indispensables dès qu'on POSE
 * des pièces, car what_search_in_grid_to_key indexe la map par les couleurs de
 * faces des voisins. make_parts() a des faces jusqu'à ~73 qui déborderaient le
 * flat[3^4] de make_free_map (le test bt_forward_check l'évitait en gardant le
 * plateau vide). 8 pièces (ids 1..8) couvrent les ids posés + les candidats. */
static struct array_part *make_small_parts(void)
{
    static struct part parts[8];
    static struct array_part ap;
    for (int i = 0; i < 8; i++) {
        memset(&parts[i], 0, sizeof(struct part));
        parts[i].id     = (int16_t)(i + 1);
        parts[i].top    = (int8_t)(i % 3);
        parts[i].right  = (int8_t)((i + 1) % 3);
        parts[i].bottom = (int8_t)((i + 2) % 3);
        parts[i].left   = (int8_t)(i % 3);
        parts[i].rotation = 0;
    }
    ap.size = 8;
    ap.parts = parts;
    return &ap;
}

/* Map uniforme dont CHAQUE clé renvoie [pièce 6, pièce 7] (toujours libres dans
 * nos fixtures) : le forward-checking trouve donc toujours un candidat. */
static map_big_array *make_free_map(void)
{
    static struct part cand[2] = { { .id = 6 }, { .id = 7 } };
    static struct array_part list = { .size = 2, .parts = cand };
    static map_big_array map;
    static struct array_part flat[3 * 3 * 3 * 3];
    map.sizearray  = 3;
    map.sizearrayM = 2;
    map.arena = NULL;
    map.flat = flat;
    for (int i = 0; i < 3 * 3 * 3 * 3; i++) flat[i] = list;
    return &map;
}

/* Listes de candidats des deux niveaux (statiques : la pile garde des pointeurs). */
static struct part lvl0_parts[3];
static struct part lvl1_parts[2];
static struct array_part lvl0_list;
static struct array_part lvl1_list;

/* Remplit board + stack (2 niveaux) et le client minimal. Renvoie top (=1). */
static int build_two_level_fixture(struct possibility_packet *board, bt_level stack[2],
                                   client_possibility_t *client)
{
    for (int i = 0; i < 3; i++) { memset(&lvl0_parts[i], 0, sizeof(struct part)); lvl0_parts[i].id = (int16_t)(i + 1); }
    for (int i = 0; i < 2; i++) { memset(&lvl1_parts[i], 0, sizeof(struct part)); lvl1_parts[i].id = (int16_t)(i + 4); }
    lvl0_list.size = 3; lvl0_list.parts = lvl0_parts; /* [1,2,3] */
    lvl1_list.size = 2; lvl1_list.parts = lvl1_parts; /* [4,5]   */

    make_empty_board(board);
    /* Chemin courant : pièce 1 en dirx[0], pièce 4 en dirx[1]. */
    board->grid[dirx[0]][diry[0]] = 1;
    board->grid[dirx[1]][diry[1]] = 4;
    set_face_used(board->b_faceused, 0, 1); /* pièce 1 */
    set_face_used(board->b_faceused, 3, 1); /* pièce 4 */
    board->alloc = 2;

    stack[0].search = &lvl0_list; stack[0].next_s = 1; stack[0].placed_pos = 0; /* pièce 1 */
    stack[1].search = &lvl1_list; stack[1].next_s = 1; stack[1].placed_pos = 3; /* pièce 4 */

    memset(client, 0, sizeof(*client));
    client->compteur = 0;
    client->map_part = make_free_map();
    client->all_rotate_part = make_small_parts();
    return 1;
}

/* bt_materialize_pending : matérialise les frères du plus profond vers la racine. */
TEST bt_materialize_pending_orders_deepest_first(void)
{
    drain_local();
    ensure_counters();

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    struct possibility_packet out[8];
    int new_next_s[2];
    int n = bt_materialize_pending(&client, &board, stack, top, 0, idParts, out, 8, new_next_s);

    /* 3 frères : pièce 5 (niveau 1, le plus profond), puis pièces 2 et 3 (niveau 0). */
    ASSERT_EQ_FMT(3, n, "%d");
    /* out[0] = niveau 1 : pièce 5 en dirx[1], alloc = 2 (= d+1, d=1). */
    ASSERT_EQ_FMT(2, (int)out[0].alloc, "%d");
    ASSERT_EQ_FMT(5, (int)out[0].grid[dirx[1]][diry[1]], "%d");
    ASSERT(is_face_used(out[0].b_faceused, 4)); /* pièce 5 */
    /* out[1] = niveau 0 : pièce 2 en dirx[0], alloc = 1 (= d+1, d=0). */
    ASSERT_EQ_FMT(1, (int)out[1].alloc, "%d");
    ASSERT_EQ_FMT(2, (int)out[1].grid[dirx[0]][diry[0]], "%d");
    /* Positions de reprise consommées : niveau 1 épuisé (2), niveau 0 épuisé (3). */
    ASSERT_EQ_FMT(2, new_next_s[1], "%d");
    ASSERT_EQ_FMT(3, new_next_s[0], "%d");

    PASS();
}

/* bt_materialize_pending : la limite max_out borne le nombre de paquets ; les
 * niveaux non parcourus gardent leur next_s d'origine. */
TEST bt_materialize_pending_respects_max_out(void)
{
    drain_local();
    ensure_counters();

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    struct possibility_packet out[8];
    int new_next_s[2];
    int n = bt_materialize_pending(&client, &board, stack, top, 0, idParts, out, 1, new_next_s);

    ASSERT_EQ_FMT(1, n, "%d");                 /* un seul (le plus profond) */
    ASSERT_EQ_FMT(2, new_next_s[1], "%d");      /* niveau 1 consommé */
    ASSERT_EQ_FMT(1, new_next_s[0], "%d");      /* niveau 0 inchangé (next_s d'origine) */

    PASS();
}

/* bt_delegate_if_needed : stock implicite sous le seuil -> aucun envoi. */
TEST bt_delegate_noop_below_threshold(void)
{
    drain_local();
    ensure_counters();
    max_stock_by_thread = 10;

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    bt_delegate_if_needed(&client, &board, stack, top, 0, idParts);

    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");          /* rien délégué */
    ASSERT_EQ_FMT(3ULL, lastfilesize[0], "%llu");        /* pending = 3 enregistré */
    ASSERT_EQ_FMT(1, stack[1].next_s, "%d");             /* pile inchangée */

    PASS();
}

/* bt_delegate_if_needed : stock au-dessus du seuil -> max_stock_by_thread délégués
 * au pool local, pile avancée, lastfilesize décrémenté du nombre envoyé. */
TEST bt_delegate_moves_excess_to_local(void)
{
    drain_local();
    ensure_counters();
    max_stock_by_thread = 1;

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    bt_delegate_if_needed(&client, &board, stack, top, 0, idParts);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");           /* 1 possibilité déléguée */
    ASSERT_EQ_FMT(2, stack[1].next_s, "%d");             /* niveau profond consommé */
    ASSERT_EQ_FMT(2ULL, lastfilesize[0], "%llu");        /* 3 - 1 restant local */

    drain_local();
    PASS();
}

/* bt_flush_pending : renvoie TOUS les frères (3) + le paquet du chemin courant. */
TEST bt_flush_pending_sends_all_plus_current(void)
{
    drain_local();
    ensure_counters();

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    bt_flush_pending(&client, &board, stack, top, 0, idParts);

    /* 3 frères matérialisés + 1 paquet « chemin courant » = 4. */
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu");

    drain_local();
    PASS();
}

/* search_packet_backtracking : un REQUEST_STOP dès la racine renvoie tout le
 * travail (ici : juste le paquet du chemin courant) au serveur local et sort
 * avec le code « arrêt demandé » (1). Couvre la branche REQUEST_STOP + bt_flush. */
TEST search_backtracking_stop_flushes_and_returns_one(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof(client));
    client.compteur = 0;
    client.map_part = make_free_map();
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    int saved = request;
    request = REQUEST_STOP;
    int rc = search_packet_backtracking(&client, &root, idParts);
    request = saved;

    ASSERT_EQ_FMT(1, rc, "%d");                  /* arrêt demandé */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");    /* le chemin courant renvoyé */

    drain_local();
    PASS();
}

#if ETERN_PARTS == 16
/* ======================================================================
 * Moteur de backtracking de bout en bout, sur le VRAI puzzle 4×4.
 *
 * L'espace de recherche 4×4 est fini et minuscule : lancé depuis la racine vide
 * (alloc = 0), search_packet_backtracking explore tout l'arbre et RETOURNE — pas
 * de boucle infinie, pas de thread. Et comme une solution existe, le parcours
 * passe forcément par record_solution. Sans serveur (server_ip == NULL),
 * send_solution est un no-op et log_solution écrit un fichier solution_*.csv
 * dans le CWD : on isole l'appel dans un fork chdir()é vers un tmpdir.
 * ====================================================================== */

/* Contenu de data/pieces16.csv, embarqué (indépendance au CWD). */
static const char *ES_PIECES16_CSV =
    "ntiles: 16\n"
    "1 3 0 1 5\n"  "2 2 4 0 0\n"  "3 0 0 1 2\n"  "4 1 7 2 0\n"
    "5 8 6 6 8\n"  "6 7 3 0 4\n"  "7 5 7 6 6\n"  "8 8 3 0 3\n"
    "9 1 0 3 7\n"  "10 0 4 2 0\n" "11 6 5 7 7\n" "12 1 0 0 3\n"
    "13 6 5 8 5\n" "14 0 4 8 4\n" "15 0 2 5 4\n" "16 2 8 1 0\n";

static struct array_part *es_make_rotate_parts(void)
{
    char path[] = "/tmp/etii_es_pieces16_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fputs(ES_PIECES16_CSV, fp);
    fclose(fp);
    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

/* Contexte partagé avec les fonctions-fils (copié par fork). */
static client_possibility_t es_client;
static struct possibility_packet es_root;
static int16_t es_idParts[ETERN_PARTS + 1][PART_SIZES];
static char es_solution_dir[256];

/* Fils : explore tout l'arbre (stop_on_solution = 0). record_solution NE sort
 * pas ; la recherche revient avec 0 (sous-arbre épuisé). */
static void es_child_full_explore(void)
{
    if (chdir(es_solution_dir) != 0) _exit(97);
    stop_on_solution = 0;
    request = REQUEST_CONTINUE;
    int rc = search_packet_backtracking(&es_client, &es_root, es_idParts);
    _exit(rc == 0 ? 0 : 3);
}

/* Fils : stop_on_solution = 1. La 1re solution déclenche record_solution ->
 * exit(EXIT_SUCCESS) : on ne revient jamais au _exit(99) ci-dessous. */
static void es_child_stop_on_solution(void)
{
    if (chdir(es_solution_dir) != 0) _exit(97);
    stop_on_solution = 1;
    request = REQUEST_CONTINUE;
    search_packet_backtracking(&es_client, &es_root, es_idParts);
    _exit(99); /* solution non trouvée / pas d'exit : échec */
}

/* Vrai : au moins un fichier solution_*.csv existe dans dir. */
static int es_has_solution_file(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "solution_", 9) == 0) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static void es_unlink_solutions(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "solution_", 9) == 0) {
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

/* Prépare es_client / es_root / es_idParts depuis pieces16.csv. */
static int es_setup(void)
{
    struct array_part *rot = es_make_rotate_parts();
    if (rot == NULL) return 0;
    map_big_array *map = prepare_map_part(rot);
    if (map == NULL) return 0;

    memset(&es_client, 0, sizeof(es_client));
    es_client.compteur = 0;
    es_client.all_rotate_part = rot;
    es_client.map_part = map;

    make_empty_board(&es_root);
    es_root.alloc = 0;

    fill_idparts(es_idParts);
    return 1;
}

/* Exploration complète : la recherche trouve la solution (fichier écrit) puis
 * épuise l'arbre et retourne 0. Couvre search_packet_backtracking (chemin
 * « solution puis goto backtrack ») et record_solution (sans exit). */
TEST search_backtracking_solves_4x4_and_returns_zero(void)
{
    ensure_counters();
    ASSERT(es_setup());

    strcpy(es_solution_dir, "/tmp/etii_es_sol_XXXXXX");
    ASSERT(mkdtemp(es_solution_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(es_child_full_explore, &pid);

    int had_solution = es_has_solution_file(es_solution_dir);
    es_unlink_solutions(es_solution_dir);
    rmdir(es_solution_dir);

    ASSERT_EQ_FMT(0, code, "%d");    /* sous-arbre entièrement exploré */
    ASSERT(had_solution);            /* la solution a bien été enregistrée */

    free_bigarray(es_client.map_part);
    free_array_part(es_client.all_rotate_part);
    PASS();
}

/* stop_on_solution = 1 : la 1re solution provoque exit(EXIT_SUCCESS) via
 * record_solution. Couvre la branche d'arrêt-sur-solution. */
TEST search_backtracking_stop_on_solution_exits_success(void)
{
    ensure_counters();
    ASSERT(es_setup());

    strcpy(es_solution_dir, "/tmp/etii_es_sos_XXXXXX");
    ASSERT(mkdtemp(es_solution_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(es_child_stop_on_solution, &pid);

    es_unlink_solutions(es_solution_dir);
    rmdir(es_solution_dir);

    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d"); /* exit via record_solution */

    free_bigarray(es_client.map_part);
    free_array_part(es_client.all_rotate_part);
    PASS();
}
#endif /* ETERN_PARTS == 16 */

/* --------------------------------------------------------------------------
 * Corps de boucle d'autosearch extrait (testable hors de la boucle infinie).
 * server_ip == NULL -> les renvois passent par le datamanager local.
 * ------------------------------------------------------------------------ */

/* requeue_unprocessed_packets : renvoie en local les paquets racines [from..size) ;
   from >= size -> aucun renvoi. */
TEST requeue_unprocessed_packets_routes_tail_locally(void)
{
    drain_local();
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    struct possibility_packet pkts[5];
    memset(pkts, 0, sizeof pkts);
    for (int i = 0; i < 5; i++) pkts[i].alloc = (uint16_t)(i + 1);
    array_possibility_packet aposs = { .size = 5, .possibilities = pkts };
    client.aposs = &aposs;

    requeue_unprocessed_packets(&client, 2);       /* [2..5) = 3 paquets renvoyés */
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");

    requeue_unprocessed_packets(&client, 5);       /* from >= size -> no-op */
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");

    drain_local();
    PASS();
}

/* autosearch_step en REQUEST_STOP : le paquet racine 0 est traité
   (search_packet_backtracking renvoie 1 sur STOP en renvoyant le chemin courant),
   les paquets suivants sont renvoyés en local, le cycle est nettoyé et la
   fonction renvoie 0 (on arrête la boucle). */
TEST autosearch_step_stop_requeues_and_returns_zero(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_free_map();
    client.all_rotate_part = make_small_parts();
    pthread_mutex_init(&client.works_mutex, NULL); /* autosearch_step verrouille works_mutex */
    client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 3;
    aposs->possibilities = calloc(3, sizeof(struct possibility_packet));
    for (int i = 0; i < 3; i++) make_empty_board(&aposs->possibilities[i]);
    client.aposs = aposs;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    init_id_parts(idParts);

    int saved = request;
    request = REQUEST_STOP;
    int cont = autosearch_step(&client, idParts);
    request = saved;

    ASSERT_EQ_FMT(0, cont, "%d");                  /* 0 -> arrêt de la boucle */
    ASSERT(client.aposs == NULL);                  /* cycle nettoyé */
    ASSERT_EQ_FMT(0, (int)client.works, "%d");     /* works réinitialisé */
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");     /* 1 (chemin courant) + 2 (paquets [1..3)) */

    pthread_mutex_destroy(&client.works_mutex);
    /* map_part / all_rotate_part pointent du stockage statique (make_free_map /
       make_small_parts) : pas de free, comme les autres tests du module. */
    drain_local();
    PASS();
}

/* autosearch_step hors arrêt (REQUEST_CONTINUE) : avec un jeu de possibilités
   VIDE (size 0, non-NULL : on sort de l'attente sans déclencher de backtracking),
   le cycle est nettoyé et la fonction renvoie 1 (on poursuit la boucle). Couvre
   la branche « continue » sans lancer de recherche. */
TEST autosearch_step_continue_returns_one(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);
    client.works = 1; /* works=1 + aposs non-NULL -> on sort de l'attente */

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 0;                                            /* aucun paquet à traiter */
    aposs->possibilities = malloc(sizeof(struct possibility_packet)); /* libéré au nettoyage */
    client.aposs = aposs;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    init_id_parts(idParts);

    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autosearch_step(&client, idParts);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");                  /* 1 -> on poursuit */
    ASSERT(client.aposs == NULL);                  /* cycle nettoyé */
    ASSERT_EQ_FMT(0, (int)client.works, "%d");     /* works réinitialisé */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");     /* rien renvoyé (pas d'arrêt) */

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* --------------------------------------------------------------------------
 * autoprune_step : corps de boucle du pruner extrait (testable hors thread).
 * ------------------------------------------------------------------------ */

/* REQUEST_STOP : la boucle de traitement (gardée par request != STOP) ne tourne
   pas -> tout le lot non vérifié est renvoyé en local, cycle nettoyé, retour 0. */
TEST autoprune_step_stop_requeues_all_and_returns_zero(void)
{
    drain_local();
    ensure_counters();
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);
    client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 3;
    aposs->possibilities = calloc(3, sizeof(struct possibility_packet));
    client.aposs = aposs;

    int saved = request;
    request = REQUEST_STOP;
    int cont = autoprune_step(&client);
    request = saved;

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT(client.aposs == NULL);
    ASSERT_EQ_FMT(0, (int)client.works, "%d");
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu"); /* lot entier renvoyé (rien traité) */

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* REQUEST_CONTINUE + lot vide : rien à contrôler, cycle nettoyé, retour 1. */
TEST autoprune_step_continue_empty_returns_one(void)
{
    drain_local();
    ensure_counters();
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);
    client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 0;
    aposs->possibilities = malloc(sizeof(struct possibility_packet));
    client.aposs = aposs;

    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT(client.aposs == NULL);
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* REQUEST_CONTINUE : un paquet VIVANT (map libre -> toujours un candidat, board
   à alloc 0 donc pas de solution complète) est renvoyé marqué `checked` dans le
   stock local, et pruner_checked est incrémenté. Couvre la branche de contrôle. */
TEST autoprune_step_keeps_live_packet(void)
{
    drain_local();
    ensure_counters();
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_free_map();
    client.all_rotate_part = make_small_parts();
    pthread_mutex_init(&client.works_mutex, NULL);
    client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 1;
    aposs->possibilities = calloc(1, sizeof(struct possibility_packet));
    make_empty_board(&aposs->possibilities[0]);
    client.aposs = aposs;

    unsigned long long checked_before = pruner_checked;
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT(client.aposs == NULL);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");                 /* vivant renvoyé en local */
    ASSERT_EQ_FMT(checked_before + 1, pruner_checked, "%llu"); /* compteur incrémenté */

    pthread_mutex_destroy(&client.works_mutex);
    /* map_part / all_rotate_part : stockage statique, pas de free. */
    drain_local();
    PASS();
}

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
    RUN_TEST(bt_materialize_pending_orders_deepest_first);
    RUN_TEST(bt_materialize_pending_respects_max_out);
    RUN_TEST(bt_delegate_noop_below_threshold);
    RUN_TEST(bt_delegate_moves_excess_to_local);
    RUN_TEST(bt_flush_pending_sends_all_plus_current);
    RUN_TEST(search_backtracking_stop_flushes_and_returns_one);
    RUN_TEST(requeue_unprocessed_packets_routes_tail_locally);
    RUN_TEST(autosearch_step_stop_requeues_and_returns_zero);
    RUN_TEST(autosearch_step_continue_returns_one);
    RUN_TEST(autoprune_step_stop_requeues_all_and_returns_zero);
    RUN_TEST(autoprune_step_continue_empty_returns_one);
    RUN_TEST(autoprune_step_keeps_live_packet);
#if ETERN_PARTS == 16
    RUN_TEST(search_backtracking_solves_4x4_and_returns_zero);
    RUN_TEST(search_backtracking_stop_on_solution_exits_success);
#endif
}
