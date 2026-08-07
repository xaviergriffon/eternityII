/*
 * Tests unitaires du registre de clients CONNUS (src/app/known_clients_registry.c,
 * PR4 de docs/conception/identification_clients.md).
 *
 * Comme control_registry.c, ce registre est un état GLOBAL statique — mais à
 * la différence de control_registry (vidé à chaque unregister), les entrées
 * de CE registre ne sont JAMAIS supprimées à la déconnexion : chaque test
 * utilise donc un `machine_uid`/`client_uid` UNIQUE (compteur global
 * `next_seed`), pour que ses assertions ne dépendent jamais des entrées
 * laissées par les tests précédents. Seul le test d'éviction (placé en
 * dernier) a besoin de connaître l'état cumulé du registre, via
 * `known_clients_registry_count()`.
 */
#include "greatest.h"
#include "app/known_clients_registry.h"
#include "app/static_variables.h"   /* MAX_KNOWN_CLIENTS, KNOWN_CLIENT_MAX_SESSIONS */

#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* Compteur global, jamais remis à zéro : garantit un machine_uid/client_uid
 * distinct à chaque appel, sur toute la durée du binaire de test. */
static uint32_t g_next_seed = 1;

static uint32_t next_seed(void)
{
    return g_next_seed++;
}

static void seed_bytes(uint8_t *out, size_t n, uint32_t seed)
{
    memset(out, 0, n);
    memcpy(out, &seed, sizeof(seed) < n ? sizeof(seed) : n);
}

static client_identity_t make_identity(uint32_t machine_seed, uint32_t client_seed,
                                        const char *label, uint8_t mode)
{
    client_identity_t id;
    memset(&id, 0, sizeof(id));
    seed_bytes(id.machine_uid, MACHINE_UID_BYTES, machine_seed);
    seed_bytes(id.client_uid, CLIENT_UID_BYTES, client_seed);
    id.fork_seq = -1;
    id.mode = mode;
    if (label != NULL) {
        strncpy(id.label, label, CLIENT_LABEL_MAX - 1);
        id.label[CLIENT_LABEL_MAX - 1] = '\0';
    }
    return id;
}

static control_stats_t make_stats(uint64_t pruner_checked, uint64_t pruner_removed,
                                   uint64_t max_result)
{
    control_stats_t s;
    memset(&s, 0, sizeof(s));
    s.pruner_checked = pruner_checked;
    s.pruner_removed = pruner_removed;
    s.max_result = max_result;
    return s;
}

/* Cherche, dans un instantané, l'entrée dont machine_uid_hex correspond à
 * celui de `identity`. Retourne NULL si absente. */
static const known_client_info_t *find_entry(const known_client_info_t *infos, int n,
                                               const client_identity_t *identity)
{
    char hex[2 * MACHINE_UID_BYTES + 1];
    client_identity_hex_encode(identity->machine_uid, MACHINE_UID_BYTES, hex, sizeof(hex));
    for (int i = 0; i < n; i++) {
        if (strcmp(infos[i].machine_uid_hex, hex) == 0) {
            return &infos[i];
        }
    }
    return NULL;
}

static const known_client_info_t *snapshot_find(const client_identity_t *identity,
                                                  known_client_info_t *buf, int cap)
{
    int n = known_clients_registry_snapshot(buf, cap);
    return find_entry(buf, n, identity);
}

/* ---------- on_connect ----------------------------------------------------- */

TEST on_connect_creates_new_entry(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "jetson-1", CLIENT_MODE_SEARCH);

    known_client_info_t buf[MAX_KNOWN_CLIENTS];
    known_clients_registry_on_connect(&id, "203.0.113.10");
    const known_client_info_t *e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);

    ASSERT(e != NULL);
    ASSERT_STR_EQ("jetson-1", e->label);
    ASSERT_STR_EQ("203.0.113.10", e->peer_ip);
    ASSERT_EQ((int)CLIENT_MODE_SEARCH, (int)e->mode);
    ASSERT_EQ(1, e->connected);
    ASSERT_EQ(1, e->nb_active_sessions);
    ASSERT_EQ(1, e->nb_connections_total);
    ASSERT_EQ(0, (int)e->total_pruner_checked);
    ASSERT_EQ(0, (int)e->total_pruner_removed);
    ASSERT_EQ(0, (int)e->best_max_result);
    PASS();
}

TEST on_connect_null_identity_is_noop(void)
{
    int before = known_clients_registry_count();
    known_clients_registry_on_connect(NULL, "203.0.113.10");
    ASSERT_EQ(before, known_clients_registry_count());
    PASS();
}

TEST on_connect_accepts_null_peer_ip_as_empty_string(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "no-ip", CLIENT_MODE_SEARCH);
    known_client_info_t buf[MAX_KNOWN_CLIENTS];

    known_clients_registry_on_connect(&id, NULL);
    const known_client_info_t *e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);

    ASSERT(e != NULL);
    ASSERT_STR_EQ("", e->peer_ip);
    PASS();
}

TEST on_connect_second_machine_updates_label_peer_ip_mode(void)
{
    uint32_t machine_seed = next_seed();
    client_identity_t first = make_identity(machine_seed, next_seed(), "old-label", CLIENT_MODE_SEARCH);
    known_clients_registry_on_connect(&first, "203.0.113.10");
    known_clients_registry_on_disconnect(first.machine_uid, first.client_uid);

    client_identity_t second = make_identity(machine_seed, next_seed(), "new-label", CLIENT_MODE_PRUNER);
    known_clients_registry_on_connect(&second, "203.0.113.20");

    known_client_info_t buf[MAX_KNOWN_CLIENTS];
    const known_client_info_t *e = snapshot_find(&second, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT_STR_EQ("new-label", e->label);
    ASSERT_STR_EQ("203.0.113.20", e->peer_ip);
    ASSERT_EQ((int)CLIENT_MODE_PRUNER, (int)e->mode);
    ASSERT_EQ(2, e->nb_connections_total);
    PASS();
}

TEST on_connect_duplicate_hello_same_session_does_not_duplicate_slot(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "dup", CLIENT_MODE_SEARCH);
    known_client_info_t buf[MAX_KNOWN_CLIENTS];

    known_clients_registry_on_connect(&id, "203.0.113.10");
    known_clients_registry_on_connect(&id, "203.0.113.10"); /* hello rejoué */

    const known_client_info_t *e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT_EQ(1, e->nb_active_sessions);
    ASSERT_EQ(2, e->nb_connections_total);

    /* La base de calcul du delta ne doit pas avoir été réarmée par le
     * deuxième hello : un CTRL_STATS après coup doit toujours cumuler par
     * accroissement depuis 0, pas depuis une nouvelle base. */
    control_stats_t stats = make_stats(50, 5, 10);
    known_clients_registry_on_stats(id.machine_uid, id.client_uid, &stats);
    e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);
    ASSERT_EQ(50, (int)e->total_pruner_checked);
    PASS();
}

/* ---------- on_stats --------------------------------------------------------- */

TEST on_stats_accumulates_by_delta_within_session(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "pruner-1", CLIENT_MODE_PRUNER);
    known_client_info_t buf[MAX_KNOWN_CLIENTS];
    known_clients_registry_on_connect(&id, "203.0.113.10");

    control_stats_t s1 = make_stats(100, 10, 50);
    known_clients_registry_on_stats(id.machine_uid, id.client_uid, &s1);
    const known_client_info_t *e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);
    ASSERT_EQ(100, (int)e->total_pruner_checked);
    ASSERT_EQ(10, (int)e->total_pruner_removed);
    ASSERT_EQ(50, (int)e->best_max_result);

    control_stats_t s2 = make_stats(150, 15, 40); /* max_result en baisse : pic conservé */
    known_clients_registry_on_stats(id.machine_uid, id.client_uid, &s2);
    e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);
    ASSERT_EQ(150, (int)e->total_pruner_checked);
    ASSERT_EQ(15, (int)e->total_pruner_removed);
    ASSERT_EQ(50, (int)e->best_max_result);
    PASS();
}

/* Le point délicat du module : le compteur PAR PROCESSUS d'une session
 * redémarre à 0 quand le client relance (nouvelle exécution, nouveau
 * client_uid), et le cumul de la MACHINE doit continuer à croître dessus au
 * lieu d'être écrasé par la valeur (plus basse) de la nouvelle session. */
TEST on_stats_accumulates_across_reconnect_instead_of_overwriting(void)
{
    uint32_t machine_seed = next_seed();
    known_client_info_t buf[MAX_KNOWN_CLIENTS];

    client_identity_t first = make_identity(machine_seed, next_seed(), "reboot-test", CLIENT_MODE_PRUNER);
    known_clients_registry_on_connect(&first, "203.0.113.10");
    control_stats_t s1 = make_stats(100, 20, 30);
    known_clients_registry_on_stats(first.machine_uid, first.client_uid, &s1);
    known_clients_registry_on_disconnect(first.machine_uid, first.client_uid);

    /* Process relancé : nouveau client_uid, compteurs pruner repartis à 0. */
    client_identity_t second = make_identity(machine_seed, next_seed(), "reboot-test", CLIENT_MODE_PRUNER);
    known_clients_registry_on_connect(&second, "203.0.113.10");
    control_stats_t s2 = make_stats(30, 5, 20);
    known_clients_registry_on_stats(second.machine_uid, second.client_uid, &s2);

    const known_client_info_t *e = snapshot_find(&second, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT_EQ(130, (int)e->total_pruner_checked); /* 100 + 30, jamais 30 seul */
    ASSERT_EQ(25, (int)e->total_pruner_removed);
    ASSERT_EQ(30, (int)e->best_max_result); /* pic conservé malgré 20 < 30 */
    PASS();
}

TEST on_stats_unknown_machine_is_noop(void)
{
    uint8_t machine_uid[MACHINE_UID_BYTES];
    uint8_t client_uid[CLIENT_UID_BYTES];
    seed_bytes(machine_uid, sizeof(machine_uid), next_seed());
    seed_bytes(client_uid, sizeof(client_uid), next_seed());

    int before = known_clients_registry_count();
    control_stats_t s = make_stats(1, 1, 1);
    known_clients_registry_on_stats(machine_uid, client_uid, &s); /* jamais connectée */
    ASSERT_EQ(before, known_clients_registry_count());
    PASS();
}

TEST on_stats_null_args_is_noop(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "x", CLIENT_MODE_SEARCH);
    known_clients_registry_on_connect(&id, "203.0.113.10");
    control_stats_t s = make_stats(1, 1, 1);

    known_clients_registry_on_stats(NULL, id.client_uid, &s);
    known_clients_registry_on_stats(id.machine_uid, NULL, &s);
    known_clients_registry_on_stats(id.machine_uid, id.client_uid, NULL);

    known_client_info_t buf[MAX_KNOWN_CLIENTS];
    const known_client_info_t *e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT_EQ(0, (int)e->total_pruner_checked);
    PASS();
}

/* ---------- on_disconnect ---------------------------------------------------- */

TEST on_disconnect_marks_disconnected_and_accumulates_uptime(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "uptime-test", CLIENT_MODE_SEARCH);
    known_client_info_t buf[MAX_KNOWN_CLIENTS];

    known_clients_registry_on_connect(&id, "203.0.113.10");
    usleep(1100 * 1000); /* garantit au moins 1s pleine d'écart en secondes entières */
    known_clients_registry_on_disconnect(id.machine_uid, id.client_uid);

    const known_client_info_t *e = snapshot_find(&id, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT_EQ(0, e->connected);
    ASSERT_EQ(0, e->nb_active_sessions);
    ASSERT(e->cumulative_uptime_seconds >= 1);
    PASS();
}

TEST on_disconnect_unknown_session_is_noop(void)
{
    uint8_t machine_uid[MACHINE_UID_BYTES];
    uint8_t client_uid[CLIENT_UID_BYTES];
    seed_bytes(machine_uid, sizeof(machine_uid), next_seed());
    seed_bytes(client_uid, sizeof(client_uid), next_seed());

    int before = known_clients_registry_count();
    known_clients_registry_on_disconnect(machine_uid, client_uid);
    ASSERT_EQ(before, known_clients_registry_count());
    PASS();
}

/* ---------- sessions simultanées -------------------------------------------- */

TEST multiple_concurrent_sessions_on_same_machine(void)
{
    uint32_t machine_seed = next_seed();
    client_identity_t search = make_identity(machine_seed, next_seed(), "multi", CLIENT_MODE_SEARCH);
    client_identity_t pruner = make_identity(machine_seed, next_seed(), "multi", CLIENT_MODE_PRUNER);
    known_client_info_t buf[MAX_KNOWN_CLIENTS];

    known_clients_registry_on_connect(&search, "203.0.113.10");
    known_clients_registry_on_connect(&pruner, "203.0.113.10");

    const known_client_info_t *e = snapshot_find(&search, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT_EQ(2, e->nb_active_sessions);
    ASSERT_EQ(1, e->connected);

    known_clients_registry_on_disconnect(search.machine_uid, search.client_uid);
    e = snapshot_find(&search, buf, MAX_KNOWN_CLIENTS);
    ASSERT_EQ(1, e->nb_active_sessions);
    ASSERT_EQ(1, e->connected); /* le pruner est encore actif */

    known_clients_registry_on_disconnect(pruner.machine_uid, pruner.client_uid);
    e = snapshot_find(&search, buf, MAX_KNOWN_CLIENTS);
    ASSERT_EQ(0, e->nb_active_sessions);
    ASSERT_EQ(0, e->connected);
    PASS();
}

TEST session_overflow_beyond_max_sessions_is_degraded_not_crashing(void)
{
    uint32_t machine_seed = next_seed();
    known_client_info_t buf[MAX_KNOWN_CLIENTS];
    client_identity_t last_identity;

    for (int i = 0; i < KNOWN_CLIENT_MAX_SESSIONS + 2; i++) {
        client_identity_t id = make_identity(machine_seed, next_seed(), "overflow", CLIENT_MODE_SEARCH);
        known_clients_registry_on_connect(&id, "203.0.113.10");
        last_identity = id;
    }

    const known_client_info_t *e = snapshot_find(&last_identity, buf, MAX_KNOWN_CLIENTS);
    ASSERT(e != NULL);
    ASSERT(e->nb_active_sessions <= KNOWN_CLIENT_MAX_SESSIONS);
    PASS();
}

/* ---------- snapshot / count -------------------------------------------------- */

TEST snapshot_null_or_zero_max_returns_zero(void)
{
    known_client_info_t buf[4];
    ASSERT_EQ(0, known_clients_registry_snapshot(NULL, 4));
    ASSERT_EQ(0, known_clients_registry_snapshot(buf, 0));
    ASSERT_EQ(0, known_clients_registry_snapshot(buf, -1));
    PASS();
}

TEST snapshot_respects_max_capacity(void)
{
    client_identity_t a = make_identity(next_seed(), next_seed(), "cap-a", CLIENT_MODE_SEARCH);
    client_identity_t b = make_identity(next_seed(), next_seed(), "cap-b", CLIENT_MODE_SEARCH);
    known_clients_registry_on_connect(&a, "203.0.113.10");
    known_clients_registry_on_connect(&b, "203.0.113.11");

    known_client_info_t buf[1];
    int n = known_clients_registry_snapshot(buf, 1);
    ASSERT_EQ(1, n);
    PASS();
}

TEST count_matches_snapshot_length(void)
{
    client_identity_t id = make_identity(next_seed(), next_seed(), "count-test", CLIENT_MODE_SEARCH);
    known_clients_registry_on_connect(&id, "203.0.113.10");

    int count = known_clients_registry_count();
    known_client_info_t buf[MAX_KNOWN_CLIENTS];
    int n = known_clients_registry_snapshot(buf, MAX_KNOWN_CLIENTS);
    ASSERT_EQ(count, n);
    PASS();
}

/* ---------- éviction (registre plein) ---------------------------------------
 *
 * Placé en DERNIER : ce test amène délibérément le registre à sa capacité
 * MAX_KNOWN_CLIENTS, en tenant compte des entrées déjà créées par les tests
 * précédents (known_clients_registry_count()) -- aucune hypothèse sur le
 * nombre exact d'entrées préexistantes. Après ce test, le registre reste
 * plein pour le reste du process : aucun autre test ne doit dépendre d'un
 * registre non saturé après celui-ci.
 */
TEST registry_full_evicts_oldest_disconnected_entry_never_a_connected_one(void)
{
    known_client_info_t buf[MAX_KNOWN_CLIENTS];

    int before = known_clients_registry_count();
    int to_fill = MAX_KNOWN_CLIENTS - before;
    ASSERT(to_fill > 0); /* sinon le test précédent a déjà tout rempli : borne trop basse */

    for (int i = 0; i < to_fill; i++) {
        client_identity_t id = make_identity(next_seed(), next_seed(), "filler", CLIENT_MODE_SEARCH);
        known_clients_registry_on_connect(&id, "203.0.113.10");
        known_clients_registry_on_disconnect(id.machine_uid, id.client_uid);
    }
    ASSERT_EQ(MAX_KNOWN_CLIENTS, known_clients_registry_count());

    /* Une machine encore CONNECTÉE ne doit jamais servir de victime : on la
     * garde active pendant toute la suite du test. Le registre étant plein,
     * son admission force déjà une première éviction (parmi les entrées
     * déconnectées -- laquelle précisément n'est pas figée ici : seule
     * l'invariant "jamais une entrée connectée" est vérifié). */
    client_identity_t survivor = make_identity(next_seed(), next_seed(), "survivor", CLIENT_MODE_SEARCH);
    known_clients_registry_on_connect(&survivor, "203.0.113.11");
    ASSERT_EQ(MAX_KNOWN_CLIENTS, known_clients_registry_count());
    ASSERT(snapshot_find(&survivor, buf, MAX_KNOWN_CLIENTS) != NULL);

    /* Deuxième admission sur un registre toujours plein : nouvelle éviction,
     * qui ne doit toujours jamais toucher `survivor` (actif) ni la machine
     * qui vient elle-même d'être admise. */
    client_identity_t newcomer = make_identity(next_seed(), next_seed(), "newcomer", CLIENT_MODE_SEARCH);
    known_clients_registry_on_connect(&newcomer, "203.0.113.12");

    ASSERT_EQ(MAX_KNOWN_CLIENTS, known_clients_registry_count());
    ASSERT(snapshot_find(&survivor, buf, MAX_KNOWN_CLIENTS) != NULL);
    ASSERT(snapshot_find(&newcomer, buf, MAX_KNOWN_CLIENTS) != NULL);
    PASS();
}

SUITE(known_clients_registry_suite)
{
    RUN_TEST(on_connect_creates_new_entry);
    RUN_TEST(on_connect_null_identity_is_noop);
    RUN_TEST(on_connect_accepts_null_peer_ip_as_empty_string);
    RUN_TEST(on_connect_second_machine_updates_label_peer_ip_mode);
    RUN_TEST(on_connect_duplicate_hello_same_session_does_not_duplicate_slot);

    RUN_TEST(on_stats_accumulates_by_delta_within_session);
    RUN_TEST(on_stats_accumulates_across_reconnect_instead_of_overwriting);
    RUN_TEST(on_stats_unknown_machine_is_noop);
    RUN_TEST(on_stats_null_args_is_noop);

    RUN_TEST(on_disconnect_marks_disconnected_and_accumulates_uptime);
    RUN_TEST(on_disconnect_unknown_session_is_noop);

    RUN_TEST(multiple_concurrent_sessions_on_same_machine);
    RUN_TEST(session_overflow_beyond_max_sessions_is_degraded_not_crashing);

    RUN_TEST(snapshot_null_or_zero_max_returns_zero);
    RUN_TEST(snapshot_respects_max_capacity);
    RUN_TEST(count_matches_snapshot_length);

    RUN_TEST(registry_full_evicts_oldest_disconnected_entry_never_a_connected_one);
}
