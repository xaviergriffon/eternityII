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
#include "app/control_registry.h"  /* sessions de contrôle : INST_CONTROL_HELLO, control_session_step */
#include "net/control_protocol.h"  /* CTRL_*, control_hello_encode, ctrl_send_frame/ctrl_recv_frame */
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/best_board.h"
#include "net/etii_protocol.h"      /* INST_*, send_instruction, recv_instruction, *_all */

#include "fork_assert.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>

extern unsigned long long *fileUpdates;   /* global défini dans etii_server.c */
extern unsigned long long *analysedFileUpdates; /* global défini dans etii_server.c (PR5) */
extern client_t *thread_params;           /* global défini dans etii_server.c */
void *communicate_with_client(void *userdata);
void create_server_thread(client_t *thread_params, int i);
void lock_all_file(void);                 /* maintenance datamanager (cf. test_datamanager.c) */
void unlock_all_file(void);
void *rmnonext_thread(void *param);       /* thread interne à etii_server.c */

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
   (en-tête + ligne Total). Exerce la boucle complète sur NB_FILE_POSSIBILITY_DEFAULT. */
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
    requeue_last_sent_possibility(NULL, NULL);
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
    requeue_last_sent_possibility(&sent, NULL);

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
    requeue_last_sent_possibility(&sent, NULL);

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
    requeue_last_sent_possibility(&sent, NULL);

    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");
    dm_drain_all();
    PASS();
}

/* Le client reste vivant (canal de contrôle toujours enregistré pour son
 * client_uid) : cette connexion de TRAVAIL a pu se terminer par un simple
 * aléa réseau -- la possibilité n'est PAS remise au stock, pour ne pas la
 * faire explorer une seconde fois en double pendant que le fork y travaille
 * peut-être toujours (même critère de vivacité que le bail d'expiration,
 * PR7 -- voir requeue_last_sent_possibility). */
TEST requeue_skipped_when_client_control_session_alive(void)
{
    restock_analysed();     /* purge un éventuel reliquat "analysed" d'un autre test */
    dm_drain_all();

    uint8_t owner[CLIENT_UID_BYTES];
    memset(owner, 0x99, sizeof owner);

    control_hello_t h = { .pid = 9001, .nb_forks = 1, .identity = { .mode = 0 } };
    memcpy(h.identity.client_uid, owner, CLIENT_UID_BYTES);
    int session_idx = control_registry_register(1, "203.0.113.30", &h);
    ASSERT(session_idx >= 0);

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 11;
    add_possibility_analysed_owned(&pkt, -1, owner);   /* le serveur l'avait servie à `owner` */

    client_t client;
    memset(&client, 0, sizeof client);
    client.has_identity = 1;
    memcpy(client.identity.client_uid, owner, CLIENT_UID_BYTES);

    array_possibility_packet sent = { .size = 1, .possibilities = &pkt };
    requeue_last_sent_possibility(&sent, &client);

    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* pas rendue au stock */
    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");          /* toujours en cours d'analyse */

    control_registry_unregister(session_idx);
    // La possibilité n'a délibérément PAS été remise (c'est le but du test) :
    // elle reste dans le pool analysé, jamais touché par dm_drain_all()
    // (qui ne draine que le stock) -- restock_analysed() la rend au stock
    // pour que dm_drain_all() puisse ensuite tout nettoyer, sans quoi elle
    // fuiterait vers le test suivant (même hasard de contenu -> faux
    // positif de hachage possible sur une autre possibilité "jamais
    // ajoutée").
    restock_analysed();
    dm_drain_all();
    PASS();
}

/* Sans session de contrôle enregistrée pour ce client_uid (client réellement
 * disparu) : comportement inchangé, remise immédiate au stock. */
TEST requeue_returns_to_stock_when_client_not_alive(void)
{
    dm_drain_all();

    uint8_t owner[CLIENT_UID_BYTES];
    memset(owner, 0x9a, sizeof owner);

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 12;
    add_possibility_analysed(&pkt, -1);

    client_t client;
    memset(&client, 0, sizeof client);
    client.has_identity = 1;
    memcpy(client.identity.client_uid, owner, CLIENT_UID_BYTES);
    /* Aucune session de contrôle enregistrée pour ce client_uid. */

    array_possibility_packet sent = { .size = 1, .possibilities = &pkt };
    requeue_last_sent_possibility(&sent, &client);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");   /* rendue au stock */
    dm_drain_all();
    PASS();
}

/* has_identity == 0 (client ancien, jamais de INST_CLIENT_HELLO) : la
 * vivacité ne peut pas être vérifiée -- comportement inchangé, remise
 * immédiate, même si une session de contrôle est par ailleurs enregistrée
 * pour un autre client_uid. */
TEST requeue_returns_to_stock_when_client_has_no_identity(void)
{
    dm_drain_all();

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 13;
    add_possibility_analysed(&pkt, -1);

    client_t client;
    memset(&client, 0, sizeof client);
    client.has_identity = 0;

    array_possibility_packet sent = { .size = 1, .possibilities = &pkt };
    requeue_last_sent_possibility(&sent, &client);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");   /* rendue au stock */
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
static unsigned long long g_analysedfileupd_buf[1];
static unsigned long long *g_saved_counters;
static unsigned long long *g_saved_fileupd;
static unsigned long long *g_saved_analysedfileupd;

static void wire_counters(void)
{
    g_saved_counters = counters;
    g_saved_fileupd = fileUpdates;
    g_saved_analysedfileupd = analysedFileUpdates;
    g_counters_buf[0] = 0;
    g_fileupd_buf[0] = 0;
    g_analysedfileupd_buf[0] = 0;
    counters = g_counters_buf;
    fileUpdates = g_fileupd_buf;
    analysedFileUpdates = g_analysedfileupd_buf;
}

static void unwire_counters(void)
{
    counters = g_saved_counters;
    fileUpdates = g_saved_fileupd;
    analysedFileUpdates = g_saved_analysedfileupd;
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

    int cont = communicate_with_client_step(&client, INST_TEST_CONNECTED, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_TEST_CONNECTED, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* compute_server_hunger (fonction pure) : cible = 2 × clients actifs,
 * faim = manque par rapport à la cible, plafonnée à SERVER_HUNGER_CAP. */
TEST compute_server_hunger_targets_two_per_client(void)
{
    /* Aucun client : pas de faim, quel que soit le stock. */
    ASSERT_EQ_FMT(0, compute_server_hunger(0ULL, 0), "%d");
    ASSERT_EQ_FMT(0, compute_server_hunger(0ULL, -1), "%d");
    /* Stock au niveau ou au-dessus de la cible : rassasié. */
    ASSERT_EQ_FMT(0, compute_server_hunger(2ULL, 1), "%d");
    ASSERT_EQ_FMT(0, compute_server_hunger(100ULL, 8), "%d");
    /* Manque = cible - stock. */
    ASSERT_EQ_FMT(2, compute_server_hunger(0ULL, 1), "%d");
    ASSERT_EQ_FMT(16, compute_server_hunger(0ULL, 8), "%d");
    ASSERT_EQ_FMT(6, compute_server_hunger(10ULL, 8), "%d");
    /* Plafond SERVER_HUNGER_CAP. */
    ASSERT_EQ_FMT((int)SERVER_HUNGER_CAP, compute_server_hunger(0ULL, SERVER_HUNGER_CAP), "%d");
    PASS();
}

/* INST_NEED_WORK (v8) : après handshake, le serveur répond sa faim (int32 ≥ 0),
 * cohérente avec compute_server_hunger sur son état courant, et on continue. */
TEST step_need_work_replies_hunger(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;                 /* handshake déjà réalisé */

    int32_t expected = compute_server_hunger(datas_size(), get_active_threads(thread_params));

    int cont = communicate_with_client_step(&client, INST_NEED_WORK, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    int32_t hunger = -1;
    ASSERT_EQ((long)sizeof(hunger), recv_all(sv[1], &hunger, sizeof(hunger)));
    ASSERT(hunger >= 0);
    ASSERT_EQ_FMT(expected, hunger, "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_NEED_WORK sans handshake de version : refus + arrêt, comme INST_GET. */
TEST step_need_work_requires_version(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;

    int cont = communicate_with_client_step(&client, INST_NEED_WORK, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT_EQ_FMT((int)INST_UNSUPPORTED_VERSION, (int)recv_instruction(sv[1]), "%d");

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

    int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");  /* ancien break */
    ASSERT_EQ_FMT((int)INST_UNSUPPORTED_VERSION, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Libellé de la cause de déconnexion (fonction pure, flux d'évènements). */
TEST disconnect_reason_classifies_last_instruction(void)
{
    ASSERT_STR_EQ("fin de session", client_disconnect_reason(INST_END));
    ASSERT_STR_EQ("connexion perdue", client_disconnect_reason(-1));
    /* Toute autre instruction (step ayant demandé l'arrêt) → protocole interrompu. */
    ASSERT_STR_EQ("protocole interrompu", client_disconnect_reason(INST_GET));
    ASSERT_STR_EQ("protocole interrompu", client_disconnect_reason((int8_t)99));
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

    int cont = communicate_with_client_step(&client, INST_CHECK_VERSION, &last, &vsupp, NULL);

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

    int cont = communicate_with_client_step(&client, (int8_t)99, &last, &vsupp, NULL);

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

    int cont = communicate_with_client_step(&client, INST_ADD, &last, &vsupp, NULL);

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
    /* Coupe au milieu du paquet — relative à sizeof : le paquet ne fait que
     * 64 octets en build ETERN_PARTS=16 (une constante absolue déborderait). */
    const size_t cut = sizeof a->pkt / 2;
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

    int cont = communicate_with_client_step(&client, INST_ADD, &last, &vsupp, NULL);
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

    int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp, NULL);

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

    int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp, NULL);
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

/* --------------------------------------------------------------------------
 * record_possibility_analysed_for_client (PR6, attribution des analyses en
 * cours) : la
 * possibilité servie n'est attribuée au client_uid déclaré QUE si son
 * identité est connue sur CETTE connexion de travail (INST_CLIENT_HELLO, v12).
 * ------------------------------------------------------------------------ */

TEST record_possibility_analysed_owns_when_identity_known(void)
{
    dm_drain_all();

    client_t client;
    memset(&client, 0, sizeof client);
    client.has_identity = 1;
    for (int i = 0; i < CLIENT_UID_BYTES; i++) {
        client.identity.client_uid[i] = (uint8_t)(0x70 + i);
    }

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 63;
    record_possibility_analysed_for_client(&client, &pkt);

    unsigned long long count = 0;
    int max_alloc = -1;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(client.identity.client_uid, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");
    ASSERT_EQ_FMT(63, max_alloc, "%d");

    ASSERT_EQ_FMT(0, remove_possibility_analysed(&pkt, -1), "%d");
    dm_drain_all();
    PASS();
}

TEST record_possibility_analysed_no_owner_when_identity_unknown(void)
{
    dm_drain_all();

    client_t client;
    memset(&client, 0, sizeof client);
    client.has_identity = 0;   /* client trop ancien, ou hello pas encore reçu */

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 64;
    record_possibility_analysed_for_client(&client, &pkt);

    /* Aucun owner_uid n'a été enregistré : même un uid tout à zéro (celui,
       non initialisé, de `client.identity`) ne doit rien trouver. */
    uint8_t zero_uid[CLIENT_UID_BYTES];
    memset(zero_uid, 0, sizeof zero_uid);
    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(zero_uid, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(0ULL, count, "%llu");
    ASSERT_EQ_FMT(-1, max_alloc, "%d");

    dm_drain_all();
    PASS();
}

/* INST_POSSIBILITY_ANALYSED : la possibilité « en analyse » est acquittée. */
TEST step_possibility_analysed_acks(void)
{
    dm_drain_all();
    wire_counters(); /* analysedFileUpdates[client->compteur]++ (PR5) */

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

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[1]), "%d");

    unwire_counters();
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

    int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK, &last, &vsupp, NULL);

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

    int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK_BATCH, &last, &vsupp, NULL);
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
    wire_counters(); /* analysedFileUpdates[client->compteur]++ (PR5) */

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

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED_BATCH, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_CONSIDERED, (int)recv_instruction(sv[1]), "%d");

    unwire_counters();
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

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED_BATCH, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");   /* ancien break, aucun INST_CONSIDERED */

    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_SOLUTION : réception incomplète (flux tronqué puis fermé) — même
 * durcissement que INST_ADD ci-dessus : recv_all ne peut renvoyer un résultat
 * court que sur EOF/erreur socket, donc le flux est irrécupérable et la
 * session doit se clore (avant le fix : elle envoyait INST_ERROR et
 * continuait sur un flux mort). */
TEST step_solution_incomplete_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    char fragment[8];
    memset(fragment, 0, sizeof fragment);
    ASSERT_EQ((ssize_t)sizeof fragment, write(sv[1], fragment, sizeof fragment));
    close(sv[1]);

    int cont = communicate_with_client_step(&client, INST_SOLUTION, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");

    close(sv[0]);
    PASS();
}

/* INST_CHECK_VERSION : version du client jamais reçue (pair fermé) — flux
 * irrécupérable, la session se clôt sans réponse. */
TEST step_check_version_recv_fail_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;

    close(sv[1]);                  /* EOF avant l'envoi de la version */

    int cont = communicate_with_client_step(&client, INST_CHECK_VERSION, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT_EQ_FMT(0, vsupp, "%d");

    close(sv[0]);
    PASS();
}

/* INST_CHECK_VERSION : version différente — client rejeté (INST_UNSUPPORTED_VERSION)
 * mais la session continue (le client décide de raccrocher). */
TEST step_check_version_mismatch_rejects(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;

    int cv = version + 1;          /* version incompatible */
    ASSERT_EQ((ssize_t)sizeof(int), write(sv[1], &cv, sizeof(int)));

    int cont = communicate_with_client_step(&client, INST_CHECK_VERSION, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(0, vsupp, "%d");   /* handshake NON validé */
    ASSERT_EQ_FMT((int)INST_UNSUPPORTED_VERSION, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* Deux INST_GET consécutifs : le lot précédent (*lastSent) est libéré avant de
 * servir le suivant — c'est la branche free du début du handler. */
TEST step_second_get_frees_previous_batch(void)
{
    dm_drain_all();
    wire_counters();

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[0].alloc = 1;
    pks[1].alloc = 2;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    for (int round = 0; round < 2; round++) {
        int cont = communicate_with_client_step(&client, INST_GET, &last, &vsupp, NULL);
        ASSERT_EQ_FMT(1, cont, "%d");
        int32_t k = 0;
        ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
        ASSERT_EQ_FMT(1, (int)k, "%d");
        struct possibility_packet got;
        ASSERT_EQ((long)sizeof got, recv_all(sv[1], &got, sizeof got));
        remove_possibility_analysed(&got, -1);       /* acquitte pour ne pas polluer */
    }
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");

    unwire_counters();
    if (last) free_array_possibility_packet(last);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_GET_TO_CHECK avec du stock non vérifié : la possibilité est servie et
 * passée « en analyse » ; un second appel libère le lot précédent. */
TEST step_get_to_check_serves_possibility(void)
{
    dm_drain_all();
    wire_counters();

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[0].alloc = 3;
    pks[1].alloc = 4;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.compteur = 0;
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    for (int round = 0; round < 2; round++) {        /* 2e tour : libère lastSent */
        int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK, &last, &vsupp, NULL);
        ASSERT_EQ_FMT(1, cont, "%d");
        int32_t k = 0;
        ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
        ASSERT_EQ_FMT(1, (int)k, "%d");
        struct possibility_packet got;
        ASSERT_EQ((long)sizeof got, recv_all(sv[1], &got, sizeof got));
        remove_possibility_analysed(&got, -1);
    }
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");        /* pool vidé */

    unwire_counters();
    if (last) free_array_possibility_packet(last);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_GET_TO_CHECK_BATCH : compte demandé jamais reçu (pair fermé) — arrêt. */
TEST step_get_to_check_batch_count_not_received_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    close(sv[1]);                  /* EOF avant l'envoi du compte */

    int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK_BATCH, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");

    close(sv[0]);
    PASS();
}

/* INST_GET_TO_CHECK_BATCH avec du stock : les K disponibles sont servis en un
 * bloc contigu (K < N demandé) ; un second appel libère le lot précédent. */
TEST step_get_to_check_batch_serves_batch(void)
{
    dm_drain_all();
    wire_counters();

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[0].alloc = 5;
    pks[1].alloc = 6;
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);

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
    int cont = communicate_with_client_step(&client, INST_GET_TO_CHECK_BATCH, &last, &vsupp, NULL);
    ASSERT_EQ_FMT(1, cont, "%d");

    int32_t k = 0;
    ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
    ASSERT_EQ_FMT(2, (int)k, "%d");
    struct possibility_packet got[2];
    ASSERT_EQ((long)sizeof got, recv_all(sv[1], got, sizeof got));
    remove_possibility_analysed(&got[0], -1);
    remove_possibility_analysed(&got[1], -1);

    /* Second lot sur pool vide : libère le lastSent précédent, renvoie K == 0. */
    ASSERT_EQ((long)sizeof requested, send_all(sv[1], &requested, sizeof requested));
    cont = communicate_with_client_step(&client, INST_GET_TO_CHECK_BATCH, &last, &vsupp, NULL);
    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ((long)sizeof k, recv_all(sv[1], &k, sizeof k));
    ASSERT_EQ_FMT(0, (int)k, "%d");

    unwire_counters();
    if (last) free_array_possibility_packet(last);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_ADD : paquet incomplet (fragment puis EOF) — flux mort, arrêt. */
TEST step_add_short_recv_stops(void)
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

    char fragment[8];
    memset(fragment, 0, sizeof fragment);
    ASSERT_EQ((ssize_t)sizeof fragment, write(sv[1], fragment, sizeof fragment));
    close(sv[1]);

    int cont = communicate_with_client_step(&client, INST_ADD, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu");   /* rien ajouté au stock */

    unwire_counters();
    close(sv[0]);
    PASS();
}

/* INST_POSSIBILITY_ANALYSED sur une possibilité jamais servie : le retrait
 * échoue, le serveur répond INST_ERROR mais la session continue. */
TEST step_analysed_not_removed_sends_error(void)
{
    dm_drain_all();

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 11;                /* jamais passée « en analyse » */
    ASSERT_EQ((long)sizeof pkt, send_all(sv[1], &pkt, sizeof pkt));

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT((int)INST_ERROR, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* INST_POSSIBILITY_ANALYSED : paquet incomplet (fragment puis EOF) — arrêt. */
TEST step_analysed_short_recv_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    char fragment[8];
    memset(fragment, 0, sizeof fragment);
    ASSERT_EQ((ssize_t)sizeof fragment, write(sv[1], fragment, sizeof fragment));
    close(sv[1]);

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");

    close(sv[0]);
    PASS();
}

/* INST_POSSIBILITY_ANALYSED_BATCH : lot annoncé de 2 mais un seul paquet reçu
 * avant EOF — INST_ERROR puis arrêt de la session. */
TEST step_analysed_batch_incomplete_packet_stops(void)
{
    dm_drain_all();
    wire_counters(); /* analysedFileUpdates[client->compteur]++ (PR5) sur le paquet retiré avant l'EOF */
    /* Le serveur répond INST_ERROR sur un pair déjà fermé : sans cela le
     * SIGPIPE résultant tuerait le runner (la production l'ignore aussi). */
    signal(SIGPIPE, SIG_IGN);

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    struct possibility_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    pkt.alloc = 12;
    add_possibility_analysed(&pkt, -1);

    int32_t m = 2;
    ASSERT_EQ((long)sizeof m, send_all(sv[1], &m, sizeof m));
    ASSERT_EQ((long)sizeof pkt, send_all(sv[1], &pkt, sizeof pkt));
    close(sv[1]);                  /* le 2e paquet ne viendra jamais */

    int cont = communicate_with_client_step(&client, INST_POSSIBILITY_ANALYSED_BATCH, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(0, cont, "%d");  /* INST_ERROR envoyé puis arrêt (pair fermé : non relu) */

    unwire_counters();
    close(sv[0]);
    PASS();
}

/* ---------- INST_SOLUTION + --stop-on-solution (chemin d'arrêt serveur) --- */
/*
 * Ce chemin appelle exit(EXIT_SUCCESS) après avoir sauvegardé le stock : on
 * l'exécute via fork_assert. Le fils bascule dans un répertoire temporaire créé
 * PAR LE PARENT (transmis par global copié au fork) pour qu'aucun fichier
 * (.back, solution_server_*, events.log) ne pollue le dépôt, et le parent
 * vérifie ensuite les artefacts dans ce répertoire.
 */
static char g_sol_dir[PATH_MAX];
static int g_sol_maintenance = 0;   /* 1 : simule une maintenance en cours */

static void fork_solution_stop_server(void)
{
    if (chdir(g_sol_dir) != 0) exit(9);
    stop_on_solution = 1;
    if (g_sol_maintenance) {
        lock_all_file();           /* backup() renverra BACKUP_SKIPPED_MAINTENANCE */
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) exit(8);
    struct possibility_packet sol;
    memset(&sol, 0, sizeof sol);
    sol.alloc = ETERN_PARTS;
    if (send_all(sv[1], &sol, sizeof sol) != (long)sizeof sol) exit(7);

    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;
    communicate_with_client_step(&client, INST_SOLUTION, &last, &vsupp, NULL);
    exit(6);                       /* le step aurait dû exit(EXIT_SUCCESS) */
}

/* Nettoie les artefacts du fils dans g_sol_dir puis supprime le répertoire. */
static void cleanup_sol_dir(pid_t child)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/eternityII.back", g_sol_dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/eternityII-in_analyse.back", g_sol_dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/solution_server_%i_0.csv", g_sol_dir, (int)child);
    unlink(path);
    snprintf(path, sizeof path, "%s/events.log", g_sol_dir);
    unlink(path);
    rmdir(g_sol_dir);
}

/* Solution reçue avec --stop-on-solution : le serveur sauvegarde la solution et
 * son stock (noms par défaut de restore) puis s'arrête avec EXIT_SUCCESS. */
TEST step_solution_stop_on_solution_backs_up_and_exits(void)
{
    strcpy(g_sol_dir, "/tmp/etii_sol_XXXXXX");
    ASSERT(mkdtemp(g_sol_dir) != NULL);
    g_sol_maintenance = 0;

    pid_t child = -1;
    int code = run_in_fork(fork_solution_stop_server, &child);
    ASSERT_EQ_FMT(0, code, "%d");   /* exit(EXIT_SUCCESS) du step */

    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/solution_server_%i_0.csv", g_sol_dir, (int)child);
    ASSERT_EQ_FMT(0, access(path, F_OK), "%d");
    snprintf(path, sizeof path, "%s/eternityII.back", g_sol_dir);
    ASSERT_EQ_FMT(0, access(path, F_OK), "%d");
    snprintf(path, sizeof path, "%s/eternityII-in_analyse.back", g_sol_dir);
    ASSERT_EQ_FMT(0, access(path, F_OK), "%d");

    cleanup_sol_dir(child);
    PASS();
}

/* Même chemin pendant une maintenance : les backups sont sautés (journalisés)
 * mais l'arrêt reste propre — la solution, elle, est bien sauvegardée. */
TEST step_solution_stop_during_maintenance_still_exits(void)
{
    strcpy(g_sol_dir, "/tmp/etii_sol_XXXXXX");
    ASSERT(mkdtemp(g_sol_dir) != NULL);
    g_sol_maintenance = 1;

    pid_t child = -1;
    int code = run_in_fork(fork_solution_stop_server, &child);
    g_sol_maintenance = 0;
    ASSERT_EQ_FMT(0, code, "%d");

    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/solution_server_%i_0.csv", g_sol_dir, (int)child);
    ASSERT_EQ_FMT(0, access(path, F_OK), "%d");
    snprintf(path, sizeof path, "%s/eternityII.back", g_sol_dir);
    ASSERT(access(path, F_OK) != 0);             /* backup sauté */

    cleanup_sol_dir(child);
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

/* ---------- check_server_step -------------------------------------------- */
/*
 * check_server_step lit `counters`/`fileUpdates` sur NB_THREADS entrées : on les
 * câble sur des tampons de taille 1 (wire_counters) et on force NB_THREADS = 1
 * pour ne jamais déborder. thread_params est mis à NULL (get_active_threads(NULL)
 * -> 0), déjà couvert isolément par active_threads_null_is_zero.
 */

/* Lit lastcheck sous son mutex documenté (contrat de static_variables.h), comme
 * le fait check_interpreter côté production. */
static char *read_lastcheck_copy(void)
{
    pthread_mutex_lock(&lastcheck_mutex);
    char *copy = lastcheck != NULL ? strdup(lastcheck) : NULL;
    pthread_mutex_unlock(&lastcheck_mutex);
    return copy;
}

/* Rapport de base : ni nouveau record ni autobackup (sous le seuil de 6 tours). */
TEST check_server_step_reports_basic_stats(void)
{
    dm_drain_all();
    wire_counters();
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    client_t *saved_tp = thread_params;
    thread_params = NULL;
    uint16_t saved_mr = max_result;
    max_result = 10;

    unsigned long long lastactive = 0;
    autobackup_state_t backup_state = {0};
    int last_record = (int)max_result;
    check_server_step(&lastactive, &backup_state, &last_record, 10);

    char *report = read_lastcheck_copy();
    ASSERT(report != NULL);
    ASSERT(strstr(report, "File queues") != NULL);
    ASSERT(strstr(report, "active thread last") != NULL);
    /* Indice cumulé + part du prunage (delta de pruner_cells_studied) */
    ASSERT(strstr(report, "études/s (recherche+prunage)") != NULL);
    ASSERT(strstr(report, "dont prunage/s") != NULL);
    free(report);
    ASSERT_EQ_FMT(10, last_record, "%d");   /* pas de nouveau record */
    ASSERT_EQ_FMT(1, backup_state.stock.lastBack, "%d");       /* sous le seuil : incrémenté */

    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Régression : `report` (check_server_step) était calloc'é à une taille FIXE
 * (4000 octets) alors que `table` (build_file_queues_table) grandit avec
 * `nb_file_possibility` (jusqu'à NB_FILE_POSSIBILITY_MAX = 128, ~8,4 Kio) :
 * `strcat(report, table)`
 * débordait, détecté par `_FORTIFY_SOURCE` en SIGILL sur une exécution réelle
 * avec `--stock-files` élevé (trouvé en vérifiant le binaire réel, pas par ce
 * test seul -- mais ASan sur cette même scène aurait suffi à l'attraper).
 * `nb_file_possibility` étant un état global partagé (cf. commentaire de
 * `tests/core/test_datamanager.c`), restauré au défaut avant PASS(). */
TEST check_server_step_handles_large_stock_files_count(void)
{
    dm_drain_all();
    wire_counters();
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    client_t *saved_tp = thread_params;
    thread_params = NULL;
    uint16_t saved_mr = max_result;
    max_result = 10;

    ASSERT_EQ_FMT(0, datamanager_configure_stock_files(NB_FILE_POSSIBILITY_MAX), "%d");

    unsigned long long lastactive = 0;
    autobackup_state_t backup_state = {0};
    int last_record = (int)max_result;
    check_server_step(&lastactive, &backup_state, &last_record, 10);

    char *report = read_lastcheck_copy();
    ASSERT(report != NULL);
    ASSERT(strstr(report, "File queues") != NULL);
    ASSERT(strstr(report, "active thread last") != NULL); /* bloc `temp` intact après `table` */
    free(report);

    datamanager_configure_stock_files(NB_FILE_POSSIBILITY_DEFAULT);
    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Seuil d'autobackup atteint + stock modifié + nouveau record : les deux
 * branches se déclenchent ensemble (backup réel sur ./temp*.back, nettoyé
 * après coup). */
TEST check_server_step_detects_record_and_autobackups(void)
{
    dm_drain_all();
    wire_counters();
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    client_t *saved_tp = thread_params;
    thread_params = NULL;
    uint16_t saved_mr = max_result;
    max_result = 99;

    unlink("events.log");
    unlink("./temp.back");
    unlink("./temp_analysed.back");
    fileUpdates[0] = 5; /* != lastUpdates(0) -> autobackup se déclenche au seuil */

    unsigned long long lastactive = 0;
    autobackup_state_t backup_state = {0};
    backup_state.stock.lastBack = 6; /* seuil atteint */
    int last_record = 10;
    check_server_step(&lastactive, &backup_state, &last_record, 10);

    ASSERT_EQ_FMT(99, last_record, "%d");        /* nouveau record détecté */
    ASSERT_EQ_FMT(0, backup_state.stock.lastBack, "%d");            /* backup déclenché -> remis à 0 */
    ASSERT_EQ_FMT(5ULL, backup_state.stock.lastUpdates, "%llu");
    ASSERT_EQ_FMT(0, access("./temp.back", F_OK), "%d");
    ASSERT_EQ_FMT(0, access("./temp_analysed.back", F_OK), "%d");

    unlink("./temp.back");
    unlink("./temp_analysed.back");
    unlink("events.log");
    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    max_result = saved_mr;
    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Seuil d'autobackup atteint pendant une maintenance : should_autobackup
 * déclenche (compteur remis à 0) mais backup()/backup_analysed() renvoient
 * BACKUP_SKIPPED_MAINTENANCE — journalisé, aucun fichier écrit. */
TEST check_server_step_autobackup_skipped_during_maintenance(void)
{
    dm_drain_all();
    wire_counters();
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    client_t *saved_tp = thread_params;
    thread_params = NULL;

    unlink("./temp.back");
    unlink("./temp_analysed.back");
    fileUpdates[0] = 3;            /* != lastUpdates(0) -> déclenchement au seuil */

    unsigned long long lastactive = 0;
    autobackup_state_t backup_state = {0};
    backup_state.stock.lastBack = 6;              /* seuil atteint */
    int last_record = (int)max_result;   /* pas de nouveau record */
    lock_all_file();               /* maintenance en cours */
    check_server_step(&lastactive, &backup_state, &last_record, 10);
    unlock_all_file();

    ASSERT_EQ_FMT(0, backup_state.stock.lastBack, "%d");             /* cadence consommée */
    ASSERT(access("./temp.back", F_OK) != 0);     /* mais aucun backup écrit */
    ASSERT(access("./temp_analysed.back", F_OK) != 0);

    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Bail à expiration (PR7) :
 * check_server_step balaie et rend au stock les possibilités attribuées dont
 * le bail a expiré ET dont le propriétaire n'est plus vivant (aucune session
 * de contrôle enregistrée pour son client_uid — ici, aucune n'est enregistrée
 * du tout, donc "pas vivant" par construction) : un client mort (kill -9,
 * coupure réseau) ne gèle plus sa part indéfiniment. Contrairement à
 * `datamanager_reclaim_expired_leases` (testé sans horloge réelle dans
 * test_datamanager.c, `now` étant un paramètre), `check_server_step` lit
 * l'horloge lui-même : ce test d'intégration utilise donc un bail réellement
 * court (1 s) et un sleep réel, comme `auto_stats_due_after_interval_elapsed`
 * (tests/app/test_control_registry.c) pour une contrainte de temporisation
 * équivalente. */
TEST check_server_step_reclaims_expired_lease(void)
{
    restock_analysed();     /* purge un éventuel reliquat "analysed" d'un autre test */
    dm_drain_all();
    wire_counters();
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    client_t *saved_tp = thread_params;
    thread_params = NULL;
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 1;

    uint8_t owner[CLIENT_UID_BYTES];
    memset(owner, 0x77, sizeof owner);
    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 33;
    add_possibility_analysed_owned(&pk, -1, owner);

    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");   /* attribuée avant le tour */

    usleep(1100 * 1000);    /* laisse le bail de 1 s expirer avant le tour */

    unsigned long long lastactive = 0;
    autobackup_state_t backup_state = {0};
    int last_record = (int)max_result;
    check_server_step(&lastactive, &backup_state, &last_record, 10);

    count = 999; max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(0ULL, count, "%llu");        /* plus attribuée : bail expiré */
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* rendue au stock non vérifié */

    analysed_lease_seconds = saved_lease;
    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Correctif (retour d'essais réels) : un client dont le canal de contrôle
 * reste enregistré (donc vivant) ne doit JAMAIS voir son travail réclamé,
 * même quand l'échéance fixe du bail (1 s ici) est largement dépassée -- rien
 * ne garantit qu'une possibilité s'analyse en moins de `analysed_lease_seconds`.
 * Reproduit exactement le scénario signalé : un client qui continue de donner
 * signe de vie (ici simulé en laissant sa session de contrôle enregistrée)
 * ne doit pas voir sa possibilité remise en stock -- ce qui créerait un
 * double travail sur la même branche si ce client termine son analyse plus
 * tard. */
TEST check_server_step_does_not_reclaim_lease_of_alive_client(void)
{
    restock_analysed();
    dm_drain_all();
    wire_counters();
    int saved_nb = NB_THREADS;
    NB_THREADS = 1;
    client_t *saved_tp = thread_params;
    thread_params = NULL;
    int saved_lease = analysed_lease_seconds;
    analysed_lease_seconds = 1;

    uint8_t owner[CLIENT_UID_BYTES];
    memset(owner, 0x78, sizeof owner);

    control_hello_t h = { .pid = 4242, .nb_forks = 1, .identity = { .mode = 0 } };
    memcpy(h.identity.client_uid, owner, CLIENT_UID_BYTES);
    int session_idx = control_registry_register(1, "203.0.113.20", &h);
    ASSERT(session_idx >= 0);

    struct possibility_packet pk;
    memset(&pk, 0, sizeof pk);
    pk.alloc = 34;
    add_possibility_analysed_owned(&pk, -1, owner);

    usleep(1100 * 1000);    /* le bail (1 s) est dépassé, mais le client reste "vivant" */

    unsigned long long lastactive = 0;
    autobackup_state_t backup_state = {0};
    int last_record = (int)max_result;
    check_server_step(&lastactive, &backup_state, &last_record, 10);

    unsigned long long count = 999;
    int max_alloc = -999;
    ASSERT_EQ_FMT(0, datamanager_analysed_owned_by(owner, &count, &max_alloc), "%d");
    ASSERT_EQ_FMT(1ULL, count, "%llu");        /* toujours attribuée : client vivant */
    ASSERT_EQ_FMT(0ULL, datas_size(), "%llu"); /* pas rendue au stock */

    control_registry_unregister(session_idx);
    analysed_lease_seconds = saved_lease;
    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Enveloppe de thread check_server : REQUEST_STOP prépositionné, la boucle
 * (while(request != REQUEST_STOP), refactor P8) ne s'exécute jamais — appel
 * direct sûr, le corps (step + sleep) est couvert via check_server_step. */
TEST check_server_stops_immediately_on_request_stop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    void *ret = check_server(NULL);
    ASSERT_EQ(NULL, ret);

    request = saved_req;
    PASS();
}

/* ---------- communicate_with_client (wrapper complet) -------------------- */
/*
 * communicate_with_client boucle sur communicate_with_client_step (déjà testée
 * unitairement ci-dessus) jusqu'à INST_END ou déconnexion, puis referme le
 * socket et rend au stock la dernière possibilité servie non acquittée.
 * On le lance dans un vrai pthread ; le socketpair joue la connexion TCP.
 */

/* Session complète : handshake de version puis INST_END -> fin propre. */
static void *mini_srv_full_handshake_then_end(void *arg)
{
    int fd = *(int *)arg;
    int8_t b = INST_CHECK_VERSION;
    send(fd, &b, 1, 0);
    int v = version;
    send(fd, &v, sizeof v, 0);
    int8_t resp = 0;
    recv(fd, &resp, 1, 0);                          /* INST_SUPPORTED_VERSION */
    b = INST_END;
    send(fd, &b, 1, 0);
    return NULL;
}

TEST communicate_with_client_full_session_ends_cleanly(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.exist = 1;
    client.compteur = 0;

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_full_handshake_then_end, &sv[1]);

    void *ret = communicate_with_client(&client);
    ASSERT_EQ(NULL, ret);
    ASSERT_EQ_FMT(-1, client.socket_id, "%d");
    ASSERT_EQ_FMT(0, client.exist, "%d");

    pthread_join(srv, NULL);
    close(sv[1]);
    PASS();
}

/* Handshake puis INST_GET (une possibilité servie), puis déconnexion brutale
 * (pas d'INST_END) : à la sortie de boucle, la dernière possibilité servie et
 * jamais acquittée doit être rendue au stock (requeue_last_sent_possibility). */
static void *mini_srv_handshake_get_then_drop(void *arg)
{
    int fd = *(int *)arg;
    int8_t b = INST_CHECK_VERSION;
    send(fd, &b, 1, 0);
    int v = version;
    send(fd, &v, sizeof v, 0);
    int8_t resp = 0;
    recv(fd, &resp, 1, 0);                          /* INST_SUPPORTED_VERSION */
    b = INST_GET;
    send(fd, &b, 1, 0);
    int32_t k = 0;
    recv(fd, &k, sizeof k, 0);
    if (k > 0) {
        struct possibility_packet pkt;
        recv(fd, &pkt, sizeof pkt, 0);
    }
    close(fd);                                      /* déconnexion sans acquittement */
    return NULL;
}

TEST communicate_with_client_disconnect_requeues_pending_get(void)
{
    dm_drain_all();
    wire_counters();

    struct possibility_packet *p = malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->alloc = 8;
    array_possibility_packet *ap = malloc(sizeof *ap);
    ap->size = 1; ap->possibilities = p;
    add_possibility(NULL, ap);
    free_array_possibility_packet(ap);
    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.exist = 1;
    client.compteur = 0;

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake_get_then_drop, &sv[1]);

    void *ret = communicate_with_client(&client);
    ASSERT_EQ(NULL, ret);

    pthread_join(srv, NULL);
    close(sv[1]);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu"); /* possibilité servie -> rendue au stock */

    unwire_counters();
    dm_drain_all();
    PASS();
}

/* Handshake puis instruction inconnue : le step demande l'arrêt (break de la
 * boucle) — la session se clôt côté serveur sans attendre INST_END. */
static void *mini_srv_handshake_then_bad_instruction(void *arg)
{
    int fd = *(int *)arg;
    int8_t b = INST_CHECK_VERSION;
    send(fd, &b, 1, 0);
    int v = version;
    send(fd, &v, sizeof v, 0);
    int8_t resp = 0;
    recv(fd, &resp, 1, 0);                          /* INST_SUPPORTED_VERSION */
    b = 99;                                         /* instruction inconnue */
    send(fd, &b, 1, 0);
    return NULL;
}

TEST communicate_with_client_stops_on_protocol_error(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    client.exist = 1;
    client.compteur = 0;

    pthread_t srv;
    pthread_create(&srv, NULL, mini_srv_handshake_then_bad_instruction, &sv[1]);

    void *ret = communicate_with_client(&client);
    ASSERT_EQ(NULL, ret);
    ASSERT_EQ_FMT(-1, client.socket_id, "%d");
    ASSERT_EQ_FMT(0, client.exist, "%d");

    pthread_join(srv, NULL);
    close(sv[1]);
    PASS();
}

/* ---------- create_server_thread ----------------------------------------- */
/*
 * Exerce les deux branches (tid == NULL / tid != NULL, libéré puis recréé) et,
 * par construction, la boucle d'attente du socket + le wrapper
 * communicate_with_client (session vidée immédiatement par un pair fermé :
 * EOF dès le premier recv_instruction -> fin de boucle sans requeue).
 */
TEST create_server_thread_recreates_tid_on_second_call(void)
{
    client_t arr[1];
    memset(&arr[0], 0, sizeof arr[0]);

    create_server_thread(arr, 0);                   /* tid == NULL : pas de free */
    ASSERT(arr[0].tid != NULL);
    ASSERT_EQ_FMT(1, arr[0].exist, "%d");
    ASSERT_EQ_FMT(-1, arr[0].socket_id, "%d");

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    close(sv[1]);                                    /* pair fermé -> EOF immédiat */
    arr[0].socket_id = sv[0];

    for (int i = 0; i < 200 && arr[0].exist != 0; i++) usleep(10000);
    ASSERT_EQ_FMT(0, arr[0].exist, "%d");             /* session terminée proprement */

    create_server_thread(arr, 0);                    /* tid != NULL : free + recréation */
    ASSERT(arr[0].tid != NULL);
    ASSERT_EQ_FMT(-1, arr[0].socket_id, "%d");

    int sv2[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv2));
    close(sv2[1]);
    arr[0].socket_id = sv2[0];
    for (int i = 0; i < 200 && arr[0].exist != 0; i++) usleep(10000);
    ASSERT_EQ_FMT(0, arr[0].exist, "%d");

    free(arr[0].tid);                                 /* thread détaché terminé : sûr */
    PASS();
}

/* ---------- init_server_thread_pool --------------------------------------- */

/* Alloue thread_params/fileUpdates (globales) et initialise chaque slot vide.
 * Les globales sont sauvegardées/restaurées : elles sont partagées entre suites. */
TEST init_pool_initializes_all_slots(void)
{
    client_t *saved_tp = thread_params;
    unsigned long long *saved_fu = fileUpdates;
    int saved_nb = NB_THREADS;
    NB_THREADS = 3;

    struct array_part rp = { .size = 0, .parts = NULL };   /* juste un pointeur partagé */
    init_server_thread_pool(&rp);

    ASSERT(thread_params != NULL);
    ASSERT(fileUpdates != NULL);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_FMT(0, thread_params[i].exist, "%d");
        ASSERT_EQ_FMT(-1, thread_params[i].socket_id, "%d");
        ASSERT_EQ(NULL, thread_params[i].tid);
        ASSERT_EQ_FMT(i, thread_params[i].compteur, "%d");
        ASSERT_EQ(&rp, thread_params[i].rotate_parts);
        ASSERT_STR_EQ("", thread_params[i].peer_ip);
        ASSERT_EQ_FMT(0ULL, fileUpdates[i], "%llu");
    }

    free(thread_params);
    free(fileUpdates);
    thread_params = saved_tp;
    fileUpdates = saved_fu;
    NB_THREADS = saved_nb;
    PASS();
}

/* ---------- configure_client_socket ---------------------------------------- */

/* Les timeouts de session (réception ET envoi) valent tcp_timeout secondes. */
TEST configure_client_socket_sets_timeouts(void)
{
    int saved_timeout = tcp_timeout;
    tcp_timeout = 7;

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    configure_client_socket(sv[0]);
    tcp_timeout = saved_timeout;

    struct timeval tv;
    socklen_t len = sizeof tv;
    memset(&tv, 0, sizeof tv);
    ASSERT_EQ(0, getsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &tv, &len));
    ASSERT_EQ_FMT(7L, (long)tv.tv_sec, "%ld");
    memset(&tv, 0, sizeof tv);
    len = sizeof tv;
    ASSERT_EQ(0, getsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &tv, &len));
    ASSERT_EQ_FMT(7L, (long)tv.tv_sec, "%ld");

    close(sv[0]); close(sv[1]);
    PASS();
}

/* ---------- try_assign_client_slot ----------------------------------------- */

/* Un thread libre (exist, en attente de socket) : affectation directe, sans
 * régénération (aucun slot vide). Chemin pur — pas d'I/O sur le client_id. */
TEST try_assign_free_slot_direct(void)
{
    client_t *saved_tp = thread_params;
    int saved_nb = NB_THREADS;

    client_t slots[2];
    memset(slots, 0, sizeof slots);
    slots[0].exist = 1; slots[0].socket_id = 8;    /* occupé */
    slots[1].exist = 1; slots[1].socket_id = -1;   /* libre */
    thread_params = slots;
    NB_THREADS = 2;

    int busy_logged = 0;
    int id = try_assign_client_slot(42, "203.0.113.10", &busy_logged);

    ASSERT_EQ_FMT(1, id, "%d");
    ASSERT_EQ_FMT(42, slots[1].socket_id, "%d");
    ASSERT_STR_EQ("203.0.113.10", slots[1].peer_ip);
    ASSERT_EQ_FMT(0, busy_logged, "%d");

    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    PASS();
}

/* peer_ip == NULL : le slot affecté garde son peer_ip précédent (cf. contrat
 * documenté dans etii_server.h) au lieu d'être écrasé par une chaîne vide. */
TEST try_assign_null_peer_ip_keeps_previous_value(void)
{
    client_t *saved_tp = thread_params;
    int saved_nb = NB_THREADS;

    client_t slots[1];
    memset(slots, 0, sizeof slots);
    slots[0].exist = 1; slots[0].socket_id = -1;   /* libre */
    strncpy(slots[0].peer_ip, "203.0.113.10", sizeof(slots[0].peer_ip) - 1);
    thread_params = slots;
    NB_THREADS = 1;

    int busy_logged = 0;
    int id = try_assign_client_slot(42, NULL, &busy_logged);

    ASSERT_EQ_FMT(0, id, "%d");
    ASSERT_EQ_FMT(42, slots[0].socket_id, "%d");
    ASSERT_STR_EQ("203.0.113.10", slots[0].peer_ip);

    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    PASS();
}

/* Aucun thread libre mais un slot vide : le slot est régénéré
 * (create_server_thread réel) et le client lui est affecté directement. */
TEST try_assign_regenerates_empty_slot(void)
{
    client_t *saved_tp = thread_params;
    int saved_nb = NB_THREADS;

    client_t slots[1];
    memset(slots, 0, sizeof slots);                /* exist=0 : slot vide */
    thread_params = slots;
    NB_THREADS = 1;

    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    close(sv[1]);                                  /* pair fermé -> session EOF immédiate */

    int busy_logged = 0;
    int id = try_assign_client_slot(sv[0], "203.0.113.10", &busy_logged);

    ASSERT_EQ_FMT(0, id, "%d");
    ASSERT_EQ_FMT(1, slots[0].exist, "%d");
    ASSERT_EQ_FMT(sv[0], slots[0].socket_id, "%d");
    ASSERT_STR_EQ("203.0.113.10", slots[0].peer_ip);

    /* La session se vide immédiatement (EOF) : le thread ferme le socket et
     * libère le slot — on attend sa fin avant de rendre la fixture. */
    for (int i = 0; i < 200 && slots[0].exist != 0; i++) usleep(10000);
    ASSERT_EQ_FMT(0, slots[0].exist, "%d");
    free(slots[0].tid);                            /* thread détaché terminé */

    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    PASS();
}

/* Tout est occupé, aucun slot régénérable : -1, épisode journalisé UNE fois
 * (le second tour n'ajoute pas d'évènement), puis cession du CPU. */
TEST try_assign_all_busy_logs_once(void)
{
    client_t *saved_tp = thread_params;
    int saved_nb = NB_THREADS;

    client_t slots[1];
    memset(slots, 0, sizeof slots);
    slots[0].exist = 1; slots[0].socket_id = 9;    /* occupé */
    thread_params = slots;
    NB_THREADS = 1;

    unlink("events.log");
    int busy_logged = 0;
    ASSERT_EQ_FMT(-1, try_assign_client_slot(42, "203.0.113.10", &busy_logged), "%d");
    ASSERT_EQ_FMT(1, busy_logged, "%d");
    ASSERT_EQ_FMT(-1, try_assign_client_slot(42, "203.0.113.10", &busy_logged), "%d");   /* déjà journalisé */
    ASSERT_EQ_FMT(1, busy_logged, "%d");
    ASSERT_EQ_FMT(9, slots[0].socket_id, "%d");    /* fixture intacte */
    unlink("events.log");

    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    PASS();
}

/* ---------- rmnonext_pass / rmnonext_thread -------------------------------- */

/* Aucun client connecté : la passe élague les impasses du stock (même fixture
 * que remove_no_next_prunes_dead_packets dans test_datamanager.c). */
TEST rmnonext_pass_prunes_when_idle(void)
{
    dm_drain_all();
    client_t *saved_tp = thread_params;
    thread_params = NULL;                          /* get_active_threads -> 0 */

    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[1].grid[dirx[0]][diry[0]] = -2;            /* impasse : (0,0,0,0) sans candidat */
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);
    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");

    rmnonext_pass(map, &rp);

    ASSERT_EQ_FMT(1ULL, datas_size(), "%llu");     /* l'impasse a été retirée */

    free_bigarray(map);
    thread_params = saved_tp;
    dm_drain_all();
    PASS();
}

/* Un client connecté : l'élagage est suspendu (les files sont en cours
 * d'alimentation), le stock reste intact. */
TEST rmnonext_pass_skips_when_client_active(void)
{
    dm_drain_all();
    client_t *saved_tp = thread_params;
    int saved_nb = NB_THREADS;

    client_t slots[1];
    memset(slots, 0, sizeof slots);
    slots[0].exist = 1; slots[0].socket_id = 5;    /* client connecté */
    thread_params = slots;
    NB_THREADS = 1;

    struct part parts[] = { { .id = 0 }, { .id = 1, .top = 1, .right = 1, .bottom = 1, .left = 1 } };
    struct array_part rp = { .size = 2, .parts = parts };
    map_big_array *map = buildBigArray(&rp, search_max_face(&rp));

    struct possibility_packet pks[2];
    memset(pks, 0, sizeof pks);
    pks[1].grid[dirx[0]][diry[0]] = -2;            /* impasse, mais pas d'élagage */
    array_possibility_packet arr = { .size = 2, .possibilities = pks };
    add_possibility(NULL, &arr);

    rmnonext_pass(map, &rp);

    ASSERT_EQ_FMT(2ULL, datas_size(), "%llu");     /* rien retiré */

    free_bigarray(map);
    thread_params = saved_tp;
    NB_THREADS = saved_nb;
    dm_drain_all();
    PASS();
}

/* Enveloppe de thread : REQUEST_STOP prépositionné, la boucle ne s'exécute
 * jamais — construction (read_parts sur le fichier par défaut du build) et
 * libération des structures sont exercées. */
TEST rmnonext_thread_stops_immediately_on_request_stop(void)
{
    int saved_req = request;
    request = REQUEST_STOP;

    void *ret = rmnonext_thread(NULL);
    ASSERT_EQ(NULL, ret);

    request = saved_req;
    PASS();
}

/* ---------- INST_CONTROL_HELLO (bascule en session de contrôle) --------- */
/*
 * Ces tests exercent le contrat de communicate_with_client_step pour
 * INST_CONTROL_HELLO : un socketpair joue le rôle du canal, control_registry
 * étant le vrai registre global (pas de mock). Chaque test désenregistre la
 * session qu'il a créée pour ne pas fausser un test suivant (ex. le test
 * « registre plein »).
 */

static void send_control_hello(int fd, int32_t pid, int32_t nb_forks, uint8_t mode)
{
    control_hello_t hello = { .pid = pid, .nb_forks = nb_forks, .identity = { .fork_seq = -1, .mode = mode } };
    uint8_t buf[CONTROL_HELLO_WIRE_MAX_SIZE];
    int32_t len = control_hello_encode(&hello, buf, sizeof(buf));
    send_all(fd, &len, sizeof(len));
    send_all(fd, buf, (size_t)len);
}

TEST step_control_hello_switches_session(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;
    int out_idx = -1;

    send_control_hello(sv[1], 4242, 7, 1);

    int cont = communicate_with_client_step(&client, INST_CONTROL_HELLO, &last, &vsupp, &out_idx);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT(out_idx >= 0);

    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(infos, MAX_CONTROL_SESSIONS);
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (infos[i].pid == 4242 && infos[i].nb_forks == 7 && infos[i].mode == 1) {
            found = 1;
        }
    }
    ASSERT(found);

    control_registry_unregister(out_idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* Sans out-param (NULL accepté) : la bascule reste opérée dans le registre
   (la session existe), mais l'appelant qui ignore le paramètre n'en est pas
   informé — comportement attendu pour un appelant qui ne gère pas le canal. */
TEST step_control_hello_out_param_null_is_accepted(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;

    int before = control_registry_count();
    ASSERT_EQ(0, before);   /* registre vide en entrée : condition du nettoyage ci-dessous */
    send_control_hello(sv[1], 1, 1, 0);

    int cont = communicate_with_client_step(&client, INST_CONTROL_HELLO, &last, &vsupp, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ(1, control_registry_count());

    /* Le out-param est NULL : impossible de récupérer l'indice enregistré
       directement. Comme le registre était vide en entrée, un balayage complet
       retrouve et libère à coup sûr le (seul) slot occupé — sans risque
       d'interférer avec un autre test de la suite. */
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        control_registry_unregister(i);
    }
    ASSERT_EQ(0, control_registry_count());

    close(sv[0]); close(sv[1]);
    PASS();
}

TEST step_control_hello_requires_version(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 0;             /* handshake jamais réalisé */
    int out_idx = -1;

    int cont = communicate_with_client_step(&client, INST_CONTROL_HELLO, &last, &vsupp, &out_idx);

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT_EQ_FMT(-1, out_idx, "%d");
    ASSERT_EQ_FMT((int)INST_UNSUPPORTED_VERSION, (int)recv_instruction(sv[1]), "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

TEST step_control_hello_bad_length_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;
    int out_idx = -1;

    int32_t bad_len = CTRL_PAYLOAD_MAX + 1;
    ASSERT_EQ((long)sizeof bad_len, send_all(sv[1], &bad_len, sizeof bad_len));

    int cont = communicate_with_client_step(&client, INST_CONTROL_HELLO, &last, &vsupp, &out_idx);

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT_EQ_FMT(-1, out_idx, "%d");

    close(sv[0]); close(sv[1]);
    PASS();
}

TEST step_control_hello_registry_full_stops(void)
{
    /* Remplit le registre pour forcer l'échec d'enregistrement. */
    control_hello_t h = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0 } };
    int idxs[MAX_CONTROL_SESSIONS];
    int filled = 0;
    while (filled < MAX_CONTROL_SESSIONS) {
        int idx = control_registry_register(1000 + filled, "203.0.113.10", &h);
        if (idx < 0) break;
        idxs[filled++] = idx;
    }
    ASSERT_EQ(MAX_CONTROL_SESSIONS, filled);

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    array_possibility_packet *last = NULL;
    int vsupp = 1;
    int out_idx = -1;

    send_control_hello(sv[1], 5, 5, 0);

    int cont = communicate_with_client_step(&client, INST_CONTROL_HELLO, &last, &vsupp, &out_idx);

    ASSERT_EQ_FMT(0, cont, "%d");
    ASSERT_EQ_FMT(-1, out_idx, "%d");

    for (int i = 0; i < filled; i++) {
        control_registry_unregister(idxs[i]);
    }
    close(sv[0]); close(sv[1]);
    PASS();
}

/* ---------- control_session_step (le SERVEUR initie l'échange) ---------- */
/*
 * Le test joue le rôle du CLIENT en face de control_session_step : il lit ce
 * que le serveur envoie via ctrl_recv_frame et répond via ctrl_send_frame,
 * exactement comme le fera la PR4 côté client (non implémentée ici).
 */

/* Argument passé au thread jouant le client en face de control_session_step. */
struct ctrl_peer_arg {
    int fd;
    int expect_cmd;          /* CTRL_COMMAND ou CTRL_GET_STATS ou CTRL_PING */
    const char *expect_line; /* pour CTRL_COMMAND : ligne attendue (NULL si N/A) */
    int reply_cmd;           /* CTRL_RESULT / CTRL_STATS / CTRL_ACK */
    int32_t reply_retcode;   /* pour CTRL_RESULT */
};

static void *ctrl_peer_thread(void *arg)
{
    struct ctrl_peer_arg *a = arg;
    void *payload = NULL;
    int32_t plen = 0;
    int rcmd = ctrl_recv_frame(a->fd, &payload, &plen);
    if (rcmd == a->expect_cmd) {
        if (a->expect_line != NULL) {
            char got[256];
            int copy = plen < (int32_t)sizeof(got) - 1 ? plen : (int32_t)sizeof(got) - 1;
            memcpy(got, payload, (size_t)copy);
            got[copy] = '\0';
            if (strcmp(got, a->expect_line) != 0) {
                free(payload);
                return NULL; /* n'envoie pas la réponse : le test verra l'échec via cont==0 */
            }
        }
        free(payload);
        if (a->reply_cmd == CTRL_RESULT) {
            ctrl_send_frame(a->fd, CTRL_RESULT, &a->reply_retcode, sizeof(a->reply_retcode));
        } else if (a->reply_cmd == CTRL_STATS) {
            control_stats_t stats = { .shots_per_second = 10, .possibility_stock = 20,
                                       .analysed_stock = 5, .max_result = 30,
                                       .pruner_checked = 1, .pruner_removed = 2 };
            uint8_t buf[CONTROL_STATS_WIRE_SIZE];
            control_stats_encode(&stats, buf);
            ctrl_send_frame(a->fd, CTRL_STATS, buf, CONTROL_STATS_WIRE_SIZE);
            // Le max_result rapporté ci-dessus (30) peut dépasser le meilleur
            // déjà connu du serveur (g_server_best_board, un global process-wide
            // que d'autres suites peuvent avoir déjà fait progresser) :
            // control_session_step tire alors CTRL_GET_BEST_BOARD sur CETTE
            // MÊME connexion avant de rendre la main. On répond systématiquement
            // "aucun plateau" (valid=0) : ce test ne porte pas sur cette
            // représentation, seulement sur le round-trip stats existant.
            void *payload2 = NULL;
            int32_t plen2 = 0;
            int rcmd2 = ctrl_recv_frame(a->fd, &payload2, &plen2);
            free(payload2);
            if (rcmd2 == CTRL_GET_BEST_BOARD) {
                uint8_t novalid = 0;
                ctrl_send_frame(a->fd, CTRL_BEST_BOARD, &novalid, sizeof(novalid));
            }
        } else if (a->reply_cmd == CTRL_ACK) {
            ctrl_send_frame(a->fd, CTRL_ACK, NULL, 0);
        }
    } else {
        free(payload);
    }
    return NULL;
}

TEST control_session_step_command_round_trip(void)
{
    control_hello_t h = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);
    ASSERT_EQ(0, control_registry_post_command(idx, CTRL_COMMAND, "pause"));

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];

    struct ctrl_peer_arg arg = { .fd = sv[1], .expect_cmd = CTRL_COMMAND,
                                  .expect_line = "pause", .reply_cmd = CTRL_RESULT,
                                  .reply_retcode = 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, ctrl_peer_thread, &arg));

    int cont = control_session_step(&client, idx, 2000);
    pthread_join(t, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");

    control_registry_unregister(idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

TEST control_session_step_get_stats_round_trip(void)
{
    // g_server_best_board est un global process-wide (cf. core/best_board.h) :
    // réinitialisé pour que le max_result=30 renvoyé par ctrl_peer_thread ci-
    // dessous déclenche DÉTERMINISTEMENT le suivi CTRL_GET_BEST_BOARD de
    // control_session_step, quel que soit ce qu'une autre suite y a déjà écrit.
    best_board_init(&g_server_best_board);

    control_hello_t h = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);
    ASSERT_EQ(0, control_registry_post_command(idx, CTRL_GET_STATS, NULL));

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];

    struct ctrl_peer_arg arg = { .fd = sv[1], .expect_cmd = CTRL_GET_STATS,
                                  .expect_line = NULL, .reply_cmd = CTRL_STATS,
                                  .reply_retcode = 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, ctrl_peer_thread, &arg));

    int cont = control_session_step(&client, idx, 2000);
    pthread_join(t, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");

    /* control_session_step doit mettre en cache les stats décodées (pour
       GET /api/v1/clients de l'API HTTP admin, cf. control_registry_record_stats). */
    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(infos, MAX_CONTROL_SESSIONS);
    ASSERT_EQ(1, n);
    ASSERT_EQ(1, infos[0].has_stats);
    ASSERT_EQ(10, (int)infos[0].stats.shots_per_second);
    ASSERT_EQ(30, (int)infos[0].stats.max_result);

    control_registry_unregister(idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

/* Verrouille le bug rapporté : un client annonçant un record UNIQUEMENT via
   CTRL_STATS (jamais via INST_ADD sur le protocole de travail) doit quand
   même faire progresser le max_result GLOBAL du serveur (logs, GET
   /api/v1/stats) — pas seulement le cache par-session (GET /api/v1/clients)
   ou g_server_best_board (GET /api/v1/best-board). */
TEST control_session_step_get_stats_updates_global_max_result(void)
{
    best_board_init(&g_server_best_board);
    uint16_t saved_mr = max_result;
    max_result = 5; /* strictement sous le 30 rapporté par ctrl_peer_thread */

    control_hello_t h = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);
    ASSERT_EQ(0, control_registry_post_command(idx, CTRL_GET_STATS, NULL));

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];

    struct ctrl_peer_arg arg = { .fd = sv[1], .expect_cmd = CTRL_GET_STATS,
                                  .expect_line = NULL, .reply_cmd = CTRL_STATS,
                                  .reply_retcode = 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, ctrl_peer_thread, &arg));

    int cont = control_session_step(&client, idx, 2000);
    pthread_join(t, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");
    ASSERT_EQ_FMT(30, (int)max_result, "%d");

    max_result = saved_mr;
    control_registry_unregister(idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

TEST control_session_step_timeout_pings_and_continues(void)
{
    control_hello_t h = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);
    /* Aucune commande postée : le tour doit expirer et déclencher un ping. */

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];

    struct ctrl_peer_arg arg = { .fd = sv[1], .expect_cmd = CTRL_PING,
                                  .expect_line = NULL, .reply_cmd = CTRL_ACK,
                                  .reply_retcode = 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, ctrl_peer_thread, &arg));

    int cont = control_session_step(&client, idx, 50 /* ms : timeout court */);
    pthread_join(t, NULL);

    ASSERT_EQ_FMT(1, cont, "%d");

    control_registry_unregister(idx);
    close(sv[0]); close(sv[1]);
    PASS();
}

TEST control_session_step_ping_without_ack_stops(void)
{
    control_hello_t h = { .pid = 1, .nb_forks = 1, .identity = { .mode = 0 } };
    int idx = control_registry_register(1, "203.0.113.10", &h);
    ASSERT(idx >= 0);

    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];
    close(sv[1]);              /* pair mort : ni ping reçu ni ack possible */

    int cont = control_session_step(&client, idx, 50);

    ASSERT_EQ_FMT(0, cont, "%d");

    control_registry_unregister(idx);
    close(sv[0]);
    PASS();
}

TEST control_session_step_invalid_index_stops(void)
{
    int sv[2];
    ASSERT_EQ(0, make_pair(sv));
    client_t client;
    memset(&client, 0, sizeof client);
    client.socket_id = sv[0];

    int cont = control_session_step(&client, -1, 50);
    ASSERT_EQ_FMT(0, cont, "%d");

    close(sv[0]); close(sv[1]);
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
    RUN_TEST(requeue_skipped_when_client_control_session_alive);
    RUN_TEST(requeue_returns_to_stock_when_client_not_alive);
    RUN_TEST(requeue_returns_to_stock_when_client_has_no_identity);

    RUN_TEST(step_test_connected_pings_back);
    RUN_TEST(compute_server_hunger_targets_two_per_client);
    RUN_TEST(step_need_work_replies_hunger);
    RUN_TEST(step_need_work_requires_version);
    RUN_TEST(step_unsupported_version_stops);
    RUN_TEST(disconnect_reason_classifies_last_instruction);
    RUN_TEST(step_check_version_ok);
    RUN_TEST(step_unknown_instruction_stops);
    RUN_TEST(step_add_stores_possibility);
    RUN_TEST(step_add_reassembles_fragmented_packet);
    RUN_TEST(step_get_empty_sends_zero_count);
    RUN_TEST(step_get_serves_possibility);
    RUN_TEST(record_possibility_analysed_owns_when_identity_known);
    RUN_TEST(record_possibility_analysed_no_owner_when_identity_unknown);
    RUN_TEST(step_possibility_analysed_acks);
    RUN_TEST(step_get_to_check_empty_sends_zero_count);
    RUN_TEST(step_get_to_check_batch_empty_returns_zero);
    RUN_TEST(step_analysed_batch_acks);
    RUN_TEST(step_analysed_batch_out_of_bounds_stops);
    RUN_TEST(step_solution_incomplete_stops);

    RUN_TEST(step_check_version_recv_fail_stops);
    RUN_TEST(step_check_version_mismatch_rejects);
    RUN_TEST(step_second_get_frees_previous_batch);
    RUN_TEST(step_get_to_check_serves_possibility);
    RUN_TEST(step_get_to_check_batch_count_not_received_stops);
    RUN_TEST(step_get_to_check_batch_serves_batch);
    RUN_TEST(step_add_short_recv_stops);
    RUN_TEST(step_analysed_not_removed_sends_error);
    RUN_TEST(step_analysed_short_recv_stops);
    RUN_TEST(step_analysed_batch_incomplete_packet_stops);
    RUN_TEST(step_solution_stop_on_solution_backs_up_and_exits);
    RUN_TEST(step_solution_stop_during_maintenance_still_exits);

    RUN_TEST(autobackup_increments_below_threshold);
    RUN_TEST(autobackup_fires_at_threshold_when_changed);
    RUN_TEST(autobackup_skips_at_threshold_when_unchanged);
    RUN_TEST(autobackup_waits_full_window_even_if_changed);

    RUN_TEST(check_server_step_reports_basic_stats);
    RUN_TEST(check_server_step_handles_large_stock_files_count);
    RUN_TEST(check_server_step_detects_record_and_autobackups);
    RUN_TEST(check_server_step_autobackup_skipped_during_maintenance);
    RUN_TEST(check_server_step_reclaims_expired_lease);
    RUN_TEST(check_server_step_does_not_reclaim_lease_of_alive_client);
    RUN_TEST(check_server_stops_immediately_on_request_stop);

    RUN_TEST(communicate_with_client_full_session_ends_cleanly);
    RUN_TEST(communicate_with_client_disconnect_requeues_pending_get);
    RUN_TEST(communicate_with_client_stops_on_protocol_error);

    RUN_TEST(create_server_thread_recreates_tid_on_second_call);

    RUN_TEST(init_pool_initializes_all_slots);
    RUN_TEST(configure_client_socket_sets_timeouts);
    RUN_TEST(try_assign_free_slot_direct);
    RUN_TEST(try_assign_null_peer_ip_keeps_previous_value);
    RUN_TEST(try_assign_regenerates_empty_slot);
    RUN_TEST(try_assign_all_busy_logs_once);
    RUN_TEST(rmnonext_pass_prunes_when_idle);
    RUN_TEST(rmnonext_pass_skips_when_client_active);
    RUN_TEST(rmnonext_thread_stops_immediately_on_request_stop);

    RUN_TEST(step_control_hello_switches_session);
    RUN_TEST(step_control_hello_out_param_null_is_accepted);
    RUN_TEST(step_control_hello_requires_version);
    RUN_TEST(step_control_hello_bad_length_stops);
    RUN_TEST(step_control_hello_registry_full_stops);

    RUN_TEST(control_session_step_command_round_trip);
    RUN_TEST(control_session_step_get_stats_round_trip);
    RUN_TEST(control_session_step_get_stats_updates_global_max_result);
    RUN_TEST(control_session_step_timeout_pings_and_continues);
    RUN_TEST(control_session_step_ping_without_ack_stops);
    RUN_TEST(control_session_step_invalid_index_stops);
}
