/*
 * Tests unitaires de l'orchestrateur de démarrage différé
 * (src/app/fork_orchestrator.c).
 *
 * Deux couches :
 *  - orchestrator_step / orchestrator_countdown_elapsed : cœur PUR, testé de
 *    façon EXHAUSTIVE (chaque état x chaque événement).
 *  - fork_orchestrator_post_event / _snapshot / _stage_config_line /
 *    _format_staged_config : driver thread-safe, testé plus légèrement
 *    (comportement observable, pas de fork réel — cf. make test-integration
 *    pour orchestrator_spawn_forks/fork_orchestrator_run).
 *
 * fork_orchestrator_reset() est appelé avant ET après chaque test qui touche
 * l'état partagé : le module est un singleton process-wide (comme fork_gate).
 */
#include "greatest.h"
#include "app/fork_orchestrator.h"
#include "app/static_variables.h"

#include <string.h>

/* ============================ orchestrator_step ============================ */

static const orch_state_t ALL_STATES[] = {
    ORCH_WAITING_CONFIG, ORCH_COUNTDOWN, ORCH_CONFIGURING,
    ORCH_RUNNING, ORCH_STOPPING, ORCH_APPLYING, ORCH_EXITING,
};
#define N_STATES (int)(sizeof(ALL_STATES) / sizeof(ALL_STATES[0]))

/* EV_CONFIG_BEGUN : WAITING_CONFIG/COUNTDOWN -> CONFIGURING ; self-loop
   partout ailleurs. Jamais une erreur, jamais de fork. */
TEST orchestrator_step_config_begun_matrix(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];
        orch_actions_t out;
        orch_state_t next = orchestrator_step(s, EV_CONFIG_BEGUN, 0, &out);

        orch_state_t expected = (s == ORCH_WAITING_CONFIG || s == ORCH_COUNTDOWN)
            ? ORCH_CONFIGURING : s;
        ASSERT_EQ_FMT((int)expected, (int)next, "%d");
        ASSERT_EQ_FMT(0, out.spawn_forks, "%d");
        ASSERT_EQ_FMT((int)ORCH_OK, (int)out.error, "%d");
    }
    PASS();
}

/* EV_START : WAITING_CONFIG/COUNTDOWN/CONFIGURING -> RUNNING (spawn_forks=1) ;
   RUNNING/STOPPING/APPLYING/EXITING -> inchangé + ALREADY_RUNNING. */
TEST orchestrator_step_start_matrix(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];
        orch_actions_t out;
        orch_state_t next = orchestrator_step(s, EV_START, 0, &out);

        int busy = (s == ORCH_RUNNING || s == ORCH_STOPPING ||
                    s == ORCH_APPLYING || s == ORCH_EXITING);
        if (busy) {
            ASSERT_EQ_FMT((int)s, (int)next, "%d");
            ASSERT_EQ_FMT(0, out.spawn_forks, "%d");
            ASSERT_EQ_FMT((int)ORCH_ERR_ALREADY_RUNNING, (int)out.error, "%d");
        } else {
            ASSERT_EQ_FMT((int)ORCH_RUNNING, (int)next, "%d");
            ASSERT_EQ_FMT(1, out.spawn_forks, "%d");
            ASSERT_EQ_FMT((int)ORCH_OK, (int)out.error, "%d");
        }
    }
    PASS();
}

/* EV_STOP_FORKS / EV_RESTART : réservés pour un futur usage — état inchangé
   + UNSUPPORTED, dans TOUS les états. */
TEST orchestrator_step_stop_forks_and_restart_are_unsupported_everywhere(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];

        orch_actions_t out1;
        orch_state_t next1 = orchestrator_step(s, EV_STOP_FORKS, 0, &out1);
        ASSERT_EQ_FMT((int)s, (int)next1, "%d");
        ASSERT_EQ_FMT(0, out1.spawn_forks, "%d");
        ASSERT_EQ_FMT((int)ORCH_ERR_UNSUPPORTED, (int)out1.error, "%d");

        orch_actions_t out2;
        orch_state_t next2 = orchestrator_step(s, EV_RESTART, 0, &out2);
        ASSERT_EQ_FMT((int)s, (int)next2, "%d");
        ASSERT_EQ_FMT(0, out2.spawn_forks, "%d");
        ASSERT_EQ_FMT((int)ORCH_ERR_UNSUPPORTED, (int)out2.error, "%d");
    }
    PASS();
}

/* EV_EXIT : toujours accepté, depuis n'importe quel état -> EXITING, jamais
   une erreur (personne ne poste encore cet événement en production, mais la
   transition pure doit déjà être correcte). */
TEST orchestrator_step_exit_matrix(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];
        orch_actions_t out;
        orch_state_t next = orchestrator_step(s, EV_EXIT, 0, &out);
        ASSERT_EQ_FMT((int)ORCH_EXITING, (int)next, "%d");
        ASSERT_EQ_FMT(0, out.spawn_forks, "%d");
        ASSERT_EQ_FMT((int)ORCH_OK, (int)out.error, "%d");
    }
    PASS();
}

/* EV_CHILD_DIED : observabilité pure (pas d'auto-respawn) — self-loop dans
   tous les états, jamais une erreur. */
TEST orchestrator_step_child_died_is_always_a_self_loop(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];
        orch_actions_t out;
        orch_state_t next = orchestrator_step(s, EV_CHILD_DIED, 0, &out);
        ASSERT_EQ_FMT((int)s, (int)next, "%d");
        ASSERT_EQ_FMT(0, out.spawn_forks, "%d");
        ASSERT_EQ_FMT((int)ORCH_OK, (int)out.error, "%d");
    }
    PASS();
}

/* `out == NULL` accepté (l'appelant ne veut pas le détail des actions) : ne
   doit jamais déréférencer un pointeur NULL. */
TEST orchestrator_step_accepts_null_out(void)
{
    orch_state_t next = orchestrator_step(ORCH_WAITING_CONFIG, EV_START, 0, NULL);
    ASSERT_EQ_FMT((int)ORCH_RUNNING, (int)next, "%d");
    PASS();
}

/* ============================ orchestrator_countdown_elapsed ================ */

TEST countdown_elapsed_before_at_and_after_deadline(void)
{
    ASSERT_EQ_FMT(0, orchestrator_countdown_elapsed(5000, 4999), "%d");
    ASSERT_EQ_FMT(1, orchestrator_countdown_elapsed(5000, 5000), "%d");
    ASSERT_EQ_FMT(1, orchestrator_countdown_elapsed(5000, 5001), "%d");
    PASS();
}

/* ============================ driver thread-safe ============================ */

/* WAITING_CONFIG + EV_START (post_event) : transition immédiate visible via
   snapshot, spawn décidé (observable seulement indirectement — le fork réel
   n'est pas déclenché par le test, cf. make test-integration). */
TEST post_event_start_from_waiting_config_transitions_to_running(void)
{
    fork_orchestrator_reset();

    orch_actions_t actions;
    fork_orchestrator_post_event(EV_START, &actions);
    ASSERT_EQ_FMT(1, actions.spawn_forks, "%d");
    ASSERT_EQ_FMT((int)ORCH_OK, (int)actions.error, "%d");

    orch_state_t state;
    fork_orchestrator_snapshot(&state, NULL);
    ASSERT_EQ_FMT((int)ORCH_RUNNING, (int)state, "%d");

    fork_orchestrator_reset();
    PASS();
}

/* Un second EV_START une fois RUNNING échoue immédiatement (pas de latence
   de sondage : le résultat est synchrone). */
TEST post_event_start_twice_reports_already_running(void)
{
    fork_orchestrator_reset();

    orch_actions_t first;
    fork_orchestrator_post_event(EV_START, &first);
    ASSERT_EQ_FMT((int)ORCH_OK, (int)first.error, "%d");

    orch_actions_t second;
    fork_orchestrator_post_event(EV_START, &second);
    ASSERT_EQ_FMT((int)ORCH_ERR_ALREADY_RUNNING, (int)second.error, "%d");
    ASSERT_EQ_FMT(0, second.spawn_forks, "%d");

    fork_orchestrator_reset();
    PASS();
}

/* fork_orchestrator_snapshot : hors COUNTDOWN, le temps restant est -1. */
TEST snapshot_countdown_remaining_is_negative_outside_countdown(void)
{
    fork_orchestrator_reset();

    orch_state_t state;
    long remaining = 12345;
    fork_orchestrator_snapshot(&state, &remaining);
    ASSERT_EQ_FMT((int)ORCH_WAITING_CONFIG, (int)state, "%d");
    ASSERT_EQ_FMT(-1L, remaining, "%ld");

    fork_orchestrator_reset();
    PASS();
}

/* fork_orchestrator_stage_config_line : une clé/valeur valide est acceptée,
   annule le décompte (COUNTDOWN -> CONFIGURING) et apparaît dans le format
   de la configuration en préparation. */
TEST stage_config_line_valid_key_cancels_countdown_and_is_formatted(void)
{
    fork_orchestrator_reset();
    orch_actions_t actions;
    fork_orchestrator_post_event(EV_CONFIG_BEGUN, &actions); /* no-op ici, juste pour partir propre */
    (void)actions;

    /* Force l'état à COUNTDOWN via un chemin déjà testé (EV_START le
       quitterait) : on part de WAITING_CONFIG et on vérifie juste
       l'annulation du décompte au sens de la transition CONFIGURING, quel
       que soit l'état de départ éligible. */
    client_config_line_status_t status = fork_orchestrator_stage_config_line("nb_forks = 4");
    ASSERT_EQ_FMT((int)CLIENT_CONFIG_LINE_SET, (int)status, "%d");

    orch_state_t state;
    fork_orchestrator_snapshot(&state, NULL);
    ASSERT_EQ_FMT((int)ORCH_CONFIGURING, (int)state, "%d");

    char buf[256];
    int n = fork_orchestrator_format_staged_config(buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT(strstr(buf, "nb_forks") != NULL);
    ASSERT(strstr(buf, "4") != NULL);

    fork_orchestrator_reset();
    PASS();
}

/* Une clé inconnue ou une valeur invalide n'annule PAS le décompte (rien à
   annuler ici puisqu'on part de WAITING_CONFIG, mais l'état ne doit
   PAS devenir CONFIGURING — une faute de frappe ne doit jamais faire perdre
   l'auto-démarrage). */
TEST stage_config_line_invalid_does_not_transition(void)
{
    fork_orchestrator_reset();

    client_config_line_status_t status_unknown = fork_orchestrator_stage_config_line("bogus_key = 1");
    ASSERT_EQ_FMT((int)CLIENT_CONFIG_LINE_UNKNOWN_KEY, (int)status_unknown, "%d");

    orch_state_t state;
    fork_orchestrator_snapshot(&state, NULL);
    ASSERT_EQ_FMT((int)ORCH_WAITING_CONFIG, (int)state, "%d");

    client_config_line_status_t status_invalid = fork_orchestrator_stage_config_line("nb_forks = not_a_number");
    ASSERT_EQ_FMT((int)CLIENT_CONFIG_LINE_INVALID_VALUE, (int)status_invalid, "%d");
    fork_orchestrator_snapshot(&state, NULL);
    ASSERT_EQ_FMT((int)ORCH_WAITING_CONFIG, (int)state, "%d");

    fork_orchestrator_reset();
    PASS();
}

/* fork_orchestrator_merge_staged_config superpose les clés stagées sur une
   configuration déjà remplie, sans toucher aux clés non stagées — c'est ce
   qui permet à `configSave` de persister une valeur préparée par
   `config <clé> <valeur>` (sans ça, elle était silencieusement perdue). */
TEST merge_staged_config_overlays_only_staged_keys(void)
{
    fork_orchestrator_reset();
    fork_orchestrator_stage_config_line("nb_forks = 9");
    fork_orchestrator_stage_config_line("server_host = staged.example");

    client_config_t effective;
    client_config_init(&effective);
    effective.has_nb_forks = 1;
    effective.nb_forks = 2; /* doit être écrasé par la valeur stagée (9) */
    effective.has_max_stock_by_thread = 1;
    effective.max_stock_by_thread = 300; /* non stagée : doit survivre intacte */

    fork_orchestrator_merge_staged_config(&effective);

    ASSERT(effective.has_nb_forks);
    ASSERT_EQ_FMT(9, effective.nb_forks, "%d");
    ASSERT(effective.has_server_host);
    ASSERT(effective.server_host != NULL);
    ASSERT_STR_EQ("staged.example", effective.server_host);
    ASSERT(effective.has_max_stock_by_thread);
    ASSERT_EQ_FMT(300, effective.max_stock_by_thread, "%d");
    ASSERT(!effective.has_parts_file); /* jamais stagée, jamais inventée */

    client_config_free(&effective);
    fork_orchestrator_reset();
    PASS();
}

/* fork_orchestrator_apply_staged_config applique IMMÉDIATEMENT (sans
   redémarrage) la configuration stagée aux globales en vigueur — c'est ce qui
   permet à "config nb_forks N" suivi de "start" de forker avec la nouvelle
   valeur tout de suite, plutôt que de nécessiter un redémarrage du process
   (seul `configSave`+redémarrage le faisait avant ce correctif). */
TEST apply_staged_config_writes_globals_immediately(void)
{
    fork_orchestrator_reset();
    int saved_nb_threads = NB_THREADS;
    int saved_max_stock = max_stock_by_thread;

    fork_orchestrator_stage_config_line("nb_forks = 7");
    fork_orchestrator_stage_config_line("max_stock_by_thread = 42");

    fork_orchestrator_apply_staged_config();

    ASSERT_EQ_FMT(7, NB_THREADS, "%d");
    ASSERT_EQ_FMT(42, max_stock_by_thread, "%d");

    NB_THREADS = saved_nb_threads;
    max_stock_by_thread = saved_max_stock;
    fork_orchestrator_reset();
    PASS();
}

/* fork_orchestrator_reset laisse une configuration en préparation vide. */
TEST reset_clears_staged_config(void)
{
    fork_orchestrator_reset();
    fork_orchestrator_stage_config_line("nb_forks = 7");

    char before[64];
    ASSERT(fork_orchestrator_format_staged_config(before, sizeof(before)) > 0);

    fork_orchestrator_reset();

    char after[64];
    int n = fork_orchestrator_format_staged_config(after, sizeof(after));
    ASSERT_EQ_FMT(0, n, "%d");
    ASSERT_STR_EQ("", after);

    fork_orchestrator_reset();
    PASS();
}

SUITE(fork_orchestrator_suite)
{
    RUN_TEST(orchestrator_step_config_begun_matrix);
    RUN_TEST(orchestrator_step_start_matrix);
    RUN_TEST(orchestrator_step_stop_forks_and_restart_are_unsupported_everywhere);
    RUN_TEST(orchestrator_step_exit_matrix);
    RUN_TEST(orchestrator_step_child_died_is_always_a_self_loop);
    RUN_TEST(orchestrator_step_accepts_null_out);
    RUN_TEST(countdown_elapsed_before_at_and_after_deadline);

    RUN_TEST(post_event_start_from_waiting_config_transitions_to_running);
    RUN_TEST(post_event_start_twice_reports_already_running);
    RUN_TEST(snapshot_countdown_remaining_is_negative_outside_countdown);
    RUN_TEST(stage_config_line_valid_key_cancels_countdown_and_is_formatted);
    RUN_TEST(stage_config_line_invalid_does_not_transition);
    RUN_TEST(merge_staged_config_overlays_only_staged_keys);
    RUN_TEST(apply_staged_config_writes_globals_immediately);
    RUN_TEST(reset_clears_staged_config);
}
