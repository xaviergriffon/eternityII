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
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/part.h"
#include "net/etii_protocol.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

/* Helpers internes de datamanager.c non exposés dans datamanager.h. */
int put_to_server(client_possibility_t *client_possibility, array_possibility_packet *possibilities);
unsigned long long count_combinations(unsigned long long x);
int check_and_connect_to_server(client_possibility_t *client_possibility);

/* Verrou global des files + variantes « nolock » (caller doit tenir le verrou). */
void lock_all_file(void);
void unlock_all_file(void);
unsigned long long regroup_datas_nolock(void);
int split_datas_nolock(int nbsplit);

/* Affichage de progression de check_duplicate (en prod, atteint uniquement après
   30 s d'attente d'un thread) + ses compteurs globaux (tableaux de nbDuplicateThread == 8). */
void print_duplicate_activity(unsigned long long dataSize, unsigned long long nbCombinations);
extern unsigned long long duplicateCount[];
extern unsigned long long duplicateErrors[];
extern unsigned long long duplicateFinish[];
extern unsigned long long duplicateAnalyzed[];

/* Globales de static_variables.c utilisées par les tests du chemin connexion. */
extern int SERVER_PORT;
extern volatile int request;
extern volatile uint16_t max_result;

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

/* remove_possibilities_with_no_next : un packet complet (alloc == ETERN_PARTS)
   doit être traité comme solution et retiré de la file — sans appeler exit().
   Régression : avant le correctif, possibility_all_has_a_next appelait
   checkIfResultFound → exit() en contexte serveur, tuant le processus. */
TEST remove_no_next_handles_complete_solution(void)
{
    drain_all();
    /* map minimale (non utilisée car alloc == ETERN_PARTS → boucle vide) */
    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = ETERN_PARTS; /* board complet */
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    /* stop_on_solution doit être 0 pour que la fonction ne call pas exit() */
    extern int stop_on_solution;
    int saved_sos = stop_on_solution;
    stop_on_solution = 0;

    silence_std();
    remove_possibilities_with_no_next(map, &rp);
    restore_std();

    stop_on_solution = saved_sos;

    /* Le packet complet doit avoir été retiré (traité comme solution, non redistributé) */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    /* Nettoyer les fichiers solution éventuellement créés (./solution_server_<pid>_*) */
    char pattern[64];
    snprintf(pattern, sizeof pattern, "./solution_server_%i_*", (int)getpid());
    glob_t gp;
    if (glob(pattern, 0, NULL, &gp) == 0) {
        for (size_t i = 0; i < gp.gl_pathc; i++)
            unlink(gp.gl_pathv[i]);
        globfree(&gp);
    }

    free_bigarray(map);
    drain_all();
    PASS();
}

/* send_solution : en mode local (pas de serveur), doit renvoyer -1 sans toucher
   au réseau ni planter — la solution reste sauvegardée localement par ailleurs.
   Régression liée au signalement des solutions au serveur. */
TEST send_solution_without_client_is_local_noop(void)
{
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;
    ASSERT_EQ_FMT(-1, send_solution(NULL, &pkt), "%d");
    PASS();
}

TEST send_solution_without_server_configured_returns_error(void)
{
    set_server_ip(NULL);                 /* aucun serveur configuré (mode test/auto) */
    client_possibility_t client;
    memset(&client, 0, sizeof client);
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;
    /* Le garde server_ip==NULL renvoie -1 AVANT tout lock/connexion : le client
       zéro-initialisé (mutex non initialisé) n'est jamais déréférencé. */
    ASSERT_EQ_FMT(-1, send_solution(&client, &pkt), "%d");
    PASS();
}

/* ==========================================================================
 * Tests du chemin réseau : scroll_from_server, send_possibility_analysed,
 * send_solution et put_to_server via socketpair
 *
 * On pré-remplit cp.socket_id avec l'extrémité client d'un socketpair AF_UNIX,
 * ce qui court-circuite create_tcp_client().  Un thread pthread joue le rôle
 * d'un mini-serveur sur l'extrémité serveur : il répond au handshake de
 * is_connected (INST_TEST_CONNECTED → INST_TEST_CONNECTED) puis au protocole
 * de la fonction testée.  Le thread est joint avant la fin du test pour
 * éliminer toute race condition.
 * ========================================================================== */

/* Lecture robuste de exactement len octets (boucle recv côté mini-serveur). */
static void recv_exact_sv(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = recv(fd, (char *)buf + done, len - done, 0);
        if (r <= 0) break;
        done += (size_t)r;
    }
}

/* Mini-serveur scroll_from_server — renvoie un paquet :
 *   1. is_connected
 *   2. recv INST_GET → send possibility_packet
 */
static void *mini_srv_get_packet(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 7;
    send(fd, &pkt, sizeof pkt, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur scroll_from_server — aucune possibilité (INST_NULL) :
 *   1. is_connected
 *   2. recv INST_GET → send INST_NULL (1 octet)
 */
static void *mini_srv_get_null(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_GET */
    b = INST_NULL;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur send_possibility_analysed — acquittement :
 *   1. is_connected
 *   2. recv INST_POSSIBILITY_ANALYSED_BATCH + int32 M + M packets → INST_CONSIDERED
 *   Répète jusqu'à ce que le client n'envoie plus rien (connexion fermée côté test).
 */
static void *mini_srv_analysed_ok(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    /* Drain all batches until the client stops sending. */
    while (recv(fd, &b, 1, 0) == 1) {
        /* b == INST_POSSIBILITY_ANALYSED_BATCH */
        int32_t m = 0;
        recv_exact_sv(fd, &m, sizeof m);
        for (int32_t i = 0; i < m; i++) {
            struct possibility_packet pkt;
            recv_exact_sv(fd, &pkt, sizeof pkt);
        }
        b = INST_CONSIDERED;
        send(fd, &b, 1, 0);
    }
    close(fd);
    return NULL;
}

/* Mini-serveur put_to_server — mauvais ACK pour le premier paquet :
 *   pkt[0] → INST_NULL (bad ack, non-fatal : item remis en local, boucle continue)
 *   pkt[1] → INST_CONSIDERED
 *   Résultat : rc=0 mais datas_size()==1 (pkt[0] dans le stock local).
 */
static void *mini_srv_put_bad_ack(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_ADD pkt[0] */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_NULL;                                    /* ACK invalide, non-fatal */
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_ADD pkt[1] */
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_CONSIDERED;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur send_solution — succès :
 *   1. is_connected : reçoit INST_TEST_CONNECTED → répond INST_TEST_CONNECTED
 *   2. reçoit INST_SOLUTION + possibility_packet → répond INST_CONSIDERED
 */
static void *mini_srv_solution_ok(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_SOLUTION */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_CONSIDERED;
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur send_solution — rejet :
 *   Même handshake, puis répond INST_NULL au lieu de INST_CONSIDERED.
 */
static void *mini_srv_solution_reject(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_SOLUTION */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    b = INST_NULL;                                    /* acquittement refusé */
    send(fd, &b, 1, 0);
    close(fd);
    return NULL;
}

/* Mini-serveur put_to_server — succès (2 paquets) :
 *   handshake + INST_ADD + pkt → INST_CONSIDERED, deux fois.
 */
static void *mini_srv_put_ok(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    for (int i = 0; i < 2; i++) {
        recv(fd, &b, 1, 0);                          /* INST_ADD */
        struct possibility_packet pkt;
        recv_exact_sv(fd, &pkt, sizeof pkt);
        b = INST_CONSIDERED;
        send(fd, &b, 1, 0);
    }
    close(fd);
    return NULL;
}

/* Mini-serveur put_to_server — connexion perdue après le premier INST_ADD :
 *   handshake + reçoit INST_ADD + pkt[0] → ferme le socket sans ACK.
 *   Le client voit INST_END (recv == 0), remet pkt[1] en local et renvoie -1.
 */
static void *mini_srv_put_drop(void *arg)
{
    int fd = *(int *)arg;
    int8_t b;
    recv(fd, &b, 1, 0);
    b = INST_TEST_CONNECTED;
    send(fd, &b, 1, 0);
    recv(fd, &b, 1, 0);                              /* INST_ADD */
    struct possibility_packet pkt;
    recv_exact_sv(fd, &pkt, sizeof pkt);
    close(fd);                                        /* fermeture sans ACK */
    return NULL;
}

/* Initialise un client_possibility_t minimal avec un socket préexistant. */
static void init_cp_with_socket(client_possibility_t *cp, int sock_fd)
{
    memset(cp, 0, sizeof *cp);
    pthread_mutex_init(&cp->socket_mutex, NULL);
    cp->socket_id = sock_fd;
    cp->id = 0;
}

/* scroll_from_server : le serveur répond avec un paquet.
 * Exercé via get_last_possibility avec pool local vide + server_ip configuré. */
TEST scroll_from_server_returns_packet(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_packet, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(1, r->size, "%d");
    ASSERT_EQ_FMT(7, (int)r->possibilities[0].alloc, "%d");
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* scroll_from_server : le serveur répond INST_NULL (stock vide côté serveur). */
TEST scroll_from_server_returns_null(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_get_null, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    array_possibility_packet *r = get_last_possibility(&cp, 1);
    restore_std();

    ASSERT(r != NULL);
    ASSERT_EQ_FMT(0, r->size, "%d"); /* serveur n'a rien donné */
    free_array_possibility_packet(r);

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* send_possibility_analysed (chemin réseau) : envoie un batch INST_POSSIBILITY_ANALYSED_BATCH,
 * le serveur ACK → la file analysed[0] est vidée. */
TEST send_possibility_analysed_success(void)
{
    drain_all();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_analysed_ok, &fds[1]);

    /* Ajoute 1 paquet dans file_possibility_analysed[0]. */
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 3;
    add_possibility_analysed(&pk, 0);
    ASSERT_EQ_FMT(1ULL, file_analysed_size(0), "%llu");

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    silence_std();
    send_possibility_analysed(&cp);
    restore_std();

    ASSERT_EQ_FMT(0ULL, file_analysed_size(0), "%llu"); /* file vidée */

    /* Fermer fds[0] pour débloquer le mini-serveur (son recv retourne 0). */
    close(fds[0]);
    pthread_join(srv, NULL);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_all();
    PASS();
}

/* put_to_server : ACK invalide non-fatal pour pkt[0] (INST_NULL ≠ INST_CONSIDERED et ≠ INST_END).
 * L'item est remis en stock local et la boucle CONTINUE pour pkt[1] → rc=0 mais datas_size()==1. */
TEST put_to_server_bad_ack_non_fatal(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_bad_ack, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 3;
    pkts[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pkts };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");          /* pas de connection_lost : rc=0 */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* pkt[0] remis en local */

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

TEST send_solution_success(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_solution_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;

    silence_std();
    int rc = send_solution(&cp, &pkt);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);                /* non fermé par send_solution en cas de succès */
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

TEST send_solution_server_rejects(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_solution_reject, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = ETERN_PARTS;

    silence_std();
    int rc = send_solution(&cp, &pkt);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

TEST put_to_server_success(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_ok, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 3;
    pkts[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pkts };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu"); /* aucune possibilité remise en local */

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* put_to_server : connexion perdue après le premier INST_ADD.
 * Le mini-serveur ferme le socket sans ACK → le client reçoit INST_END pour
 * l'acquittement du paquet 0 → remet pkt[1] en local et renvoie -1.
 * Vérifie : retour -1 et datas_size() > 0 (pkt[1] remis en stock local). */
TEST put_to_server_connection_lost(void)
{
    drain_datamanager();

    int fds[2];
    ASSERT_EQ_FMT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds), "%d");

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_put_drop, &fds[1]);

    client_possibility_t cp;
    init_cp_with_socket(&cp, fds[0]);
    set_server_ip("127.0.0.1");

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 3;
    pkts[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pkts };

    silence_std();
    int rc = put_to_server(&cp, &arr);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    ASSERT(datas_size() > 0); /* pkts[1] doit avoir été remis en stock local */

    pthread_join(srv, NULL);
    set_server_ip(NULL);
    close(fds[0]);            /* put_to_server ne ferme pas le socket en cas d'erreur */
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* ==========================================================================
 * Tests de check_and_connect_to_server : chemin « socket_id == -1 »
 *
 * Approche : mini-serveur TCP local sur 127.0.0.1:0 (port éphémère).
 * SERVER_PORT (extern int) est écrit avec le port assigné par bind() ; aucune
 * modification du Makefile n'est nécessaire — create_tcp_client() est exercé
 * tel quel.  Pour le cas « connexion refusée » on règle request=REQUEST_STOP
 * avant l'appel afin que la boucle de reconnexion s'arrête après la première
 * tentative (RECONNECT_SHOULD_ABORT() court-circuite le sleep de 1 s × 10).
 * ========================================================================== */

/* Ouvre un socket TCP en écoute sur 127.0.0.1:0, écrit le port dans *port et
 * renvoie le fd.  Retourne -1 en cas d'échec. */
static int make_local_tcp_server(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 1) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Argument partagé entre le test et le thread mini-serveur de handshake. */
typedef struct {
    int     srv_fd;     /* fd d'écoute à passer à accept() */
    int8_t  response;   /* octet à renvoyer après avoir reçu INST_CHECK_VERSION */
} handshake_srv_arg_t;

/* Thread mini-serveur : accepte une connexion, lit INST_CHECK_VERSION + version,
 * renvoie response.  Simule exactement ce que etii_server fait au handshake. */
static void *mini_srv_handshake(void *arg)
{
    handshake_srv_arg_t *a = arg;
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    int cli_fd = accept(a->srv_fd, (struct sockaddr *)&cli, &clen);
    if (cli_fd < 0) return NULL;
    int8_t b;
    recv(cli_fd, &b, 1, 0);                       /* INST_CHECK_VERSION */
    int ver;
    recv_exact_sv(cli_fd, &ver, sizeof(ver));      /* numéro de version */
    send(cli_fd, &a->response, 1, 0);
    close(cli_fd);
    return NULL;
}

/* check_and_connect_to_server : handshake accepté (INST_SUPPORTED_VERSION).
 * Vérifie que la fonction retourne un fd >= 0 et met à jour cp.socket_id. */
TEST connect_and_handshake_ok(void)
{
    drain_datamanager();

    int port;
    int srv_fd = make_local_tcp_server(&port);
    ASSERT(srv_fd >= 0);

    handshake_srv_arg_t ha = { .srv_fd = srv_fd, .response = INST_SUPPORTED_VERSION };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake, &ha);

    set_server_ip("127.0.0.1");
    SERVER_PORT = port;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT(rc >= 0);
    ASSERT_EQ_FMT(rc, cp.socket_id, "%d");

    pthread_join(srv, NULL);
    close(srv_fd);
    if (cp.socket_id >= 0) close(cp.socket_id);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* check_and_connect_to_server : version refusée (INST_UNSUPPORTED_VERSION).
 * Vérifie retour == -1 et request == REQUEST_STOP. */
TEST connect_handshake_version_rejected(void)
{
    drain_datamanager();

    int saved_request = request;
    request = REQUEST_CONTINUE;

    int port;
    int srv_fd = make_local_tcp_server(&port);
    ASSERT(srv_fd >= 0);

    handshake_srv_arg_t ha = { .srv_fd = srv_fd, .response = INST_UNSUPPORTED_VERSION };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake, &ha);

    set_server_ip("127.0.0.1");
    SERVER_PORT = port;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    ASSERT_EQ_FMT(REQUEST_STOP, request, "%d");

    request = saved_request;
    pthread_join(srv, NULL);
    close(srv_fd);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* check_and_connect_to_server : réponse de handshake non reconnue (INST_END →
 * HANDSHAKE_RETRY).  Vérifie retour == -1 sans modifier request. */
TEST connect_handshake_retry(void)
{
    drain_datamanager();

    int saved_request = request;
    request = REQUEST_CONTINUE;

    int port;
    int srv_fd = make_local_tcp_server(&port);
    ASSERT(srv_fd >= 0);

    handshake_srv_arg_t ha = { .srv_fd = srv_fd, .response = INST_END };
    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake, &ha);

    set_server_ip("127.0.0.1");
    SERVER_PORT = port;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");
    /* HANDSHAKE_RETRY ne doit PAS positionner REQUEST_STOP. */
    ASSERT_EQ_FMT(REQUEST_CONTINUE, request, "%d");

    request = saved_request;
    pthread_join(srv, NULL);
    close(srv_fd);
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* check_and_connect_to_server : create_tcp_client échoue (rien n'écoute).
 * On règle request=REQUEST_STOP pour que la boucle de reconnexion s'arrête
 * immédiatement après la première tentative (pas de sleep de 10 × 100 ms).
 * Vérifie retour == -1. */
TEST connect_create_tcp_client_fails(void)
{
    drain_datamanager();

    /* Obtient un port garanti libre : bind sans listen, ferme aussitôt. */
    int tmp = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(tmp >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(tmp, (struct sockaddr *)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(tmp, (struct sockaddr *)&addr, &len);
    int free_port = ntohs(addr.sin_port);
    close(tmp);   /* personne n'écoute sur ce port */

    set_server_ip("127.0.0.1");
    SERVER_PORT = free_port;

    /* REQUEST_STOP court-circuite la boucle de retry dès la 1re tentative. */
    int saved_request = request;
    request = REQUEST_STOP;

    client_possibility_t cp;
    memset(&cp, 0, sizeof(cp));
    pthread_mutex_init(&cp.socket_mutex, NULL);
    cp.socket_id = -1;

    silence_std();
    int rc = check_and_connect_to_server(&cp);
    restore_std();

    ASSERT_EQ_FMT(-1, rc, "%d");

    request = saved_request;
    set_server_ip(NULL);
    pthread_mutex_destroy(&cp.socket_mutex);
    drain_datamanager();
    PASS();
}

/* ==========================================================================
 * Vérification d'intégrité des files (check_file / check_files / check_datas)
 *
 * check_one_file (static) est exercé transitivement par check_file/check_files.
 * Ces fonctions ne dépendent que de la cohérence interne des structures File ;
 * un stock construit via add_possibility est forcément cohérent -> 0.
 * ========================================================================== */

/* check_files / check_file : stock vide et stock cohérent -> 0 (pas d'incohérence). */
TEST check_files_reports_consistent_stock(void)
{
    drain_all();
    /* Stock vide : toutes les files sont cohérentes (size==0, start/end==NULL). */
    ASSERT_EQ_FMT(0, check_files(), "%d");
    ASSERT_EQ_FMT(0, check_file(0), "%d");

    /* Stock peuplé via add_possibility : la liste chaînée reste cohérente. */
    int allocs[] = { 1, 2, 3, 4, 5 };
    add_packets(allocs, 5);
    ASSERT_EQ_FMT(0, check_files(), "%d"); /* aucune incohérence détectée */
    ASSERT_EQ_FMT(0, check_file(0), "%d");

    drain_all();
    PASS();
}

/* check_one_file : détecte chaque incohérence structurelle d'une File. Exposée
 * exprès (non statique) car aucune API publique ne permet de corrompre les pools
 * internes — on monte donc des File/Element à la main, sur la pile (la fonction
 * ne fait que LIRE, elle ne libère rien). Les log_info sont mis en sourdine.
 * Couvre les branches restées mortes : size==0+start/end résiduels, value NULL,
 * chaîne plus longue que size, end/size désynchronisés, et le retour 0. */
TEST check_one_file_flags_each_inconsistency(void)
{
    int dummy = 42; /* valeur non NULL pour les éléments « sains » */

    silence_std();

    /* (1) size==0 mais start != NULL (start résiduel). */
    Element e1 = { .value = &dummy, .previous = NULL, .next = NULL };
    File f1 = { .start = &e1, .end = NULL, .size = 0, .sizeofvalue = sizeof dummy };
    int r1 = check_one_file(&f1, 0, "test");

    /* (2) size==0 mais end != NULL (end résiduel). */
    Element e2 = { .value = &dummy, .previous = NULL, .next = NULL };
    File f2 = { .start = NULL, .end = &e2, .size = 0, .sizeofvalue = sizeof dummy };
    int r2 = check_one_file(&f2, 1, "test");

    /* (3) un élément unique dont value == NULL. */
    Element e3 = { .value = NULL, .previous = NULL, .next = NULL };
    File f3 = { .start = &e3, .end = &e3, .size = 1, .sizeofvalue = sizeof dummy };
    int r3 = check_one_file(&f3, 2, "test");

    /* (4) size annoncée (1) < longueur réelle (2) -> currElement non NULL en fin
       de boucle (chaîne plus longue que size). */
    Element a = { .value = &dummy, .previous = NULL, .next = NULL };
    Element b = { .value = &dummy, .previous = &a,   .next = NULL };
    a.next = &b;
    File f4 = { .start = &a, .end = &b, .size = 1, .sizeofvalue = sizeof dummy };
    int r4 = check_one_file(&f4, 3, "test");

    /* (5) taille cohérente (1 élément) mais pointeur end faux -> mismatch end. */
    Element c = { .value = &dummy, .previous = NULL, .next = NULL };
    File f5 = { .start = &c, .end = NULL, .size = 1, .sizeofvalue = sizeof dummy };
    int r5 = check_one_file(&f5, 4, "test");

    /* (0) File parfaitement cohérente -> 0 (retour OK exercé directement). */
    Element ok = { .value = &dummy, .previous = NULL, .next = NULL };
    File f0 = { .start = &ok, .end = &ok, .size = 1, .sizeofvalue = sizeof dummy };
    int r0 = check_one_file(&f0, 5, "test");

    restore_std();

    ASSERT_EQ_FMT(-1, r1, "%d"); /* start résiduel             */
    ASSERT_EQ_FMT(-1, r2, "%d"); /* end résiduel               */
    ASSERT_EQ_FMT(-1, r3, "%d"); /* value NULL                 */
    ASSERT_EQ_FMT(-1, r4, "%d"); /* chaîne > size              */
    ASSERT_EQ_FMT(-1, r5, "%d"); /* end/size désynchronisés    */
    ASSERT_EQ_FMT(0,  r0, "%d"); /* File cohérente             */
    PASS();
}

/* check_datas : sur un stock vide, lit le CSV (présent à la racine du dépôt),
 * ne trouve aucune possibilité -> 0 erreur. */
TEST check_datas_empty_stock_is_ok(void)
{
    drain_all();
    silence_std();
    int rc = check_datas();
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");
    drain_all();
    PASS();
}

/* check_datas : une possibilité manifestement invalide (alloc > ETERN_PARTS)
 * est détectée par check_possibility (-4) -> check_datas renvoie -1.
 * Indépendant de la taille du puzzle (le garde alloc précède tout le reste). */
TEST check_datas_flags_invalid_packet(void)
{
    drain_all();
    int saved_max = max_result;

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = (uint16_t)(ETERN_PARTS + 1); /* > ETERN_PARTS -> check_possibility renvoie -4 */
    pk.checked = 0;
    array_possibility_packet arr = { .size = 1, .possibilities = &pk };
    add_possibility(NULL, &arr);

    silence_std();
    int rc = check_datas();
    restore_std();
    ASSERT_EQ_FMT(-1, rc, "%d"); /* au moins une possibilité invalide */

    max_result = saved_max; /* add_possibility a pu monter max_result, on le restaure */
    drain_all();
    PASS();
}

/* ==========================================================================
 * Tri multi-thread (sort_descending_mthread -> sortdmthread -> split_datas_nolock)
 * et détection de doublons (check_duplicate -> *_thread).
 * ========================================================================== */

/* sort_descending_mthread : variante parallèle du tri. Vérifie que le total
 * est préservé et que les files restent cohérentes après les passes de
 * split/regroup/tri exécutées par les threads. */
TEST sort_descending_mthread_preserves_count(void)
{
    drain_all();
    int allocs[] = { 5, 2, 8, 1, 6, 3, 9, 4 };
    add_packets(allocs, 8);
    ASSERT_EQ_FMT(8ULL, datas_size(), "%llu");

    silence_std();
    int rc = sort_descending_mthread();
    restore_std();

    ASSERT_EQ_FMT(0, rc, "%d");
    ASSERT_EQ_FMT(8ULL, datas_size(), "%llu"); /* total inchangé */
    ASSERT_EQ_FMT(0, check_files(), "%d");      /* files toujours cohérentes */

    drain_all();
    PASS();
}

/* regroup_datas_nolock / split_datas_nolock : variantes « caller tient le verrou ».
 * On encadre l'appel par lock_all_file()/unlock_all_file() comme le ferait le code
 * de production, et on vérifie la préservation du total. */
TEST regroup_split_nolock_preserve_count(void)
{
    drain_all();
    int allocs[10];
    for (int i = 0; i < 10; i++) allocs[i] = i + 1;
    add_packets(allocs, 10);

    silence_std();
    lock_all_file();
    split_datas_nolock(NB_FILE_POSSIBILITY);   /* réparti sur toutes les files */
    regroup_datas_nolock();                    /* re-consolidé dans la file 0 */
    unlock_all_file();
    restore_std();

    ASSERT_EQ_FMT(10ULL, datas_size(), "%llu"); /* total préservé */
    ASSERT_EQ_FMT(10ULL, file_size(0), "%llu"); /* tout regroupé dans la file 0 */
    ASSERT_EQ_FMT(0, check_files(), "%d");

    drain_all();
    PASS();
}

/* check_duplicate : sur un stock vide, nbCombinations == 0 -> aucun thread lancé.
 * Régression : avant le correctif, la boucle de jointure attendait les 8 threads
 * (duplicateFinish[t]==1) alors qu'aucun n'était lancé -> blocage infini. Désormais
 * elle ne joint que les `spawned` threads réellement créés -> retour immédiat 0. */
TEST check_duplicate_empty_stock_returns_immediately(void)
{
    drain_all();
    silence_std();
    int rc = check_duplicate();
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d");
    drain_all();
    PASS();
}

/* check_duplicate : petit stock (3 possibilités) -> moins de nbDuplicateThread (8)
 * threads lancés. Régression du même blocage : la jointure attendait des threads
 * jamais créés. Les 3 possibilités sont deux à deux distinctes (allocs différents
 * + première case du parcours différente) -> aucun doublon -> 0. */
TEST check_duplicate_small_stock_no_error(void)
{
    drain_all();
    struct possibility_packet pks[3];
    memset(pks, 0, sizeof pks);
    for (int i = 0; i < 3; i++) {
        pks[i].alloc = (uint16_t)(i + 1);                  /* allocs distincts */
        pks[i].grid[dirx[0]][diry[0]] = (int16_t)(i + 1);  /* préfixes divergents */
    }
    array_possibility_packet arr = { .size = 3, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(3ULL, datas_size(), "%llu");

    silence_std();
    int rc = check_duplicate();
    restore_std();
    ASSERT_EQ_FMT(0, rc, "%d"); /* aucun doublon, et surtout : pas de blocage */

    drain_all();
    PASS();
}

/* import_json : importe une possibilité depuis la chaîne JSON CODÉE EN DUR du
 * module (et non depuis STDIN). Elle draine d'abord toutes les files puis ajoute
 * exactement 1 possibilité au stock local (add_possibility, server_ip == NULL).
 * Le plateau JSON est 16×16 mais compute_grid borne l'écriture à ETERN_SIZE :
 * l'import est donc sûr aussi bien en build 256 qu'en build 16. Le flag `checked`
 * du paquet n'est pas initialisé par read_from_json, mais put_to_pool route
 * chaque paquet dans exactement un pool et datas_size() somme les deux -> le
 * total vaut 1 quelle que soit la valeur résiduelle. */
TEST import_json_loads_single_possibility(void)
{
    drain_all();
    silence_std();              /* read_from_json/compute_grid impriment la trace de parsing */
    int rc = import_json();
    restore_std();
    ASSERT_EQ_FMT(1, rc, "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");
    drain_all();
    PASS();
}

/* print_duplicate_activity : purement de l'affichage de progression. En prod elle
 * n'est atteinte qu'au bout de 30 s d'attente d'un thread dans la boucle de
 * jointure de check_duplicate -> jamais touchée par les tests fonctionnels. On
 * l'appelle directement avec les compteurs globaux positionnés à la main, en
 * couvrant les deux branches du décompte (thread actif vs terminé). */
TEST print_duplicate_activity_aggregates_counters(void)
{
    for (int t = 0; t < 8; t++) {                 /* nbDuplicateThread == 8 */
        duplicateCount[t]    = (unsigned long long)t;
        duplicateErrors[t]   = (t == 0) ? 2ULL : 0ULL;
        duplicateAnalyzed[t] = (unsigned long long)(t * 3);
        duplicateFinish[t]   = (unsigned long long)(t % 2); /* moitié actifs, moitié finis */
    }
    silence_std();
    print_duplicate_activity(100, 50);            /* nbCombinations > 0 -> ratio fini */
    restore_std();
    PASS();
}

SUITE(datamanager_suite)
{
    RUN_TEST(server_ip_round_trip);
    RUN_TEST(send_solution_without_client_is_local_noop);
    RUN_TEST(send_solution_without_server_configured_returns_error);
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
    RUN_TEST(remove_no_next_handles_complete_solution);
    RUN_TEST(scroll_from_server_returns_packet);
    RUN_TEST(scroll_from_server_returns_null);
    RUN_TEST(send_possibility_analysed_success);
    RUN_TEST(put_to_server_bad_ack_non_fatal);
    RUN_TEST(send_solution_success);
    RUN_TEST(send_solution_server_rejects);
    RUN_TEST(put_to_server_success);
    RUN_TEST(put_to_server_connection_lost);
    RUN_TEST(connect_and_handshake_ok);
    RUN_TEST(connect_handshake_version_rejected);
    RUN_TEST(connect_handshake_retry);
    RUN_TEST(connect_create_tcp_client_fails);
    RUN_TEST(check_files_reports_consistent_stock);
    RUN_TEST(check_one_file_flags_each_inconsistency);
    RUN_TEST(check_datas_empty_stock_is_ok);
    RUN_TEST(check_datas_flags_invalid_packet);
    RUN_TEST(sort_descending_mthread_preserves_count);
    RUN_TEST(regroup_split_nolock_preserve_count);
    RUN_TEST(check_duplicate_empty_stock_returns_immediately);
    RUN_TEST(check_duplicate_small_stock_no_error);
    RUN_TEST(import_json_loads_single_possibility);
    RUN_TEST(print_duplicate_activity_aggregates_counters);
}
