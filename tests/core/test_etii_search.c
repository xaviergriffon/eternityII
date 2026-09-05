/*
 * Tests unitaires de etii_search.c.
 *
 * Ce fichier inclut directement l'unité de compilation `core/etii_search.c`
 * (et non son seul en-tête). Deux raisons :
 *   1. Il expose les fonctions globales mais non déclarées dans etii_search.h
 *      (checkAndDelegatePossibilitiesIfNeeded).
 *   2. Surtout, il rend testables les helpers `static` de la boucle chaude de
 *      backtracking (bt_init_constraints, bt_propagate_place/undo,
 *      bt_count_pending, bt_forward_check) — autrement inatteignables.
 *
 * CONSÉQUENCE BUILD : comme cette TU définit ici tous les symboles de
 * etii_search.c, ce module est RETIRÉ de TEST_MODULES dans le Makefile (sinon
 * doubles définitions au link). Ce fichier de test est donc l'unique fournisseur
 * des symboles de etii_search pour tout le binaire de test.
 *
 * `search_packet_backtracking` reste une boucle chaude de thread, hors
 * périmètre unitaire. `autosearch`/`autoprune` (les enveloppes de thread
 * elles-mêmes) sont couvertes en fin de fichier : REQUEST_STOP positionné
 * avant l'appel fait sortir leur `while` dès le premier tour, sans thread réel
 * ni fork — même pattern que control_thread/feed_thread_aposs (etii_client.c).
 */
#include "greatest.h"

/* L'unité de compilation complète : helpers static + fonctions globales. */
#include "core/etii_search.c"

#include "app/etii_client.h"       /* client_possibility_t */
#include "core/lifo.h"             /* File */
#include "core/datamanager.h"      /* datas_size, get_last_possibility */
#include "core/possibility.h"
#include "core/part.h"             /* prepare_map_part, rotate_all_parts */
#include "core/readdata.h"         /* read_parts */
#include "core/core_static_variables.h"  /* max_stock_by_thread, request, counters… */
#include "fork_assert.h"           /* run_in_fork (chemins qui exit()) */

#include "net/etii_protocol.h"     /* INST_TEST_CONNECTED / INST_END (mini-serveurs) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <dirent.h>

/* ======================================================================
 * checkAndDelegatePossibilitiesIfNeeded
 *
 * Délègue au serveur les possibilités au-delà de max_stock_by_thread. En
 * l'absence de serveur (server_ip == NULL), add_possibility route vers le
 * datamanager local : on passe client = NULL et on observe le stock local.
 * ====================================================================== */

static void drain_local(void)
{
    while (datas_size() > 0) {
        array_possibility_packet *r = get_last_possibility(NULL, 1000, NULL);
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


/* ======================================================================
 * Échec d'envoi au serveur (add_possibility != 0).
 *
 * Un socketpair joue le serveur : il acquitte le premier INST_ADD par
 * INST_END (« connexion perdue »), ce qui fait échouer put_to_server —
 * qui remet lui-même les possibilités au stock local — et renvoie -1 à
 * add_possibility. On couvre ainsi les branches d'erreur des délégations,
 * inatteignables en mode local (put_to_local n'échoue jamais).
 * ====================================================================== */

static int es_g_fd1 = -1, es_g_fd2 = -1;
static void es_silence_std(void)
{
    fflush(stdout); fflush(stderr);
    es_g_fd1 = dup(1); es_g_fd2 = dup(2);
    int dn = open("/dev/null", O_WRONLY);
    dup2(dn, 1); dup2(dn, 2); close(dn);
}
static void es_restore_std(void)
{
    fflush(stdout); fflush(stderr);
    dup2(es_g_fd1, 1); dup2(es_g_fd2, 2);
    close(es_g_fd1); close(es_g_fd2);
}

static void es_recv_exact(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char *)buf + got, len - got, 0);
        if (n <= 0) return;
        got += (size_t)n;
    }
}

/* Mini-serveur : répond au probe is_connected, lit UN INST_ADD + paquet,
 * acquitte INST_END (connexion perdue) et ferme. */
static void *es_mini_srv_add_end(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);                 /* probe is_connected */
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                 /* INST_ADD */
    struct possibility_packet pkt;
    es_recv_exact(fd, &pkt, sizeof pkt);
    b = INST_END;                       /* « connexion perdue » : échec du put */
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Branche le client (déjà construit) sur un mini-serveur défaillant. */
static void es_attach_failing_server(client_possibility_t *cp, int fds[2], pthread_t *srv)
{
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    pthread_create(srv, NULL, es_mini_srv_add_end, &fds[1]);
    pthread_mutex_init(&cp->socket_mutex, NULL);
    cp->socket_id = fds[0];
    /* Garde-fou anti-blocage : si le protocole déraille, échec après 5 s. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    set_server_ip("127.0.0.1");
}
static void es_detach_failing_server(client_possibility_t *cp, int fds[2], pthread_t *srv)
{
    pthread_join(*srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp->socket_mutex);
}

/* checkAndDelegatePossibilitiesIfNeeded : échec d'envoi -> log + les possibilités
 * extraites sont remises au stock local par put_to_server lui-même. */
TEST delegate_file_error_reputs_locally(void)
{
    drain_local();
    max_stock_by_thread = 2;

    client_possibility_t cp;
    memset(&cp, 0, sizeof cp);
    int fds[2]; pthread_t srv;
    es_attach_failing_server(&cp, fds, &srv);

    File db;
    init_file(&db, sizeof(struct possibility_packet));
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    for (int i = 0; i < 5; i++) { pk.alloc = (uint16_t)i; put(&db, &pk); }

    es_silence_std();
    checkAndDelegatePossibilitiesIfNeeded(&cp, &db);
    es_restore_std();

    ASSERT_EQ_FMT(3ULL, (unsigned long long)db.size, "%llu"); /* extraites de la file */
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");                /* remises en local */

    es_detach_failing_server(&cp, fds, &srv);
    while (db.size > 0) { struct possibility_packet t; scroll(&db, &t); }
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
 * bt_propagate_place/undo aux COINS du plateau : sur (0,0) et (SIZE-1,SIZE-1),
 * certaines gardes de voisinage (cx>0, cy>0, cx<SIZE-1, cy<SIZE-1) sont fausses
 * — jamais prises par le test en case intérieure (1,1). On vérifie qu'un
 * place+undo au coin retrouve exactement le cache du plateau vide (pas d'écriture
 * hors bornes). Couvre les côtés « bord » des gardes de bt_propagate_place/undo.
 */
TEST bt_propagate_covers_border_guards(void)
{
    struct array_part *all = make_parts();
    const int idx = 6;
    int corners[2][2] = { { 0, 0 }, { ETERN_SIZE - 1, ETERN_SIZE - 1 } };

    for (int i = 0; i < 2; i++) {
        int cx = corners[i][0], cy = corners[i][1];

        struct possibility_packet empty;
        make_empty_board(&empty);
        key_part Cempty[ETERN_SIZE][ETERN_SIZE];
        bt_init_constraints(Cempty, &empty, all, ALL_FACE);

        key_part C[ETERN_SIZE][ETERN_SIZE];
        memcpy(C, Cempty, sizeof(C));
        bt_propagate_place(C, cx, cy, &all->parts[idx]);
        bt_propagate_undo(C, cx, cy, ALL_FACE);

        for (int x = 0; x < ETERN_SIZE; x++)
            for (int y = 0; y < ETERN_SIZE; y++)
                ASSERT(key_eq(&C[x][y], &Cempty[x][y]));
    }
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

/* Map uniforme SANS candidat : toute clé renvoie une liste vide (case morte). */
static map_big_array *make_dead_map(void)
{
    static struct array_part empty = { .size = 0, .parts = NULL };
    return make_uniform_map(&empty);
}

/* Résout le cache de pointeur de masque depuis `constraints`, comme
 * `bt_mask_init` le fait au démarrage du moteur. Les tests appellent
 * `mrv_choose_cell`/`bt_forward_check` sur des plateaux fabriqués à la main :
 * ils re-résolvent donc à chaque appel, là où la boucle chaude maintient le
 * cache incrémentalement. Passer par le `bt_mask_init` de production garantit
 * qu'on teste le contrat « masque de CETTE clé », pas une ré-implémentation. */
static void fill_mask_cache(const uint64_t *cell_mask[BT_CELLS], map_big_array *m,
                            key_part C[ETERN_SIZE][ETERN_SIZE])
{
    bt_mask_init(cell_mask, m, C);
}

#if FORWARD_CHECK_K > 0
/* Appelle bt_forward_check en dérivant le miroir des pièces utilisées du
 * plateau, au lieu de le maintenir comme le fait la boucle chaude. Le miroir
 * est reconstruit à CHAQUE appel : plusieurs tests modifient `b_faceused`
 * entre deux appels (set_face_used) et doivent voir le changement. */
static int fc_verdict(key_part C[ETERN_SIZE][ETERN_SIZE], struct possibility_packet *b,
                      map_big_array *m, int cx, int cy)
{
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, b);
    const uint64_t *cell_mask[BT_CELLS];
    fill_mask_cache(cell_mask, m, C);
    return bt_forward_check(C, b, m, used, cell_mask, cx, cy);
}

/* bt_forward_check : 1 si chaque voisine VIDE de la pièce qu'on vient de
 * placer en (cx, cy) a un candidat libre, 0 si l'une est morte (aucun
 * candidat / tous utilisés). Coin (0,0) : 2 voisines dans la grille,
 * (1,0) et (0,1), toutes deux vides sur un plateau neuf. */
TEST bt_forward_check_detects_dead_cells(void)
{
    struct possibility_packet board;
    make_empty_board(&board);

    /* all_face DOIT valoir map->sizearrayM (=2) pour que les clés indexent flat. */
    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_parts(), 2);

    /* (a) un candidat libre (pièce 9) pour toute clé -> toutes les voisines vivent. */
    struct part p_free[1] = { { .id = 9 } };
    struct array_part cand_free = { .size = 1, .parts = p_free };
    map_big_array *map = make_uniform_map(&cand_free);
    ASSERT_EQ_FMT(1, fc_verdict(C, &board, map, 0, 0), "%d");

    /* (b) le seul candidat (pièce 9) est déjà utilisé -> voisine morte -> 0. */
    set_face_used(board.b_faceused, 8, 1); /* pièce 9 */
    ASSERT_EQ_FMT(0, fc_verdict(C, &board, map, 0, 0), "%d");
    set_face_used(board.b_faceused, 8, 0);

    /* (c) aucun candidat pour aucune clé -> voisine morte -> 0. */
    struct array_part cand_empty = { .size = 0, .parts = NULL };
    map_big_array *map_empty = make_uniform_map(&cand_empty);
    ASSERT_EQ_FMT(0, fc_verdict(C, &board, map_empty, 0, 0), "%d");

    PASS();
}

/* bt_forward_check : une voisine déjà remplie de (cx, cy) est sautée sans
 * lookup (grid != -2), et un candidat id 0 (trou de map) est ignoré sans
 * tuer la voisine restante. */
TEST bt_forward_check_skips_prefilled_and_zero_id(void)
{
    struct possibility_packet board;
    make_empty_board(&board);

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_parts(), 2);

    /* Une des deux voisines du coin (0,0) est déjà remplie : sautée sans lookup. */
    board.grid[1][0] = 3;

    /* Candidats [id 0 (ignoré), id 9 (libre)] : la voisine restante (0,1) reste vivante. */
    struct part cand[2] = { { .id = 0 }, { .id = 9 } };
    struct array_part list = { .size = 2, .parts = cand };
    map_big_array *map = make_uniform_map(&list);
    ASSERT_EQ_FMT(1, fc_verdict(C, &board, map, 0, 0), "%d");

    PASS();
}

/* bt_forward_check n'inspecte QUE les voisines géométriques de (cx, cy) — au
 * plus 4, jamais une case plus lointaine du parcours (l'ancien comportement
 * à fenêtre, cf. docs/conception/elagage_recherche.md §4.1). Verrouillé en
 * comptant les voisines réellement inspectées via fc_cells_studied : sur le
 * coin (0,0), au plus 2 (jamais 4, jamais un nombre dépendant de
 * FORWARD_CHECK_K). */
TEST bt_forward_check_inspects_at_most_geometric_neighbors(void)
{
    struct possibility_packet board;
    make_empty_board(&board);

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_parts(), 2);

    struct part p_free[1] = { { .id = 9 } };
    struct array_part cand_free = { .size = 1, .parts = p_free };
    map_big_array *map = make_uniform_map(&cand_free);

    unsigned long long before = fc_cells_studied;
    /* Coin (0,0) : 2 voisines dans la grille, (1,0) et (0,1). */
    ASSERT_EQ_FMT(1, fc_verdict(C, &board, map, 0, 0), "%d");
    ASSERT_EQ_FMT(before + 2, fc_cells_studied, "%llu");

    before = fc_cells_studied;
    /* Case intérieure (1,1) (existe dès ETERN_SIZE >= 3) : 4 voisines. */
    ASSERT_EQ_FMT(1, fc_verdict(C, &board, map, 1, 1), "%d");
    ASSERT_EQ_FMT(before + 4, fc_cells_studied, "%llu");

    PASS();
}

/* Neutraliser `packed` désactive du même coup le masque d'ids (map_bucket_id_mask
 * l'exige) : ce test compare donc la voie RAPIDE d'une map réelle — le masque
 * d'ids — au parcours des entrées de `flat`, les deux extrémités de la chaîne
 * de replis. Sur une map RÉELLE (bâtie par buildBigArray, donc pourvue de ses
 * index — les fixtures faites main ci-dessus exercent, elles, le repli), les
 * représentations doivent conduire au MÊME verdict pour CHAQUE case
 * candidate : c'est l'invariant « même élagage, donc mêmes nœuds explorés ».
 * Balaie les 16 cases (coin, bordure, intérieur) plutôt qu'une fenêtre de
 * parcours : la case candidate détermine désormais directement l'ensemble
 * des voisines inspectées. */
TEST bt_forward_check_same_verdict_with_and_without_packed_index(void)
{
    /* Jeu couvrant les trois natures de case du plateau : coin (deux bords de
     * grille), bordure (un seul) et intérieur (aucun). Toutes les faces non
     * nulles valent 1, si bien que n'importe quelle case VIDE a un candidat —
     * sans quoi, sur un plateau 4×4 (ETERN_PARTS=16) où toutes les cases sont
     * des bordures, le forward-check serait mort partout. */
    static struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 0, .right = 1, .bottom = 1, .left = 0 }, /* coin haut-gauche */
        { .id = 2, .top = 0, .right = 0, .bottom = 1, .left = 1 }, /* coin haut-droit */
        { .id = 3, .top = 1, .right = 0, .bottom = 0, .left = 1 }, /* coin bas-droit */
        { .id = 4, .top = 1, .right = 1, .bottom = 0, .left = 0 }, /* coin bas-gauche */
        { .id = 5, .top = 0, .right = 1, .bottom = 1, .left = 1 }, /* bordure haute */
        { .id = 6, .top = 1, .right = 0, .bottom = 1, .left = 1 }, /* bordure droite */
        { .id = 7, .top = 1, .right = 1, .bottom = 0, .left = 1 }, /* bordure basse */
        { .id = 8, .top = 1, .right = 1, .bottom = 1, .left = 0 }, /* bordure gauche */
        { .id = 9, .top = 1, .right = 1, .bottom = 1, .left = 1 }, /* intérieur */
    };
    static struct array_part a = { .size = 10, .parts = parts };
    const int nb_ids = 9;

    map_big_array *map = prepare_map_part(&a);
    ASSERT(map->packed != NULL);

    struct possibility_packet board;
    make_empty_board(&board);

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, &a, (int8_t)map->sizearrayM);

    /* Chaque fenêtre est évaluée deux fois : avec l'index compact, puis en le
     * neutralisant pour forcer la lecture de `flat`. Les deux verdicts doivent
     * coïncider — c'est l'invariant « même élagage ». On balaie d'abord toutes
     * les pièces libres (verdict vivant), puis toutes utilisées (case morte). */
    int seen_alive = 0, seen_dead = 0;
    for (int phase = 0; phase < 2; phase++) {
        if (phase == 1) {
            for (int id = 1; id <= nb_ids; id++) set_face_used(board.b_faceused, id - 1, 1);
        }
        for (int cx = 0; cx < ETERN_SIZE; cx++) for (int cy = 0; cy < ETERN_SIZE; cy++) {
            int with_index = fc_verdict(C, &board, map, cx, cy);

            uint32_t *saved = map->packed;
            map->packed = NULL; /* force le repli sur `flat` */
            int without_index = fc_verdict(C, &board, map, cx, cy);
            map->packed = saved;

            ASSERT_EQ_FMT(without_index, with_index, "%d");
            if (with_index) seen_alive = 1; else seen_dead = 1;
        }
    }
    ASSERT(seen_alive);
    ASSERT(seen_dead);

    free_bigarray(map);
    PASS();
}

/* Pendant du test précédent, mais isolant le SEUL maillon ajouté : la voie
 * rapide par masque d'ids (`map_mask_any_free`) contre le parcours des entrées
 * du compartiment. On neutralise `bucket_id_mask` en gardant `packed`, si bien
 * que le repli lit l'index compact — les deux chemins voient donc exactement le
 * même compartiment, et seule la façon de le questionner change.
 *
 * Ce que le test verrouille est une ÉQUIVALENCE, pas une valeur : le masque ne
 * porte que les ids `> 0` réellement présents, exactement le filtre `id != 0`
 * du parcours. Les deux phases (toutes pièces libres, puis toutes utilisées)
 * garantissent qu'on observe les deux verdicts, faute de quoi l'égalité serait
 * triviale. */
TEST bt_forward_check_same_verdict_with_and_without_id_mask(void)
{
    static struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 0, .right = 1, .bottom = 1, .left = 0 },
        { .id = 2, .top = 0, .right = 0, .bottom = 1, .left = 1 },
        { .id = 3, .top = 1, .right = 0, .bottom = 0, .left = 1 },
        { .id = 4, .top = 1, .right = 1, .bottom = 0, .left = 0 },
        { .id = 5, .top = 0, .right = 1, .bottom = 1, .left = 1 },
        { .id = 6, .top = 1, .right = 0, .bottom = 1, .left = 1 },
        { .id = 7, .top = 1, .right = 1, .bottom = 0, .left = 1 },
        { .id = 8, .top = 1, .right = 1, .bottom = 1, .left = 0 },
        { .id = 9, .top = 1, .right = 1, .bottom = 1, .left = 1 },
    };
    static struct array_part a = { .size = 10, .parts = parts };
    const int nb_ids = 9;

    map_big_array *map = prepare_map_part(&a);
    ASSERT(map->packed != NULL);
    ASSERT(map->bucket_id_mask != NULL); /* sinon le test ne prouverait rien */

    struct possibility_packet board;
    make_empty_board(&board);

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, &a, (int8_t)map->sizearrayM);

    int seen_alive = 0, seen_dead = 0;
    for (int phase = 0; phase < 2; phase++) {
        if (phase == 1) {
            for (int id = 1; id <= nb_ids; id++) set_face_used(board.b_faceused, id - 1, 1);
        }
        for (int cx = 0; cx < ETERN_SIZE; cx++) for (int cy = 0; cy < ETERN_SIZE; cy++) {
            int with_mask = fc_verdict(C, &board, map, cx, cy);

            uint64_t *saved = map->bucket_id_mask;
            map->bucket_id_mask = NULL; /* force le parcours des entrées */
            int without_mask = fc_verdict(C, &board, map, cx, cy);
            map->bucket_id_mask = saved;

            ASSERT_EQ_FMT(without_mask, with_mask, "%d");
            if (with_mask) seen_alive = 1; else seen_dead = 1;
        }
    }
    ASSERT(seen_alive);
    ASSERT(seen_dead);

    free_bigarray(map);
    PASS();
}

/* ======================================================================
 * §4.4 — conflit de singletons (`singleton_conflict_check`).
 *
 * Fixture partagée par les trois tests suivants : pièce B posée en (1,1),
 * dont les deux voisines VIDES (0,1) et (1,0) ont des clés distinctes,
 * chacune injectée à la main dans `flat` avec un compartiment à candidat
 * UNIQUE — même technique que `mrv_choose_cell_picks_the_most_constrained_cell`
 * plus haut dans ce fichier (hors de ce bloc `#if`, donc non réutilisable
 * telle quelle). Les deux autres voisines géométriques de (1,1), (2,1) et
 * (1,2), sont déjà remplies (grid != -2) : bt_forward_check(1,1) n'inspecte
 * donc bien QUE (0,1) et (1,0), exactement les deux compartiments injectés.
 *
 * @param id_a Id du candidat unique injecté dans le compartiment de (0,1).
 * @param id_b Id du candidat unique injecté dans le compartiment de (1,0).
 * @return     La map construite (à libérer par l'appelant) ; *out_C reçoit
 *             le cache de contraintes ; *out_board le plateau.
 */
static map_big_array *build_singleton_conflict_fixture(int16_t id_a, int16_t id_b,
                                                        struct possibility_packet *out_board,
                                                        key_part out_C[ETERN_SIZE][ETERN_SIZE])
{
    make_empty_board(out_board);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            out_board->grid[x][y] = 1; /* remplissage : idParts[1][0], faces 0 */
    out_board->grid[0][0] = 2; /* pièce A : idParts[2][0] */
    out_board->grid[1][1] = 3; /* pièce B (« juste posée ») : idParts[3][0] */
    out_board->grid[0][1] = -2;
    out_board->grid[1][0] = -2;

    static struct part rot_parts[4] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 0, .bottom = 0, .left = 0 },
        { .id = 2, .top = 0, .right = 5, .bottom = 0, .left = 0 },
        { .id = 3, .top = 0, .right = 0, .bottom = 0, .left = 6 },
    };
    static struct array_part rot_ap = { .size = 4, .parts = rot_parts };

    map_big_array *map = prepare_map_part(&rot_ap);
    bt_init_constraints(out_C, out_board, &rot_ap, (int8_t)map->sizearrayM);

    /* Mêmes clés que le fixture mrv_choose_cell : (0,1) -> (0,6,0,0), (1,0) -> (0,0,0,5). */
    static struct part cand_a[1];
    static struct array_part list_a;
    static struct part cand_b[1];
    static struct array_part list_b;
    cand_a[0] = (struct part){ .id = id_a, .top = 0, .right = 5, .bottom = 6, .left = 0 };
    list_a = (struct array_part){ .size = 1, .parts = cand_a };
    cand_b[0] = (struct part){ .id = id_b, .top = 0, .right = 0, .bottom = 0, .left = 5 };
    list_b = (struct array_part){ .size = 1, .parts = cand_b };

    int M = map->sizearray;
    unsigned long long idx_a = (((unsigned long long)0 * M + 6) * M + 0) * M + 0;
    unsigned long long idx_b = (((unsigned long long)0 * M + 0) * M + 0) * M + 5;
    map->flat[idx_a] = list_a;
    map->flat[idx_b] = list_b;
    map->packed = NULL; /* force le repli sur flat pour ce test */

    return map;
}

/* --------------------------------------------------------------------------
 * bt_mask_refresh : le QUATRIÈME cache en lockstep avec le plateau
 *
 * `cell_mask` doit à tout instant valoir la re-résolution complète depuis
 * `constraints` — c'est sa définition. Le test le vérifie après chaque
 * pose ET chaque retrait, sur toutes les cases de la grille.
 *
 * Il porte un CONTRE-CONTRÔLE : une pose sans rafraîchissement doit faire
 * diverger le cache. Sans lui, le test passerait encore si `bt_mask_refresh`
 * devenait un no-op — c'est-à-dire s'il ne prouvait plus rien.
 * ------------------------------------------------------------------------ */
TEST bt_mask_cache_stays_in_lockstep_with_constraints(void)
{
    static struct part parts[] = {
        { .id = 0 },                                              /* bouchon bordure */
        { .id = 1, .top = 0, .right = 1, .bottom = 1, .left = 0 },
        { .id = 2, .top = 0, .right = 0, .bottom = 1, .left = 1 },
        { .id = 3, .top = 1, .right = 0, .bottom = 0, .left = 1 },
        { .id = 4, .top = 1, .right = 1, .bottom = 0, .left = 0 },
        { .id = 5, .top = 0, .right = 1, .bottom = 1, .left = 1 },
        { .id = 6, .top = 1, .right = 0, .bottom = 1, .left = 1 },
        { .id = 7, .top = 1, .right = 1, .bottom = 0, .left = 1 },
        { .id = 8, .top = 1, .right = 1, .bottom = 1, .left = 0 },
        { .id = 9, .top = 1, .right = 1, .bottom = 1, .left = 1 },
    };
    static struct array_part a = { .size = 10, .parts = parts };

    map_big_array *map = prepare_map_part(&a);
    ASSERT(map->bucket_id_mask != NULL); /* sinon tous les pointeurs seraient NULL */
    const int8_t all_face = (int8_t)map->sizearrayM;

    struct possibility_packet board;
    make_empty_board(&board);
    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, &a, all_face);

    const uint64_t *cache[BT_CELLS];
    const uint64_t *expected[BT_CELLS];
    bt_mask_init(cache, map, C);

    int saw_non_null = 0;
    for (int cx = 0; cx < ETERN_SIZE; cx++) {
        for (int cy = 0; cy < ETERN_SIZE; cy++) {
            const struct part *piece = &parts[1 + ((cx * ETERN_SIZE + cy) % 9)];

            /* Pose : constraints change pour les <=4 voisines, le cache suit. */
            bt_propagate_place(C, cx, cy, piece);
            bt_mask_refresh(cache, map, C, cx, cy);
            bt_mask_init(expected, map, C);
            for (int c = 0; c < BT_CELLS; c++) {
                if (expected[c] != NULL) saw_non_null = 1;
                ASSERT_EQ(expected[c], cache[c]);
            }

            /* Retrait : les mêmes voisines redeviennent non contraintes. */
            bt_propagate_undo(C, cx, cy, all_face);
            bt_mask_refresh(cache, map, C, cx, cy);
            bt_mask_init(expected, map, C);
            for (int c = 0; c < BT_CELLS; c++) {
                ASSERT_EQ(expected[c], cache[c]);
            }
        }
    }
    ASSERT(saw_non_null); /* la fixture doit produire de vrais masques */

    /* Contre-contrôle : une pose SANS rafraîchissement doit diverger. On vise
     * une case intérieure, dont les 4 voisines existent. */
    const int mx = ETERN_SIZE / 2, my = ETERN_SIZE / 2;
    bt_propagate_place(C, mx, my, &parts[1]);
    bt_mask_init(expected, map, C);
    int diverged = 0;
    for (int c = 0; c < BT_CELLS; c++) {
        if (expected[c] != cache[c]) { diverged = 1; break; }
    }
    ASSERT(diverged);
    bt_propagate_undo(C, mx, my, all_face);

    free_bigarray(map);
    PASS();
}

/* Cas « doit tirer » : (0,1) et (1,0) exigent chacune, comme SEUL candidat
 * libre, la même pièce (id 20) -> conflit de Hall |S|=2 -> branche morte. */
TEST bt_forward_check_singleton_conflict_detects_matching_ids(void)
{
    struct possibility_packet board;
    key_part C[ETERN_SIZE][ETERN_SIZE];
    map_big_array *map = build_singleton_conflict_fixture(20, 20, &board, C);

    int saved = singleton_conflict_check;
    singleton_conflict_check = 1;
    unsigned long long before = fc_singleton_conflict;

    ASSERT_EQ_FMT(0, fc_verdict(C, &board, map, 1, 1), "%d");
    ASSERT_EQ_FMT(before + 1, fc_singleton_conflict, "%llu");

    singleton_conflict_check = saved;
    free_bigarray(map);
    PASS();
}

/* Cas « ne doit pas tirer » : (0,1) et (1,0) ont bien chacune un candidat
 * UNIQUE, mais des ids DISTINCTS (20 et 21) -> aucun conflit, condition
 * nécessaire respectée (pas de faux positif). */
TEST bt_forward_check_singleton_conflict_ignores_distinct_ids(void)
{
    struct possibility_packet board;
    key_part C[ETERN_SIZE][ETERN_SIZE];
    map_big_array *map = build_singleton_conflict_fixture(20, 21, &board, C);

    int saved = singleton_conflict_check;
    singleton_conflict_check = 1;
    unsigned long long before = fc_singleton_conflict;

    ASSERT_EQ_FMT(1, fc_verdict(C, &board, map, 1, 1), "%d");
    ASSERT_EQ_FMT(before, fc_singleton_conflict, "%llu");

    singleton_conflict_check = saved;
    free_bigarray(map);
    PASS();
}

/* Drapeau BAS (comportement historique) : même configuration « ids
 * identiques » que le premier cas, mais singleton_conflict_check = 0 ->
 * chaque voisine a bien >= 1 candidat libre, donc vivante — le test de
 * conflit ne doit jamais s'exécuter quand le drapeau est désactivé. */
TEST bt_forward_check_singleton_conflict_disabled_by_default(void)
{
    struct possibility_packet board;
    key_part C[ETERN_SIZE][ETERN_SIZE];
    map_big_array *map = build_singleton_conflict_fixture(20, 20, &board, C);

    int saved = singleton_conflict_check;
    singleton_conflict_check = 0;
    unsigned long long before = fc_singleton_conflict;

    ASSERT_EQ_FMT(1, fc_verdict(C, &board, map, 1, 1), "%d");
    ASSERT_EQ_FMT(before, fc_singleton_conflict, "%llu");

    singleton_conflict_check = saved;
    free_bigarray(map);
    PASS();
}
#endif /* FORWARD_CHECK_K > 0 */

/* bt_count_pending : un niveau sans décision (case pré-remplie : search == NULL,
 * placed_pos == -1) ne compte rien, et un candidat id 0 (trou de map) est ignoré. */
TEST bt_count_pending_skips_no_decision_and_zero_id(void)
{
    struct part cand[3] = { { .id = 0 }, { .id = 2 }, { .id = 3 } };
    struct array_part list = { .size = 3, .parts = cand };

    bt_level stack[2];
    /* niveau 0 : case pré-remplie -> ni candidats ni placement */
    stack[0].search = NULL;  stack[0].next_s = 0; stack[0].placed_pos = -1;
    /* niveau 1 : id 0 ignoré, ids 2 et 3 libres */
    stack[1].search = &list; stack[1].next_s = 0; stack[1].placed_pos = -1;

    struct possibility_packet board;
    make_empty_board(&board);

    ASSERT_EQ_FMT(2ULL, bt_count_pending(&board, stack, 1), "%llu");
    PASS();
}

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
    if (lastroot == NULL) {
        lastroot = malloc(NB_THREADS * sizeof(*lastroot));
        for (int i = 0; i < NB_THREADS; i++) lastroot[i] = -1;
    }
    if (lastdepth == NULL) {
        lastdepth = malloc(NB_THREADS * sizeof(*lastdepth));
        for (int i = 0; i < NB_THREADS; i++) lastdepth[i] = -1;
    }
}

/* idParts comme dans autosearch : idParts[p][r] = p + ETERN_PARTS*r. */
static void fill_idparts(int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    for (int p = 0; p <= ETERN_PARTS; p++) {
        int base = p;
        for (int r = 0; r < PART_SIZES; r++) { idParts[p][r] = (int16_t)base; base += ETERN_PARTS; }
    }
}

/* ======================================================================
 * mrv_choose_cell (§4.7, prototype de mesure). Non gardée par
 * FORWARD_CHECK_K (indépendante de bt_forward_check).
 * ====================================================================== */

/* Pièce de « remplissage » à faces PETITES (0), utilisée pour pré-remplir un
 * plateau AVANT bt_init_constraints — what_search_in_grid_to_key indexe
 * DIRECTEMENT par la valeur de grille (`all_rotate_parts->parts[grid[x][y]]`,
 * sans -1) : l'indice 0 est le bouchon conventionnel, l'indice 1 correspond
 * à la valeur de grille 1 (= idParts[1][0], pièce 1 rotation 0). */
static struct array_part *make_filler_part(void)
{
    static struct part p[2] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 0, .bottom = 0, .left = 0, .rotation = 0 },
    };
    static struct array_part ap = { .size = 2, .parts = p };
    return &ap;
}

/* Cas « choisit la case la plus contrainte » : une vraie map (pas uniforme —
 * une map uniforme renverrait forcément le même compte à toute case vide,
 * puisque `faceused` est un état global, pas par case) où deux cases vides
 * ont des CLÉS différentes (voisinages différents), donc des compartiments de
 * tailles différentes. Plateau : pièce A en (0,0) (right=5), pièce B en
 * (1,1) (left=6), tout le reste rempli par la pièce de remplissage
 * (faces 0). Case (0,1) : voisines (0,0).bottom=0, (1,1).left=6, (0,2)=0,
 * bord=0 -> clé (0,6,0,0). Case (1,0) : bord=0, (2,0)=0, (1,1).top=0,
 * (0,0).right=5 -> clé (0,0,0,5). Un candidat unique (id 20) est placé dans
 * le compartiment de (0,1) ; deux candidats (id 21, 22) dans celui de (1,0) :
 * mrv_choose_cell doit choisir (0,1), strictement plus contrainte. */
TEST mrv_choose_cell_picks_the_most_constrained_cell(void)
{
    struct possibility_packet board;
    make_empty_board(&board);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            board.grid[x][y] = 1; /* remplissage : idParts[1][0] = pièce d'indice 1, faces 0 */
    board.grid[0][0] = 2;  /* pièce A : idParts[2][0] = indice 2 */
    board.grid[1][1] = 3;  /* pièce B : idParts[3][0] = indice 3 */
    board.grid[0][1] = -2; /* case la plus contrainte (1 seul candidat) */
    board.grid[1][0] = -2; /* case moins contrainte (2 candidats) */

    static struct part rot_parts[4] = {
        { .id = 0 },
        { .id = 1, .top = 0, .right = 0, .bottom = 0, .left = 0 },             /* remplissage */
        { .id = 2, .top = 0, .right = 5, .bottom = 0, .left = 0 },             /* pièce A */
        { .id = 3, .top = 0, .right = 0, .bottom = 0, .left = 6 },             /* pièce B */
    };
    static struct array_part rot_ap = { .size = 4, .parts = rot_parts };

    map_big_array *map = prepare_map_part(&rot_ap);

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, &rot_ap, (int8_t)map->sizearrayM);

    /* Clé de (0,1) attendue : k1=(0,0).bottom=0, k2=(1,1).left=6, k3=(0,2).top=0
     * (case de remplissage), k4=bord=0 -> (0,6,0,0). */
    ASSERT_EQ_FMT(0, (int)C[0][1].k1, "%d");
    ASSERT_EQ_FMT(6, (int)C[0][1].k2, "%d");
    ASSERT_EQ_FMT(0, (int)C[0][1].k3, "%d");
    ASSERT_EQ_FMT(0, (int)C[0][1].k4, "%d");
    /* Clé de (1,0) attendue : k1=bord=0, k2=(2,0).left=0, k3=(1,1).top=0, k4=(0,0).right=5 -> (0,0,0,5). */
    ASSERT_EQ_FMT(0, (int)C[1][0].k1, "%d");
    ASSERT_EQ_FMT(0, (int)C[1][0].k2, "%d");
    ASSERT_EQ_FMT(0, (int)C[1][0].k3, "%d");
    ASSERT_EQ_FMT(5, (int)C[1][0].k4, "%d");

    /* Injecte artificiellement des candidats dans les deux compartiments
     * ciblés : id 20 (1 seul) dans (0,6,0,0), id 21/22 (deux) dans (0,0,0,5).
     * On construit ces deux array_part à la main et on les insère dans le
     * flat de la map réelle, aux index calculés comme buildBigArray. */
    static struct part cand_scarce[1] = { { .id = 20, .top = 0, .right = 5, .bottom = 6, .left = 0 } };
    static struct array_part list_scarce = { .size = 1, .parts = cand_scarce };
    static struct part cand_plenty[2] = {
        { .id = 21, .top = 0, .right = 0, .bottom = 0, .left = 5 },
        { .id = 22, .top = 1, .right = 0, .bottom = 0, .left = 5 },
    };
    static struct array_part list_plenty = { .size = 2, .parts = cand_plenty };

    int M = map->sizearray;
    unsigned long long idx_scarce = (((unsigned long long)0 * M + 6) * M + 0) * M + 0;
    unsigned long long idx_plenty = (((unsigned long long)0 * M + 0) * M + 0) * M + 5;
    map->flat[idx_scarce] = list_scarce;
    map->flat[idx_plenty] = list_plenty;
    map->packed = NULL; /* neutralise l'index compact : force le repli sur flat pour ce test */

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    uint8_t x = 255, y = 255;
    int count = -99;
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, &board);
    bt_frontier front;
    bt_frontier_init(&front, &board);
    const uint64_t *cell_mask_0[BT_CELLS];
    fill_mask_cache(cell_mask_0, map, C);
    int rc = mrv_choose_cell(&board, C, map, used, &front, cell_mask_0, &x, &y, &count);

    ASSERT_EQ_FMT(1, rc, "%d");
    ASSERT_EQ_FMT(0, (int)x, "%d");
    ASSERT_EQ_FMT(1, (int)y, "%d"); /* (0,1) : 1 seul candidat, la plus contrainte */
    ASSERT_EQ_FMT(1, count, "%d"); /* score MRV de la case choisie : 1 candidat */

    free_bigarray(map);
    PASS();
}

/* Cas « détecte une case sans issue » : une case vide dont le compartiment
 * n'offre AUCUN candidat -> mrv_choose_cell doit renvoyer 0 sans se soucier
 * du reste du plateau (branche morte, peu importe les autres cases). */
TEST mrv_choose_cell_detects_dead_cell(void)
{
    struct possibility_packet board;
    make_empty_board(&board);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            board.grid[x][y] = 1;
    board.grid[0][0] = 1;
    board.grid[0][1] = -2; /* seule case vide : sans candidat */

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_filler_part(), 2);

    struct array_part empty_list = { .size = 0, .parts = NULL };
    map_big_array *map = make_uniform_map(&empty_list);

    uint8_t x = 255, y = 255;
    int count = -99;
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, &board);
    bt_frontier front;
    bt_frontier_init(&front, &board);
    const uint64_t *cell_mask_1[BT_CELLS];
    fill_mask_cache(cell_mask_1, map, C);
    int rc = mrv_choose_cell(&board, C, map, used, &front, cell_mask_1, &x, &y, &count);

    ASSERT_EQ_FMT(0, rc, "%d");

    PASS();
}

/* ======================================================================
 * bt_frontier : la frontière incrémentale du balayage MRV.
 *
 * L'enjeu de ces tests n'est pas « le masque a-t-il l'air correct » mais
 * « énumérer la frontière par masque donne-t-il EXACTEMENT la même réponse
 * que l'ancien balayage des 256 cases ». Toute divergence — y compris un
 * simple départage d'égalité différent — changerait l'arbre exploré.
 * ====================================================================== */

/* Teste le bit d'une case dans un masque de bt_frontier. */
static int frontier_bit(const uint64_t *mask, int x, int y)
{
    int pos = BT_CELL_POS(x, y);
    return (int)((mask[pos / 64] >> (pos % 64)) & 1);
}

/* Générateur déterministe (pas de rand() : la mesure doit être rejouable). */
static unsigned lcg_next(unsigned *state)
{
    *state = (*state) * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fff;
}

/* Plateau pseudo-aléatoire : chaque case est vide (-2) ou porte la pièce de
 * remplissage (indice 1, faces 0 — cf. make_filler_part). */
static void make_random_board(struct possibility_packet *b, unsigned *state)
{
    make_empty_board(b);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            if (lcg_next(state) & 1)
                b->grid[x][y] = 1;
}

/* bt_frontier_init doit reproduire EXACTEMENT les deux prédicats de l'ancien
 * balayage : `empty` = `grid == -2`, `constrained` = « au moins une des 4 clés
 * diffère de all_face » — ce dernier lu dans le cache que bt_init_constraints
 * vient de calculer, et non redérivé. */
TEST bt_frontier_init_matches_constraint_cache(void)
{
    unsigned state = 20260830u;
    for (int iter = 0; iter < 40; iter++) {
        struct possibility_packet board;
        make_random_board(&board, &state);

        key_part C[ETERN_SIZE][ETERN_SIZE];
        const int8_t all_face = 2;
        bt_init_constraints(C, &board, make_filler_part(), all_face);

        bt_frontier f;
        bt_frontier_init(&f, &board);

        for (int x = 0; x < ETERN_SIZE; x++) {
            for (int y = 0; y < ETERN_SIZE; y++) {
                int expect_empty = (board.grid[x][y] == -2);
                int expect_constrained = !(C[x][y].k1 == all_face && C[x][y].k2 == all_face
                                        && C[x][y].k3 == all_face && C[x][y].k4 == all_face);
                ASSERT_EQ_FMT(expect_empty, frontier_bit(f.empty, x, y), "%d");
                ASSERT_EQ_FMT(expect_constrained, frontier_bit(f.constrained, x, y), "%d");
            }
        }
    }
    PASS();
}

/* Même contrat que bt_propagate_matches_full_recompute, pour l'autre cache :
 * la mise à jour incrémentale doit donner le même état qu'un recalcul complet
 * sur le plateau résultant, et bt_frontier_undo doit ramener à l'identique.
 * Couvre les coins (gardes de bord) et l'intérieur. */
TEST bt_frontier_place_undo_matches_full_recompute(void)
{
    /* Coordonnées dérivées d'ETERN_SIZE : la suite est rejouée en 4x4
     * (ETERN_SIZE=4, puzzle 16 pièces) où toute constante > 3 déborde. */
    const int cells[][2] = { {0,0}, {0,1}, {1,0}, {ETERN_SIZE/2, ETERN_SIZE-2},
                             {ETERN_SIZE-1,ETERN_SIZE-1}, {ETERN_SIZE-1,0},
                             {0,ETERN_SIZE-1} };
    unsigned state = 7u;
    for (size_t c = 0; c < sizeof(cells) / sizeof(cells[0]); c++) {
        int cx = cells[c][0], cy = cells[c][1];
        struct possibility_packet board;
        make_random_board(&board, &state);
        board.grid[cx][cy] = -2; /* la case doit être vide avant le placement */

        bt_frontier before, f;
        bt_frontier_init(&before, &board);
        bt_frontier_init(&f, &board);

        /* Pose : la grille est mise à jour, puis la frontière incrémentalement. */
        board.grid[cx][cy] = 1;
        bt_frontier_place(&f, cx, cy);

        bt_frontier full;
        bt_frontier_init(&full, &board);
        for (int w = 0; w < BT_FRONTIER_WORDS; w++) {
            ASSERT_EQ(full.empty[w], f.empty[w]);
            ASSERT_EQ(full.constrained[w], f.constrained[w]);
        }
        for (int p = 0; p < BT_CELLS; p++) {
            ASSERT_EQ_FMT((int)full.nconstr[p], (int)f.nconstr[p], "%d");
        }

        /* Retrait : retour à l'état initial, bit pour bit. */
        board.grid[cx][cy] = -2;
        bt_frontier_undo(&f, cx, cy);
        for (int w = 0; w < BT_FRONTIER_WORDS; w++) {
            ASSERT_EQ(before.empty[w], f.empty[w]);
            ASSERT_EQ(before.constrained[w], f.constrained[w]);
        }
        for (int p = 0; p < BT_CELLS; p++) {
            ASSERT_EQ_FMT((int)before.nconstr[p], (int)f.nconstr[p], "%d");
        }
    }
    PASS();
}

/* Réimplémentation indépendante du balayage des 256 cases : c'est l'oracle. Si
 * les deux divergent d'une seule case sur un seul plateau, l'arbre exploré
 * n'est plus le même.
 *
 * Le nombre de côtés contraints est ici dérivé du cache `constraints[][]`
 * (« côté contraint » = clé différente de `all_face`, c'est la définition),
 * et NON lu dans `bt_frontier.nconstr` qui le calcule, lui, depuis la géométrie
 * et la grille. Les deux dérivations sont indépendantes : l'oracle vérifie donc
 * du même coup que `nconstr` dit bien ce qu'il prétend. */
static int reference_choose_cell(struct possibility_packet *board,
                                 key_part constraints[ETERN_SIZE][ETERN_SIZE],
                                 map_big_array *mapParts,
                                 const uint64_t used[MRV_USED_WORDS],
                                 int8_t all_face,
                                 uint8_t *out_x, uint8_t *out_y, int *out_count)
{
    int best_count = -1;
    uint8_t best_x = 0, best_y = 0;
    int best_nc = 0;
    int fallback_found = 0;
    uint8_t fallback_x = 0, fallback_y = 0;

    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (board->grid[x][y] != -2) {
                continue;
            }
            const key_part *key = &constraints[x][y];
            if (key->k1 == all_face && key->k2 == all_face
                && key->k3 == all_face && key->k4 == all_face) {
                if (!fallback_found) {
                    fallback_found = 1;
                    fallback_x = (uint8_t)x;
                    fallback_y = (uint8_t)y;
                }
                continue;
            }
            int count = mrv_free_candidates(mapParts, key, board, used, map_bucket_id_mask(mapParts, key));
            if (count == 0) {
                return 0;
            }
            /* Côtés contraints, dérivés du cache et non de bt_frontier. */
            int nc = 0;
            if (key->k1 != all_face) nc++;
            if (key->k2 != all_face) nc++;
            if (key->k3 != all_face) nc++;
            if (key->k4 != all_face) nc++;
            if (best_count < 0 || count < best_count) {
                best_count = count;
                best_x = (uint8_t)x;
                best_y = (uint8_t)y;
                best_nc = nc;
            } else if (count == best_count && nc > best_nc) {
                best_x = (uint8_t)x;
                best_y = (uint8_t)y;
                best_nc = nc;
            }
        }
    }
    if (best_count < 0) {
        if (!fallback_found) {
            return 0;
        }
        *out_x = fallback_x;
        *out_y = fallback_y;
        *out_count = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
        return 1;
    }
    *out_x = best_x;
    *out_y = best_y;
    *out_count = best_count;
    return 1;
}

/* L'oracle, sur 200 plateaux pseudo-aléatoires. La map est UNIFORME : toute
 * case contrainte offre le même nombre de candidats, donc tout est à égalité —
 * c'est précisément le cas qui met le départage à l'épreuve, et c'est là que
 * l'ordre d'énumération doit être identique au `for x { for y }` d'origine. */
TEST mrv_choose_cell_matches_full_scan_reference(void)
{
    static struct part cand[2] = { { .id = 6 }, { .id = 7 } };
    static struct array_part list = { .size = 2, .parts = cand };
    map_big_array *map = make_uniform_map(&list);
    const int8_t all_face = (int8_t)map->sizearrayM;

    unsigned state = 424242u;
    for (int iter = 0; iter < 200; iter++) {
        struct possibility_packet board;
        make_random_board(&board, &state);

        key_part C[ETERN_SIZE][ETERN_SIZE];
        bt_init_constraints(C, &board, make_filler_part(), all_face);

        uint64_t used[MRV_USED_WORDS];
        mrv_used_init(used, &board);

        bt_frontier f;
        bt_frontier_init(&f, &board);

        uint8_t rx = 200, ry = 200; int rcount = -99;
        uint8_t nx = 100, ny = 100; int ncount = -77;
        int rrc = reference_choose_cell(&board, C, map, used, all_face, &rx, &ry, &rcount);
        const uint64_t *cell_mask_2[BT_CELLS];
        fill_mask_cache(cell_mask_2, map, C);
        int nrc = mrv_choose_cell(&board, C, map, used, &f, cell_mask_2, &nx, &ny, &ncount);

        ASSERT_EQ_FMT(rrc, nrc, "%d");
        if (rrc == 1) {
            ASSERT_EQ_FMT((int)rx, (int)nx, "%d");
            ASSERT_EQ_FMT((int)ry, (int)ny, "%d");
            ASSERT_EQ_FMT(rcount, ncount, "%d");
        }
    }
    PASS();
}

/* Départage des égalités par le nombre de côtés contraints.
 *
 * Fixture : plateau plein sauf TROIS cases vides, sur une map UNIFORME — toute
 * case contrainte offre donc le même nombre de candidats, et le score MRV ne
 * départage rien. Restent les côtés contraints (`nconstr` = 4 − voisines vides) :
 *
 *   (0,0) et (0,1), adjacentes l'une à l'autre  -> une voisine vide  -> nconstr 3
 *   (ETERN_SIZE/2, ETERN_SIZE/2), isolée        -> aucune voisine vide -> nconstr 4
 *
 * L'ordre d'énumération donnerait (0,0), qui vient en premier. Le départage doit
 * lui préférer la case ISOLÉE, strictement plus contrainte — c'est le sens que la
 * mesure sur stock de production a retenu, à l'INVERSE de l'heuristique de degré
 * classique (cf. §4.12). Ce test verrouille ce sens : l'inverser le fait tomber. */
TEST mrv_choose_cell_breaks_ties_by_constrained_sides(void)
{
    static struct part cand[2] = { { .id = 6 }, { .id = 7 } };
    static struct array_part list = { .size = 2, .parts = cand };
    map_big_array *map = make_uniform_map(&list);
    const int8_t all_face = (int8_t)map->sizearrayM;
    const int ix = ETERN_SIZE / 2, iy = ETERN_SIZE / 2;

    struct possibility_packet board;
    make_empty_board(&board);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            board.grid[x][y] = 1;
    board.grid[0][0] = -2;
    board.grid[0][1] = -2;
    board.grid[ix][iy] = -2;

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_filler_part(), all_face);
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, &board);
    bt_frontier f;
    bt_frontier_init(&f, &board);

    /* Le fixture ne vaut que si les trois cases sont bien à égalité de score. */
    ASSERT_EQ_FMT(3, (int)f.nconstr[BT_CELL_POS(0, 0)], "%d");
    ASSERT_EQ_FMT(3, (int)f.nconstr[BT_CELL_POS(0, 1)], "%d");
    ASSERT_EQ_FMT(4, (int)f.nconstr[BT_CELL_POS(ix, iy)], "%d");

    uint8_t x = 200, y = 200; int count = -99;
    const uint64_t *cell_mask_3[BT_CELLS];
    fill_mask_cache(cell_mask_3, map, C);
    ASSERT_EQ_FMT(1, mrv_choose_cell(&board, C, map, used, &f, cell_mask_3, &x, &y, &count), "%d");
    ASSERT_EQ_FMT(ix, (int)x, "%d");
    ASSERT_EQ_FMT(iy, (int)y, "%d");

    /* Sans la case isolée, les deux restantes sont à égalité sur nconstr aussi :
     * l'ordre d'énumération reprend la main, et c'est (0,0). */
    board.grid[ix][iy] = 1;
    bt_init_constraints(C, &board, make_filler_part(), all_face);
    bt_frontier_init(&f, &board);
    const uint64_t *cell_mask_4[BT_CELLS];
    fill_mask_cache(cell_mask_4, map, C);
    ASSERT_EQ_FMT(1, mrv_choose_cell(&board, C, map, used, &f, cell_mask_4, &x, &y, &count), "%d");
    ASSERT_EQ_FMT(0, (int)x, "%d");
    ASSERT_EQ_FMT(0, (int)y, "%d");

    PASS();
}

/* Deux sorties que les plateaux aléatoires n'atteignent pas.
 *
 * (a) plateau PLEIN : aucune case vide -> 0, comme l'ancien balayage.
 * (b) repli `fallback` : structurellement inatteignable depuis un vrai plateau
 *     (toute case vide touche un bord ou une voisine posée — cf. la doc de
 *     mrv_choose_cell), donc forcé ici en abaissant le masque `constrained`.
 *     C'est une branche DÉFENSIVE : ce test dit qu'elle fait ce qu'elle
 *     annonce, pas qu'elle se déclenche en production. */
TEST mrv_choose_cell_full_board_and_unconstrained_fallback(void)
{
    static struct part cand[2] = { { .id = 6 }, { .id = 7 } };
    static struct array_part list = { .size = 2, .parts = cand };
    map_big_array *map = make_uniform_map(&list);
    const int8_t all_face = (int8_t)map->sizearrayM;

    struct possibility_packet board;
    make_empty_board(&board);
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++)
            board.grid[x][y] = 1;

    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_filler_part(), all_face);
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, &board);

    uint8_t x = 200, y = 200; int count = -99;

    /* (a) plateau plein */
    bt_frontier full_f;
    bt_frontier_init(&full_f, &board);
    const uint64_t *cell_mask_5[BT_CELLS];
    fill_mask_cache(cell_mask_5, map, C);
    ASSERT_EQ_FMT(0, mrv_choose_cell(&board, C, map, used, &full_f, cell_mask_5, &x, &y, &count), "%d");

    /* (b) une case vide, mais déclarée non contrainte */
    const int hx = ETERN_SIZE / 2, hy = ETERN_SIZE / 2;
    board.grid[hx][hy] = -2;
    bt_frontier f;
    bt_frontier_init(&f, &board);
    for (int w = 0; w < BT_FRONTIER_WORDS; w++) {
        f.constrained[w] = 0;
    }
    const uint64_t *cell_mask_6[BT_CELLS];
    fill_mask_cache(cell_mask_6, map, C);
    ASSERT_EQ_FMT(1, mrv_choose_cell(&board, C, map, used, &f, cell_mask_6, &x, &y, &count), "%d");
    ASSERT_EQ_FMT(hx, (int)x, "%d");
    ASSERT_EQ_FMT(hy, (int)y, "%d");
    ASSERT_EQ_FMT(POSSIBILITY_MIN_CANDIDATS_UNKNOWN, count, "%d");

    PASS();
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
    /* Case du niveau : en ordre fixe elle vaut dirx[d]/diry[d] (cf. bt_level). */
    stack[0].x = dirx[0]; stack[0].y = diry[0];
    stack[1].x = dirx[1]; stack[1].y = diry[1];

    memset(client, 0, sizeof(*client));
    client->compteur = 0;
    client->map_part = make_free_map();
    client->all_rotate_part = make_small_parts();
    return 1;
}


/* --------------------------------------------------------------------------
 * Chemin rapide « masques complets » (bt_masks_complete)
 *
 * Fixture : une map RÉELLE (prepare_map_part, donc index compact + masque
 * d'ids) dont l'id maximal vaut ETERN_PARTS, pour que `id_mask_words` soit
 * exactement BT_MASK_WORDS quelle que soit la taille de puzzle compilée —
 * sans cela la map ne serait pas « complète » et le chemin rapide ne serait
 * jamais exercé (sous ETERN_PARTS=256, 15 ids ne font qu'un mot sur 4).
 *
 * Un côté joker de la map n'accepte que les faces NON NULLES (0 est la couleur
 * de bordure, cf. search_face) : pour qu'une clé de plateau aléatoire — 0 sur
 * les côtés bordure/voisine posée (pièce de remplissage à faces 0), joker
 * ailleurs — soit toujours non vide, il faut, pour chaque sous-ensemble S de
 * côtés, une pièce à 0 exactement sur S et non nulle ailleurs. Les ids 1..15
 * portent les 15 motifs à au moins un zéro (bits de l'id) en couleur 1 ; la
 * pièce ETERN_PARTS porte le motif sans zéro en couleur 2 — ce qui laisse
 * en outre des compartiments réellement VIDES (clés en 1 pur, ou mêlant 2 et
 * 0) pour le test du masque à zéro. Une case ne meurt donc que par pièces
 * UTILISÉES : les deux verdicts sont observables.
 * ------------------------------------------------------------------------ */
#define COMPLETE_MAP_NB_SMALL_IDS 15
static struct array_part *make_complete_map_parts(void)
{
    static struct part parts[COMPLETE_MAP_NB_SMALL_IDS + 2];
    static struct array_part a;
    parts[0] = (struct part){ .id = 0 }; /* bouchon bordure */
    for (int id = 1; id <= COMPLETE_MAP_NB_SMALL_IDS; id++) {
        int zeros = id; /* bit levé = côté à 0 (motifs 1..15) */
        parts[id] = (struct part){ .id = (int16_t)id,
                                   .top = (int8_t)(((zeros >> 3) & 1) ? 0 : 1),
                                   .right = (int8_t)(((zeros >> 2) & 1) ? 0 : 1),
                                   .bottom = (int8_t)(((zeros >> 1) & 1) ? 0 : 1),
                                   .left = (int8_t)((zeros & 1) ? 0 : 1) };
    }
    parts[COMPLETE_MAP_NB_SMALL_IDS + 1] = (struct part){ .id = ETERN_PARTS, .top = 2, .right = 2, .bottom = 2, .left = 2 };
    a = (struct array_part){ .size = COMPLETE_MAP_NB_SMALL_IDS + 2, .parts = parts };
    return &a;
}

/* Marque « utilisés », avec probabilité 1/8 chacun, les ids de la fixture
 * ci-dessus (1..15 et ETERN_PARTS) — chaque clé n'ayant qu'une pièce, une
 * probabilité plus forte tuerait presque tout plateau 16×16. */
static void mark_random_used(struct possibility_packet *b, unsigned *state)
{
    for (int id = 1; id <= COMPLETE_MAP_NB_SMALL_IDS; id++) {
        if ((lcg_next(state) & 7) == 0) set_face_used(b->b_faceused, (uint16_t)(id - 1), 1);
    }
    if ((lcg_next(state) & 7) == 0) set_face_used(b->b_faceused, (uint16_t)(ETERN_PARTS - 1), 1);
}

/* Première clé de la map dont le compartiment est VIDE (taille 0), ou 0 si
 * aucune — la fixture en garantit au moins une (couleur 2 isolée). */
static int find_empty_key(map_big_array *map, key_part *out)
{
    int m = map->sizearray;
    for (int k1 = 0; k1 < m; k1++) for (int k2 = 0; k2 < m; k2++)
        for (int k3 = 0; k3 < m; k3++) for (int k4 = 0; k4 < m; k4++) {
            key_part key = { .k1 = (int8_t)k1, .k2 = (int8_t)k2, .k3 = (int8_t)k3, .k4 = (int8_t)k4 };
            if (get_parts_bigarray_with_key(map, &key)->size == 0) {
                *out = key;
                return 1;
            }
        }
    return 0;
}

/* bt_mask_resolve : sur une map qui porte l'index, un compartiment VIDE donne
 * `bt_zero_mask` dans le cache (jamais NULL) ; sans index, NULL reste NULL
 * (le repli par parcours des lecteurs génériques en dépend). Le contrat de
 * map_bucket_id_mask lui-même — NULL pour vide — est inchangé. */
TEST bt_mask_cache_uses_zero_mask_for_empty_bucket_in_fast_mode(void)
{
    map_big_array *map = prepare_map_part(make_complete_map_parts());
    ASSERT(bt_masks_complete(map));

    key_part empty_key;
    ASSERT(find_empty_key(map, &empty_key)); /* sinon le test ne prouverait rien */
    ASSERT_EQ(NULL, map_bucket_id_mask(map, &empty_key));
    ASSERT_EQ(bt_zero_mask, bt_mask_resolve(map, &empty_key));

    /* Un compartiment non vide garde son vrai masque. */
    key_part full_key = { .k1 = 0, .k2 = 0, .k3 = 0, .k4 = 0 }; /* pièce 15 (et le bouchon) */
    ASSERT(get_parts_bigarray_with_key(map, &full_key)->size > 0);
    ASSERT_EQ(map_bucket_id_mask(map, &full_key), bt_mask_resolve(map, &full_key));
    ASSERT(bt_mask_resolve(map, &full_key) != bt_zero_mask);

    /* Verdict identique des deux côtés : zéro pièce libre. */
    struct possibility_packet board;
    make_empty_board(&board);
    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, &board);
    ASSERT_EQ_FMT(0, mrv_free_candidates(map, &empty_key, &board, used, bt_zero_mask), "%d");
    ASSERT_EQ_FMT(0, mrv_free_candidates(map, &empty_key, &board, used, NULL), "%d");

    /* Contre-contrôle : sans index compact, plus de map « complète », et le
     * cache garde NULL. */
    uint32_t *saved = map->packed;
    map->packed = NULL;
    ASSERT(!bt_masks_complete(map));
    ASSERT_EQ(NULL, bt_mask_resolve(map, &empty_key));
    map->packed = saved;

    /* Sur une map complète, bt_mask_init ne laisse AUCUN NULL. */
    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, make_filler_part(), (int8_t)map->sizearrayM);
    const uint64_t *cache[BT_CELLS];
    bt_mask_init(cache, map, C);
    for (int c = 0; c < BT_CELLS; c++) {
        ASSERT(cache[c] != NULL);
    }

    free_bigarray(map);
    PASS();
}

/* mrv_choose_cell_fast doit rendre EXACTEMENT ce que rendent la version
 * générique et l'oracle indépendant (reference_choose_cell) : même verdict,
 * même case, même score — sur 300 plateaux pseudo-aléatoires avec des
 * sous-ensembles de pièces utilisées variés, sur une map réelle complète.
 * Les deux issues (case choisie / sous-arbre mort) doivent être observées,
 * sinon l'égalité ne prouverait rien. */
TEST mrv_choose_cell_fast_matches_generic_on_real_map(void)
{
    map_big_array *map = prepare_map_part(make_complete_map_parts());
    ASSERT(bt_masks_complete(map)); /* sinon le chemin rapide n'est pas testé */
    const int8_t all_face = (int8_t)map->sizearrayM;

    unsigned state = 20260905u;
    int seen_chosen = 0, seen_dead = 0;
    for (int iter = 0; iter < 300; iter++) {
        struct possibility_packet board;
        make_random_board(&board, &state);
        mark_random_used(&board, &state);

        key_part C[ETERN_SIZE][ETERN_SIZE];
        bt_init_constraints(C, &board, make_filler_part(), all_face);
        uint64_t used[MRV_USED_WORDS];
        mrv_used_init(used, &board);
        bt_frontier f;
        bt_frontier_init(&f, &board);
        const uint64_t *cell_mask[BT_CELLS];
        bt_mask_init(cell_mask, map, C);

        uint8_t rx = 200, ry = 200; int rcount = -99;
        uint8_t gx = 201, gy = 201; int gcount = -98;
        uint8_t fx = 202, fy = 202; int fcount = -97;
        int rrc = reference_choose_cell(&board, C, map, used, all_face, &rx, &ry, &rcount);
        int grc = mrv_choose_cell(&board, C, map, used, &f, cell_mask, &gx, &gy, &gcount);
        int frc = mrv_choose_cell_fast(used, &f, cell_mask, &fx, &fy, &fcount);

        ASSERT_EQ_FMT(rrc, frc, "%d");
        ASSERT_EQ_FMT(grc, frc, "%d");
        if (frc == 1) {
            seen_chosen = 1;
            ASSERT_EQ_FMT((int)rx, (int)fx, "%d");
            ASSERT_EQ_FMT((int)ry, (int)fy, "%d");
            ASSERT_EQ_FMT(rcount, fcount, "%d");
            ASSERT_EQ_FMT((int)gx, (int)fx, "%d");
            ASSERT_EQ_FMT((int)gy, (int)fy, "%d");
            ASSERT_EQ_FMT(gcount, fcount, "%d");
        } else {
            seen_dead = 1;
        }
    }
    ASSERT(seen_chosen);
    ASSERT(seen_dead);

    free_bigarray(map);
    PASS();
}

#if FORWARD_CHECK_K > 0
/* bt_forward_check_fast : même verdict que bt_forward_check (chemin masque
 * ET chemin parcours, index neutralisé) sur toutes les cases d'une map réelle
 * complète, sous trois occupations (aucune pièce utilisée, un tirage
 * aléatoire, toutes utilisées) — les deux verdicts doivent être observés. */
TEST bt_forward_check_fast_same_verdict_as_generic(void)
{
    struct array_part *a = make_complete_map_parts();
    map_big_array *map = prepare_map_part(a);
    ASSERT(bt_masks_complete(map));
    const int nb_ids = COMPLETE_MAP_NB_SMALL_IDS;

    struct possibility_packet board;
    make_empty_board(&board);
    key_part C[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(C, &board, a, (int8_t)map->sizearrayM);

    unsigned state = 777u;
    int seen_alive = 0, seen_dead = 0;
    for (int phase = 0; phase < 3; phase++) {
        if (phase == 1) {
            mark_random_used(&board, &state);
        } else if (phase == 2) {
            for (int id = 1; id <= nb_ids; id++) set_face_used(board.b_faceused, (uint16_t)(id - 1), 1);
            set_face_used(board.b_faceused, (uint16_t)(ETERN_PARTS - 1), 1);
        }
        uint64_t used[MRV_USED_WORDS];
        mrv_used_init(used, &board);
        const uint64_t *cell_mask[BT_CELLS];
        bt_mask_init(cell_mask, map, C);
        for (int cx = 0; cx < ETERN_SIZE; cx++) for (int cy = 0; cy < ETERN_SIZE; cy++) {
            int generic = bt_forward_check(C, &board, map, used, cell_mask, cx, cy);
            int fast = bt_forward_check_fast(&board, used, cell_mask, cx, cy);
            ASSERT_EQ_FMT(generic, fast, "%d");

            uint32_t *saved = map->packed;
            map->packed = NULL; /* force le parcours des entrées */
            const uint64_t *cell_mask_generic[BT_CELLS];
            bt_mask_init(cell_mask_generic, map, C);
            int traversal = bt_forward_check(C, &board, map, used, cell_mask_generic, cx, cy);
            map->packed = saved;
            ASSERT_EQ_FMT(traversal, fast, "%d");

            if (fast) seen_alive = 1; else seen_dead = 1;
        }
    }
    ASSERT(seen_alive);
    ASSERT(seen_dead);

    free_bigarray(map);
    PASS();
}
#endif // FORWARD_CHECK_K > 0

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
    int n = bt_materialize_pending(&client, &board, stack, top, idParts, out, 8, new_next_s);

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

/* ======================================================================
 * §4.7 (cf. docs/autosearch_step.md) — délégation en ordre
 * DYNAMIQUE : `alloc` des paquets émis est fixé par RECOMPTAGE
 * (`possibility_placed_count`), plus par re-canonisation sur le premier trou
 * du parcours (`bt_canonicalize_packet`, supprimée par PR1 : `directions[]`
 * n'est plus un référentiel d'état). C'est ce qui permet à un paquet délégué
 * par un client MRV d'être repris SANS PERTE par n'importe quel autre client
 * MRV, quel que soit l'ordre dans lequel ses cases ont été posées, sans
 * toucher au format de paquet (donc sans bump de `VERSION` pour CE changement
 * précis — `VERSION` est bumpée par ailleurs, pour le changement de SENS de
 * `alloc` lui-même).
 * ====================================================================== */

/* bt_materialize_pending : un niveau dont la case n'est PAS
 * celle de sa profondeur de pile (ordre MRV) produit des paquets dont `alloc`
 * est le nombre RÉEL de pièces posées (ici 1 : le plateau racine est vide,
 * une seule pièce est posée par ce niveau), jamais la profondeur du niveau
 * ni un index de premier trou. */
TEST bt_materialize_pending_dynamic_order_recomputes_alloc(void)
{
    drain_local();
    ensure_counters();

    static struct part cand[2];
    static struct array_part list;
    for (int i = 0; i < 2; i++) { memset(&cand[i], 0, sizeof(struct part)); cand[i].id = (int16_t)(i + 6); }
    list.size = 2; list.parts = cand;

    struct possibility_packet board;
    make_empty_board(&board);

    /* Un seul niveau, sur une case CHOISIE (index 5 du parcours) : rien n'est
     * posé avant elle, exactement ce que fait MRV. */
    bt_level stack[1];
    stack[0].search = &list;
    stack[0].next_s = 0;
    stack[0].placed_pos = -1;
    stack[0].x = dirx[5];
    stack[0].y = diry[5];
    stack[0].mrv_score = 3; /* score MRV calculé quand ce niveau a été ouvert */

    client_possibility_t client;
    memset(&client, 0, sizeof(client));
    client.compteur = 0;
    client.map_part = make_free_map();
    client.all_rotate_part = make_small_parts();

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    struct possibility_packet out[4];
    int new_next_s[1];
    int n = bt_materialize_pending(&client, &board, stack, 0, idParts, out, 4, new_next_s);

    ASSERT_EQ_FMT(2, n, "%d");
    for (int i = 0; i < n; i++) {
        /* La pièce est bien posée sur la case CHOISIE… */
        ASSERT(out[i].grid[dirx[5]][diry[5]] != -2);
        /* …et alloc = nombre RÉEL de pièces posées (1), pas la profondeur du
         * niveau (qui vaudrait 1 par coïncidence ici avec l'ordre fixe -- le
         * point du test est que c'est un RECOMPTAGE, cf. l'assertion suivante). */
        ASSERT_EQ_FMT(1, (int)out[i].alloc, "%d");
        ASSERT_EQ_FMT(possibility_placed_count(&out[i]), (int)out[i].alloc, "%d");
        ASSERT_EQ_FMT(0, (int)out[i].checked, "%d");
        /* min_candidats est repris tel quel de stack[0].mrv_score : le score
         * que mrv_choose_cell avait calculé pour cette case, gratuit. */
        ASSERT_EQ_FMT(3, (int)out[i].min_candidats, "%d");
    }
    ASSERT_EQ_FMT(2, new_next_s[0], "%d"); /* niveau épuisé */

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
    int n = bt_materialize_pending(&client, &board, stack, top, idParts, out, 1, new_next_s);

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

    bt_delegate_if_needed(&client, &board, stack, top, idParts);

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

    bt_delegate_if_needed(&client, &board, stack, top, idParts);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");           /* 1 possibilité déléguée */
    ASSERT_EQ_FMT(2, stack[1].next_s, "%d");             /* niveau profond consommé */
    ASSERT_EQ_FMT(2ULL, lastfilesize[0], "%llu");        /* 3 - 1 restant local */
    /* La délégation a alloué le buffer pré-alloué du thread (capacité = seuil). */
    ASSERT(client.delegate_buf != NULL);
    ASSERT_EQ_FMT(1, client.delegate_buf_capacity, "%d");

    free(client.delegate_buf);                           /* libéré ici hors thread */
    drain_local();
    PASS();
}

/* bt_delegate_if_needed : le buffer pré-alloué est réutilisé d'un appel à l'autre
 * et n'est agrandi (realloc) que si max_stock_by_thread augmente à chaud — jamais
 * rétréci. Garde contre une régression de débordement si la limite grandit. */
TEST bt_delegate_reuses_and_grows_buffer(void)
{
    drain_local();
    ensure_counters();
    max_stock_by_thread = 1;

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    /* 1er appel : alloue le buffer à la capacité = seuil courant (1). */
    bt_delegate_if_needed(&client, &board, stack, 1, idParts);
    ASSERT(client.delegate_buf != NULL);
    ASSERT_EQ_FMT(1, client.delegate_buf_capacity, "%d");
    struct possibility_packet *first_buf = client.delegate_buf;

    /* 2e appel, même seuil : le buffer est réutilisé tel quel (pas de realloc). */
    build_two_level_fixture(&board, stack, &client);   /* reset pile/plateau... */
    client.delegate_buf = first_buf;                   /* ...sans perdre le buffer */
    client.delegate_buf_capacity = 1;
    bt_delegate_if_needed(&client, &board, stack, 1, idParts);
    ASSERT_EQ(first_buf, client.delegate_buf);         /* même pointeur */
    ASSERT_EQ_FMT(1, client.delegate_buf_capacity, "%d");

    /* 3e appel après hausse de la limite (1 -> 2, < 3 pending pour rester au-dessus
     * du seuil) : le buffer grandit pour tenir la nouvelle capacité. */
    max_stock_by_thread = 2;
    build_two_level_fixture(&board, stack, &client);
    client.delegate_buf = first_buf;
    client.delegate_buf_capacity = 1;
    bt_delegate_if_needed(&client, &board, stack, 1, idParts);
    ASSERT(client.delegate_buf_capacity >= 2);          /* capacité suffisante */

    free(client.delegate_buf);
    drain_local();
    PASS();
}

/* bt_delegation_quota : au-dessus du seuil, règle historique — max_stock cédés,
 * que le serveur ait faim ou non. */
TEST delegation_quota_above_threshold_ignores_hunger(void)
{
    ASSERT_EQ_FMT(10, bt_delegation_quota(11ULL, 10, 0), "%d");
    ASSERT_EQ_FMT(10, bt_delegation_quota(11ULL, 10, 500), "%d");
    ASSERT_EQ_FMT(10, bt_delegation_quota(1000ULL, 10, -3), "%d");
    PASS();
}

/* bt_delegation_quota : sous le seuil, aucune délégation sans faim (0 ou
 * négative), ni quand il ne reste qu'un frère (le thread ne se vide pas). */
TEST delegation_quota_below_threshold_needs_hunger(void)
{
    ASSERT_EQ_FMT(0, bt_delegation_quota(3ULL, 10, 0), "%d");
    ASSERT_EQ_FMT(0, bt_delegation_quota(3ULL, 10, -1), "%d");
    ASSERT_EQ_FMT(0, bt_delegation_quota(0ULL, 10, 5), "%d");
    ASSERT_EQ_FMT(0, bt_delegation_quota(1ULL, 10, 5), "%d");
    PASS();
}

/* bt_delegation_quota : délégation anticipée = min(faim, pending/2). */
TEST delegation_quota_anticipates_capped_at_half(void)
{
    ASSERT_EQ_FMT(3, bt_delegation_quota(10ULL, 300, 3), "%d");   /* faim < moitié */
    ASSERT_EQ_FMT(5, bt_delegation_quota(10ULL, 300, 50), "%d");  /* moitié < faim */
    ASSERT_EQ_FMT(1, bt_delegation_quota(2ULL, 300, 50), "%d");   /* moitié = 1     */
    ASSERT_EQ_FMT(1, bt_delegation_quota(3ULL, 300, 1), "%d");
    PASS();
}

/* bt_should_abandon_shallow_root : désactivé (abandon_depth <= 0) -> jamais,
 * quels que soient root_depth/placed_count. */
TEST abandon_shallow_root_disabled_never_triggers(void)
{
    ASSERT_EQ_FMT(0, bt_should_abandon_shallow_root(6, 200, 0), "%d");
    ASSERT_EQ_FMT(0, bt_should_abandon_shallow_root(6, 200, -1), "%d");
    PASS();
}

/* bt_should_abandon_shallow_root : racine reçue déjà AU-DESSUS du seuil (pas
 * "trop peu profonde") -> jamais, même très creusée. */
TEST abandon_shallow_root_deep_root_never_triggers(void)
{
    ASSERT_EQ_FMT(0, bt_should_abandon_shallow_root(128, 200, 128), "%d");
    ASSERT_EQ_FMT(0, bt_should_abandon_shallow_root(129, 200, 128), "%d");
    PASS();
}

/* bt_should_abandon_shallow_root : racine peu profonde MAIS pas encore
 * creusée jusqu'au seuil -> pas encore. */
TEST abandon_shallow_root_not_deep_enough_yet(void)
{
    ASSERT_EQ_FMT(0, bt_should_abandon_shallow_root(6, 127, 128), "%d");
    PASS();
}

/* bt_should_abandon_shallow_root : racine peu profonde ET creusée jusqu'au
 * seuil (frontière incluse, placed_count >= abandon_depth) -> déclenche. */
TEST abandon_shallow_root_triggers_at_boundary(void)
{
    ASSERT_EQ_FMT(1, bt_should_abandon_shallow_root(6, 128, 128), "%d");
    ASSERT_EQ_FMT(1, bt_should_abandon_shallow_root(0, 300, 128), "%d");
    PASS();
}

/* bt_delegate_if_needed : stock sous le seuil MAIS serveur affamé
 * (server_hunger > 0) -> délégation anticipée d'au plus pending/2, pile
 * avancée, faim décrémentée du nombre envoyé. */
TEST bt_delegate_hunger_moves_below_threshold(void)
{
    drain_local();
    ensure_counters();
    max_stock_by_thread = 10;                 /* pending (3) sous le seuil */
    __atomic_store_n(&server_hunger, 1, __ATOMIC_RELAXED);

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    bt_delegate_if_needed(&client, &board, stack, top, idParts);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");           /* 1 possibilité cédée   */
    ASSERT_EQ_FMT(2, stack[1].next_s, "%d");             /* niveau profond avancé */
    ASSERT_EQ_FMT(2ULL, lastfilesize[0], "%llu");        /* 3 - 1 restant local   */
    /* Faim consommée : 1 - 1 = 0. */
    ASSERT_EQ_FMT(0, __atomic_load_n(&server_hunger, __ATOMIC_RELAXED), "%d");

    if (client.delegate_buf != NULL) free(client.delegate_buf);
    __atomic_store_n(&server_hunger, 0, __ATOMIC_RELAXED);
    drain_local();
    PASS();
}

/* bt_min_pending_depth : le niveau 0 (le moins profond) a encore un candidat
 * non essayé (id 2) alors que le chemin courant est déjà au niveau 1 (2
 * pièces posées) -- la profondeur minimale en attente (1) doit être TROUVÉE,
 * pas confondue avec placed_count (2). C'est exactement le cas que placed_count
 * seul manquait : le fil peut détenir plus superficiel que sa position courante. */
TEST bt_min_pending_depth_finds_shallowest_level(void)
{
    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    ASSERT_EQ_FMT(1, bt_min_pending_depth(&board, stack, top), "%d");
    PASS();
}

/* bt_min_pending_depth : niveau 0 ÉPUISÉ (plus aucun candidat, next_s == size)
 * -- le parcours doit continuer au niveau 1 plutôt que de s'arrêter, et
 * trouver son candidat restant (id 5), donnant la même profondeur que le
 * chemin courant (2) puisque rien de plus superficiel n'existe. */
TEST bt_min_pending_depth_skips_exhausted_shallow_level(void)
{
    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);
    stack[0].next_s = stack[0].search->size; /* niveau 0 épuisé : id 2,3 déjà essayés ailleurs */

    ASSERT_EQ_FMT(2, bt_min_pending_depth(&board, stack, top), "%d");
    PASS();
}

/* bt_min_pending_depth : les DEUX niveaux épuisés -- rien en attente, -1. */
TEST bt_min_pending_depth_returns_minus_one_when_nothing_pending(void)
{
    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);
    stack[0].next_s = stack[0].search->size;
    stack[1].next_s = stack[1].search->size;

    ASSERT_EQ_FMT(-1, bt_min_pending_depth(&board, stack, top), "%d");
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

    bt_flush_pending(&client, &board, stack, top, idParts);

    /* 3 frères matérialisés + 1 paquet « chemin courant » = 4. */
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu");

    drain_local();
    PASS();
}

/* bt_abandon_shallow_root : même effet que bt_flush_pending (tout le travail
 * restant rendu) PLUS le comptage de l'abandon (shallow_root_abandoned). */
TEST bt_abandon_shallow_root_flushes_and_counts(void)
{
    drain_local();
    ensure_counters();
    unsigned long long before = __atomic_load_n(&shallow_root_abandoned, __ATOMIC_RELAXED);

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    bt_abandon_shallow_root(&client, &board, stack, top, idParts);

    /* Même bilan que bt_flush_pending : 3 frères + 1 chemin courant = 4. */
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(before + 1, __atomic_load_n(&shallow_root_abandoned, __ATOMIC_RELAXED), "%llu");

    drain_local();
    PASS();
}

/* bt_materialize_pending : un niveau sans décision (case pré-remplie :
 * search == NULL, placed_pos == -1) est traversé sans rien produire, et un
 * candidat id 0 (trou de map) est consommé sans être matérialisé. */
TEST bt_materialize_skips_no_decision_level_and_zero_id(void)
{
    drain_local();
    ensure_counters();

    /* niveau 1 : candidats [6, 0, 7], pièce 6 posée (next_s=1). */
    static struct part cand[3];
    memset(cand, 0, sizeof cand);
    cand[0].id = 6; cand[1].id = 0; cand[2].id = 7;
    static struct array_part list;
    list.size = 3; list.parts = cand;

    struct possibility_packet board;
    make_empty_board(&board);
    board.grid[dirx[0]][diry[0]] = 3;   /* case pré-remplie (indice d'origine) */
    board.grid[dirx[1]][diry[1]] = 6;   /* pièce 6 posée au niveau 1 */
    set_face_used(board.b_faceused, 5, 1);
    board.alloc = 2;

    bt_level stack[2];
    stack[0].search = NULL;  stack[0].next_s = 0; stack[0].placed_pos = -1;
    stack[1].search = &list; stack[1].next_s = 1; stack[1].placed_pos = 5;
    stack[0].x = dirx[0]; stack[0].y = diry[0];
    stack[1].x = dirx[1]; stack[1].y = diry[1];

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_free_map();
    client.all_rotate_part = make_small_parts();

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    struct possibility_packet out[4];
    int new_next_s[2];
    int n = bt_materialize_pending(&client, &board, stack, 1, idParts, out, 4, new_next_s);

    /* Seul id 7 est matérialisé : id 0 sauté, niveau 0 sans décision. */
    ASSERT_EQ_FMT(1, n, "%d");
    ASSERT_EQ_FMT(7, (int)out[0].grid[dirx[1]][diry[1]], "%d");
    ASSERT_EQ_FMT(3, new_next_s[1], "%d");   /* niveau 1 épuisé (id 0 consommé) */
    ASSERT_EQ_FMT(0, new_next_s[0], "%d");   /* niveau sans décision : inchangé */

    PASS();
}

#if FORWARD_CHECK_K > 0
/* bt_materialize_pending : avec une map morte, chaque frère matérialisé échoue
 * au forward-checking -> consommé sans être produit (branche « branche morte »). */
TEST bt_materialize_dead_map_produces_nothing(void)
{
    drain_local();
    ensure_counters();

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);
    client.map_part = make_dead_map();   /* toutes les cases suivantes sont mortes */

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    struct possibility_packet out[8];
    int new_next_s[2];
    int n = bt_materialize_pending(&client, &board, stack, top, idParts, out, 8, new_next_s);

    ASSERT_EQ_FMT(0, n, "%d");               /* rien produit... */
    ASSERT_EQ_FMT(2, new_next_s[1], "%d");   /* ...mais les candidats sont consommés */
    ASSERT_EQ_FMT(3, new_next_s[0], "%d");

    PASS();
}

/* bt_delegate_if_needed : stock au-dessus du seuil mais matérialisation vide
 * (tous les frères élagués) -> aucun envoi, pile inchangée. */
TEST bt_delegate_dead_map_sends_nothing(void)
{
    drain_local();
    ensure_counters();
    max_stock_by_thread = 1;

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);
    client.map_part = make_dead_map();

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    bt_delegate_if_needed(&client, &board, stack, top, idParts);

    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* rien envoyé */
    ASSERT_EQ_FMT(1, stack[1].next_s, "%d");     /* pile non consommée */
    ASSERT_EQ_FMT(3ULL, lastfilesize[0], "%llu"); /* pending compté avant élagage */

    free(client.delegate_buf);
    PASS();
}
#endif /* FORWARD_CHECK_K > 0 */

/* bt_delegate_if_needed : échec d'envoi -> la pile n'est PAS consommée, le
 * travail reste local (put_to_server a remis les paquets au stock local). */
TEST bt_delegate_error_keeps_stack(void)
{
    drain_local();
    ensure_counters();
    max_stock_by_thread = 1;

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int fds[2]; pthread_t srv;
    es_attach_failing_server(&client, fds, &srv);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    es_silence_std();
    bt_delegate_if_needed(&client, &board, stack, top, idParts);
    es_restore_std();

    ASSERT_EQ_FMT(1, stack[1].next_s, "%d");      /* pile inchangée (pas d'avance) */
    ASSERT_EQ_FMT(3ULL, lastfilesize[0], "%llu"); /* stock implicite intact */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");    /* paquet remis en local par put_to_server */

    es_detach_failing_server(&client, fds, &srv);
    free(client.delegate_buf);
    drain_local();
    PASS();
}

/* bt_flush_pending : échec d'envoi -> log, les paquets sont remis au stock
 * local par put_to_server (aucune perte de travail). */
TEST bt_flush_error_reputs_locally(void)
{
    drain_local();
    ensure_counters();

    struct possibility_packet board;
    bt_level stack[2];
    client_possibility_t client;
    int top = build_two_level_fixture(&board, stack, &client);

    int fds[2]; pthread_t srv;
    es_attach_failing_server(&client, fds, &srv);

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    es_silence_std();
    bt_flush_pending(&client, &board, stack, top, idParts);
    es_restore_std();

    /* 3 frères + le chemin courant : tous remis en local malgré l'échec. */
    ASSERT_EQ_FMT(4ULL, datas_size(), "%llu");

    es_detach_failing_server(&client, fds, &srv);
    drain_local();
    PASS();
}

/* ======================================================================
 * bt_materialize_pending : une solution complète parmi les frères.
 *
 * Plateau presque complet (une seule case vide) : le frère matérialisé a
 * `alloc` (RECOMPTAGE, `possibility_placed_count`) == ETERN_PARTS ->
 * record_solution (fichier + notification) puis, sans --stop-on-solution, il
 * n'est PAS matérialisé (rien à explorer au-delà) et la boucle continue.
 * Fork : log_solution écrit un fichier solution_* dans le CWD.
 * ====================================================================== */

static client_possibility_t mz_client;
static struct possibility_packet mz_board;
static bt_level mz_stack[1];
static int16_t mz_idParts[ETERN_PARTS + 1][PART_SIZES];
static char mz_dir[256];
static struct part mz_cand[1];
static struct array_part mz_list;

/* Définis plus bas (section 4×4) ; déclarés ici pour le test de matérialisation. */
static int es_has_solution_file(const char *dir);
static void es_unlink_solutions(const char *dir);

static void mz_child_sibling_solution(void)
{
    if (chdir(mz_dir) != 0) exit(97);
    stop_on_solution = 0;
    struct possibility_packet out[4];
    int new_next_s[1];
    int n = bt_materialize_pending(&mz_client, &mz_board, mz_stack, 0,
                                   mz_idParts, out, 4, new_next_s);
    if (n != 0) exit(50);                       /* la solution n'est pas matérialisée */
    if (new_next_s[0] != 1) exit(51);           /* mais le candidat est consommé */
    if (!es_has_solution_file(".")) exit(52);   /* et elle a été enregistrée */
    exit(42); /* exit() (pas _exit) : flush de la couverture du fils */
}

TEST bt_materialize_sibling_solution_records_and_continues(void)
{
    ensure_counters();

    memset(&mz_client, 0, sizeof mz_client);
    mz_client.compteur = 0;
    mz_client.map_part = make_free_map();
    mz_client.all_rotate_part = make_small_parts();

    /* Plateau « presque complet » : grille d'indices 0 (valide pour
     * make_small_parts), seule la dernière case du parcours reste à décider. */
    memset(&mz_board, 0, sizeof mz_board);
    mz_board.alloc = ETERN_PARTS - 1;

    memset(&mz_cand[0], 0, sizeof mz_cand[0]);
    mz_cand[0].id = 6;                          /* pièce libre : frère matérialisable */
    mz_list.size = 1; mz_list.parts = mz_cand;
    mz_stack[0].search = &mz_list; mz_stack[0].next_s = 0; mz_stack[0].placed_pos = -1;
    mz_stack[0].x = dirx[ETERN_PARTS - 1]; mz_stack[0].y = diry[ETERN_PARTS - 1];

    fill_idparts(mz_idParts);

    strcpy(mz_dir, "/tmp/etii_mz_sol_XXXXXX");
    ASSERT(mkdtemp(mz_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(mz_child_sibling_solution, &pid);

    es_unlink_solutions(mz_dir);
    rmdir(mz_dir);

    ASSERT_EQ_FMT(42, code, "%d");
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

/* search_packet_backtracking : des cases PRÉ-REMPLIES en tête de parcours créent
 * des niveaux sans décision (grid != -2), puis une case morte (map vide) fait
 * remonter le backtracking à travers eux jusqu'à l'épuisement (retour 0).
 * Couvre aussi les deux côtés de la mise à jour de max_result sur case remplie. */
TEST search_backtracking_prefilled_cells_no_decision(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_dead_map();          /* les cases vides sont mortes */
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.grid[dirx[0]][diry[0]] = 3;            /* pièce 4 pré-placée */
    root.grid[dirx[1]][diry[1]] = 4;            /* pièce 5 pré-placée */
    set_face_used(root.b_faceused, 3, 1);
    set_face_used(root.b_faceused, 4, 1);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    int saved_req = request;
    uint16_t saved_max = max_result;
    request = REQUEST_CONTINUE;
    max_result = 1;   /* case 1 : 1 > 1 faux ; case 2 : 2 > 1 vrai */
    int rc = search_packet_backtracking(&client, &root, idParts);
    request = saved_req;
    max_result = saved_max;

    ASSERT_EQ_FMT(0, rc, "%d");                 /* sous-arbre épuisé */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");  /* rien délégué */
    PASS();
}

/* search_packet_backtracking : exploration réelle avec placements, élagage et
 * retours arrière. Map uniforme [0, 6, 7] : deux pièces réelles seulement, donc
 * l'arbre s'épuise en quelques nœuds — chaque niveau revisite un placement
 * (annulation L659), saute le trou id 0 et les pièces déjà utilisées. */
TEST search_backtracking_explores_and_exhausts(void)
{
    drain_local();
    ensure_counters();

    static struct part cand[3];
    memset(cand, 0, sizeof cand);
    cand[0].id = 0; cand[1].id = 6; cand[2].id = 7;
    static struct array_part list;
    list.size = 3; list.parts = cand;

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_uniform_map(&list);
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    unsigned long long nodes_before = counters[0];
    int saved_req = request;
    uint16_t saved_max = max_result;
    request = REQUEST_CONTINUE;
    max_result = 0;   /* les placements réels font progresser max_result */
    int rc = search_packet_backtracking(&client, &root, idParts);
    request = saved_req;
    max_result = saved_max;

    ASSERT_EQ_FMT(0, rc, "%d");                 /* arbre entièrement exploré */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");  /* jamais délégué (trop petit) */
    ASSERT(counters[0] > nodes_before);         /* des nœuds ont bien été visités */
    PASS();
}

/* ======================================================================
 * search_packet_backtracking_budgeted (§4.6b de
 * docs/conception/elagage_recherche.md) : même cœur MRV que
 * search_packet_backtracking (search_packet_backtracking_mrv, seul moteur
 * depuis docs/autosearch_step.md), plafonné en nœuds et SANS
 * délégation (allow_delegate = 0). Deux volets, comme l'exige la doctrine de
 * tests du document de conception (§5) : un plateau où la fermeture DOIT être
 * prouvée (budget large), un où elle NE DOIT PAS l'être (budget insuffisant)
 * — plus la divergence volontaire avec la variante illimitée sur REQUEST_STOP
 * (jamais de flush réseau).
 * ====================================================================== */

/* Budget large sur un arbre minuscule (map uniforme [0,6,7], même fixture que
 * search_backtracking_explores_and_exhausts) : le sous-arbre s'épuise entièrement
 * bien avant le budget -> BT_CORE_EXHAUSTED, jamais délégué (allow_delegate=0). */
TEST search_backtracking_budgeted_closes_when_budget_suffices(void)
{
    drain_local();
    ensure_counters();

    static struct part cand[3];
    memset(cand, 0, sizeof cand);
    cand[0].id = 0; cand[1].id = 6; cand[2].id = 7;
    static struct array_part list;
    list.size = 3; list.parts = cand;

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_uniform_map(&list);
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    int saved_req = request;
    uint16_t saved_max = max_result;
    request = REQUEST_CONTINUE;
    max_result = 0;
    unsigned long long nodes = 0;
    bt_core_result_t rc = search_packet_backtracking_budgeted(&client, &root, idParts, 10000, &nodes);
    request = saved_req;
    max_result = saved_max;

    ASSERT_EQ_FMT(BT_CORE_EXHAUSTED, rc, "%d");
    ASSERT(nodes > 0);                          /* coût rapporté, non nul */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* jamais délégué/flushé */

    PASS();
}

/* Même arbre, mais budget d'UN seul nœud : ne peut pas suffire à l'épuiser
 * (le seul nœud racine ne place déjà aucune pièce) -> BT_CORE_BUDGET, statut
 * indéterminé. Contrairement à REQUEST_STOP sur la variante illimitée, aucun
 * travail n'est renvoyé (allow_delegate=0) : une preuve avortée n'abandonne
 * rien, elle laisse l'appelant décider (conserver, comme avant cette PR). */
TEST search_backtracking_budgeted_returns_budget_when_insufficient(void)
{
    drain_local();
    ensure_counters();

    static struct part cand[3];
    memset(cand, 0, sizeof cand);
    cand[0].id = 0; cand[1].id = 6; cand[2].id = 7;
    static struct array_part list;
    list.size = 3; list.parts = cand;

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_uniform_map(&list);
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    int saved_req = request;
    uint16_t saved_max = max_result;
    request = REQUEST_CONTINUE;
    max_result = 0;
    unsigned long long nodes = 0;
    bt_core_result_t rc = search_packet_backtracking_budgeted(&client, &root, idParts, 1, &nodes);
    request = saved_req;
    max_result = saved_max;

    ASSERT_EQ_FMT(BT_CORE_BUDGET, rc, "%d");
    ASSERT(nodes >= 1);
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* aucun flush : rien n'est perdu ni renvoyé */

    PASS();
}

/* REQUEST_STOP en cours de preuve : contrairement à search_packet_backtracking
 * (search_backtracking_stop_flushes_and_returns_one, qui renvoie 1 possibilité
 * flushée), la variante bornée ne délègue ni ne flushe JAMAIS (allow_delegate=0)
 * — déléguer casserait la preuve de fermeture elle-même. Résultat :
 * BT_CORE_STOPPED, stock local inchangé. */
TEST search_backtracking_budgeted_stop_returns_stopped_without_flush(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_dead_map();
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    int saved = request;
    request = REQUEST_STOP;
    unsigned long long nodes = 12345;
    bt_core_result_t rc = search_packet_backtracking_budgeted(&client, &root, idParts, 10000, &nodes);
    request = saved;

    ASSERT_EQ_FMT(BT_CORE_STOPPED, rc, "%d");
    ASSERT_EQ_FMT(1ULL, nodes, "%llu");          /* seul le nœud racine compté */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* jamais flushé, contrairement à la recherche illimitée */

    drain_local();
    PASS();
}

/* search_packet_backtracking : REQUEST_PAUSE fait patienter la boucle, puis un
 * REQUEST_STOP (posé par un thread auxiliaire) déclenche le flush et la sortie. */
static void *es_flip_pause_to_stop(void *arg)
{
    (void)arg;
    usleep(20000);
    request = REQUEST_STOP;
    return NULL;
}

TEST search_backtracking_pause_waits_then_stops(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_dead_map();
    client.all_rotate_part = make_small_parts();

    struct possibility_packet root;
    make_empty_board(&root);
    root.alloc = 0;

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    fill_idparts(idParts);

    int saved_req = request;
    request = REQUEST_PAUSE;
    pthread_t th;
    pthread_create(&th, NULL, es_flip_pause_to_stop, NULL);
    int rc = search_packet_backtracking(&client, &root, idParts);
    pthread_join(th, NULL);
    request = saved_req;

    ASSERT_EQ_FMT(1, rc, "%d");                 /* arrêt demandé après la pause */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");   /* chemin courant renvoyé */

    drain_local();
    PASS();
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
 * pas ; la recherche revient avec 0 (sous-arbre épuisé). exit() (pas _exit) :
 * la couverture du fils est flushée à la sortie. */
static void es_child_full_explore(void)
{
    if (chdir(es_solution_dir) != 0) exit(97);
    stop_on_solution = 0;
    request = REQUEST_CONTINUE;
    int rc = search_packet_backtracking(&es_client, &es_root, es_idParts);
    exit(rc == 0 ? 0 : 3);
}

/* Fils : stop_on_solution = 1. La 1re solution déclenche record_solution ->
 * exit(EXIT_SUCCESS) : on ne revient jamais au exit(99) ci-dessous. */
static void es_child_stop_on_solution(void)
{
    if (chdir(es_solution_dir) != 0) exit(97);
    stop_on_solution = 1;
    request = REQUEST_CONTINUE;
    search_packet_backtracking(&es_client, &es_root, es_idParts);
    exit(99); /* solution non trouvée / pas d'exit : échec */
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

/* Fils : la preuve BORNÉE, jouée depuis la racine vide du vrai 4×4, doit
 * fermer entièrement l'arbre (qui tient largement dans le budget) et ne
 * jamais escamoter une solution : `BT_CORE_EXHAUSTED` signifie « sous-arbre
 * entièrement parcouru », et toute solution rencontrée en chemin a été
 * enregistrée par record_solution — c'est ce qui autorise le pruner à
 * éliminer la possibilité. */
static void es_child_budgeted_closes_4x4(void)
{
    if (chdir(es_solution_dir) != 0) exit(97);
    stop_on_solution = 0;
    request = REQUEST_CONTINUE;
    max_result = 0;

    unsigned long long nodes = 0;
    bt_core_result_t rc = search_packet_backtracking_budgeted(&es_client, &es_root,
                                                               es_idParts, 5000000, &nodes);
    if (rc != BT_CORE_EXHAUSTED) exit(10);
    if (!es_has_solution_file(".")) exit(11);   /* la solution N'est PAS perdue */
    exit(0);
}

/* Verrou de la règle §5 du document de conception : un élagage est une
 * condition nécessaire, il ne doit jamais coûter une solution. */
TEST search_backtracking_budgeted_closes_4x4(void)
{
    ensure_counters();
    ASSERT(es_setup());

    strcpy(es_solution_dir, "/tmp/etii_es_dfsmrv_XXXXXX");
    ASSERT(mkdtemp(es_solution_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(es_child_budgeted_closes_4x4, &pid);

    es_unlink_solutions(es_solution_dir);
    rmdir(es_solution_dir);

    ASSERT_EQ_FMT(0, code, "%d");

    free_bigarray(es_client.map_part);
    free_array_part(es_client.all_rotate_part);
    PASS();
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

/* Compte les fichiers solution_*.csv dans dir (variante dénombrante de
 * es_has_solution_file). */
static int es_count_solution_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) return 0;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "solution_", 9) == 0) count++;
    }
    closedir(d);
    return count;
}

/* §4.4 — CONFLIT DE SINGLETONS (`singleton_conflict_check`). C'est une
 * condition NÉCESSAIRE : elle ne doit jamais coûter une seule solution.
 * Verrou sur le moteur MRV (seul moteur depuis
 * docs/autosearch_step.md — le drapeau vit dans
 * bt_forward_check, partagé avec l'ancien moteur à ordre fixe avant sa
 * suppression) : exploration exhaustive du vrai puzzle 4×4, drapeau levé puis
 * baissé, même nombre de solutions. */
TEST search_backtracking_singleton_conflict_check_preserves_solution_count(void)
{
    ensure_counters();
    ASSERT(es_setup());

    char dir_on[256], dir_off[256];
    strcpy(dir_on, "/tmp/etii_es_sgc_on_XXXXXX");
    strcpy(dir_off, "/tmp/etii_es_sgc_off_XXXXXX");
    ASSERT(mkdtemp(dir_on) != NULL);
    ASSERT(mkdtemp(dir_off) != NULL);

    int saved_scc = singleton_conflict_check;

    singleton_conflict_check = 1;
    strcpy(es_solution_dir, dir_on);
    pid_t pid_on = 0;
    int code_on = run_in_fork(es_child_full_explore, &pid_on);

    singleton_conflict_check = 0;
    strcpy(es_solution_dir, dir_off);
    pid_t pid_off = 0;
    int code_off = run_in_fork(es_child_full_explore, &pid_off);

    singleton_conflict_check = saved_scc;

    int count_on = es_count_solution_files(dir_on);
    int count_off = es_count_solution_files(dir_off);
    es_unlink_solutions(dir_on);
    es_unlink_solutions(dir_off);
    rmdir(dir_on);
    rmdir(dir_off);

    ASSERT_EQ_FMT(0, code_on, "%d");
    ASSERT_EQ_FMT(0, code_off, "%d");
    ASSERT(count_off > 0);
    ASSERT_EQ_FMT(count_off, count_on, "%d");

    free_bigarray(es_client.map_part);
    free_array_part(es_client.all_rotate_part);
    PASS();
}

/* Drapeau de fin de recherche, lu par le thread d'arrêt (fils du fork : un
 * seul thread écrit, un seul lit, valeur non composite). */
static volatile int es_mrv_search_done = 0;

/* Demande l'arrêt dès que la recherche a franchi quelques nœuds, pour que le
 * renvoi du travail restant (bt_flush_pending) porte sur une VRAIE pile MRV
 * partiellement explorée, pas sur la seule racine. Sort aussi si la recherche
 * s'est terminée d'elle-même avant (l'assertion du test reste valide dans les
 * deux cas — voir la doc du test). */
static void *es_mrv_stop_requester(void *arg)
{
    (void)arg;
    while (!es_mrv_search_done && counters[0] < 40) {
        /* attente active : la recherche 4×4 dure des millisecondes */
    }
    request = REQUEST_STOP;
    return NULL;
}

/* §4.7 / PR1 — INTÉGRITÉ de la délégation en ordre dynamique.
 *
 * Le point le plus délicat de l'implémentation complète : un client MRV cède
 * du travail sous forme de `possibility_packet`, et ces paquets doivent être
 * repris SANS PERTE par n'importe quel client MRV, y compris troués par
 * rapport à `directions[]`. Le test rejoue exactement ce scénario sur le vrai
 * puzzle 4×4 :
 *   1. exploration depuis la racine vide, interrompue en cours de route
 *      (REQUEST_STOP) : le travail restant part dans le stock local ;
 *   2. reprise du stock jusqu'à épuisement, en vérifiant au passage que chaque
 *      paquet reçu est cohérent (`check_possibility`, qui couvre TOUTES les
 *      cases posées depuis PR1) et que `alloc` est bien un RECOMPTAGE exact
 *      (`possibility_placed_count`), jamais un curseur re-canonisé sur le
 *      premier trou de `directions[]` (`bt_canonicalize_packet`/
 *      `normalize_possibility_packet`, supprimées dès PR1) ;
 *   3. le nombre TOTAL de solutions doit être exactement celui d'une
 *      exploration exhaustive ininterrompue.
 *
 * Si l'arrêt arrive après la fin de la recherche (course sans conséquence),
 * l'étape 2 ne fait rien et le comptage reste exact : le test ne peut pas
 * être instable, seulement moins couvrant. */
static void es_child_mrv_delegating_explore(void)
{
    if (chdir(es_solution_dir) != 0) exit(97);
    stop_on_solution = 0;
    request = REQUEST_CONTINUE;
    es_mrv_search_done = 0;
    // Remise à zéro : le compteur est cumulé par les tests précédents du même
    // processus, et le seuil du thread d'arrêt serait déjà franchi.
    counters[0] = 0;

    pthread_t stopper;
    if (pthread_create(&stopper, NULL, es_mrv_stop_requester, NULL) != 0) exit(96);
    int rc = search_packet_backtracking(&es_client, &es_root, es_idParts);
    es_mrv_search_done = 1;
    pthread_join(stopper, NULL);
    // Arrêt effectivement pris en cours de route : du travail DOIT avoir été
    // renvoyé, sans quoi l'étape 2 ne prouverait rien.
    if (rc == 1 && datas_size() == 0) exit(62);

    // Reprise du travail délégué, paquet par paquet.
    request = REQUEST_CONTINUE;
    for (;;) {
        array_possibility_packet *r = get_last_possibility(NULL, 64, NULL);
        if (r->size == 0) {
            free_array_possibility_packet(r);
            break;
        }
        for (int i = 0; i < r->size; i++) {
            if (check_possibility(&r->possibilities[i], es_client.all_rotate_part) < 0) exit(60);
            if (possibility_placed_count(&r->possibilities[i]) != (int)r->possibilities[i].alloc) exit(61);
            search_packet_backtracking(&es_client, &r->possibilities[i], es_idParts);
        }
        free_array_possibility_packet(r);
    }
    exit(rc == 0 || rc == 1 ? 0 : 3);
}

TEST search_backtracking_mrv_delegation_preserves_solution_count(void)
{
    ensure_counters();
    drain_local();
    ASSERT(es_setup());

    char dir_mrv[256], dir_ref[256];
    strcpy(dir_mrv, "/tmp/etii_es_mrvdel_XXXXXX");
    strcpy(dir_ref, "/tmp/etii_es_mrvref_XXXXXX");
    ASSERT(mkdtemp(dir_mrv) != NULL);
    ASSERT(mkdtemp(dir_ref) != NULL);

    strcpy(es_solution_dir, dir_mrv);
    pid_t pid_mrv = 0;
    int code_mrv = run_in_fork(es_child_mrv_delegating_explore, &pid_mrv);

    strcpy(es_solution_dir, dir_ref);
    pid_t pid_ref = 0;
    int code_ref = run_in_fork(es_child_full_explore, &pid_ref);

    request = REQUEST_CONTINUE;
    drain_local();

    int count_mrv = es_count_solution_files(dir_mrv);
    int count_ref = es_count_solution_files(dir_ref);
    es_unlink_solutions(dir_mrv);
    es_unlink_solutions(dir_ref);
    rmdir(dir_mrv);
    rmdir(dir_ref);

    ASSERT_EQ_FMT(0, code_mrv, "%d");   /* aucun paquet incohérent ni non canonique */
    ASSERT_EQ_FMT(0, code_ref, "%d");
    ASSERT(count_ref > 0);
    ASSERT_EQ_FMT(count_ref, count_mrv, "%d");

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

/* requeue_unprocessed_packets : échec d'envoi -> log, paquets remis au stock
 * local par put_to_server (aucune perte). */
TEST requeue_error_reputs_locally(void)
{
    drain_local();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    int fds[2]; pthread_t srv;
    es_attach_failing_server(&client, fds, &srv);

    struct possibility_packet pkts[5];
    memset(pkts, 0, sizeof pkts);
    array_possibility_packet aposs = { .size = 5, .possibilities = pkts };
    client.aposs = &aposs;

    es_silence_std();
    requeue_unprocessed_packets(&client, 2);       /* [2..5) = 3 paquets */
    es_restore_std();

    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");     /* remis en local malgré l'échec */

    es_detach_failing_server(&client, fds, &srv);
    drain_local();
    PASS();
}

/* --------------------------------------------------------------------------
 * Boucles d'attente de travail (works/aposs) : un thread auxiliaire alimente le
 * client par étapes (works=1 d'abord, aposs ensuite), ce qui exerce chaque
 * sous-condition de l'attente sous REQUEST_CONTINUE.
 * ------------------------------------------------------------------------ */

static void *es_feed_after_delay(void *arg)
{
    client_possibility_t *cp = arg;
    usleep(15000);
    request = REQUEST_CONTINUE;          /* l'attente a d'abord tourné en PAUSE */
    usleep(15000);
    pthread_mutex_lock(&cp->works_mutex);
    cp->works = 1;                       /* aposs encore NULL : l'attente continue */
    pthread_mutex_unlock(&cp->works_mutex);
    usleep(15000);
    array_possibility_packet *a = malloc(sizeof *a);
    a->size = 0;                         /* lot vide : rien à traiter */
    a->possibilities = malloc(sizeof(struct possibility_packet));
    pthread_mutex_lock(&cp->works_mutex);
    cp->aposs = a;
    pthread_mutex_unlock(&cp->works_mutex);
    return NULL;
}

TEST autosearch_step_waits_until_fed(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);
    /* works == 0 et aposs == NULL : l'attente tourne jusqu'à l'alimentation */

    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    init_id_parts(idParts);

    int saved = request;
    request = REQUEST_PAUSE;   /* l'attente tolère la pause ; le feeder repasse en CONTINUE */
    pthread_t th;
    pthread_create(&th, NULL, es_feed_after_delay, &client);
    int cont = autosearch_step(&client, idParts);
    pthread_join(th, NULL);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT(client.aposs == NULL);                  /* cycle nettoyé */
    ASSERT_EQ_FMT(0, (int)client.works, "%d");

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

TEST autoprune_step_waits_until_fed(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);

    int saved = request;
    request = REQUEST_PAUSE;   /* même scénario : pause pendant l'attente puis reprise */
    pthread_t th;
    pthread_create(&th, NULL, es_feed_after_delay, &client);
    int cont = autoprune_step(&client);
    pthread_join(th, NULL);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT(client.aposs == NULL);
    ASSERT_EQ_FMT(0, (int)client.works, "%d");

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* --------------------------------------------------------------------------
 * Enveloppes de thread : un premier tour complet (step -> 1) puis un
 * REQUEST_STOP posé par un thread auxiliaire termine la boucle au tour suivant.
 * Complète les tests « arrêt immédiat » qui ne prenaient jamais le corps du while.
 * ------------------------------------------------------------------------ */

static void *es_stop_after_delay(void *arg)
{
    (void)arg;
    usleep(40000);
    request = REQUEST_STOP;
    return NULL;
}

TEST autosearch_loops_once_then_stops(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);
    pthread_mutex_init(&client.socket_mutex, NULL);
    client.socket_id = -1;
    client.works = 1;
    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 0;
    aposs->possibilities = malloc(sizeof(struct possibility_packet));
    client.aposs = aposs;                /* 1er tour : lot vide -> step renvoie 1 */

    int saved = request;
    request = REQUEST_CONTINUE;
    pthread_t th;
    pthread_create(&th, NULL, es_stop_after_delay, NULL);
    void *ret = autosearch(&client);
    pthread_join(th, NULL);
    request = saved;

    ASSERT_EQ(NULL, ret);
    ASSERT(client.aposs == NULL);

    pthread_mutex_destroy(&client.works_mutex);
    pthread_mutex_destroy(&client.socket_mutex);
    drain_local();
    PASS();
}

TEST autoprune_loops_once_then_stops(void)
{
    drain_local();
    ensure_counters();

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    pthread_mutex_init(&client.works_mutex, NULL);
    pthread_mutex_init(&client.socket_mutex, NULL);
    client.socket_id = -1;
    client.works = 1;
    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 0;
    aposs->possibilities = malloc(sizeof(struct possibility_packet));
    client.aposs = aposs;

    int saved = request;
    request = REQUEST_CONTINUE;
    pthread_t th;
    pthread_create(&th, NULL, es_stop_after_delay, NULL);
    void *ret = autoprune(&client);
    pthread_join(th, NULL);
    request = saved;

    ASSERT_EQ(NULL, ret);
    ASSERT(client.aposs == NULL);

    pthread_mutex_destroy(&client.works_mutex);
    pthread_mutex_destroy(&client.socket_mutex);
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

    /* §4.6b : make_free_map() n'expose que 2 ids réels, un sous-arbre que la
       preuve de fermeture bornée fermerait entièrement (cf. les tests dédiés
       autoprune_step_dfs_budget_*) -- désactivée ici, ce test porte sur le
       seul contrôle superficiel (comportement d'avant cette PR). */
    int saved_dfs_budget = pruner_dfs_budget;
    pruner_dfs_budget = 0;
    unsigned long long checked_before = pruner_checked;
    unsigned long long cells_before = pruner_cells_studied;
    unsigned long long counter_before = counters[0];
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;
    pruner_dfs_budget = saved_dfs_budget;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT(client.aposs == NULL);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");                 /* vivant renvoyé en local */
    ASSERT_EQ_FMT(checked_before + 1, pruner_checked, "%llu"); /* compteur incrémenté */
    /* Compteur de coups : une possibilité étudiée (sémantique historique).
       Plateau vide + map libre : le contrôle balaie les ETERN_PARTS cases,
       toutes créditées au flux disjoint `pruner_cells_studied` (« dont
       prunage/s » des rapports check). */
    ASSERT_EQ_FMT(counter_before + 1, counters[0], "%llu");
    ASSERT_EQ_FMT(cells_before + ETERN_PARTS, pruner_cells_studied, "%llu");

    pthread_mutex_destroy(&client.works_mutex);
    /* map_part / all_rotate_part : stockage statique, pas de free. */
    drain_local();
    PASS();
}

/* REQUEST_PAUSE au milieu du lot : la boucle patiente (usleep) puis reprend le
 * traitement quand un thread auxiliaire repasse en REQUEST_CONTINUE. */
static void *es_flip_pause_to_continue(void *arg)
{
    (void)arg;
    usleep(20000);
    request = REQUEST_CONTINUE;
    return NULL;
}

TEST autoprune_step_pauses_then_resumes(void)
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

    /* §4.6b : voir le commentaire de autoprune_step_keeps_live_packet -- ce
       test porte sur la reprise après pause, pas sur la preuve de fermeture. */
    int saved_dfs_budget = pruner_dfs_budget;
    pruner_dfs_budget = 0;
    int saved = request;
    request = REQUEST_PAUSE;              /* le lot attend la reprise */
    pthread_t th;
    pthread_create(&th, NULL, es_flip_pause_to_continue, NULL);
    int cont = autoprune_step(&client);
    pthread_join(th, NULL);
    request = saved;
    pruner_dfs_budget = saved_dfs_budget;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* le paquet a bien été traité après la pause */

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* Paquet MORT non vérifié : éliminé du stock (pruner_removed), rien renvoyé. */
TEST autoprune_step_removes_dead_packet(void)
{
    drain_local();
    ensure_counters();
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_dead_map();    /* aucune case n'a de candidat */
    client.all_rotate_part = make_small_parts();
    pthread_mutex_init(&client.works_mutex, NULL);
    client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 1;
    aposs->possibilities = calloc(1, sizeof(struct possibility_packet));
    make_empty_board(&aposs->possibilities[0]);
    client.aposs = aposs;

    unsigned long long removed_before = pruner_removed;
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");                 /* éliminé */
    ASSERT_EQ_FMT(removed_before + 1, pruner_removed, "%llu");

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* Paquet MORT mais déjà marqué `checked` : le verdict antérieur prime, il est
 * renvoyé au stock (court-circuit work.checked || has_next). */
TEST autoprune_step_keeps_checked_dead_packet(void)
{
    drain_local();
    ensure_counters();
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    client.compteur = 0;
    client.map_part = make_dead_map();
    client.all_rotate_part = make_small_parts();
    pthread_mutex_init(&client.works_mutex, NULL);
    client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 1;
    aposs->possibilities = calloc(1, sizeof(struct possibility_packet));
    make_empty_board(&aposs->possibilities[0]);
    aposs->possibilities[0].checked = 1;  /* déjà vérifié : conservé tel quel */
    client.aposs = aposs;

    unsigned long long checked_before = pruner_checked;
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");                 /* renvoyé au stock */
    ASSERT_EQ_FMT(checked_before + 1, pruner_checked, "%llu");

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* ======================================================================
 * autoprune_step : intégration de la preuve de fermeture bornée (§4.6b).
 *
 * Fixture partagée avec autoprune_step_keeps_live_packet : make_free_map()
 * (candidats [6,7] partout) + plateau vide -> possibility_all_has_a_next_counted
 * répond « vivant » (has_next=1) sans jamais poser de pièce forcée (2 candidats
 * par case, jamais 1 seul) -- work.checked reste à 0 en entrant dans la
 * nouvelle branche. Comme make_free_map n'expose que 2 ids réels, le
 * sous-arbre s'épuise en quelques nœuds (même raisonnement que
 * search_backtracking_explores_and_exhausts) : un budget confortable le ferme
 * entièrement.
 * ====================================================================== */

/* Budget confortable : la fermeture est prouvée -> éliminée (comme une
 * branche morte du contrôle superficiel), jamais renvoyée au stock. */
TEST autoprune_step_dfs_budget_closes_possibility(void)
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

    int saved_budget = pruner_dfs_budget;
    pruner_dfs_budget = 10000;
    unsigned long long removed_before = pruner_removed;
    unsigned long long checked_before = pruner_checked;
    unsigned long long closed_before = pruner_dfs_closed;
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;
    pruner_dfs_budget = saved_budget;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");                    /* jamais redistribuée */
    ASSERT_EQ_FMT(removed_before + 1, pruner_removed, "%llu");    /* même compteur qu'une branche morte */
    ASSERT_EQ_FMT(checked_before, pruner_checked, "%llu");        /* jamais marquée checked */
    ASSERT_EQ_FMT(closed_before + 1, pruner_dfs_closed, "%llu");  /* contribution isolée du mécanisme */

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}


/* Budget d'UN seul nœud : ne peut pas suffire à prouver la fermeture ->
 * comportement d'avant cette PR inchangé (conservée, marquée checked). */
TEST autoprune_step_dfs_budget_too_small_keeps_possibility(void)
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

    int saved_budget = pruner_dfs_budget;
    pruner_dfs_budget = 1;
    unsigned long long checked_before = pruner_checked;
    unsigned long long closed_before = pruner_dfs_closed;
    unsigned long long dfs_nodes_before = pruner_dfs_nodes;
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;
    pruner_dfs_budget = saved_budget;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");                    /* conservée, comme avant cette PR */
    ASSERT_EQ_FMT(checked_before + 1, pruner_checked, "%llu");
    ASSERT_EQ_FMT(closed_before, pruner_dfs_closed, "%llu");      /* pas de fermeture prouvée */
    ASSERT(pruner_dfs_nodes > dfs_nodes_before);                  /* mais la tentative a un coût rapporté */

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* pruner_dfs_budget <= 0 : la nouvelle branche est entièrement désactivée --
 * comportement IDENTIQUE à autoprune_step_keeps_live_packet, aucun coût. */
TEST autoprune_step_dfs_budget_disabled_skips_dfs(void)
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

    int saved_budget = pruner_dfs_budget;
    pruner_dfs_budget = 0;
    unsigned long long checked_before = pruner_checked;
    unsigned long long dfs_nodes_before = pruner_dfs_nodes;
    int saved = request;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&client);
    request = saved;
    pruner_dfs_budget = saved_budget;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    ASSERT_EQ_FMT(checked_before + 1, pruner_checked, "%llu");
    ASSERT_EQ_FMT(dfs_nodes_before, pruner_dfs_nodes, "%llu");    /* jamais appelée : aucun nœud compté */

    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* Échec d'envoi du paquet vivant : log + remise en local par put_to_server. */
TEST autoprune_step_add_error_reputs_locally(void)
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

    int fds[2]; pthread_t srv;
    es_attach_failing_server(&client, fds, &srv);

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 1;
    aposs->possibilities = calloc(1, sizeof(struct possibility_packet));
    make_empty_board(&aposs->possibilities[0]);
    client.aposs = aposs;

    /* §4.6b : voir le commentaire de autoprune_step_keeps_live_packet -- ce
       test porte sur l'échec réseau de add_possibility, pas sur la preuve de
       fermeture (qui court-circuiterait cet appel en prouvant la mort avant). */
    int saved_dfs_budget = pruner_dfs_budget;
    pruner_dfs_budget = 0;
    int saved = request;
    request = REQUEST_CONTINUE;
    es_silence_std();
    int cont = autoprune_step(&client);
    es_restore_std();
    request = saved;
    pruner_dfs_budget = saved_dfs_budget;

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* remis en local malgré l'échec */

    es_detach_failing_server(&client, fds, &srv);
    pthread_mutex_destroy(&client.works_mutex);
    drain_local();
    PASS();
}

/* --------------------------------------------------------------------------
 * autoprune_step : plateau complet dans le lot (solution trouvée par le pruner).
 *
 * Comportement attendu (aligné sur le pruner GPU) : record_solution enregistre
 * la solution (fichier + notification serveur) et, SANS --stop-on-solution, le
 * processus survit, le plateau complet n'est pas remis en circulation et la
 * boucle continue. L'ancien code appelait checkIfResultFound, qui sortait
 * inconditionnellement (EXIT_SUCCESS) : serveur jamais prévenu, lot jamais
 * acquitté, mode « continuer » ignoré.
 * ------------------------------------------------------------------------ */

/* Contexte partagé avec les fonctions-fils (copié par fork). */
static client_possibility_t ap_client;
static char ap_solution_dir[256];

/* Prépare ap_client avec un lot d'un seul paquet COMPLET (alloc == ETERN_PARTS,
   grille remplie de l'indice 0 : valide pour make_small_parts). */
static void ap_setup_complete_batch(void)
{
    memset(&ap_client, 0, sizeof ap_client);
    ap_client.compteur = 0;
    ap_client.map_part = make_free_map();
    ap_client.all_rotate_part = make_small_parts();
    pthread_mutex_init(&ap_client.works_mutex, NULL);
    ap_client.works = 1;

    array_possibility_packet *aposs = malloc(sizeof *aposs);
    aposs->size = 1;
    aposs->possibilities = calloc(1, sizeof(struct possibility_packet));
    aposs->possibilities[0].alloc = ETERN_PARTS; /* grille = zéros via calloc */
    ap_client.aposs = aposs;
}

/* Fils : stop_on_solution = 0. Nouvelle sémantique : autoprune_step revient
   (retour 1), le plateau complet n'est PAS remis en circulation et un fichier
   solution a été écrit. Sur l'ancien code, checkIfResultFound sortait
   EXIT_SUCCESS avant tout cela (le _exit(42) n'était jamais atteint). */
static void ap_child_solution_continue(void)
{
    if (chdir(ap_solution_dir) != 0) exit(97);
    stop_on_solution = 0;
    request = REQUEST_CONTINUE;
    int cont = autoprune_step(&ap_client);
    if (cont != 1) exit(43);
    if (datas_size() != 0ULL) exit(44);             /* pas de remise en stock */
    if (!es_has_solution_file(".")) exit(45);       /* solution enregistrée */
    exit(42); /* exit() (pas _exit) : flush de la couverture du fils */
}

/* Fils : stop_on_solution = 1. record_solution doit sortir EXIT_SUCCESS après
   l'enregistrement ; atteindre le exit(99) signifierait « pas d'arrêt ». */
static void ap_child_solution_stop(void)
{
    if (chdir(ap_solution_dir) != 0) exit(97);
    stop_on_solution = 1;
    request = REQUEST_CONTINUE;
    autoprune_step(&ap_client);
    exit(99);
}

/* Sans --stop-on-solution : le pruner survit à une solution et continue. */
TEST autoprune_step_complete_board_records_solution_and_continues(void)
{
    drain_local();
    ensure_counters();
    ap_setup_complete_batch();
    strcpy(ap_solution_dir, "/tmp/etii_ap_sol_XXXXXX");
    ASSERT(mkdtemp(ap_solution_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(ap_child_solution_continue, &pid);

    es_unlink_solutions(ap_solution_dir);
    rmdir(ap_solution_dir);
    free_array_possibility_packet(ap_client.aposs); /* copie du parent */
    pthread_mutex_destroy(&ap_client.works_mutex);

    ASSERT_EQ_FMT(42, code, "%d");
    PASS();
}

/* Avec --stop-on-solution : record_solution enregistre PUIS sort EXIT_SUCCESS. */
TEST autoprune_step_complete_board_stop_on_solution_exits(void)
{
    drain_local();
    ensure_counters();
    ap_setup_complete_batch();
    strcpy(ap_solution_dir, "/tmp/etii_ap_sos_XXXXXX");
    ASSERT(mkdtemp(ap_solution_dir) != NULL);

    pid_t pid = 0;
    int code = run_in_fork(ap_child_solution_stop, &pid);

    int had_solution = es_has_solution_file(ap_solution_dir);
    es_unlink_solutions(ap_solution_dir);
    rmdir(ap_solution_dir);
    free_array_possibility_packet(ap_client.aposs); /* copie du parent */
    pthread_mutex_destroy(&ap_client.works_mutex);

    ASSERT_EQ_FMT(EXIT_SUCCESS, code, "%d");
    ASSERT(had_solution); /* la solution est écrite AVANT l'exit */
    PASS();
}

/* ==========================================================================
 * autosearch / autoprune : les enveloppes de thread elles-mêmes.
 *
 * `autosearch_step`/`autoprune_step` (le corps de boucle) sont déjà testés en
 * détail ci-dessus. Les enveloppes `while (…_step(...)) usleep(...);` ne
 * l'étaient pas : REQUEST_STOP avant l'appel fait sortir la boucle dès le
 * premier tour (0 itération), ce qui exerce sans risque l'entrée/sortie de la
 * fonction (allocation de idParts, libération de delegate_buf) — comme déjà
 * fait pour control_thread/feed_thread_aposs (etii_client.c). Appel direct,
 * pas de thread réel : aucune des deux ne bloque tant que REQUEST_STOP est
 * déjà positionné.
 * ========================================================================== */

TEST autosearch_stops_immediately_on_request_stop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    pthread_mutex_init(&client.works_mutex, NULL);
    pthread_mutex_init(&client.socket_mutex, NULL);
    client.socket_id = -1;

    void *ret = autosearch(&client);
    ASSERT_EQ(NULL, ret);

    pthread_mutex_destroy(&client.works_mutex);
    pthread_mutex_destroy(&client.socket_mutex);
    request = saved_req;
    PASS();
}

/* delegate_buf déjà alloué (délégation antérieure) : autosearch doit le
 * libérer à la sortie, quel que soit le nombre de tours effectués. */
TEST autosearch_frees_delegate_buffer_on_exit(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    pthread_mutex_init(&client.works_mutex, NULL);
    pthread_mutex_init(&client.socket_mutex, NULL);
    client.socket_id = -1;
    client.delegate_buf = malloc(sizeof(struct possibility_packet) * 4);
    client.delegate_buf_capacity = 4;

    void *ret = autosearch(&client);
    ASSERT_EQ(NULL, ret);
    ASSERT_EQ(NULL, client.delegate_buf);
    ASSERT_EQ_FMT(0, client.delegate_buf_capacity, "%d");

    pthread_mutex_destroy(&client.works_mutex);
    pthread_mutex_destroy(&client.socket_mutex);
    request = saved_req;
    PASS();
}

TEST autoprune_stops_immediately_on_request_stop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    client_possibility_t client;
    memset(&client, 0, sizeof client);
    pthread_mutex_init(&client.works_mutex, NULL);
    pthread_mutex_init(&client.socket_mutex, NULL);
    client.socket_id = -1;

    void *ret = autoprune(&client);
    ASSERT_EQ(NULL, ret);

    pthread_mutex_destroy(&client.works_mutex);
    pthread_mutex_destroy(&client.socket_mutex);
    request = saved_req;
    PASS();
}

SUITE(etii_search_suite)
{
    RUN_TEST(delegate_noop_below_threshold);
    RUN_TEST(delegate_moves_excess_to_local_pool);
    RUN_TEST(delegate_file_error_reputs_locally);
    RUN_TEST(bt_init_constraints_empty_board);
    RUN_TEST(bt_propagate_matches_full_recompute);
    RUN_TEST(bt_propagate_covers_border_guards);
    RUN_TEST(bt_count_pending_counts_remaining_free);
    RUN_TEST(bt_count_pending_skips_no_decision_and_zero_id);
#if FORWARD_CHECK_K > 0
    RUN_TEST(bt_forward_check_detects_dead_cells);
    RUN_TEST(bt_forward_check_skips_prefilled_and_zero_id);
    RUN_TEST(bt_forward_check_inspects_at_most_geometric_neighbors);
    RUN_TEST(bt_forward_check_same_verdict_with_and_without_packed_index);
    RUN_TEST(bt_forward_check_same_verdict_with_and_without_id_mask);
    RUN_TEST(bt_mask_cache_stays_in_lockstep_with_constraints);
    RUN_TEST(bt_forward_check_singleton_conflict_detects_matching_ids);
    RUN_TEST(bt_forward_check_singleton_conflict_ignores_distinct_ids);
    RUN_TEST(bt_forward_check_singleton_conflict_disabled_by_default);
#endif
    RUN_TEST(mrv_choose_cell_picks_the_most_constrained_cell);
    RUN_TEST(mrv_choose_cell_detects_dead_cell);
    RUN_TEST(bt_frontier_init_matches_constraint_cache);
    RUN_TEST(bt_frontier_place_undo_matches_full_recompute);
    RUN_TEST(mrv_choose_cell_matches_full_scan_reference);
    RUN_TEST(mrv_choose_cell_breaks_ties_by_constrained_sides);
    RUN_TEST(mrv_choose_cell_full_board_and_unconstrained_fallback);
    RUN_TEST(bt_mask_cache_uses_zero_mask_for_empty_bucket_in_fast_mode);
    RUN_TEST(mrv_choose_cell_fast_matches_generic_on_real_map);
#if FORWARD_CHECK_K > 0
    RUN_TEST(bt_forward_check_fast_same_verdict_as_generic);
#endif
    RUN_TEST(bt_materialize_pending_orders_deepest_first);
    RUN_TEST(bt_materialize_pending_respects_max_out);
    RUN_TEST(bt_materialize_pending_dynamic_order_recomputes_alloc);
    RUN_TEST(bt_materialize_skips_no_decision_level_and_zero_id);
#if FORWARD_CHECK_K > 0
    RUN_TEST(bt_materialize_dead_map_produces_nothing);
    RUN_TEST(bt_delegate_dead_map_sends_nothing);
#endif
    RUN_TEST(bt_materialize_sibling_solution_records_and_continues);
    RUN_TEST(bt_delegate_noop_below_threshold);
    RUN_TEST(bt_delegate_moves_excess_to_local);
    RUN_TEST(bt_delegate_reuses_and_grows_buffer);
    RUN_TEST(bt_delegate_error_keeps_stack);
    RUN_TEST(delegation_quota_above_threshold_ignores_hunger);
    RUN_TEST(delegation_quota_below_threshold_needs_hunger);
    RUN_TEST(delegation_quota_anticipates_capped_at_half);
    RUN_TEST(abandon_shallow_root_disabled_never_triggers);
    RUN_TEST(abandon_shallow_root_deep_root_never_triggers);
    RUN_TEST(abandon_shallow_root_not_deep_enough_yet);
    RUN_TEST(abandon_shallow_root_triggers_at_boundary);
    RUN_TEST(bt_delegate_hunger_moves_below_threshold);
    RUN_TEST(bt_min_pending_depth_finds_shallowest_level);
    RUN_TEST(bt_min_pending_depth_skips_exhausted_shallow_level);
    RUN_TEST(bt_min_pending_depth_returns_minus_one_when_nothing_pending);
    RUN_TEST(bt_flush_pending_sends_all_plus_current);
    RUN_TEST(bt_abandon_shallow_root_flushes_and_counts);
    RUN_TEST(bt_flush_error_reputs_locally);
    RUN_TEST(search_backtracking_stop_flushes_and_returns_one);
    RUN_TEST(search_backtracking_prefilled_cells_no_decision);
    RUN_TEST(search_backtracking_explores_and_exhausts);
    RUN_TEST(search_backtracking_budgeted_closes_when_budget_suffices);
    RUN_TEST(search_backtracking_budgeted_returns_budget_when_insufficient);
    RUN_TEST(search_backtracking_budgeted_stop_returns_stopped_without_flush);
    RUN_TEST(search_backtracking_pause_waits_then_stops);
    RUN_TEST(requeue_unprocessed_packets_routes_tail_locally);
    RUN_TEST(requeue_error_reputs_locally);
    RUN_TEST(autosearch_step_stop_requeues_and_returns_zero);
    RUN_TEST(autosearch_step_continue_returns_one);
    RUN_TEST(autosearch_step_waits_until_fed);
    RUN_TEST(autoprune_step_waits_until_fed);
    RUN_TEST(autosearch_loops_once_then_stops);
    RUN_TEST(autoprune_loops_once_then_stops);
    RUN_TEST(autoprune_step_stop_requeues_all_and_returns_zero);
    RUN_TEST(autoprune_step_continue_empty_returns_one);
    RUN_TEST(autoprune_step_keeps_live_packet);
    RUN_TEST(autoprune_step_pauses_then_resumes);
    RUN_TEST(autoprune_step_removes_dead_packet);
    RUN_TEST(autoprune_step_keeps_checked_dead_packet);
    RUN_TEST(autoprune_step_dfs_budget_closes_possibility);
    RUN_TEST(autoprune_step_dfs_budget_too_small_keeps_possibility);
    RUN_TEST(autoprune_step_dfs_budget_disabled_skips_dfs);
    RUN_TEST(autoprune_step_add_error_reputs_locally);
    RUN_TEST(autoprune_step_complete_board_records_solution_and_continues);
    RUN_TEST(autoprune_step_complete_board_stop_on_solution_exits);
#if ETERN_PARTS == 16
    RUN_TEST(search_backtracking_solves_4x4_and_returns_zero);
    RUN_TEST(search_backtracking_budgeted_closes_4x4);
    RUN_TEST(search_backtracking_stop_on_solution_exits_success);
    RUN_TEST(search_backtracking_mrv_delegation_preserves_solution_count);
    RUN_TEST(search_backtracking_singleton_conflict_check_preserves_solution_count);
#endif

    RUN_TEST(autosearch_stops_immediately_on_request_stop);
    RUN_TEST(autosearch_frees_delegate_buffer_on_exit);
    RUN_TEST(autoprune_stops_immediately_on_request_stop);
}
