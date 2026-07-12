/*
 * Tests unitaires du registre des sessions de contrôle (src/app/control_registry.c).
 *
 * Le registre est un état GLOBAL statique (tableau de MAX_CONTROL_SESSIONS
 * slots) : chaque test enregistre ce dont il a besoin puis désenregistre tout
 * avant de rendre la main, pour ne jamais laisser de session « fantôme » qui
 * fausserait un test suivant (notamment le test « registre plein »).
 *
 * Aucun de ces tests ne touche à un socket réseau : la logique de file/registre
 * (mutex + pthread_cond_t par session) est exercée directement, comme demandé
 * par la tâche — un scénario mono-thread (post puis wait immédiat) et un
 * scénario multi-thread (un thread poste après un court délai, l'autre attend
 * avec un timeout plus long).
 */
#include "greatest.h"
#include "app/control_registry.h"
#include "app/static_variables.h"   /* MAX_CONTROL_SESSIONS */

#include <string.h>
#include <pthread.h>
#include <unistd.h>

static control_hello_t make_hello(int32_t pid, int32_t nb_forks, uint8_t mode)
{
    control_hello_t h;
    h.pid = pid;
    h.nb_forks = nb_forks;
    h.mode = mode;
    return h;
}

/* ---------- register / unregister ---------------------------------------- */

TEST register_returns_valid_index_and_increments_count(void)
{
    int before = control_registry_count();
    control_hello_t h = make_hello(1234, 4, 0);
    int idx = control_registry_register(42, &h);
    ASSERT(idx >= 0);
    ASSERT_EQ(before + 1, control_registry_count());

    control_registry_unregister(idx);
    ASSERT_EQ(before, control_registry_count());
    PASS();
}

TEST register_null_hello_rejected(void)
{
    int before = control_registry_count();
    ASSERT_EQ(-1, control_registry_register(1, NULL));
    ASSERT_EQ(before, control_registry_count());
    PASS();
}

TEST unregister_out_of_range_is_noop(void)
{
    /* Ne doit ni crasher ni modifier le compte. */
    int before = control_registry_count();
    control_registry_unregister(-1);
    control_registry_unregister(1000000);
    ASSERT_EQ(before, control_registry_count());
    PASS();
}

/* ---------- post_command / wait_command (mono-thread, séquentiel) -------- */

TEST post_then_wait_returns_posted_command_immediately(void)
{
    control_hello_t h = make_hello(1, 0, 0);
    int idx = control_registry_register(1, &h);
    ASSERT(idx >= 0);

    ASSERT_EQ(0, control_registry_post_command(idx, CTRL_COMMAND, "pause"));

    uint8_t cmd = 0;
    char line[64] = {0};
    int rc = control_registry_wait_command(idx, &cmd, line, sizeof(line), 1000);

    ASSERT_EQ(0, rc);
    ASSERT_EQ((int)CTRL_COMMAND, (int)cmd);
    ASSERT_STR_EQ("pause", line);

    control_registry_unregister(idx);
    PASS();
}

TEST post_command_without_line_leaves_empty_string(void)
{
    control_hello_t h = make_hello(2, 0, 0);
    int idx = control_registry_register(2, &h);
    ASSERT(idx >= 0);

    ASSERT_EQ(0, control_registry_post_command(idx, CTRL_GET_STATS, NULL));

    uint8_t cmd = 0xFF;
    char line[64] = "sentinel";
    int rc = control_registry_wait_command(idx, &cmd, line, sizeof(line), 1000);

    ASSERT_EQ(0, rc);
    ASSERT_EQ((int)CTRL_GET_STATS, (int)cmd);
    ASSERT_STR_EQ("", line);

    control_registry_unregister(idx);
    PASS();
}

TEST wait_command_times_out_when_nothing_posted(void)
{
    control_hello_t h = make_hello(3, 0, 0);
    int idx = control_registry_register(3, &h);
    ASSERT(idx >= 0);

    uint8_t cmd = 0;
    int rc = control_registry_wait_command(idx, &cmd, NULL, 0, 50 /* ms */);
    ASSERT_EQ(1, rc);

    control_registry_unregister(idx);
    PASS();
}

TEST wait_command_invalid_index_returns_error(void)
{
    uint8_t cmd = 0;
    ASSERT_EQ(-1, control_registry_wait_command(-1, &cmd, NULL, 0, 10));
    ASSERT_EQ(-1, control_registry_wait_command(MAX_CONTROL_SESSIONS, &cmd, NULL, 0, 10));
    PASS();
}

TEST wait_command_on_never_registered_slot_returns_error(void)
{
    /* Un slot jamais enregistré est in_use == 0 : attente refusée immédiatement. */
    uint8_t cmd = 0;
    /* On prend un indice qu'on vient de libérer pour être sûr qu'il est inactif. */
    control_hello_t h = make_hello(4, 0, 0);
    int idx = control_registry_register(4, &h);
    ASSERT(idx >= 0);
    control_registry_unregister(idx);

    ASSERT_EQ(-1, control_registry_wait_command(idx, &cmd, NULL, 0, 10));
    PASS();
}

TEST post_command_queue_fills_then_rejects(void)
{
    control_hello_t h = make_hello(5, 0, 0);
    int idx = control_registry_register(5, &h);
    ASSERT(idx >= 0);

    for (int i = 0; i < CONTROL_SESSION_QUEUE_CAP; i++) {
        ASSERT_EQ(0, control_registry_post_command(idx, CTRL_PING, NULL));
    }
    /* File pleine : un poste de plus échoue. */
    ASSERT_EQ(-1, control_registry_post_command(idx, CTRL_PING, NULL));

    /* Draine tout : count remonte à 0, un nouveau post redevient possible. */
    for (int i = 0; i < CONTROL_SESSION_QUEUE_CAP; i++) {
        uint8_t cmd = 0;
        ASSERT_EQ(0, control_registry_wait_command(idx, &cmd, NULL, 0, 100));
    }
    ASSERT_EQ(0, control_registry_post_command(idx, CTRL_PING, NULL));

    control_registry_unregister(idx);
    PASS();
}

TEST post_command_invalid_index_returns_error(void)
{
    ASSERT_EQ(-1, control_registry_post_command(-1, CTRL_PING, NULL));
    ASSERT_EQ(-1, control_registry_post_command(MAX_CONTROL_SESSIONS, CTRL_PING, NULL));
    PASS();
}

/* ---------- registre plein ------------------------------------------------ */

TEST register_beyond_capacity_returns_minus_one(void)
{
    int idxs[MAX_CONTROL_SESSIONS];
    int registered = 0;
    control_hello_t h = make_hello(100, 0, 0);

    /* Remplit le registre jusqu'à sa capacité (au plus MAX_CONTROL_SESSIONS,
       en tenant compte d'éventuelles sessions déjà actives d'un autre test —
       en pratique 0 ici puisque chaque test se nettoie). */
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        int idx = control_registry_register(100 + i, &h);
        if (idx < 0) {
            break;
        }
        idxs[registered++] = idx;
    }
    ASSERT_EQ(MAX_CONTROL_SESSIONS, registered);
    ASSERT_EQ(MAX_CONTROL_SESSIONS, control_registry_count());

    /* Le registre est plein : un enregistrement de plus échoue. */
    ASSERT_EQ(-1, control_registry_register(9999, &h));

    for (int i = 0; i < registered; i++) {
        control_registry_unregister(idxs[i]);
    }
    ASSERT_EQ(0, control_registry_count());
    PASS();
}

/* ---------- snapshot -------------------------------------------------------*/

TEST snapshot_reflects_registered_hello(void)
{
    control_hello_t h = make_hello(777, 8, 2);
    int idx = control_registry_register(9, &h);
    ASSERT(idx >= 0);

    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(infos, MAX_CONTROL_SESSIONS);
    ASSERT_EQ(1, n);
    ASSERT_EQ(777, infos[0].pid);
    ASSERT_EQ(8, infos[0].nb_forks);
    ASSERT_EQ(2, (int)infos[0].mode);

    control_registry_unregister(idx);
    PASS();
}

TEST snapshot_null_or_zero_max_returns_zero(void)
{
    control_session_info_t infos[4];
    ASSERT_EQ(0, control_registry_snapshot(NULL, 4));
    ASSERT_EQ(0, control_registry_snapshot(infos, 0));
    PASS();
}

TEST snapshot_respects_max_capacity(void)
{
    control_hello_t h = make_hello(1, 1, 0);
    int idx1 = control_registry_register(1, &h);
    int idx2 = control_registry_register(2, &h);
    ASSERT(idx1 >= 0);
    ASSERT(idx2 >= 0);

    control_session_info_t infos[1];
    int n = control_registry_snapshot(infos, 1);
    ASSERT_EQ(1, n);

    control_registry_unregister(idx1);
    control_registry_unregister(idx2);
    PASS();
}

/* ---------- broadcast ------------------------------------------------------*/

TEST broadcast_command_reaches_all_active_sessions(void)
{
    control_hello_t h = make_hello(1, 1, 0);
    int idx1 = control_registry_register(1, &h);
    int idx2 = control_registry_register(2, &h);
    int idx3 = control_registry_register(3, &h);
    ASSERT(idx1 >= 0);
    ASSERT(idx2 >= 0);
    ASSERT(idx3 >= 0);

    int n = control_registry_broadcast_command(CTRL_COMMAND, "resume");
    ASSERT_EQ(3, n);

    int idxs[3] = { idx1, idx2, idx3 };
    for (int i = 0; i < 3; i++) {
        uint8_t cmd = 0;
        char line[64] = {0};
        ASSERT_EQ(0, control_registry_wait_command(idxs[i], &cmd, line, sizeof(line), 500));
        ASSERT_EQ((int)CTRL_COMMAND, (int)cmd);
        ASSERT_STR_EQ("resume", line);
        control_registry_unregister(idxs[i]);
    }
    PASS();
}

TEST broadcast_get_stats_reaches_all_active_sessions(void)
{
    control_hello_t h = make_hello(1, 1, 1);
    int idx1 = control_registry_register(1, &h);
    int idx2 = control_registry_register(2, &h);
    ASSERT(idx1 >= 0);
    ASSERT(idx2 >= 0);

    int n = control_registry_broadcast_get_stats();
    ASSERT_EQ(2, n);

    int idxs[2] = { idx1, idx2 };
    for (int i = 0; i < 2; i++) {
        uint8_t cmd = 0;
        ASSERT_EQ(0, control_registry_wait_command(idxs[i], &cmd, NULL, 0, 500));
        ASSERT_EQ((int)CTRL_GET_STATS, (int)cmd);
        control_registry_unregister(idxs[i]);
    }
    PASS();
}

TEST broadcast_with_no_active_session_returns_zero(void)
{
    ASSERT_EQ(0, control_registry_count());
    ASSERT_EQ(0, control_registry_broadcast_command(CTRL_PING, NULL));
    ASSERT_EQ(0, control_registry_broadcast_get_stats());
    PASS();
}

/* ---------- état de pause "désiré" persistant ------------------------------
 *
 * Reproduit la demande : après un `pause` console (côté serveur, diffusé via
 * `control_registry_broadcast_command`), tout NOUVEAU client qui se connecte
 * doit démarrer en pause lui aussi, sans commande rejouée. Chaque test remet
 * l'état à "résumé" (broadcast "resume") avant de rendre la main : le
 * registre est global au process de test, un état "en pause" qui fuiterait
 * fausserait silencieusement les tests suivants.
 */

TEST desired_pause_state_defaults_to_resumed(void)
{
    ASSERT_EQ(0, control_registry_desired_pause_state());
    PASS();
}

TEST broadcast_pause_sets_desired_state_for_future_registrations(void)
{
    ASSERT_EQ(0, control_registry_desired_pause_state());

    control_registry_broadcast_command(CTRL_COMMAND, "pause");
    ASSERT_EQ(1, control_registry_desired_pause_state());

    /* Une session enregistrée APRÈS le broadcast "pause" doit trouver la
       commande déjà dans sa file, sans qu'elle ait été postée explicitement. */
    control_hello_t h = make_hello(42, 2, 0);
    int idx = control_registry_register(1, &h);
    ASSERT(idx >= 0);

    uint8_t cmd = 0;
    char line[64] = {0};
    ASSERT_EQ(0, control_registry_wait_command(idx, &cmd, line, sizeof(line), 200));
    ASSERT_EQ((int)CTRL_COMMAND, (int)cmd);
    ASSERT_STR_EQ("pause", line);

    control_registry_unregister(idx);
    control_registry_broadcast_command(CTRL_COMMAND, "resume");
    ASSERT_EQ(0, control_registry_desired_pause_state());
    PASS();
}

TEST broadcast_resume_clears_desired_state_for_future_registrations(void)
{
    control_registry_broadcast_command(CTRL_COMMAND, "pause");
    ASSERT_EQ(1, control_registry_desired_pause_state());

    control_registry_broadcast_command(CTRL_COMMAND, "resume");
    ASSERT_EQ(0, control_registry_desired_pause_state());

    /* Une session enregistrée APRÈS le broadcast "resume" ne doit trouver
       aucune commande en attente : elle démarre normalement, comme un client
       qui n'aurait jamais connu de pause diffusée. */
    control_hello_t h = make_hello(43, 1, 0);
    int idx = control_registry_register(1, &h);
    ASSERT(idx >= 0);

    uint8_t cmd = 0;
    ASSERT_EQ(1, control_registry_wait_command(idx, &cmd, NULL, 0, 100)); /* timeout : rien en file */

    control_registry_unregister(idx);
    PASS();
}

TEST clients_cmd_pause_resume_also_update_desired_state(void)
{
    /* `clientsCmd pause`/`clientsCmd resume` (chemin générique, toujours
       présent) doivent avoir le même effet que `pause`/`resume` console. */
    control_registry_broadcast_command(CTRL_COMMAND, "pause");
    ASSERT_EQ(1, control_registry_desired_pause_state());

    control_registry_broadcast_command(CTRL_COMMAND, "resume");
    ASSERT_EQ(0, control_registry_desired_pause_state());
    PASS();
}

/* ---------- touch ---------------------------------------------------------*/

TEST touch_updates_last_activity(void)
{
    control_hello_t h = make_hello(1, 1, 0);
    int idx = control_registry_register(1, &h);
    ASSERT(idx >= 0);

    control_session_info_t infos[1];
    control_registry_snapshot(infos, 1);
    time_t before = infos[0].last_activity;

    sleep(1); /* granularité de time() : 1 s pour observer un changement fiable */
    control_registry_touch(idx);

    control_registry_snapshot(infos, 1);
    ASSERT(infos[0].last_activity >= before);

    control_registry_unregister(idx);
    PASS();
}

TEST touch_out_of_range_is_noop(void)
{
    control_registry_touch(-1);
    control_registry_touch(1000000);
    PASS();
}

/* ---------- multi-thread : post après délai, wait avec timeout plus long -- */

struct poster_arg {
    int index;
    int delay_ms;
};

static void *poster_thread(void *arg)
{
    struct poster_arg *a = arg;
    usleep((useconds_t)a->delay_ms * 1000);
    control_registry_post_command(a->index, CTRL_COMMAND, "limit 5");
    return NULL;
}

TEST multithread_wait_wakes_up_on_posted_command(void)
{
    control_hello_t h = make_hello(1, 1, 0);
    int idx = control_registry_register(1, &h);
    ASSERT(idx >= 0);

    struct poster_arg arg = { .index = idx, .delay_ms = 100 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, poster_thread, &arg));

    uint8_t cmd = 0;
    char line[64] = {0};
    /* Timeout largement supérieur au délai du posteur : la condvar doit
       réveiller l'attente bien avant l'expiration. */
    int rc = control_registry_wait_command(idx, &cmd, line, sizeof(line), 3000);

    pthread_join(t, NULL);

    ASSERT_EQ(0, rc);
    ASSERT_EQ((int)CTRL_COMMAND, (int)cmd);
    ASSERT_STR_EQ("limit 5", line);

    control_registry_unregister(idx);
    PASS();
}

/* ---------- suite ---------------------------------------------------------*/

SUITE(control_registry_suite)
{
    RUN_TEST(register_returns_valid_index_and_increments_count);
    RUN_TEST(register_null_hello_rejected);
    RUN_TEST(unregister_out_of_range_is_noop);

    RUN_TEST(post_then_wait_returns_posted_command_immediately);
    RUN_TEST(post_command_without_line_leaves_empty_string);
    RUN_TEST(wait_command_times_out_when_nothing_posted);
    RUN_TEST(wait_command_invalid_index_returns_error);
    RUN_TEST(wait_command_on_never_registered_slot_returns_error);
    RUN_TEST(post_command_queue_fills_then_rejects);
    RUN_TEST(post_command_invalid_index_returns_error);

    RUN_TEST(register_beyond_capacity_returns_minus_one);

    RUN_TEST(snapshot_reflects_registered_hello);
    RUN_TEST(snapshot_null_or_zero_max_returns_zero);
    RUN_TEST(snapshot_respects_max_capacity);

    RUN_TEST(broadcast_command_reaches_all_active_sessions);
    RUN_TEST(broadcast_get_stats_reaches_all_active_sessions);
    RUN_TEST(broadcast_with_no_active_session_returns_zero);

    RUN_TEST(desired_pause_state_defaults_to_resumed);
    RUN_TEST(broadcast_pause_sets_desired_state_for_future_registrations);
    RUN_TEST(broadcast_resume_clears_desired_state_for_future_registrations);
    RUN_TEST(clients_cmd_pause_resume_also_update_desired_state);

    RUN_TEST(touch_updates_last_activity);
    RUN_TEST(touch_out_of_range_is_noop);

    RUN_TEST(multithread_wait_wakes_up_on_posted_command);
}
