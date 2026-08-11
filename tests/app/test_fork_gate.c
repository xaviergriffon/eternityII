/*
 * Tests unitaires de l'infrastructure de quiescence coopérative
 * (src/app/fork_gate.c).
 *
 * Chaque test appelle fork_gate_reset() avant ET après pour repartir d'un
 * état propre : le module est un singleton process-wide, partagé entre tous
 * les tests de la suite (comme childrens_pid/NB_THREADS ailleurs).
 */
#include "greatest.h"
#include "app/fork_gate.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

/* ============================ register / unregister ======================= */

TEST fork_gate_register_returns_distinct_slots(void)
{
    fork_gate_reset();
    int a = fork_gate_register("a");
    int b = fork_gate_register("b");
    ASSERT(a >= 0);
    ASSERT(b >= 0);
    ASSERT(a != b);
    fork_gate_unregister(a);
    fork_gate_unregister(b);
    fork_gate_reset();
    PASS();
}

TEST fork_gate_register_fails_when_table_full(void)
{
    fork_gate_reset();
    int slots[FORK_GATE_MAX_PARTICIPANTS];
    for (int i = 0; i < FORK_GATE_MAX_PARTICIPANTS; i++) {
        slots[i] = fork_gate_register("p");
        ASSERT(slots[i] >= 0);
    }
    ASSERT_EQ_FMT((int)FORK_GATE_FULL, fork_gate_register("overflow"), "%d");

    for (int i = 0; i < FORK_GATE_MAX_PARTICIPANTS; i++) {
        fork_gate_unregister(slots[i]);
    }
    /* Un slot libéré redevient disponible. */
    int again = fork_gate_register("again");
    ASSERT(again >= 0);
    fork_gate_unregister(again);
    fork_gate_reset();
    PASS();
}

/* checkpoint/unregister/mark_blocked sur un slot négatif : no-op, jamais de crash
   (un appelant dont fork_gate_register a échoué peut appeler sans condition). */
TEST fork_gate_negative_slot_is_noop(void)
{
    fork_gate_reset();
    fork_gate_checkpoint(-1);
    fork_gate_mark_blocked(-1, 1);
    fork_gate_mark_blocked(-1, 0);
    fork_gate_unregister(-1);
    fork_gate_unregister(FORK_GATE_MAX_PARTICIPANTS + 5);
    fork_gate_reset();
    PASS();
}

/* ============================ chemin rapide ================================ */

/* Sans quiescence demandée, fork_gate_checkpoint doit revenir immédiatement
   (chemin rapide) : ce test échouerait par timeout du runner s'il bloquait. */
TEST fork_gate_checkpoint_returns_immediately_without_quiesce(void)
{
    fork_gate_reset();
    int slot = fork_gate_register("solo");
    for (int i = 0; i < 1000; i++) {
        fork_gate_checkpoint(slot);
    }
    ASSERT_EQ_FMT(0, fork_gate_is_quiescing(), "%d");
    fork_gate_unregister(slot);
    fork_gate_reset();
    PASS();
}

/* ============================ mark_blocked ================================= */

/* Un participant marqué BLOCKED est déjà quiescent : la demande n'a pas
   besoin d'attendre qu'il se gare sur la condvar. */
TEST fork_gate_blocked_participant_is_already_quiescent(void)
{
    fork_gate_reset();
    int slot = fork_gate_register("blocked");
    fork_gate_mark_blocked(slot, 1);

    fork_gate_result_t rc = fork_gate_request_quiesce(300);
    ASSERT_EQ_FMT((int)FORK_GATE_QUIESCED, (int)rc, "%d");

    fork_gate_release_quiesce();
    fork_gate_mark_blocked(slot, 0);
    fork_gate_unregister(slot);
    fork_gate_reset();
    PASS();
}

/* ============================ quiescence multi-thread ====================== */

static volatile int g_worker_stop = 0;
static volatile int g_worker_slot = -1;
static volatile int g_worker_checkpoints = 0;

static void *quiesce_worker(void *arg)
{
    (void)arg;
    int slot = fork_gate_register("worker");
    /* Publié en dernier : le thread principal attend cette écriture avant de
       considérer le worker "prêt", cf. boucle d'attente ci-dessous. */
    g_worker_slot = slot;
    while (!g_worker_stop) {
        fork_gate_checkpoint(slot);
        g_worker_checkpoints++;
        usleep(1000);
    }
    fork_gate_unregister(slot);
    return NULL;
}

/* Un thread qui boucle sur fork_gate_checkpoint doit se garer dès qu'une
   quiescence est demandée : fork_gate_request_quiesce doit réussir bien avant
   son budget, puis fork_gate_release_quiesce doit le laisser reprendre. */
TEST fork_gate_request_quiesce_succeeds_when_worker_parks(void)
{
    fork_gate_reset();
    g_worker_stop = 0;
    g_worker_slot = -1;
    g_worker_checkpoints = 0;

    pthread_t t;
    ASSERT_EQ_FMT(0, pthread_create(&t, NULL, quiesce_worker, NULL), "%d");
    while (g_worker_slot < 0) {
        usleep(1000);
    }
    /* Laisse le worker faire au moins un tour de boucle avant la demande,
       pour exercer le chemin "déjà en cours d'exécution -> se gare". */
    usleep(5000);

    fork_gate_result_t rc = fork_gate_request_quiesce(1000);
    ASSERT_EQ_FMT((int)FORK_GATE_QUIESCED, (int)rc, "%d");

    int checkpoints_while_quiesced = g_worker_checkpoints;
    /* Le worker est garé : il ne doit plus progresser tant que la
       quiescence n'est pas levée. */
    usleep(20000);
    ASSERT_EQ_FMT(checkpoints_while_quiesced, g_worker_checkpoints, "%d");

    fork_gate_release_quiesce();
    /* Laisse le temps au worker de reprendre au moins un tour. */
    int reached = 0;
    for (int i = 0; i < 200 && !reached; i++) {
        usleep(1000);
        if (g_worker_checkpoints > checkpoints_while_quiesced) {
            reached = 1;
        }
    }
    ASSERT(reached);

    g_worker_stop = 1;
    pthread_join(t, NULL);
    fork_gate_reset();
    PASS();
}

/* Un participant enregistré mais qui ne rejoint jamais son checkpoint (bug,
   ou simplement un thread très occupé) fait échouer la demande sur timeout —
   et la demande est ANNULÉE (jamais de fork dans le doute, cf. D2). */
TEST fork_gate_request_quiesce_times_out_on_stuck_participant(void)
{
    fork_gate_reset();
    int stuck = fork_gate_register("stuck");
    ASSERT(stuck >= 0);

    fork_gate_result_t rc = fork_gate_request_quiesce(50);
    ASSERT_EQ_FMT((int)FORK_GATE_TIMEOUT, (int)rc, "%d");
    /* La demande a bien été annulée : plus aucune quiescence en cours. */
    ASSERT_EQ_FMT(0, fork_gate_is_quiescing(), "%d");

    fork_gate_unregister(stuck);
    fork_gate_reset();
    PASS();
}

/* ============================ primitives d'E/S ============================== */

/* Smoke test : acquire/release ne doivent jamais se bloquer mutuellement
   (même thread, appels successifs) — ces primitives sont ensuite appelées
   réellement autour d'un fork() par l'orchestrateur (fork_orchestrator.c). */
TEST fork_gate_io_locks_acquire_and_release(void)
{
    fork_gate_acquire_io_locks();
    fork_gate_release_io_locks();
    PASS();
}

SUITE(fork_gate_suite)
{
    RUN_TEST(fork_gate_register_returns_distinct_slots);
    RUN_TEST(fork_gate_register_fails_when_table_full);
    RUN_TEST(fork_gate_negative_slot_is_noop);
    RUN_TEST(fork_gate_checkpoint_returns_immediately_without_quiesce);
    RUN_TEST(fork_gate_blocked_participant_is_already_quiescent);
    RUN_TEST(fork_gate_request_quiesce_succeeds_when_worker_parks);
    RUN_TEST(fork_gate_request_quiesce_times_out_on_stuck_participant);
    RUN_TEST(fork_gate_io_locks_acquire_and_release);
}
