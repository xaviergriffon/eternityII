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
#include "app/static_variables.h"   /* counters, version */
#include "core/datamanager.h"
#include "core/possibility.h"
#include "net/etii_protocol.h"      /* INST_*, send_instruction, recv_instruction, *_all */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>

extern unsigned long long *fileUpdates;   /* global défini dans etii_server.c */

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

/* ---------- communicate_with_client_step -------------------------------- */
/*
 * Le step fait de vraies I/O sur client->socket_id : on lui passe un bout d'un
 * socketpair (sv[0]) et on joue le client sur l'autre bout (sv[1]). counters et
 * fileUpdates sont indexés par client->compteur dans certaines branches : on les
 * câble sur des tampons locaux (sauvegarde/restauration du global).
 */

/* Timeout de réception : si le code sous test n'envoie pas ce que le test
 * attend (régression protocolaire), recv/recv_all échouent après 5 s au lieu
 * de bloquer le runner indéfiniment — le test devient rouge, pas suspendu. */
static void set_recv_timeout(int fd)
{
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static int make_pair(int sv[2])
{
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (rc == 0) {
        set_recv_timeout(sv[0]);
        set_recv_timeout(sv[1]);
    }
    return rc;
}

static unsigned long long g_counters_buf[1];
static unsigned long long g_fileupd_buf[1];
static unsigned long long *g_saved_counters;
static unsigned long long *g_saved_fileupd;

static void wire_counters(void)
{
    g_saved_counters = counters;
    g_saved_fileupd = fileUpdates;
    g_counters_buf[0] = 0;
    g_fileupd_buf[0] = 0;
    counters = g_counters_buf;
    fileUpdates = g_fileupd_buf;
}

static void unwire_counters(void)
{
    counters = g_saved_counters;
    fileUpdates = g_saved_fileupd;
}

/* INST_TEST_CONNECTED : ping renvoyé tel quel, on continue. */
TEST step_test_connected_pings_back(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;

    int cont = communicate_with_client_step(&client, INST_TEST_CONNECTED, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_TEST_CONNECTED, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Instruction métier sans handshake de version : refus + arrêt. */
TEST step_unsupported_version_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;                 /* handshake jamais réalisé */

    int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp);

    ASSERT_EQ_FMT(0, cont, "%d");  /* ancien break */
    ASSERT_EQ_FMT((int)INST_UNSUPPORTED_VERSION, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Handshake de version réussi : version_supported passe à 1, on continue. */
TEST step_check_version_ok(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;

    int cv = version;              /* envoie exactement la version serveur */
    ASSERT_EQ((ssize_t)sizeof(int), write(sv[1], &cv, sizeof(int)));

    int cont = communicate_with_client_step(&client, INST_CHECK_VERSION, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(1, vsupp, "%d");
    ASSERT_EQ_FMT((int)INST_SUPPORTED_VERSION, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Instruction inconnue (handshake fait) : arrêt. */
TEST step_unknown_instruction_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int cont = communicate_with_client_step(&client, (int8_t)99, &last, &vsupp);

    ASSERT_EQ_FMT(0, cont, "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_ADD : la possibilité reçue est ajoutée au stock, INST_CONSIDERED renvoyé. */
TEST step_add_stores_possibility(void)
{
    dm_drain_all();
    wire_counters();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 3;
    ASSERT_EQ((ssize_t)sizeof pkt, write(sv[1], &pkt, sizeof pkt));

    int cont = communicate_with_client_step(&client, INST_ADD, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[1]), "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    unwire_counters();
    dm_drain_all();
    close(sv[0]); close(sv[1]);
    PASS();
}

/* Écrivain du test fragmenté : envoie le paquet en 2 send() séparés par un
 * usleep, pour que le serveur lise d'abord un fragment incomplet. */
struct frag_writer_arg { int fd; struct possibility_packet pkt; };
static void *frag_writer(void *arg)
{
    struct frag_writer_arg *a = arg;
    const size_t cut = 200;                          /* coupe en plein paquet */
    send(a->fd, &a->pkt, cut, 0);
    usleep(50000);                    /* laisse le serveur consommer le fragment */
    send(a->fd, (const char *)&a->pkt + cut, sizeof a->pkt - cut, 0);
    return NULL;
}

/* INST_ADD : la possibilité arrive en deux fragments TCP — recv_all doit la
 * réassembler (échouait avant VERSION 7 : le recv() brut prenait le 1er
 * fragment pour la réception complète → INST_ERROR + flux désynchronisé). */
TEST step_add_reassembles_fragmented_packet(void)
{
    dm_drain_all();
    wire_counters();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    struct frag_writer_arg wa;
    wa.fd = sv[1];
    memset(&wa.pkt, 0, sizeof wa.pkt);
    wa.pkt.alloc = 6;
    pthread_t writer;
    ASSERT_EQ(0, pthread_create(&writer, NULL, frag_writer, &wa));

    int cont = communicate_with_client_step(&client, INST_ADD, &last, &vsupp);
    pthread_join(writer, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[1]), "%d");
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    unwire_counters();
    dm_drain_all();
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_GET sur stock vide : compte K == 0 renvoyé (trame VERSION 7), on continue. */
TEST step_get_empty_sends_zero_count(void)
{
    dm_drain_all();
    wire_counters();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    int32_t k = -1;
    ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
    ASSERT_EQ_FMT(0, (int)k, "%d");

    unwire_counters();
    if (last) free_array_possibility_packet(last);  /* tableau vide alloué par get_last_possibility */
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_GET avec une possibilité en stock : elle est envoyée au client et
   retirée du stock (passée « en analyse »). */
TEST step_get_serves_possibility(void)
{
    dm_drain_all();
    wire_counters();

    struct possibility_packet *p = malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->alloc = 5;
    array_possibility_packet *ap = malloc(sizeof *ap);
    ap->size = 1;
    ap->possibilities = p;
    add_possibility(NULL, ap);
    free_array_possibility_packet(ap);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp);
    ASSERT_EQ_FMT(1, cont, "%d");

    /* Trame VERSION 7 : compte K puis le bloc de K paquets. */
    int32_t k = 0;
    ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
    ASSERT_EQ_FMT(1, (int)k, "%d");
    struct possibility_packet got;
    memset(&got, 0, sizeof got);
    ASSERT_EQ((long)sizeof got, recv_all(sv[1], &got, sizeof got));
    ASSERT_EQ_FMT(5, (int)got.alloc, "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");      /* retirée du stock */

    unwire_counters();
    if (last) free_array_possibility_packet(last);
    dm_drain_all();
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_POSSIBILITY_ANALYSED : la possibilité « en analyse » est acquittée. */
TEST step_possibility_analysed_acks(void)
{
    dm_drain_all();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 4;
    add_possibility_analysed(&pkt, -1);
    ASSERT_EQ((long)sizeof pkt, send_all(sv[1], &pkt, sizeof pkt));

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_GET_TO_CHECK (pruner, unité) sur pool « à vérifier » vide : K == 0. */
TEST step_get_to_check_empty_sends_zero_count(void)
{
    dm_drain_all();
    wire_counters();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    int32_t k = -1;
    ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
    ASSERT_EQ_FMT(0, (int)k, "%d");

    unwire_counters();
    if (last) free_array_possibility_packet(last);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_GET_TO_CHECK_BATCH sur pool vide : on relit un compte K == 0. */
TEST step_get_to_check_batch_empty_returns_zero(void)
{
    dm_drain_all();
    wire_counters();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int32_t requested = 5;
    ASSERT_EQ((long)sizeof requested, send_all(sv[1], &requested, sizeof requested));

    int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK_BATCH, &last, &vsupp);
    ASSERT_EQ_FMT(1, cont, "%d");

    int32_t k = -1;
    ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
    ASSERT_EQ_FMT(0, (int)k, "%d");

    unwire_counters();
    if (last) free_array_possibility_packet(last);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_POSSIBILITY_ANALYSED_BATCH : M acquittements en un aller-retour. */
TEST step_analysed_batch_acks(void)
{
    dm_drain_all();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    struct possibility_packet pkts[2];
    memset(pkts, 0, sizeof pkts);
    pkts[0].alloc = 1;
    pkts[1].alloc = 2;
    add_possibility_analysed(&pkts[0], -1);
    add_possibility_analysed(&pkts[1], -1);

    int32_t m = 2;
    ASSERT_EQ((long)sizeof m, send_all(sv[1], &m, sizeof m));
    ASSERT_EQ((long)sizeof pkts, send_all(sv[1], pkts, sizeof pkts));

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED_BATCH, &last, &vsupp);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_POSSIBILITY_ANALYSED_BATCH avec un compte hors borne : arrêt sans ack. */
TEST step_analysed_batch_out_of_bounds_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int32_t m = PRUNER_BATCH_MAX + 1;
    ASSERT_EQ((long)sizeof m, send_all(sv[1], &m, sizeof m));

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED_BATCH, &last, &vsupp);

    ASSERT_EQ_FMT(0, cont, "%d");   /* ancien break, aucun INST_CONSIDERED */

    close(sv[0]); close(sv[1]);
    PASS();
}

/* ---------- should_autobackup -------------------------------------------- */
/*
 * Cadence de la sauvegarde automatique : tous les 6 tours ET seulement si le
 * total de mises à jour a changé depuis le dernier backup.
 */

/* Sous le seuil : on incrémente le compteur, pas de backup, état de réf inchangé. */
TEST autobackup_increments_below_threshold(void)
{
    int lastBack = 0;
    unsigned long long lastBk = 0;
    ASSERT_EQ_FMT(0, should_autobackup(&lastBack, &lastBk, 5), "%d");
    ASSERT_EQ_FMT(1, lastBack, "%d");
    ASSERT_EQ_FMT(0ULL, lastBk, "%llu");   /* pas de backup -> référence inchangée */
    PASS();
}

/* Seuil atteint + stock modifié : backup, compteur remis à 0, référence mémorisée. */
TEST autobackup_fires_at_threshold_when_changed(void)
{
    int lastBack = 6;
    unsigned long long lastBk = 100;
    ASSERT_EQ_FMT(1, should_autobackup(&lastBack, &lastBk, 150), "%d");
    ASSERT_EQ_FMT(0, lastBack, "%d");
    ASSERT_EQ_FMT(150ULL, lastBk, "%llu");
    PASS();
}

/* Seuil atteint mais stock figé : pas de backup, compteur reste armé (pas d'incrément). */
TEST autobackup_skips_at_threshold_when_unchanged(void)
{
    int lastBack = 6;
    unsigned long long lastBk = 100;
    ASSERT_EQ_FMT(0, should_autobackup(&lastBack, &lastBk, 100), "%d");
    ASSERT_EQ_FMT(6, lastBack, "%d");        /* reste à 6, prêt à déclencher au prochain changement */
    ASSERT_EQ_FMT(100ULL, lastBk, "%llu");
    PASS();
}

/* La fenêtre complète est respectée : aucun backup avant 6 tours, même si le
   stock bouge à chaque tour ; déclenchement au tour suivant. */
TEST autobackup_waits_full_window_even_if_changed(void)
{
    int lastBack = 0;
    unsigned long long lastBk = 0;
    for (int t = 0; t < 6; t++) {
        ASSERT_EQ_FMT(0, should_autobackup(&lastBack, &lastBk, (unsigned long long)(t + 1)), "%d");
    }
    ASSERT_EQ_FMT(6, lastBack, "%d");
    ASSERT_EQ_FMT(1, should_autobackup(&lastBack, &lastBk, 999), "%d");
    ASSERT_EQ_FMT(0, lastBack, "%d");
    ASSERT_EQ_FMT(999ULL, lastBk, "%llu");
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

    RUN_TEST(step_test_connected_pings_back);
    RUN_TEST(step_unsupported_version_stops);
    RUN_TEST(step_check_version_ok);
    RUN_TEST(step_unknown_instruction_stops);
    RUN_TEST(step_add_stores_possibility);
    RUN_TEST(step_add_reassembles_fragmented_packet);
    RUN_TEST(step_get_empty_sends_zero_count);
    RUN_TEST(step_get_serves_possibility);
    RUN_TEST(step_possibility_analysed_acks);
    RUN_TEST(step_get_to_check_empty_sends_zero_count);
    RUN_TEST(step_get_to_check_batch_empty_returns_zero);
    RUN_TEST(step_analysed_batch_acks);
    RUN_TEST(step_analysed_batch_out_of_bounds_stops);

    RUN_TEST(autobackup_increments_below_threshold);
    RUN_TEST(autobackup_fires_at_threshold_when_changed);
    RUN_TEST(autobackup_skips_at_threshold_when_unchanged);
    RUN_TEST(autobackup_waits_full_window_even_if_changed);
}
