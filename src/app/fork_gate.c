#include "app/fork_gate.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif // __linux__

#include "ui/logger.h"

/** @brief État d'un participant enregistré. */
typedef enum {
    FORK_GATE_ST_RUNNING = 0, /**< Tour de boucle normal, aucune quiescence en attente pour lui. */
    FORK_GATE_ST_PARKED,      /**< Garé sur `g_released` (fork_gate_checkpoint). */
    FORK_GATE_ST_BLOCKED,     /**< Dans un appel bloquant connu sans verrou (fork_gate_mark_blocked). */
} fork_gate_state_t;

typedef struct {
    int in_use;
    fork_gate_state_t state;
    char name[24];
} fork_gate_slot_t;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Signalée à chaque changement d'état d'un slot (enregistrement,
   désenregistrement, PARKED, BLOCKED) : c'est ce que fork_gate_request_quiesce
   attend pour réévaluer si tous les participants sont quiescents. */
static pthread_cond_t g_state_changed = PTHREAD_COND_INITIALIZER;
/* Signalée par fork_gate_release_quiesce (et par l'abandon sur timeout) :
   c'est ce sur quoi les threads garés en RUNNING->PARKED attendent. */
static pthread_cond_t g_released = PTHREAD_COND_INITIALIZER;

static fork_gate_slot_t g_slots[FORK_GATE_MAX_PARTICIPANTS];

/* Lu par le chemin rapide de fork_gate_checkpoint SANS prendre g_mutex : une
   seule lecture atomique par tour de boucle, au même patron que les compteurs
   fc_attempts/fc_pruned de etii_client.c. Toute écriture passe par g_mutex. */
static volatile int g_quiesce_requested = 0;

/* Journal de trace (diagnostic, cf. fork_gate.h) : ring signal-safe rempli par
   un simple __atomic_fetch_add, même patron que child_death_record
   (app_runtime.c). Délibérément SANS verrou et SANS appel système sur le
   chemin chaud (clock_gettime passe par le VDSO sous Linux — pas de
   syscall() réel), pour rester invisible à strace/ptrace : entourer le
   process de strace, même limité à quelques appels système, a empêché la
   reproduction du blocage étudié (le ralentissement introduit referme la
   fenêtre de course) — ce journal est la façon d'observer sans perturber. */
fork_gate_trace_record_t g_fork_gate_trace_buf[FORK_GATE_TRACE_CAPACITY];
unsigned long g_fork_gate_trace_write_index = 0;

static long trace_tid(void)
{
#ifdef __linux__
    return (long)syscall(SYS_gettid);
#else
    return (long)(unsigned long)pthread_self();
#endif // __linux__
}

void fork_gate_trace_record(fork_gate_trace_event_t event, int slot)
{
    unsigned long idx = __atomic_fetch_add(&g_fork_gate_trace_write_index, 1, __ATOMIC_RELAXED)
        % FORK_GATE_TRACE_CAPACITY;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_fork_gate_trace_buf[idx].timestamp_ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    g_fork_gate_trace_buf[idx].tid = trace_tid();
    g_fork_gate_trace_buf[idx].slot = slot;
    g_fork_gate_trace_buf[idx].event = event;
}

void fork_gate_reset(void)
{
    pthread_mutex_lock(&g_mutex);
    memset(g_slots, 0, sizeof(g_slots));
    __atomic_store_n(&g_quiesce_requested, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_mutex);
}

int fork_gate_register(const char *name)
{
    pthread_mutex_lock(&g_mutex);
    int slot = -1;
    for (int i = 0; i < FORK_GATE_MAX_PARTICIPANTS; i++) {
        if (!g_slots[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_mutex);
        return FORK_GATE_FULL;
    }
    g_slots[slot].in_use = 1;
    g_slots[slot].state = FORK_GATE_ST_RUNNING;
    g_slots[slot].name[0] = '\0';
    if (name != NULL) {
        strncpy(g_slots[slot].name, name, sizeof(g_slots[slot].name) - 1);
    }
    pthread_cond_broadcast(&g_state_changed);
    pthread_mutex_unlock(&g_mutex);
    fork_gate_trace_record(FGT_REGISTER, slot);
    return slot;
}

void fork_gate_unregister(int slot)
{
    if (slot < 0 || slot >= FORK_GATE_MAX_PARTICIPANTS) {
        return;
    }
    fork_gate_trace_record(FGT_UNREGISTER, slot);
    pthread_mutex_lock(&g_mutex);
    g_slots[slot].in_use = 0;
    pthread_cond_broadcast(&g_state_changed);
    pthread_mutex_unlock(&g_mutex);
}

void fork_gate_checkpoint(int slot)
{
    if (slot < 0 || slot >= FORK_GATE_MAX_PARTICIPANTS) {
        return;
    }
    /* Chemin rapide : l'écrasante majorité des tours de boucle ne voient
       aucune quiescence demandée. */
    if (!__atomic_load_n(&g_quiesce_requested, __ATOMIC_RELAXED)) {
        return;
    }

    pthread_mutex_lock(&g_mutex);
    if (!g_quiesce_requested) {
        /* Levée entre la lecture rapide et l'acquisition du verrou. */
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    g_slots[slot].state = FORK_GATE_ST_PARKED;
    pthread_cond_broadcast(&g_state_changed);
    fork_gate_trace_record(FGT_PARK_BEGIN, slot);
    while (g_quiesce_requested) {
        pthread_cond_wait(&g_released, &g_mutex);
    }
    fork_gate_trace_record(FGT_PARK_END, slot);
    g_slots[slot].state = FORK_GATE_ST_RUNNING;
    pthread_mutex_unlock(&g_mutex);
}

void fork_gate_mark_blocked(int slot, int blocked)
{
    if (slot < 0 || slot >= FORK_GATE_MAX_PARTICIPANTS) {
        return;
    }
    fork_gate_trace_record(blocked ? FGT_BLOCKED_ON : FGT_BLOCKED_OFF, slot);
    pthread_mutex_lock(&g_mutex);
    g_slots[slot].state = blocked ? FORK_GATE_ST_BLOCKED : FORK_GATE_ST_RUNNING;
    pthread_cond_broadcast(&g_state_changed);
    pthread_mutex_unlock(&g_mutex);
}

/** @brief Vrai si tous les participants enregistrés sont PARKED ou BLOCKED. Appelant sous g_mutex. */
static int all_participants_quiescent_locked(void)
{
    for (int i = 0; i < FORK_GATE_MAX_PARTICIPANTS; i++) {
        if (g_slots[i].in_use && g_slots[i].state == FORK_GATE_ST_RUNNING) {
            return 0;
        }
    }
    return 1;
}

fork_gate_result_t fork_gate_request_quiesce(long timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }

    fork_gate_trace_record(FGT_REQUEST_QUIESCE_BEGIN, -1);
    pthread_mutex_lock(&g_mutex);
    __atomic_store_n(&g_quiesce_requested, 1, __ATOMIC_RELAXED);

    while (!all_participants_quiescent_locked()) {
        int rc = pthread_cond_timedwait(&g_state_changed, &g_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            if (all_participants_quiescent_locked()) {
                break;
            }
            /* Budget écoulé sans quiescence complète : on ne fork JAMAIS dans
               le doute. On annule la demande et on relâche tout
               participant déjà garé avant de rendre la main. */
            __atomic_store_n(&g_quiesce_requested, 0, __ATOMIC_RELAXED);
            pthread_cond_broadcast(&g_released);
            pthread_mutex_unlock(&g_mutex);
            fork_gate_trace_record(FGT_REQUEST_QUIESCE_TIMEOUT, -1);
            return FORK_GATE_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    fork_gate_trace_record(FGT_REQUEST_QUIESCE_QUIESCED, -1);
    return FORK_GATE_QUIESCED;
}

void fork_gate_release_quiesce(void)
{
    fork_gate_trace_record(FGT_RELEASE_QUIESCE_BEGIN, -1);
    pthread_mutex_lock(&g_mutex);
    __atomic_store_n(&g_quiesce_requested, 0, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&g_released);
    pthread_mutex_unlock(&g_mutex);
    // Si le process se retrouve un jour bloqué à demeure dans le
    // pthread_cond_broadcast ci-dessus (cf. docs/investigations/blocage_fork_gate_release_quiesce.md),
    // cette trace n'est JAMAIS atteinte — l'entrée FGT_RELEASE_QUIESCE_BEGIN
    // la plus récente sans FGT_RELEASE_QUIESCE_END correspondant qui la suit
    // EST alors la preuve directe, dans le journal lui-même, que c'est cet
    // appel précis qui est resté bloqué.
    fork_gate_trace_record(FGT_RELEASE_QUIESCE_END, -1);
}

int fork_gate_is_quiescing(void)
{
    return __atomic_load_n(&g_quiesce_requested, __ATOMIC_RELAXED);
}

void fork_gate_acquire_io_locks(void)
{
    // Deux correctifs découverts au premier usage réel de cette primitive,
    // en forkant réellement à chaud avec l'orchestrateur :
    //
    // 1. PAS de fflush(NULL) : il ne flush pas seulement stdout/stderr, il
    //    parcourt TOUS les FILE* ouverts du process (_fwalk) et prend le
    //    verrou de CHACUN — y compris stdin. Or le thread console détient le
    //    verrou stdio de stdin pour toute la durée de son fgetc() bloquant
    //    (mécanique interne de la libc, indépendante de
    //    fork_gate_mark_blocked : ce dernier ne fait que déclarer la
    //    quiescence au sens de CE module, il ne touche à aucun verrou libc).
    //    Un opérateur simplement assis au prompt — le cas courant —
    //    bloquait donc systématiquement ici. On ne flush que stdout/stderr.
    //
    // 2. PAS de flockfile(stdout)/flockfile(stderr) : contrairement à un
    //    pthread_mutex_t "normal" (sans suivi de propriétaire, donc sûr à
    //    déverrouiller depuis le fils — un seul thread y existe), le verrou
    //    stdio RÉCURSIF de flockfile suit un PROPRIÉTAIRE. Sous macOS, ce
    //    suivi ne survit PAS fiablement à fork() dans un process
    //    multi-thread : le fils hérite un verrou marqué comme détenu par un
    //    thread dont l'identité OS (port Mach) a changé — un flockfile()
    //    ultérieur du fils (ex. le tout premier log_info après le fork)
    //    bloque alors indéfiniment en attendant un "propriétaire" qui n'a
    //    plus cette identité (reproduit systématiquement : sample(1) montre
    //    le fils bloqué dans flockfile→_pthread_mutex_firstfit_lock_wait
    //    au tout premier log_info). C'est PRÉCISÉMENT le risque que
    //    flockfile visait à couvrir pour les AUTRES threads du parent — la
    //    quiescence coopérative le résout déjà entièrement pour eux (aucun
    //    thread parké/bloqué ne touche stdio) ;
    //    flockfile(stdout)/flockfile(stderr) par le thread FORKEUR lui-même
    //    n'apportait donc aucune protection supplémentaire (rien d'autre ne
    //    peut écrire pendant la fenêtre de quiescence) tout en introduisant
    //    ce risque d'interblocage propre à macOS. `logger_lock_output`
    //    (un pthread_mutex_t "normal", sans suivi de propriétaire) est
    //    conservé, car son déverrouillage dans le fils est sûr — mais la protection
    //    réelle contre un fork() pendant une écriture concurrente vient
    //    entièrement de la quiescence coopérative, pas de ce verrou-ci.
    logger_lock_output();
    fflush(stdout);
    fflush(stderr);
}

void fork_gate_release_io_locks(void)
{
    logger_unlock_output();
}
