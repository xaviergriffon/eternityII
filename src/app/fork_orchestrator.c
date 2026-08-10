#include "app/fork_orchestrator.h"

#include <errno.h>
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
        if (s == ORCH_RUNNING || s == ORCH_STOPPING || s == ORCH_APPLYING || s == ORCH_EXITING) {
            /* "erreur si déjà en RUNNING" — étendu aux autres états où
               démarrer n'a pas de sens plutôt que d'inventer un nouveau code
               d'erreur. */
            out->error = ORCH_ERR_ALREADY_RUNNING;
            break;
        }
        out->spawn_forks = 1;
        return ORCH_RUNNING;

    case EV_STOP_FORKS:
    case EV_RESTART:
        /* Réservé pour une future séquence arrêt/escalade/récolte
           (EV_STOP_FORKS) et réapplication + re-fork (EV_RESTART).
           Sémantique volontairement non devinée ici : où atterrit-on après
           STOPPING, distinction entre un simple arrêt et une réapplication —
           tout cela dépend de détails que seule une implémentation complète
           de la séquence d'arrêt peut trancher sans fabriquer un
           comportement non vérifié. */
        out->error = ORCH_ERR_UNSUPPORTED;
        break;

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

/* ============================ Driver impur ================================ */

static pthread_mutex_t g_orch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_orch_cond = PTHREAD_COND_INITIALIZER;
static orch_state_t g_orch_state = ORCH_WAITING_CONFIG;
static long g_countdown_deadline_ms = 0;
static int g_orch_pending_spawn = 0;
static client_config_t g_staged_config;
static int g_staged_config_ready = 0;

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
    pthread_mutex_unlock(&g_orch_mutex);
}

void fork_orchestrator_apply_staged_config(void)
{
    pthread_mutex_lock(&g_orch_mutex);
    ensure_staged_config_locked();
    client_config_apply_direct(&g_staged_config, &g_client_server_host);
    pthread_mutex_unlock(&g_orch_mutex);
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
 * couverture gcov/llvm-cov (cf. AGENTS.md, pas de raison ici de s'en passer).
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
            // log_info/log_error qui suit (cf. commentaire ci-dessus).
            fork_gate_release_io_locks();
            fork_gate_release_quiesce();

            if (child_pid != 0) {
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
    }

    g_active_forks = created;
    control_channel_request_reconnect();
    return created;
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

void fork_orchestrator_run(int config_loaded_at_boot)
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
            } else {
                // Aucun process créé (quiescence en échec pour chaque slot
                // tenté, ou ressources épuisées) : jamais de crash, on
                // redonne la main à l'opérateur.
                log_error("orchestrateur : démarrage échoué — retour en attente de configuration\n");
                pthread_mutex_lock(&g_orch_mutex);
                g_orch_state = ORCH_WAITING_CONFIG;
                pthread_mutex_unlock(&g_orch_mutex);
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
                fork_orchestrator_post_event(EV_CHILD_DIED, NULL);
            }
        }

        int remaining_forks = count_created_forks(childrens_pid, NB_THREADS);
        if (remaining_forks == 0 && (ever_running || request == REQUEST_STOP)) {
            break;
        }
    }
}
