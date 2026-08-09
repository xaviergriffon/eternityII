#include "app/fork_gate.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

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
    return slot;
}

void fork_gate_unregister(int slot)
{
    if (slot < 0 || slot >= FORK_GATE_MAX_PARTICIPANTS) {
        return;
    }
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
    /* Chemin rapide : la très grande majorité des tours de boucle, en
       production, ne voient jamais de quiescence demandée (PR B ne câble
       fork_gate_request_quiesce nulle part). */
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
    while (g_quiesce_requested) {
        pthread_cond_wait(&g_released, &g_mutex);
    }
    g_slots[slot].state = FORK_GATE_ST_RUNNING;
    pthread_mutex_unlock(&g_mutex);
}

void fork_gate_mark_blocked(int slot, int blocked)
{
    if (slot < 0 || slot >= FORK_GATE_MAX_PARTICIPANTS) {
        return;
    }
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

    pthread_mutex_lock(&g_mutex);
    __atomic_store_n(&g_quiesce_requested, 1, __ATOMIC_RELAXED);

    while (!all_participants_quiescent_locked()) {
        int rc = pthread_cond_timedwait(&g_state_changed, &g_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            if (all_participants_quiescent_locked()) {
                break;
            }
            /* Budget écoulé sans quiescence complète : on ne fork JAMAIS dans
               le doute (D2). On annule la demande et on relâche tout
               participant déjà garé avant de rendre la main. */
            __atomic_store_n(&g_quiesce_requested, 0, __ATOMIC_RELAXED);
            pthread_cond_broadcast(&g_released);
            pthread_mutex_unlock(&g_mutex);
            return FORK_GATE_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return FORK_GATE_QUIESCED;
}

void fork_gate_release_quiesce(void)
{
    pthread_mutex_lock(&g_mutex);
    __atomic_store_n(&g_quiesce_requested, 0, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&g_released);
    pthread_mutex_unlock(&g_mutex);
}

int fork_gate_is_quiescing(void)
{
    return __atomic_load_n(&g_quiesce_requested, __ATOMIC_RELAXED);
}

void fork_gate_acquire_io_locks(void)
{
    logger_lock_output();
    flockfile(stdout);
    flockfile(stderr);
    fflush(NULL);
}

void fork_gate_release_io_locks(void)
{
    funlockfile(stderr);
    funlockfile(stdout);
    logger_unlock_output();
}
