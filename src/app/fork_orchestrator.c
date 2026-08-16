#include "app/fork_orchestrator.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <pthread.h>

#include "app/app_runtime.h"
#include "app/client_config.h"
#include "app/etii_client.h"
#include "app/etii_control.h"
#include "app/fork_gate.h"
#include "app/static_variables.h"
#include "core/datamanager.h"
#include "net/local_socket.h"
#include "ui/logger.h"

/* ============================ Cœur pur ==================================== */

orch_state_t orchestrator_step(orch_state_t s, orch_event_t ev, long now_ms, orch_actions_t *out)
{
    (void)now_ms; /* réservé, cf. fork_orchestrator.h */
    orch_actions_t local_actions;
    if (out == NULL) {
        out = &local_actions;
    }
    out->spawn_forks = 0;
    out->stop_forks = 0;
    out->error = ORCH_OK;

    switch (ev) {
    case EV_CONFIG_BEGUN:
        /* Décompte annulé DÉFINITIVEMENT : une fois en CONFIGURING, aucun
           chemin ne revient jamais vers COUNTDOWN dans cette table. Dans
           tout autre état (RUNNING, STOPPING, APPLYING, EXITING), c'est un
           self-loop harmless : la valeur reste stagée pour une future
           application de configuration, sans effet sur le cycle de vie en
           cours. */
        if (s == ORCH_WAITING_CONFIG || s == ORCH_COUNTDOWN) {
            return ORCH_CONFIGURING;
        }
        break;

    case EV_START:
        if (s == ORCH_RUNNING || s == ORCH_STOPPING || s == ORCH_EXITING) {
            /* "erreur si déjà en RUNNING" — étendu aux autres états où
               démarrer n'a pas de sens plutôt que d'inventer un nouveau code
               d'erreur. ORCH_APPLYING est volontairement ABSENT de cette
               liste (cf. branche commune ci-dessous) : c'est le MÊME chemin
               EV_START qui déclenche le re-fork à la fin d'un `configApply`
               NEEDS_RESTART, exactement comme il déclenche déjà le premier
               fork après WAITING_CONFIG/COUNTDOWN/CONFIGURING — un seul
               chemin de code pour "il faut forker maintenant", testé une
               fois (même principe que le décompte de COUNTDOWN, cf.
               fork_orchestrator_run). */
            out->error = ORCH_ERR_ALREADY_RUNNING;
            break;
        }
        out->spawn_forks = 1;
        return ORCH_RUNNING;

    case EV_STOP_FORKS:
    case EV_RESTART:
        /* stopForks (arrêt simple) et configApply NEEDS_RESTART (arrêt puis
           réapplication + re-fork) partagent la MÊME transition pure
           RUNNING -> STOPPING : la distinction entre "revenir à
           WAITING_CONFIG une fois arrêté" et "enchaîner sur APPLYING puis
           EV_START" ne peut pas être portée par cet état pur seul (ce serait
           un second axe orthogonal à orch_state_t) — exactement comme la
           deadline du décompte de COUNTDOWN n'est pas dans l'état pur non
           plus. Le driver (fork_orchestrator_post_event) mémorise ce choix
           dans une variable locale au module (même convention), au vu de
           quel ÉVÉNEMENT a réussi cette transition précise. */
        if (s != ORCH_RUNNING) {
            /* Rien à arrêter hors RUNNING : ni un décompte, ni une
               configuration en préparation seule ne constituent des fils
               vivants. */
            out->error = ORCH_ERR_NOT_RUNNING;
            break;
        }
        out->stop_forks = 1;
        return ORCH_STOPPING;

    case EV_EXIT:
        /* Toujours acceptée, depuis n'importe quel état — jamais une erreur.
           Aucun driver ne poste encore cet événement : exit_interpreter
           (src/ui/command_lines.c) garde son comportement actuel (kill
           direct + exit()), inchangé. */
        return ORCH_EXITING;

    case EV_CHILD_DIED:
        /* Observabilité pure (pas de re-spawn automatique) : jamais de
           changement d'état, quel que soit l'état courant. */
        break;
    }
    return s;
}

int orchestrator_countdown_elapsed(long countdown_deadline_ms, long now_ms)
{
    return now_ms >= countdown_deadline_ms;
}

int stuck_forks_threshold_elapsed(long running_since_ms, long now_ms)
{
    return (now_ms - running_since_ms) >= STUCK_FORKS_WARN_MS;
}

int fork_stat_is_zero(const struct client_statistics *stat)
{
    if (stat == NULL) {
        return 1;
    }
    return stat->possibilities_in_stock == 0
        && stat->analyses_in_stock == 0
        && stat->shots_per_second == 0;
}

int fork_stats_all_zero(const struct client_statistics *stats, int nb)
{
    if (nb <= 0 || stats == NULL) {
        return 1;
    }
    for (int f = 0; f < nb; f++) {
        if (!fork_stat_is_zero(&stats[f])) {
            return 0;
        }
    }
    return 1;
}

stop_escalation_action_t stop_escalation_next(long elapsed_ms)
{
    if (elapsed_ms >= STOP_ESCALATION_SIGKILL_MS) {
        return STOP_ESCALATION_SIGKILL;
    }
    if (elapsed_ms >= STOP_ESCALATION_SIGTERM_MS) {
        return STOP_ESCALATION_SIGTERM;
    }
    return STOP_ESCALATION_NONE;
}

int waitpid_target_is_reaped(pid_t waitpid_result, pid_t target_pid, int wait_errno)
{
    if (waitpid_result == target_pid) {
        return 1;
    }
    if (waitpid_result == -1 && wait_errno == ECHILD) {
        return 1;
    }
    return 0;
}

/* ============================ Driver impur ================================ */

static pthread_mutex_t g_orch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_orch_cond = PTHREAD_COND_INITIALIZER;
static orch_state_t g_orch_state = ORCH_WAITING_CONFIG;
static long g_countdown_deadline_ms = 0;
static int g_orch_pending_spawn = 0;
static client_config_t g_staged_config;
static int g_staged_config_ready = 0;
/** @brief 1 si la STOPPING en cours doit enchaîner sur APPLYING + re-fork
 *         (déclenchée par EV_RESTART), 0 si elle doit s'arrêter là
 *         (EV_STOP_FORKS). Comme `g_countdown_deadline_ms`, cette distinction
 *         n'est PAS portée par l'état pur `orch_state_t` — mémorisée ici par
 *         le driver, sous le même mutex, au vu de l'événement qui a réussi la
 *         transition RUNNING -> STOPPING (cf. orchestrator_step). */
static int g_restart_after_stop = 0;
/** @brief Horodatage (ms) du dernier (re)fork réussi — base de
 *         `stuck_forks_threshold_elapsed`. 0 tant qu'aucun fork n'a jamais
 *         réussi. Comme `g_countdown_deadline_ms`, cet axe temporel n'est PAS
 *         porté par l'état pur `orch_state_t`. */
static long g_running_since_ms = 0;
/** @brief Tableau (taille NB_THREADS au (re)fork) : 1 une fois l'avertissement
 *         "ce fork ne produit rien" émis pour le SLOT correspondant du
 *         (re)fork en cours — un par slot, jamais un seul drapeau global.
 *
 * Remplace un premier avertissement agrégé "AUCUN fork ne rapporte de
 * travail" (tous à zéro) — trouvé insuffisant dès la première reproduction
 * en conditions réelles (256 pièces, `nb_forks=3`) : deux forks sur trois
 * restaient collés à zéro (`Fork 1`/`Fork 2` du rapport `check`) pendant que
 * le troisième travaillait normalement — l'agrégat ne s'alarme QUE si TOUS
 * les forks sont à zéro, donc ce cas concret (partiel, la vraie panne
 * observée) ne déclenchait jamais rien, ni en console ni dans events.log.
 * Réalloué (`calloc`, taille NB_THREADS) à chaque (re)fork réussi
 * (`orchestrator_spawn_forks`) : NULL accepté partout en aval (désactive
 * silencieusement le filet plutôt que de planter sur un OOM — diagnostic
 * seulement, jamais critique). */
static int *g_stuck_fork_warned = NULL;

static long current_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/** @brief (Ré)initialise `g_staged_config` si besoin. Appelant sous g_orch_mutex. */
static void ensure_staged_config_locked(void)
{
    if (!g_staged_config_ready) {
        client_config_init(&g_staged_config);
        g_staged_config_ready = 1;
    }
}

void fork_orchestrator_reset(void)
{
    pthread_mutex_lock(&g_orch_mutex);
    g_orch_state = ORCH_WAITING_CONFIG;
    g_countdown_deadline_ms = 0;
    g_orch_pending_spawn = 0;
    g_restart_after_stop = 0;
    if (g_staged_config_ready) {
        client_config_free(&g_staged_config);
    }
    client_config_init(&g_staged_config);
    g_staged_config_ready = 1;
    pthread_mutex_unlock(&g_orch_mutex);
}

void fork_orchestrator_post_event(orch_event_t ev, orch_actions_t *out)
{
    orch_actions_t local_actions;
    if (out == NULL) {
        out = &local_actions;
    }
    long now_ms = current_time_ms();

    pthread_mutex_lock(&g_orch_mutex);
    orch_state_t new_state = orchestrator_step(g_orch_state, ev, now_ms, out);
    g_orch_state = new_state;
    if (out->spawn_forks) {
        g_orch_pending_spawn = 1;
    }
    if (out->stop_forks && out->error == ORCH_OK) {
        /* Mémorise, pour le driver, si cette STOPPING doit enchaîner sur un
           re-fork (EV_RESTART) ou s'arrêter là (EV_STOP_FORKS) — cf. le
           commentaire de g_restart_after_stop. */
        g_restart_after_stop = (ev == EV_RESTART);
    }
    pthread_cond_broadcast(&g_orch_cond);
    pthread_mutex_unlock(&g_orch_mutex);
}

void fork_orchestrator_snapshot(orch_state_t *out_state, long *out_countdown_remaining_ms)
{
    pthread_mutex_lock(&g_orch_mutex);
    if (out_state != NULL) {
        *out_state = g_orch_state;
    }
    if (out_countdown_remaining_ms != NULL) {
        *out_countdown_remaining_ms = (g_orch_state == ORCH_COUNTDOWN)
            ? (g_countdown_deadline_ms - current_time_ms())
            : -1;
    }
    pthread_mutex_unlock(&g_orch_mutex);
}

client_config_line_status_t fork_orchestrator_stage_config_line(const char *line)
{
    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    client_config_line_status_t status = client_config_parse_line(line, &g_staged_config);
    pthread_mutex_unlock(&g_orch_mutex);

    if (status == CLIENT_CONFIG_LINE_SET) {
        /* Décompte annulé UNIQUEMENT sur une ligne effectivement acceptée —
           une clé inconnue ou une valeur invalide ne doit pas faire perdre
           l'auto-démarrage à l'opérateur pour une faute de frappe. */
        fork_orchestrator_post_event(EV_CONFIG_BEGUN, NULL);
    }
    return status;
}

int fork_orchestrator_format_staged_config(char *out, size_t out_size)
{
    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    int n = client_config_format(&g_staged_config, out, out_size);
    pthread_mutex_unlock(&g_orch_mutex);
    return n;
}

void fork_orchestrator_merge_staged_config(client_config_t *out)
{
    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    if (g_staged_config.has_nb_forks) {
        out->has_nb_forks = 1;
        out->nb_forks = g_staged_config.nb_forks;
    }
    if (g_staged_config.has_server_host) {
        free(out->server_host);
        out->has_server_host = 1;
        out->server_host = g_staged_config.server_host != NULL
            ? strdup(g_staged_config.server_host) : NULL;
    }
    if (g_staged_config.has_parts_file) {
        free(out->parts_file);
        out->has_parts_file = 1;
        out->parts_file = g_staged_config.parts_file != NULL
            ? strdup(g_staged_config.parts_file) : NULL;
    }
    if (g_staged_config.has_max_stock_by_thread) {
        out->has_max_stock_by_thread = 1;
        out->max_stock_by_thread = g_staged_config.max_stock_by_thread;
    }
    if (g_staged_config.has_limit) {
        out->has_limit = 1;
        out->limit = g_staged_config.limit;
    }
    if (g_staged_config.has_pruner_batch) {
        out->has_pruner_batch = 1;
        out->pruner_batch = g_staged_config.pruner_batch;
    }
    if (g_staged_config.has_dfs_budget) {
        out->has_dfs_budget = 1;
        out->dfs_budget = g_staged_config.dfs_budget;
    }
    pthread_mutex_unlock(&g_orch_mutex);
}

void fork_orchestrator_apply_staged_config(void)
{
    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    client_config_apply_direct(&g_staged_config, &g_client_server_host);
    pthread_mutex_unlock(&g_orch_mutex);
}

client_config_diff_t fork_orchestrator_diff_staged_config(const client_config_t *effective)
{
    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    client_config_diff_t d = client_config_diff(effective, &g_staged_config);
    pthread_mutex_unlock(&g_orch_mutex);
    return d;
}

void fork_orchestrator_apply_hot_staged_config(void)
{
    int has_max_stock, has_limit, has_pruner_batch, has_dfs_budget;
    int max_stock_val = 0;
    unsigned long long limit_val = 0;
    int pruner_batch_val = 0;
    int dfs_budget_val = 0;

    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    client_config_apply_direct(&g_staged_config, &g_client_server_host);
    has_max_stock = g_staged_config.has_max_stock_by_thread;
    max_stock_val = g_staged_config.max_stock_by_thread;
    has_limit = g_staged_config.has_limit;
    limit_val = g_staged_config.limit;
    has_pruner_batch = g_staged_config.has_pruner_batch;
    pruner_batch_val = g_staged_config.pruner_batch;
    has_dfs_budget = g_staged_config.has_dfs_budget;
    dfs_budget_val = g_staged_config.dfs_budget;
    pthread_mutex_unlock(&g_orch_mutex);

    /* Diffusion aux fils DÉJÀ en cours d'exécution — jamais sous
       g_orch_mutex (send_command_to_childs fait des appels réseau/IPC). Même
       texte de commande que les interpréteurs console homonymes, pour ne
       dupliquer aucun format. */
    char cmd[64];
    if (has_max_stock) {
        snprintf(cmd, sizeof(cmd), "maxStockByThread %d", max_stock_val);
        send_command_to_childs(cmd);
    }
    if (has_limit) {
        snprintf(cmd, sizeof(cmd), "limit %llu", limit_val);
        send_command_to_childs(cmd);
    }
    if (has_pruner_batch) {
        snprintf(cmd, sizeof(cmd), "prunerBatch %d", pruner_batch_val);
        send_command_to_childs(cmd);
    }
    if (has_dfs_budget) {
        snprintf(cmd, sizeof(cmd), "prunerDfsBudget %d", dfs_budget_val);
        send_command_to_childs(cmd);
    }
}

/**
 * @brief Exécute la recherche dans un process de recherche forké (ou la
 *        branche fusionnée `DEBUG_IN_MONO_PROCESS`) — anciennement défini
 *        dans `main.c`, déplacé ici avec le reste de la boucle de fork :
 *        `orchestrator_spawn_forks` est désormais l'unique appelant, et
 *        `fork_orchestrator.c` doit rester linkable dans le binaire de test
 *        (qui n'inclut jamais `main.c`).
 */
static void run_client(const char *hostname, const char *file, int fork_seq)
{
    // On indique au manager de passer par un serveur
    set_server_ip(hostname);

    run_mono_client(file, fork_seq);

    // Sauvegarde de secours si les files ne sont pas vides (anomalie en mode
    // client) — extraite dans app_runtime.c pour être testable.
    backup_failed_exit();
}

/**
 * @brief Corps exécuté par le fils juste après son `fork()` (ou par la
 *        branche fusionnée sous `DEBUG_IN_MONO_PROCESS`) : exécute la
 *        recherche puis TERMINE le process explicitement via `exit()`.
 *
 * Les verrous de quiescence sont déjà relâchés par l'appelant (juste après le
 * `fork()`, cf. `orchestrator_spawn_forks` — AVANT tout `log_*`, voir sa doc)
 * : rien à faire ici de ce côté.
 *
 * Ne retourne JAMAIS : à la différence de l'ancienne boucle de `main.c`, où
 * un fils qui tombait hors de `run_client()` (fin de recherche, Ctrl-C)
 * finissait par ressortir de `handle_client()` — via le porte-à-faux
 * `NB_THREADS = 1` qui vidait sa propre copie de la boucle de fork puis deux
 * gardes `parent_pid == getpid()` successives qui sautaient tout le reste —
 * jusqu'au `exit(EXIT_SUCCESS)` final de `main()`. Ici, `orchestrator_spawn_forks`
 * est appelée par la boucle de l'orchestrateur (`fork_orchestrator_run`) :
 * si ce fils y REVENAIT normalement, il reprendrait à tort la boucle
 * d'orchestration. `exit()` plutôt que `_exit()` : garde le flush de
 * couverture gcov/llvm-cov, pas de raison ici de s'en passer.
 */
static void spawn_child_body(int fork_seq)
{
    NB_THREADS = 1;
    run_fork_checker(main_addr);
    run_client(g_client_server_host, parts_files, fork_seq);

    if (fork_checker_socket_id > 0) {
        close(fork_checker_socket_id);
    }
    char socket_fork[50];
    int socket_fork_len = sprintf(socket_fork, "etii_fork.%d", getpid());
    socket_fork[socket_fork_len] = '\0';
    struct sockaddr_un *fork_addr = build_sockaddr(socket_fork);
#ifdef DEBUG_LOCAL_SOCKET
    log_debug("remove : %s\n", fork_addr->sun_path);
    flush_debug();
#endif // DEBUG_LOCAL_SOCKET
    remove(fork_addr->sun_path);
    free(fork_addr);

    exit(EXIT_SUCCESS);
}

int orchestrator_spawn_forks(void)
{
    pid_t child_pid = -1;
    int fork_error = 0;
    for (int c = 0; c < NB_THREADS; c++) {
        if (request == REQUEST_STOP) {
            // Un arrêt (SIGINT/SIGHUP/SIGTERM — ce dernier atteint un process
            // lancé en arrière-plan détaché, ex. nohup/screen/tmux/systemd,
            // sans qu'aucune commande console n'ait jamais été tapée) peut
            // survenir PENDANT ce lot de forks : sans ce garde-fou, la boucle
            // continuait à créer TOUS les NB_THREADS forks restants et à
            // répéter des cycles quiesce/fork/release même après la demande
            // d'arrêt — élargissant sans raison la fenêtre de course pendant
            // laquelle d'autres threads (console, canal de contrôle, checker)
            // réagissent au MÊME signal et se désenregistrent de `fork_gate`
            // concurremment à ces cycles. On s'arrête net : les forks déjà
            // créés restent valides (comptés normalement ci-dessous), on
            // n'en tente simplement aucun de plus.
            log_info("orchestrateur : arrêt demandé pendant le lot de forks — "
                      "%i/%i tentés, arrêt de la création de nouveaux process\n",
                      c, NB_THREADS);
            break;
        }
        if (parent_pid == getpid()) {
            // Quiescence + verrous d'E/S pris JUSTE AVANT ce fork() précis
            // et relâchés JUSTE APRÈS, dans le parent ET dans le fils — PAS
            // une seule fois pour toute la boucle : un `log_error`/`log_info` de bilan
            // (fork raté, ligne DEBUG_THREAD) prend lui aussi le verrou de
            // sortie du logger (non récursif), et le garder au-delà du
            // fork() provoquerait un auto-interblocage du thread forkeur
            // sur son propre message. Section critique volontairement
            // réduite au strict `fork()`.
            fork_gate_result_t qr = fork_gate_request_quiesce(FORK_GATE_DEFAULT_TIMEOUT_MS);
            if (qr != FORK_GATE_QUIESCED) {
                log_error("orchestrateur : quiescence non atteinte — fork du process %i/%i refusé "
                          "(jamais dans le doute)\n", c + 1, NB_THREADS);
                fork_error++;
                childrens_pid[c] = -1;
                if (fork_error >= 10) {
                    log_error("création de process interrompue après %i échecs ; "
                              "poursuite avec les process déjà créés\n", fork_error);
                    break;
                }
                continue;
            }
            fork_gate_acquire_io_locks();
#ifdef DEBUG_IN_MONO_PROCESS
            child_pid = getpid();
#else
            child_pid = fork();
#endif // DEBUG_IN_MONO_PROCESS
            // Relâché IMMÉDIATEMENT, dans les DEUX branches, avant tout
            // log_info/log_error qui suit (cf. commentaire ci-dessus) — le
            // fils a hérité (COW) ce verrou VERROUILLÉ et doit relâcher SA
            // PROPRE copie pour ne pas s'auto-bloquer à son premier log_*.
            fork_gate_release_io_locks();
            // `fork_gate_release_quiesce()` en revanche NE DOIT JAMAIS être
            // appelée par le fils — trouvé en investiguant un blocage
            // permanent et intermittent (aarch64 ET x86_64, glibc 2.35 ET
            // 2.39 : pas un bug d'architecture précis), via le journal de
            // trace en mémoire de fork_gate.c (cf.
            // docs/investigations/blocage_fork_gate_release_quiesce.md) —
            // celui-ci a montré, sur un process réellement bloqué, que
            // l'appelant de `fork_gate_release_quiesce` avait un tid DIFFÉRENT
            // de celui qui venait de réussir le `fork_gate_request_quiesce`
            // précédent : le "process" observé n'était en réalité PAS le
            // parent mais LE FILS fraîchement créé, toujours en train
            // d'exécuter cette même fonction (son pile d'appels contient
            // encore `main()`, hérité du parent, puisqu'il n'a pas encore
            // atteint la branche `spawn_child_body` ci-dessous). Avant ce
            // correctif, l'appel était placé ICI — donc exécuté par les DEUX
            // branches. Le fils hérite (COW) `g_mutex`/`g_released`/`g_slots`
            // du parent : si l'état interne de `pthread_cond_t g_released`
            // était, à l'instant précis du `fork()`, en cours de transition
            // dans un AUTRE thread du parent (un réveil de
            // `pthread_cond_wait` pas encore totalement achevé — le fils n'a
            // par construction copié QUE le thread appelant, jamais les
            // autres), le fils hérite un instantané figé et incohérent de
            // cette condvar ; son propre `pthread_cond_broadcast` dessus peut
            // alors rester bloqué à jamais sur une comptabilité interne que
            // plus aucun thread (le fils n'en a qu'un, fraîchement créé) ne
            // peut jamais compléter. Un piège général et documenté de
            // `fork()` combiné aux condvars (POSIX ne garantit leur
            // cohérence dans le fils que si elles étaient IDLE au moment du
            // fork — jamais si un AUTRE thread du parent était en transition
            // dessus), pas un bug glibc précis : cohérent avec la
            // reproduction sur deux architectures et deux versions de glibc
            // très éloignées. Le fils n'a de toute façon RIEN à relâcher : il
            // vient de naître avec un seul thread, aucun participant à lui.
            if (child_pid != 0) {
                fork_gate_release_quiesce();

                if (child_pid == -1) {
                    // Échec de création de CE process : on le signale et on
                    // poursuit avec les autres — cf. main.c historique.
                    log_error("fork error : process %i/%i non créé (errno=%i)\n",
                              c + 1, NB_THREADS, errno);
                    fork_error++;
                    childrens_pid[c] = -1;
                    if (fork_error >= 10) {
                        log_error("création de process interrompue après %i échecs ; "
                                  "poursuite avec les process déjà créés\n", fork_error);
                        break;
                    }
                    continue;
                }
#ifdef DEBUG_THREAD
                log_info("child %i created\n", child_pid);
#endif // DEBUG_THREAD
                int sp_len = sprintf(forkId[c], "etii_fork.%d", child_pid);
                forkId[c][sp_len] = '\0';
                childrens_pid[c] = child_pid;
                int childStatus = 0;
                waitpid(child_pid, &childStatus, WNOHANG);
                if (childStatus != 0) {
                    log_error("child %i error %i\n", child_pid, childStatus);
                    c--;
                    continue;
                }
#ifndef DEBUG_IN_MONO_PROCESS
            } else {
                // Neutralise IMMÉDIATEMENT, avant tout autre code (y compris
                // le log_info DEBUG_THREAD ci-dessous), l'atexit(status_zone_teardown)
                // hérité du parent : status_zone_init() (console.c, appelée
                // AVANT tout fork, cf. le démarrage différé plus haut) l'enregistre dans le parent, et
                // fork() duplique la liste des handlers atexit — ce fils
                // l'hérite donc aussi, bien qu'il ne "possède" jamais le
                // terminal partagé. Sans ce garde-fou, le exit() normal de ce
                // fils (spawn_child_body, en fin de recherche OU après un
                // SIGINT/SIGTERM de stopForks/configApply) ré-exécute ce
                // handler hérité et restaure le terminal (endwin() en
                // NCURSES=1, région de défilement complète en ANSI) — visible
                // depuis le PARENT puisque le terminal est un état PARTAGÉ,
                // pas un état par-process : "on quitte le mode ncurses" à
                // chaque fils qui meurt proprement (SIGKILL, qui saute
                // atexit, n'est pas concerné — d'où le caractère
                // intermittent observé). `status_zone_disown_child()` ne
                // touche JAMAIS le terminal lui-même : elle ne fait que
                // rendre le handler hérité NO-OP dans CE process (écriture
                // dans la copie COW du fils, sans effet sur le parent).
                status_zone_disown_child();
#endif // DEBUG_IN_MONO_PROCESS
#ifdef DEBUG_THREAD
                log_info("NEW thread %i\n", getpid());
#endif // DEBUG_THREAD
                spawn_child_body(c);
                /* Jamais atteint : spawn_child_body() se termine par exit(). */
            }
        }
    }

    int created = count_created_forks(childrens_pid, NB_THREADS);
    if (created == 0) {
        log_error("orchestrateur : aucun process enfant n'a pu être créé\n");
        return 0;
    }
    if (fork_error > 0) {
        log_info("%i/%i process créés ; %i non créés (ressources insuffisantes) — poursuite\n",
                 created, NB_THREADS, fork_error);
    } else {
        // Confirmation explicite du succès complet : avant ce log, un
        // démarrage/redémarrage sans le moindre échec ne laissait AUCUNE
        // trace de son résultat effectif (seul le côté échec était couvert
        // ci-dessus) — l'opérateur devait déduire le succès de l'absence
        // d'erreur plutôt que de le lire directement.
        log_info("orchestrateur : %i process de recherche démarrés\n", created);
    }

    g_active_forks = created;
    control_channel_request_reconnect();

    // Base du filet de sécurité "fork(s) sans indicateur après démarrage"
    // (cf. STUCK_FORKS_WARN_MS) : un avertissement par SLOT, jamais réémis
    // pour le même slot tant que ce (re)fork tourne. `calloc` peut renvoyer
    // NULL (OOM) : toléré, le filet se désactive silencieusement plutôt que
    // de planter — diagnostic seulement, jamais critique.
    free(g_stuck_fork_warned);
    g_stuck_fork_warned = calloc((size_t)NB_THREADS, sizeof(int));
    g_running_since_ms = current_time_ms();

    return created;
}

/**
 * @brief Séquence d'arrêt/escalade/récolte des fils vivants (ORCH_STOPPING).
 *
 * SIGCHLD masqué pour toute la durée SUR CE THREAD (`pthread_sigmask`) :
 * `sigchld_handler` moissonne en `WNOHANG` sur N'IMPORTE QUEL pid, ce qui
 * rendrait le `waitpid(pid, …)` ciblé ci-dessous non déterministe sans ce
 * masquage. SIGINT à chaque slot vivant,
 * puis scrutation bornée (`waitpid(pid, …, WNOHANG)`, cadence `MICRO_SLEEP`)
 * avec escalade `stop_escalation_next` (SIGTERM à +5 s, SIGKILL à +10 s) —
 * un process déjà mort au moment du SIGINT (recherche terminée entre-temps,
 * `--stop-on-solution`) est simplement récolté au premier tour, sans jamais
 * recevoir d'escalade. Slots nettoyés au fil de l'eau, comme
 * `reap_dead_child_slots`. Ne retourne qu'une fois tous les slots vides.
 *
 * Le masquage de SIGCHLD ne porte QUE sur ce thread — les autres threads du
 * parent (checker, `server_tcp`, canal de contrôle, console) ne le bloquent
 * pas. Un enfant qui meurt pendant cette séquence peut donc être moissonné
 * par `sigchld_handler` sur N'IMPORTE LEQUEL de ces autres threads AVANT que
 * le `waitpid(pid, …)` ci-dessous n'ait sa chance : sans
 * `waitpid_target_is_reaped` (qui traite `-1`/`ECHILD` — « déjà réclamé
 * ailleurs » — comme une mort, pas comme « encore vivant ») la boucle
 * tournait INDÉFINIMENT, croyant l'enfant toujours vivant même après
 * escalade SIGKILL. Bogue réel trouvé en testant manuellement `configApply`
 * (état `STOPPING` qui ne se résorbait jamais).
 */
static void orchestrator_do_stop_forks(void)
{
    if (childrens_pid == NULL) {
        return;
    }

    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block_set, &old_set);

    int any_live = 0;
    for (int c = 0; c < NB_THREADS; c++) {
        if (childrens_pid[c] > 0) {
            kill(childrens_pid[c], SIGINT);
            any_live = 1;
        }
    }

    if (any_live) {
        long start_ms = current_time_ms();
        stop_escalation_action_t last_action = STOP_ESCALATION_NONE;
        for (;;) {
            int remaining = 0;
            for (int c = 0; c < NB_THREADS; c++) {
                pid_t pid = childrens_pid[c];
                if (pid <= 0) {
                    continue;
                }
                int status = 0;
                errno = 0;
                pid_t r = waitpid(pid, &status, WNOHANG);
                int wait_errno = errno;
                if (waitpid_target_is_reaped(r, pid, wait_errno)) {
                    childrens_pid[c] = -1;
                    if (forkId[c] != NULL) {
                        forkId[c][0] = '\0';
                    }
                    memset(&fork_statistics[c], 0, sizeof(fork_statistics[c]));
                } else {
                    remaining++;
                }
            }
            if (remaining == 0) {
                break;
            }

            long elapsed_ms = current_time_ms() - start_ms;
            stop_escalation_action_t action = stop_escalation_next(elapsed_ms);
            if (action != last_action && action != STOP_ESCALATION_NONE) {
                int sig = (action == STOP_ESCALATION_SIGKILL) ? SIGKILL : SIGTERM;
                log_error("orchestrateur : %i fils encore vivants après %lds — escalade %s\n",
                          remaining, elapsed_ms / 1000,
                          action == STOP_ESCALATION_SIGKILL ? "SIGKILL" : "SIGTERM");
                for (int c = 0; c < NB_THREADS; c++) {
                    if (childrens_pid[c] > 0) {
                        kill(childrens_pid[c], sig);
                    }
                }
            }
            last_action = action;
            usleep(MICRO_SLEEP);
        }
    }

    pthread_sigmask(SIG_SETMASK, &old_set, NULL);

    g_active_forks = 0;
    control_channel_request_reconnect();
    log_info("orchestrateur : fils arrêtés\n");
}

int orchestrator_apply_restart_config(struct search_parts *shared_parts)
{
    // Quiescence coopérative (ORCH_APPLYING) : les lecteurs des tableaux
    // sont garés, donc aucun mutex dédié n'est nécessaire — OUBLIÉE dans la
    // première version de cette fonction. `childrens_pid`/`forkId`/`fork_statistics` sont libérés PUIS
    // réalloués ci-dessous quand `nb_forks` change, et la map de recherche
    // partagée est libérée PUIS reconstruite quand `parts_file` change ; sans
    // garer le checker, `server_tcp`, le canal de contrôle et la console (les
    // quatre lecteurs concurrents de ces tableaux), n'importe lequel peut
    // déréférencer un pointeur déjà libéré pendant la fenêtre de
    // reconstruction. Signalé par un crash réel du thread console sous
    // `NCURSES=1` (rafraîchissement très fréquent de la bannière de stats,
    // donc fenêtre de course touchée presque systématiquement) et, plus
    // discrètement en mode ANSI, par une configuration qui « ne semblait pas
    // prise en compte » (même course, juste moins souvent fatale — un lecteur
    // concurrent pouvant aussi écrire dans un slot fraîchement réalloué au
    // mauvais moment). Locké par `apply_restart_config_quiesces_concurrent_array_readers`
    // (tests/app/test_fork_orchestrator.c), qui prouve — par construction,
    // pas par mesure de timing — qu'un lecteur en boucle serrée ne peut
    // jamais observer ces tableaux à NULL pendant cet appel.
    //
    // Jamais dans le doute : un timeout ici annule TOUT le redémarrage à
    // chaud (rien n'est modifié — configuration, tableaux et map restent
    // inchangés) plutôt que de risquer la même corruption avec un budget de
    // temps expiré. Les fils sont déjà arrêtés à ce stade (appelée après
    // `orchestrator_do_stop_forks`) : l'appelant retombe simplement en
    // `ORCH_WAITING_CONFIG`, l'opérateur peut retenter `configApply`/`start`.
    fork_gate_result_t qr = fork_gate_request_quiesce(FORK_GATE_DEFAULT_TIMEOUT_MS);
    if (qr != FORK_GATE_QUIESCED) {
        log_error("orchestrateur : quiescence non atteinte — redémarrage à chaud refusé "
                  "(jamais de reconstruction dans le doute)\n");
        return 0;
    }

    int old_nb_threads = NB_THREADS;
    char *old_parts_files = (parts_files != NULL) ? strdup(parts_files) : NULL;

    fork_orchestrator_apply_staged_config();

    if (NB_THREADS != old_nb_threads) {
        log_info("orchestrateur : nb_forks %i -> %i — reconstruction des tableaux de fils\n",
                  old_nb_threads, NB_THREADS);
        free_childs();
        init_childs();
        init_counters();
    }

    int parts_file_changed = (parts_files != NULL) &&
        (old_parts_files == NULL || strcmp(parts_files, old_parts_files) != 0);
    if (parts_file_changed && shared_parts != NULL) {
        log_info("orchestrateur : parts_file \"%s\" -> \"%s\" — reconstruction de la map de recherche\n",
                  old_parts_files != NULL ? old_parts_files : "(aucun)", parts_files);
        set_inherited_search_parts(NULL);
        free_search_parts(shared_parts);
        build_search_parts(shared_parts, parts_files);
        set_inherited_search_parts(shared_parts);
    }

    free(old_parts_files);

    fork_gate_release_quiesce();
    return 1;
}

/** @brief Calcule la prochaine échéance absolue à `ORCH_TICK_MS` de maintenant. */
static void next_tick_deadline(struct timespec *deadline)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_nsec += (ORCH_TICK_MS % 1000) * 1000000L;
    deadline->tv_sec += ORCH_TICK_MS / 1000;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_nsec -= 1000000000L;
        deadline->tv_sec += 1;
    }
}

void fork_orchestrator_init_state(int config_loaded_at_boot)
{
    long now_ms = current_time_ms();

    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    g_orch_pending_spawn = 0;
    if (config_loaded_at_boot) {
        g_orch_state = ORCH_COUNTDOWN;
        g_countdown_deadline_ms = now_ms + ORCH_COUNTDOWN_MS;
    } else {
        g_orch_state = ORCH_WAITING_CONFIG;
        g_countdown_deadline_ms = 0;
    }
    pthread_mutex_unlock(&g_orch_mutex);
}

void fork_orchestrator_run(int config_loaded_at_boot, search_parts_t *shared_parts)
{
    long now_ms;

    if (!config_loaded_at_boot) {
        // log_console (pas log_info) : ce message doit être visible
        // IMMÉDIATEMENT, avant même que le thread console ait atteint sa
        // première lecture bloquante — log_info n'est flushé que si une
        // saisie interactive est déjà active (cf. write_stream_locked,
        // src/ui/logger.c), ce qui n'est pas garanti à ce stade puisque ce
        // log survient juste après le lancement (asynchrone) du thread
        // console, pas après.
        log_console("orchestrateur : aucune configuration trouvée — en attente de "
                  "\"start\" ou \"config <clé> <valeur>\"\n");
    } else {
        // Affichée AVANT le décompte : l'opérateur doit pouvoir juger, sans
        // rien taper, si la configuration chargée lui convient ou s'il doit
        // l'interrompre pour la modifier (config <clé> <valeur>) ou la
        // confirmer immédiatement (start). log_console (voir commentaire
        // ci-dessus) : sans le flush explicite, ce bloc restait invisible
        // dans le tampon de stdio jusqu'au premier événement qui le vidait
        // par ailleurs (ou jamais, en mode --headless).
        client_config_t effective;
        client_config_capture_effective(&effective, g_client_server_host);
        char buf[1024];
        client_config_format(&effective, buf, sizeof(buf));
        client_config_free(&effective);
        log_console("orchestrateur : configuration chargée depuis \"%s\" — "
                  "démarrage automatique dans %ds sauf interruption :\n%s",
                  client_config_file_path, ORCH_COUNTDOWN_MS / 1000,
                  buf[0] != '\0' ? buf : "  (aucune valeur)\n");
    }

    int ever_running = 0;
    long last_logged_sec = -1;
    /* 1 quand le process parent a délibérément zéro fils vivant (arrêt via
       `stopForks`, ou fenêtre STOPPING/APPLYING d'un `configApply`
       NEEDS_RESTART) : sans ce drapeau, la condition de sortie de boucle
       ci-dessous — historiquement "plus aucun fork ET on a déjà tourné" —
       confondrait cet arrêt VOLONTAIRE avec la fin naturelle des fils
       (solution + --stop-on-solution, ou tous morts) et terminerait le
       process PARENT tout entier, à l'exact opposé de l'objectif : arrêter
       les fils sans jamais arrêter le process principal.
       Remis à 0 dès qu'un (re)fork réussit. */
    int forks_parked = 0;

    for (;;) {
        struct timespec deadline;
        next_tick_deadline(&deadline);

        pthread_mutex_lock(&g_orch_mutex);
        while (!g_orch_pending_spawn) {
            int rc = pthread_cond_timedwait(&g_orch_cond, &g_orch_mutex, &deadline);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
        int do_spawn = g_orch_pending_spawn;
        g_orch_pending_spawn = 0;
        orch_state_t state_snapshot = g_orch_state;
        pthread_mutex_unlock(&g_orch_mutex);

        now_ms = current_time_ms();

        // Drainage des morts d'enfants capturées par sigchld_handler (cf.
        // app_runtime.h) — à CHAQUE tour, quel que soit l'état : un fork peut
        // mourir pendant STOPPING/APPLYING (arrêt piloté, attendu) tout comme
        // pendant RUNNING (inattendu, la vraie cible de ce filet). On draine
        // systématiquement pour ne jamais laisser le ring déborder entre deux
        // passages en RUNNING, mais on n'ALARME (log_error) que si la mort
        // n'est pas expliquée par une séquence d'arrêt en cours.
        // Compte, pour ce seul tour, les morts classées "disparu de façon
        // inattendue" ci-dessous (ni sortie propre, ni arrêt piloté) — lu par
        // la branche ORCH_RUNNING un peu plus bas pour choisir le ton du
        // résumé de `reap_dead_child_slots` (log_error seulement si au moins
        // une mort de CE tour était réellement anormale, jamais pour un tour
        // qui n'a nettoyé que des sorties propres).
        int nb_unexpected_deaths_this_tick = 0;
        {
            child_death_record_t deaths[CHILD_DEATH_RING_CAPACITY];
            int nb_deaths = child_death_drain(deaths, CHILD_DEATH_RING_CAPACITY);
            for (int d = 0; d < nb_deaths; d++) {
                char reason[64];
                child_death_format_reason(deaths[d].status, reason, sizeof(reason));
                if (child_death_is_clean_exit(deaths[d].status)) {
                    // Sortie normale (code 0) : TOUJOURS bénin, quel que soit
                    // l'état — un fork peut légitimement s'arrêter de
                    // lui-même en exhaustant tout son espace de recherche
                    // local (cf. child_death_is_clean_exit) sans que cela
                    // n'ait le moindre rapport avec stopForks/configApply.
                    // Vérifié EN PREMIER, avant les branches par état
                    // ci-dessous, pour ne jamais le classer à tort comme
                    // "disparu de façon inattendue".
                    log_info("orchestrateur : fork %d terminé proprement (%s)\n",
                              (int)deaths[d].pid, reason);
                } else if (state_snapshot == ORCH_STOPPING || state_snapshot == ORCH_APPLYING) {
                    log_info("orchestrateur : fork %d terminé (%s) — arrêt piloté en cours\n",
                              (int)deaths[d].pid, reason);
                } else if (state_snapshot == ORCH_RUNNING) {
                    nb_unexpected_deaths_this_tick++;
                    log_error("orchestrateur : fork %d disparu de façon inattendue (%s)\n",
                              (int)deaths[d].pid, reason);
                } else {
                    // WAITING_CONFIG/COUNTDOWN/CONFIGURING/EXITING : aucun fork
                    // ne devrait exister ici — trace tardive d'une mort déjà
                    // traitée par ailleurs (ex. queue de la séquence d'arrêt
                    // juste avant le changement d'état ci-dessous).
                    log_info("orchestrateur : fork %d terminé (%s)\n", (int)deaths[d].pid, reason);
                }
            }
            int dropped = child_death_dropped_count();
            if (dropped > 0) {
                log_error("orchestrateur : %d évènement(s) de mort d'enfant perdu(s) "
                          "(ring de diagnostic saturé entre deux tours)\n", dropped);
            }
        }

        if (do_spawn) {
            // Applique la configuration "en préparation" (config <clé>
            // <valeur>) aux globales AVANT le fork effectif — pour un start
            // manuel comme pour un décompte qui va à son terme, même point
            // de code : sans cela, "config nb_forks 8" puis "start" forkait
            // toujours avec l'ancienne valeur, un redémarrage du process
            // étant sinon nécessaire (cf. fork_orchestrator_apply_staged_config).
            fork_orchestrator_apply_staged_config();
            // `childrens_pid`/`forkId`/`fork_statistics` sont dimensionnés par
            // `init_childs()` sur le NB_THREADS D'ORIGINE, avant tout fork — un
            // `nb_forks` stagé au-dessus de cette capacité vient d'être écrit
            // dans NB_THREADS par l'appel ci-dessus. Sans cet agrandissement,
            // la boucle de `orchestrator_spawn_forks` écrivait hors bornes dans
            // ces tableaux : crash réel reproduit par un opérateur (segfault du
            // parent, les fils déjà forkés restant vivants) via exactement ce
            // scénario : démarrage, `config nb_forks <n plus grand>`,
            // `configSave`, `start`.
            ensure_childs_capacity(NB_THREADS);
            int created = orchestrator_spawn_forks();
            if (created > 0) {
                ever_running = 1;
                forks_parked = 0;
            } else {
                // Aucun process créé (quiescence en échec pour chaque slot
                // tenté, ou ressources épuisées) : jamais de crash, on
                // redonne la main à l'opérateur. `forks_parked` reste/passe à 1
                // pour qu'un `configApply` NEEDS_RESTART dont le re-fork échoue
                // laisse le parent EN VIE, en attente d'un nouveau `start` —
                // sans lui la condition de sortie de boucle terminerait le
                // process au tour suivant (ever_running déjà vrai depuis avant).
                log_error("orchestrateur : démarrage échoué — retour en attente de configuration\n");
                pthread_mutex_lock(&g_orch_mutex);
                g_orch_state = ORCH_WAITING_CONFIG;
                pthread_mutex_unlock(&g_orch_mutex);
                forks_parked = 1;
            }
        } else if (state_snapshot == ORCH_COUNTDOWN) {
            long deadline_ms;
            pthread_mutex_lock(&g_orch_mutex);
            deadline_ms = g_countdown_deadline_ms;
            pthread_mutex_unlock(&g_orch_mutex);

            if (orchestrator_countdown_elapsed(deadline_ms, now_ms)) {
                // Même chemin EXACT qu'un `start` manuel (cf. fork_orchestrator.h) :
                // le fork effectif attend le prochain tour (do_spawn), une
                // fois pending_spawn observé.
                fork_orchestrator_post_event(EV_START, NULL);
            } else {
                long remaining_ms = deadline_ms - now_ms;
                long whole_sec = (remaining_ms + 999) / 1000;
                if (whole_sec != last_logged_sec) {
                    last_logged_sec = whole_sec;
                    log_console("orchestrateur : auto-démarrage dans %lds "
                              "(toute commande de configuration l'annule)\n", whole_sec);
                }
            }
        } else if (state_snapshot == ORCH_RUNNING) {
            int cleaned = reap_dead_child_slots(childrens_pid, forkId, fork_statistics, NB_THREADS, NULL);
            if (cleaned > 0) {
                // Avant ce bloc, un nettoyage de slots ici ne mettait JAMAIS
                // à jour `g_active_forks` (contrairement à
                // `orchestrator_do_stop_forks`, qui le fait pour un arrêt
                // piloté) : le canal de contrôle continuait d'annoncer au
                // serveur un nombre de forks qui n'existaient déjà plus.
                int remaining = count_created_forks(childrens_pid, NB_THREADS);
                g_active_forks = remaining;
                control_channel_request_reconnect();
                // Ton du résumé aligné sur les lignes détaillées ci-dessus :
                // log_error seulement si au moins une des morts nettoyées ce
                // tour était réellement anormale (nb_unexpected_deaths_this_tick) —
                // un tour qui n'a nettoyé que des sorties propres (ex. un
                // tout petit puzzle exhaustant tout son espace de recherche)
                // ne doit jamais s'afficher comme une alarme.
                if (nb_unexpected_deaths_this_tick > 0) {
                    log_error("orchestrateur : %d fork(s) disparu(s) de façon inattendue, "
                              "%d restant(s) sur %d — voir les lignes ci-dessus pour la cause\n",
                              cleaned, remaining, NB_THREADS);
                } else {
                    log_info("orchestrateur : %d fork(s) terminé(s), %d restant(s) sur %d\n",
                              cleaned, remaining, NB_THREADS);
                }
                fork_orchestrator_post_event(EV_CHILD_DIED, NULL);
            }

            // Filet de sécurité PAR FORK : un slot vivant qui ne rapporte
            // RIEN depuis STUCK_FORKS_WARN_MS — le symptôme rapporté à
            // plusieurs reprises. PAR SLOT et non par agrégat : reproduit en
            // conditions réelles (256 pièces, nb_forks=3), seuls 2 des 3
            // forks étaient bloqués à zéro pendant que le 3ᵉ travaillait
            // normalement — un agrégat "tous à zéro" ne se déclenche jamais
            // dans ce cas précis (cf. g_stuck_fork_warned). Un avertissement
            // par SLOT, jamais réémis pour le même slot tant que ce (re)fork
            // tourne (`g_stuck_fork_warned[c]`, remis à zéro au (re)fork
            // suivant par `orchestrator_spawn_forks`).
            if (g_running_since_ms != 0 && g_stuck_fork_warned != NULL
                && stuck_forks_threshold_elapsed(g_running_since_ms, now_ms)) {
                for (int c = 0; c < NB_THREADS; c++) {
                    if (childrens_pid[c] <= 0 || g_stuck_fork_warned[c]) {
                        continue;
                    }
                    if (fork_stat_is_zero(&fork_statistics[c])) {
                        g_stuck_fork_warned[c] = 1;
                        log_error("orchestrateur : fork %d (slot %d) ne rapporte aucun "
                                  "travail (stock/analysé/coups-s à 0) depuis %lds — "
                                  "vérifier CE fork spécifiquement (connexion serveur "
                                  "bloquée, deadlock, ou stock serveur épuisé pour lui)\n",
                                  (int)childrens_pid[c], c,
                                  (now_ms - g_running_since_ms) / 1000);
                    }
                }
            }
        } else if (state_snapshot == ORCH_STOPPING) {
            // Séquence bornée (~10 s au pire) exécutée SYNCHRONEMENT sur ce
            // thread — c'est le seul thread qui forke/attend ses enfants (D1),
            // donc rien d'autre ne peut avancer le cycle de vie pendant ce
            // temps de toute façon.
            // Log de DÉBUT de séquence (avant, pas seulement le "fils arrêtés"
            // de fin déjà loggé par orchestrator_do_stop_forks) : sans lui,
            // rien ne distinguait dans les logs un arrêt en cours (potentiellement
            // plusieurs secondes, escalade SIGTERM/SIGKILL incluse) d'un
            // orchestrateur simplement inactif. Lu sous mutex (comme juste en
            // dessous) plutôt que la globale directement, par cohérence avec
            // le reste de cette fonction.
            int restart_after_stop;
            pthread_mutex_lock(&g_orch_mutex);
            restart_after_stop = g_restart_after_stop;
            pthread_mutex_unlock(&g_orch_mutex);
            log_info("orchestrateur : arrêt des fils en cours (%s)\n",
                      restart_after_stop ? "redémarrage à chaud" : "stopForks");
            orchestrator_do_stop_forks();

            if (restart_after_stop) {
                pthread_mutex_lock(&g_orch_mutex);
                g_orch_state = ORCH_APPLYING;
                pthread_mutex_unlock(&g_orch_mutex);

                forks_parked = 1; // levé par le prochain do_spawn réussi

                log_info("orchestrateur : application de la configuration à chaud "
                          "(reconstruction éventuelle des structures) en cours\n");
                if (orchestrator_apply_restart_config(shared_parts)) {
                    // Même chemin EV_START qu'un `start` manuel ou qu'un
                    // décompte écoulé (cf. orchestrator_step) : le fork
                    // effectif attend le prochain tour, une fois
                    // pending_spawn observé.
                    fork_orchestrator_post_event(EV_START, NULL);
                } else {
                    // Quiescence refusée (timeout) : rien n'a été modifié —
                    // jamais de reconstruction dans le doute. Les fils sont
                    // déjà arrêtés (orchestrator_do_stop_forks ci-dessus) ;
                    // on retombe en WAITING_CONFIG plutôt que de tenter un
                    // fork avec un état potentiellement à moitié appliqué.
                    pthread_mutex_lock(&g_orch_mutex);
                    g_orch_state = ORCH_WAITING_CONFIG;
                    pthread_mutex_unlock(&g_orch_mutex);
                }
            } else {
                pthread_mutex_lock(&g_orch_mutex);
                g_orch_state = ORCH_WAITING_CONFIG;
                pthread_mutex_unlock(&g_orch_mutex);
                forks_parked = 1;
                log_console("orchestrateur : fils arrêtés — en attente de "
                          "\"start\" ou \"config <clé> <valeur>\"\n");
            }
        }

        int remaining_forks = count_created_forks(childrens_pid, NB_THREADS);
        if (remaining_forks == 0 && (request == REQUEST_STOP || (ever_running && !forks_parked))) {
            break;
        }
    }
}
