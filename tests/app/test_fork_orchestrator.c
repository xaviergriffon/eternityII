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
#include "app/app_runtime.h"
#include "app/fork_gate.h"
#include "app/static_variables.h"
#include "app/etii_statistic.h"

#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

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

/* EV_START : WAITING_CONFIG/COUNTDOWN/CONFIGURING/APPLYING -> RUNNING
   (spawn_forks=1) ; RUNNING/STOPPING/EXITING -> inchangé + ALREADY_RUNNING.
   APPLYING est volontairement spawn-éligible : c'est le MÊME chemin
   EV_START qui déclenche le re-fork à la fin d'un `configApply`
   NEEDS_RESTART, cf. fork_orchestrator.c. */
TEST orchestrator_step_start_matrix(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];
        orch_actions_t out;
        orch_state_t next = orchestrator_step(s, EV_START, 0, &out);

        int busy = (s == ORCH_RUNNING || s == ORCH_STOPPING || s == ORCH_EXITING);
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

/* EV_STOP_FORKS / EV_RESTART : RUNNING -> STOPPING (stop_forks=1,
   OK) ; tout autre état -> inchangé + ORCH_ERR_NOT_RUNNING, stop_forks=0.
   Les deux événements partagent EXACTEMENT la même transition pure — la
   distinction "faut-il re-forker après" est portée par le driver, pas par
   cet état (cf. g_restart_after_stop, fork_orchestrator.c), donc le cœur pur
   ne peut pas non plus la distinguer ici. */
TEST orchestrator_step_stop_forks_and_restart_matrix(void)
{
    for (int i = 0; i < N_STATES; i++) {
        orch_state_t s = ALL_STATES[i];

        orch_actions_t out1;
        orch_state_t next1 = orchestrator_step(s, EV_STOP_FORKS, 0, &out1);
        orch_actions_t out2;
        orch_state_t next2 = orchestrator_step(s, EV_RESTART, 0, &out2);

        if (s == ORCH_RUNNING) {
            ASSERT_EQ_FMT((int)ORCH_STOPPING, (int)next1, "%d");
            ASSERT_EQ_FMT(1, out1.stop_forks, "%d");
            ASSERT_EQ_FMT(0, out1.spawn_forks, "%d");
            ASSERT_EQ_FMT((int)ORCH_OK, (int)out1.error, "%d");

            ASSERT_EQ_FMT((int)ORCH_STOPPING, (int)next2, "%d");
            ASSERT_EQ_FMT(1, out2.stop_forks, "%d");
            ASSERT_EQ_FMT(0, out2.spawn_forks, "%d");
            ASSERT_EQ_FMT((int)ORCH_OK, (int)out2.error, "%d");
        } else {
            ASSERT_EQ_FMT((int)s, (int)next1, "%d");
            ASSERT_EQ_FMT(0, out1.stop_forks, "%d");
            ASSERT_EQ_FMT((int)ORCH_ERR_NOT_RUNNING, (int)out1.error, "%d");

            ASSERT_EQ_FMT((int)s, (int)next2, "%d");
            ASSERT_EQ_FMT(0, out2.stop_forks, "%d");
            ASSERT_EQ_FMT((int)ORCH_ERR_NOT_RUNNING, (int)out2.error, "%d");
        }
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

/* ============================ waitpid_target_is_reaped ======================= */

/*
 * Régression : `orchestrator_do_stop_forks` tournait indéfiniment quand un
 * enfant était réclamé par `sigchld_handler` sur un AUTRE thread (checker,
 * console, …) avant que le `waitpid(pid, …)` explicite de la séquence
 * d'arrêt n'ait sa chance — `waitpid` renvoie alors `-1`/`ECHILD` ("plus mon
 * enfant"), que l'ancien code traitait à tort comme "encore vivant" au lieu
 * de "déjà mort ailleurs". Trouvé en testant manuellement `configApply`
 * (l'état `STOPPING` ne se résorbait jamais, même après l'escalade SIGKILL).
 */
TEST waitpid_target_is_reaped_matrix(void)
{
    /* Réclamé par CET appel : le cas nominal. */
    ASSERT_EQ_FMT(1, waitpid_target_is_reaped(4242, 4242, 0), "%d");

    /* Encore vivant (WNOHANG n'a rien trouvé) : jamais "reaped". */
    ASSERT_EQ_FMT(0, waitpid_target_is_reaped(0, 4242, 0), "%d");

    /* Déjà réclamé PAR UN AUTRE THREAD (sigchld_handler) entre-temps :
       waitpid(pid, …) renvoie -1/ECHILD, PAS le pid — compte comme mort. */
    ASSERT_EQ_FMT(1, waitpid_target_is_reaped(-1, 4242, ECHILD), "%d");

    /* Erreur transitoire (ex. EINTR) : ni mort ni vivant confirmé — on
       retentera au tour suivant, jamais traité comme "reaped". */
    ASSERT_EQ_FMT(0, waitpid_target_is_reaped(-1, 4242, EINTR), "%d");

    /* Un pid renvoyé qui NE correspond PAS au pid demandé (ne devrait jamais
       arriver avec waitpid(pid, …) ciblé, mais la fonction ne doit pas le
       confondre avec une réussite). */
    ASSERT_EQ_FMT(0, waitpid_target_is_reaped(9999, 4242, 0), "%d");

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

/* ============================ stuck_forks_threshold_elapsed ================== */

TEST stuck_forks_threshold_before_at_and_after_deadline(void)
{
    ASSERT_EQ_FMT(0, stuck_forks_threshold_elapsed(0, STUCK_FORKS_WARN_MS - 1), "%d");
    ASSERT_EQ_FMT(1, stuck_forks_threshold_elapsed(0, STUCK_FORKS_WARN_MS), "%d");
    ASSERT_EQ_FMT(1, stuck_forks_threshold_elapsed(0, STUCK_FORKS_WARN_MS + 1), "%d");
    /* running_since_ms non nul : c'est bien un delta qui compte, pas une
       horloge absolue. */
    ASSERT_EQ_FMT(0, stuck_forks_threshold_elapsed(1000000, 1000000 + STUCK_FORKS_WARN_MS - 1), "%d");
    ASSERT_EQ_FMT(1, stuck_forks_threshold_elapsed(1000000, 1000000 + STUCK_FORKS_WARN_MS), "%d");
    PASS();
}

/* ============================ fork_stat_is_zero ================================ */

/* Version PAR FORK (un seul struct, pas un tableau) — base du filet par slot
   (g_stuck_fork_warned) qui repère un sous-ensemble de forks bloqués pendant
   que les autres travaillent, un cas que fork_stats_all_zero (agrégat) ne
   détecte jamais (cf. son commentaire dans fork_orchestrator.h). */
TEST fork_stat_is_zero_detects_any_nonzero_field(void)
{
    struct client_statistics stat;
    memset(&stat, 0, sizeof(stat));
    ASSERT_EQ_FMT(1, fork_stat_is_zero(&stat), "%d");

    stat.possibilities_in_stock = 1;
    ASSERT_EQ_FMT(0, fork_stat_is_zero(&stat), "%d");
    stat.possibilities_in_stock = 0;

    stat.analyses_in_stock = 1;
    ASSERT_EQ_FMT(0, fork_stat_is_zero(&stat), "%d");
    stat.analyses_in_stock = 0;

    stat.shots_per_second = 1;
    ASSERT_EQ_FMT(0, fork_stat_is_zero(&stat), "%d");
    PASS();
}

TEST fork_stat_is_zero_treats_null_as_zero(void)
{
    ASSERT_EQ_FMT(1, fork_stat_is_zero(NULL), "%d");
    PASS();
}

/* ============================ fork_stats_all_zero ============================= */

TEST fork_stats_all_zero_detects_any_nonzero_indicator(void)
{
    struct client_statistics stats[3];
    memset(stats, 0, sizeof(stats));
    ASSERT_EQ_FMT(1, fork_stats_all_zero(stats, 3), "%d");

    stats[1].possibilities_in_stock = 1;
    ASSERT_EQ_FMT(0, fork_stats_all_zero(stats, 3), "%d");
    stats[1].possibilities_in_stock = 0;

    stats[2].analyses_in_stock = 1;
    ASSERT_EQ_FMT(0, fork_stats_all_zero(stats, 3), "%d");
    stats[2].analyses_in_stock = 0;

    stats[0].shots_per_second = 1;
    ASSERT_EQ_FMT(0, fork_stats_all_zero(stats, 3), "%d");
    PASS();
}

/* nb <= 0 ou stats == NULL : "rien à montrer" compte comme suspect (1), ne
   bloque jamais la détection sur un NB_THREADS mal lu. */
TEST fork_stats_all_zero_treats_empty_input_as_zero(void)
{
    struct client_statistics stats[1];
    memset(stats, 0, sizeof(stats));
    ASSERT_EQ_FMT(1, fork_stats_all_zero(stats, 0), "%d");
    ASSERT_EQ_FMT(1, fork_stats_all_zero(NULL, 3), "%d");
    PASS();
}

/* ============================ stop_escalation_next =========================== */

/* Avant 5s : NONE (on attend juste, le SIGINT initial suffit peut-être).
   [5s, 10s[ : SIGTERM. >= 10s : SIGKILL. Bornes exactes testées. */
TEST stop_escalation_next_thresholds(void)
{
    ASSERT_EQ_FMT((int)STOP_ESCALATION_NONE, (int)stop_escalation_next(0), "%d");
    ASSERT_EQ_FMT((int)STOP_ESCALATION_NONE, (int)stop_escalation_next(4999), "%d");
    ASSERT_EQ_FMT((int)STOP_ESCALATION_SIGTERM, (int)stop_escalation_next(5000), "%d");
    ASSERT_EQ_FMT((int)STOP_ESCALATION_SIGTERM, (int)stop_escalation_next(9999), "%d");
    ASSERT_EQ_FMT((int)STOP_ESCALATION_SIGKILL, (int)stop_escalation_next(10000), "%d");
    ASSERT_EQ_FMT((int)STOP_ESCALATION_SIGKILL, (int)stop_escalation_next(999999), "%d");
    PASS();
}

/* ============================ child_idle_ms =========================== */

/* Fils qui rapporte de l'activité en continu : l'inactivité reste bornée au
   temps écoulé depuis SA dernière activité connue, jamais depuis le début de
   la fenêtre d'arrêt — un fils qui vide encore sa file d'acquittements en
   attente ne doit jamais être vu comme inactif tant qu'il rapporte. */
TEST child_idle_ms_counts_since_last_activity(void)
{
    time_t escalation_start = 1000;
    time_t last_activity = 1007; /* a rapporté 7s après le début de l'arrêt */
    ASSERT_EQ_FMT(0L, child_idle_ms(last_activity, escalation_start, 1007), "%ld");
    ASSERT_EQ_FMT(3000L, child_idle_ms(last_activity, escalation_start, 1010), "%ld");
    PASS();
}

/* Fils qui n'a JAMAIS rapporté d'activité (last_activity == 0, ex. client
   sans cette instrumentation, ou mort avant son premier rapport) : compté
   inactif depuis escalation_start — comportement identique à avant
   l'introduction du suivi par fils, jamais protégé indéfiniment. */
TEST child_idle_ms_falls_back_to_escalation_start_when_never_reported(void)
{
    time_t escalation_start = 1000;
    ASSERT_EQ_FMT(0L, child_idle_ms(0, escalation_start, 1000), "%ld");
    ASSERT_EQ_FMT(5000L, child_idle_ms(0, escalation_start, 1005), "%ld");
    PASS();
}

/* `now` antérieur ou égal à la référence (horloge injectée incohérente,
   jamais censé arriver en pratique) : jamais négatif. */
TEST child_idle_ms_never_negative(void)
{
    ASSERT_EQ_FMT(0L, child_idle_ms(1010, 1000, 1005), "%ld");
    ASSERT_EQ_FMT(0L, child_idle_ms(1005, 1000, 1005), "%ld");
    PASS();
}

/* ============================ fork_diagnostic_summary ========================= */

/* reported == 0 : `stat` ne doit jamais être présenté comme un état réel,
   même s'il n'est pas NULL (compteurs d'initialisation à zéro, jamais mis à
   jour par un vrai IPC_MSG_STATS). */
TEST fork_diagnostic_summary_never_reported(void)
{
    struct client_statistics stat;
    memset(&stat, 0, sizeof(stat));
    stat.possibilities_in_stock = 42; /* ne doit PAS apparaître : reported == 0 */
    char buf[96];
    fork_diagnostic_summary(&stat, 0, 0, buf, sizeof(buf));
    ASSERT_STR_EQ("jamais rapporté", buf);
    PASS();
}

/* `stat == NULL` : même comportement que reported == 0, jamais de déréférencement. */
TEST fork_diagnostic_summary_null_stat(void)
{
    char buf[96];
    fork_diagnostic_summary(NULL, 1, 0, buf, sizeof(buf));
    ASSERT_STR_EQ("jamais rapporté", buf);
    PASS();
}

/* Mode recherche (pruner_mode == 0) : stock/analysé/coups-s/profondeur max,
   plus l'indicateur d'échange serveur (serveur=non ici : pas en train de
   communiquer au moment du rapport). */
TEST fork_diagnostic_summary_search_mode(void)
{
    struct client_statistics stat;
    memset(&stat, 0, sizeof(stat));
    stat.possibilities_in_stock = 1234;
    stat.analyses_in_stock = 56;
    stat.shots_per_second = 78900;
    stat.max_result = 186;
    stat.server_io_active = 0;
    char buf[160];
    fork_diagnostic_summary(&stat, 1, 0, buf, sizeof(buf));
    ASSERT_STR_EQ("stock=1234 analysé=56 coups/s=78900 max=186 serveur=non", buf);
    PASS();
}

/* Mode pruner (pruner_mode != 0) : vérifiées/éliminées/cases-s, jamais les
   compteurs de recherche — plus l'indicateur d'échange serveur, ici vrai
   (serveur=oui) : c'est exactement le cas qui motive cet indicateur, un fils
   vivant mais en train de PARLER au serveur au moment de l'escalade. */
TEST fork_diagnostic_summary_pruner_mode(void)
{
    struct client_statistics stat;
    memset(&stat, 0, sizeof(stat));
    stat.pruner_checked = 1000;
    stat.pruner_removed = 250;
    stat.pruner_cells_per_second = 54321;
    stat.server_io_active = 1;
    char buf[160];
    fork_diagnostic_summary(&stat, 1, 1, buf, sizeof(buf));
    ASSERT_STR_EQ("vérifiées=1000 éliminées=250 cases/s=54321 serveur=oui", buf);
    PASS();
}

/* `out == NULL` / `out_size == 0` : no-op sûr, jamais de déréférencement. */
TEST fork_diagnostic_summary_null_out_is_safe(void)
{
    struct client_statistics stat;
    memset(&stat, 0, sizeof(stat));
    fork_diagnostic_summary(&stat, 1, 0, NULL, 160);
    char buf[160];
    fork_diagnostic_summary(&stat, 1, 0, buf, 0);
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

/* stopForks (EV_STOP_FORKS) : refusé hors RUNNING (ORCH_ERR_NOT_RUNNING),
   accepté depuis RUNNING (-> STOPPING, visible via snapshot). */
TEST post_event_stop_forks_requires_running(void)
{
    fork_orchestrator_reset();

    orch_actions_t not_running;
    fork_orchestrator_post_event(EV_STOP_FORKS, &not_running);
    ASSERT_EQ_FMT((int)ORCH_ERR_NOT_RUNNING, (int)not_running.error, "%d");

    orch_actions_t start;
    fork_orchestrator_post_event(EV_START, &start);
    ASSERT_EQ_FMT((int)ORCH_OK, (int)start.error, "%d");

    orch_actions_t stop;
    fork_orchestrator_post_event(EV_STOP_FORKS, &stop);
    ASSERT_EQ_FMT((int)ORCH_OK, (int)stop.error, "%d");
    ASSERT_EQ_FMT(1, stop.stop_forks, "%d");

    orch_state_t state;
    fork_orchestrator_snapshot(&state, NULL);
    ASSERT_EQ_FMT((int)ORCH_STOPPING, (int)state, "%d");

    fork_orchestrator_reset();
    PASS();
}

/* configApply (EV_RESTART) : même garde-fou que stopForks. */
TEST post_event_restart_requires_running(void)
{
    fork_orchestrator_reset();

    orch_actions_t not_running;
    fork_orchestrator_post_event(EV_RESTART, &not_running);
    ASSERT_EQ_FMT((int)ORCH_ERR_NOT_RUNNING, (int)not_running.error, "%d");

    fork_orchestrator_post_event(EV_START, NULL);

    orch_actions_t restart;
    fork_orchestrator_post_event(EV_RESTART, &restart);
    ASSERT_EQ_FMT((int)ORCH_OK, (int)restart.error, "%d");
    ASSERT_EQ_FMT(1, restart.stop_forks, "%d");

    orch_state_t state;
    fork_orchestrator_snapshot(&state, NULL);
    ASSERT_EQ_FMT((int)ORCH_STOPPING, (int)state, "%d");

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

/* ==================== orchestrator_apply_restart_config (quiescence) ======== */

/*
 * Régression : `orchestrator_apply_restart_config` (appelée en ORCH_APPLYING)
 * libère puis réalloue
 * `childrens_pid`/`forkId`/`fork_statistics` quand `nb_forks` change — un
 * lecteur concurrent (checker, server_tcp, canal de contrôle, console) qui ne
 * serait pas garé pendant cette fenêtre peut déréférencer un pointeur déjà
 * libéré. Signalé par l'utilisateur comme un crash réel du thread console
 * sous NCURSES=1 après un `configApply` changeant `nb_forks` (et, plus
 * discrètement en mode ANSI, comme "la configuration ne semble pas prise en
 * compte" — même course, juste moins souvent fatale). Cause : la fonction ne
 * demandait aucune quiescence coopérative (`fork_gate_request_quiesce`)
 * avant de toucher ces tableaux, alors que D2/APPLYING l'exige explicitement.
 *
 * Ce test fait tourner un thread compagnon qui boucle en tâche serrée sur
 * `fork_gate_checkpoint` (comme le checker/la console réels) et, à CHAQUE
 * tour où il n'est pas garé, lit `childrens_pid`/`forkId`/`fork_statistics` —
 * exactement comme un vrai lecteur le ferait. Si la quiescence protège
 * réellement la fenêtre de reconstruction, ce thread ne peut PAR CONSTRUCTION
 * jamais observer ces pointeurs à NULL (il est bloqué dans
 * `fork_gate_checkpoint` tant que la quiescence est active) — un test de
 * contrat déterministe, pas une simple mesure de timing probabiliste.
 */
static volatile int g_race_worker_stop = 0;
static volatile int g_race_worker_slot = -1;
static volatile int g_race_worker_saw_null = 0;
static volatile long g_race_worker_iterations = 0;

static void *restart_race_worker(void *arg)
{
    (void)arg;
    int slot = fork_gate_register("race-worker");
    g_race_worker_slot = slot;
    while (!g_race_worker_stop) {
        fork_gate_checkpoint(slot);
        if (childrens_pid == NULL || forkId == NULL || fork_statistics == NULL) {
            g_race_worker_saw_null = 1;
        } else {
            volatile pid_t p = childrens_pid[0];
            (void)p;
        }
        g_race_worker_iterations++;
    }
    fork_gate_unregister(slot);
    return NULL;
}

TEST apply_restart_config_quiesces_concurrent_array_readers(void)
{
    fork_gate_reset();
    fork_orchestrator_reset();

    int saved_nb = NB_THREADS;
    pid_t *spid = childrens_pid;
    char **sfork = forkId;
    struct client_statistics *sstat = fork_statistics;
    char *saved_parts_files = parts_files;

    NB_THREADS = 2;
    init_childs();
    parts_files = NULL; /* NULL : pas de reconstruction de map dans ce test (shared_parts=NULL) */

    fork_orchestrator_stage_config_line("nb_forks = 5");

    g_race_worker_stop = 0;
    g_race_worker_slot = -1;
    g_race_worker_saw_null = 0;
    g_race_worker_iterations = 0;

    pthread_t t;
    ASSERT_EQ_FMT(0, pthread_create(&t, NULL, restart_race_worker, NULL), "%d");
    while (g_race_worker_slot < 0) {
        usleep(200);
    }
    usleep(2000); /* laisse le worker tourner un peu avant l'appel */

    int rc = orchestrator_apply_restart_config(NULL);

    g_race_worker_stop = 1;
    pthread_join(t, NULL);

    int ok = (childrens_pid != NULL && forkId != NULL && fork_statistics != NULL);
    for (int i = 0; ok && i < NB_THREADS; i++) {
        free(forkId[i]);
    }
    free(childrens_pid); free(forkId); free(fork_statistics);
    childrens_pid = spid; forkId = sfork; fork_statistics = sstat;
    NB_THREADS = saved_nb;
    parts_files = saved_parts_files;

    ASSERT_EQ_FMT(1, rc, "%d");
    ASSERT_EQ_FMT(0, g_race_worker_saw_null, "%d");
    ASSERT(g_race_worker_iterations > 0); /* le worker a bien tourné pendant le test */

    fork_orchestrator_reset();
    fork_gate_reset();
    PASS();
}

/* log_startup_diagnostics : instantané de la configuration effective +
   identité écrit dans events.log (jamais sur la console — cf. log_file,
   ui/logger.h). Exposée (non static) précisément pour être appelable ici
   sans passer par un vrai fork()/driver — voir sa doc dans
   fork_orchestrator.h. */
TEST log_startup_diagnostics_writes_effective_config_to_events_log(void)
{
    unlink("events.log");

    client_identity_t saved_identity = g_client_identity_template;
    int saved_nb_threads = NB_THREADS;

    NB_THREADS = 3;
    memset(&g_client_identity_template, 0xAB, sizeof(g_client_identity_template));
    g_client_identity_template.mode = CLIENT_MODE_PRUNER;
    strncpy(g_client_identity_template.label, "test-diag-label", CLIENT_LABEL_MAX - 1);
    g_client_identity_template.label[CLIENT_LABEL_MAX - 1] = '\0';

    char machine_uid_hex[2 * MACHINE_UID_BYTES + 1];
    char client_uid_hex[2 * CLIENT_UID_BYTES + 1];
    client_identity_hex_encode(g_client_identity_template.machine_uid, MACHINE_UID_BYTES,
                                machine_uid_hex, sizeof(machine_uid_hex));
    client_identity_hex_encode(g_client_identity_template.client_uid, CLIENT_UID_BYTES,
                                client_uid_hex, sizeof(client_uid_hex));

    log_startup_diagnostics(3);

    g_client_identity_template = saved_identity;
    NB_THREADS = saved_nb_threads;

    FILE *f = fopen("events.log", "r");
    ASSERT(f != NULL);
    char line[2048] = {0};
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    (void)n;
    unlink("events.log");

    char expect_pid[32];
    snprintf(expect_pid, sizeof expect_pid, "pid=%d", (int)getpid());
    ASSERT(strstr(line, expect_pid) != NULL);
    ASSERT(strstr(line, "3 fork(s) lanc") != NULL);
    ASSERT(strstr(line, "mode=pruner") != NULL);
    ASSERT(strstr(line, "label=\"test-diag-label\"") != NULL);
    ASSERT(strstr(line, machine_uid_hex) != NULL);
    ASSERT(strstr(line, client_uid_hex) != NULL);
    ASSERT(strstr(line, "configuration effective") != NULL);
    ASSERT(strstr(line, "nb_forks") != NULL); /* client_config_format */
    ASSERT(strstr(line, "[") != NULL);        /* horodatage entre crochets */
    PASS();
}

SUITE(fork_orchestrator_suite)
{
    RUN_TEST(orchestrator_step_config_begun_matrix);
    RUN_TEST(orchestrator_step_start_matrix);
    RUN_TEST(orchestrator_step_stop_forks_and_restart_matrix);
    RUN_TEST(orchestrator_step_exit_matrix);
    RUN_TEST(orchestrator_step_child_died_is_always_a_self_loop);
    RUN_TEST(orchestrator_step_accepts_null_out);
    RUN_TEST(countdown_elapsed_before_at_and_after_deadline);
    RUN_TEST(stuck_forks_threshold_before_at_and_after_deadline);
    RUN_TEST(fork_stat_is_zero_detects_any_nonzero_field);
    RUN_TEST(fork_stat_is_zero_treats_null_as_zero);
    RUN_TEST(fork_stats_all_zero_detects_any_nonzero_indicator);
    RUN_TEST(fork_stats_all_zero_treats_empty_input_as_zero);
    RUN_TEST(stop_escalation_next_thresholds);
    RUN_TEST(child_idle_ms_counts_since_last_activity);
    RUN_TEST(child_idle_ms_falls_back_to_escalation_start_when_never_reported);
    RUN_TEST(child_idle_ms_never_negative);
    RUN_TEST(fork_diagnostic_summary_never_reported);
    RUN_TEST(fork_diagnostic_summary_null_stat);
    RUN_TEST(fork_diagnostic_summary_search_mode);
    RUN_TEST(fork_diagnostic_summary_pruner_mode);
    RUN_TEST(fork_diagnostic_summary_null_out_is_safe);
    RUN_TEST(waitpid_target_is_reaped_matrix);

    RUN_TEST(post_event_start_from_waiting_config_transitions_to_running);
    RUN_TEST(post_event_start_twice_reports_already_running);
    RUN_TEST(post_event_stop_forks_requires_running);
    RUN_TEST(post_event_restart_requires_running);
    RUN_TEST(snapshot_countdown_remaining_is_negative_outside_countdown);
    RUN_TEST(stage_config_line_valid_key_cancels_countdown_and_is_formatted);
    RUN_TEST(stage_config_line_invalid_does_not_transition);
    RUN_TEST(merge_staged_config_overlays_only_staged_keys);
    RUN_TEST(apply_staged_config_writes_globals_immediately);
    RUN_TEST(reset_clears_staged_config);
    RUN_TEST(apply_restart_config_quiesces_concurrent_array_readers);
    RUN_TEST(log_startup_diagnostics_writes_effective_config_to_events_log);
}
