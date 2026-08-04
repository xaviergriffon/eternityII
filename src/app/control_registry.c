#include "app/control_registry.h"

#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "app/static_variables.h"   /* MAX_CONTROL_SESSIONS */

/**
 * @brief Une commande en attente dans la file circulaire d'une session.
 */
typedef struct {
    uint8_t cmd;
    char command_line[CONTROL_COMMAND_LINE_MAX];
} control_pending_command_t;

/**
 * @brief État complet d'un slot du registre (non exposé, cf. control_registry.h
 *        pour la vue légère `control_session_info_t`).
 */
typedef struct {
    int in_use;
    int socket_id;
    control_hello_t hello;
    time_t last_activity;

    control_pending_command_t queue[CONTROL_SESSION_QUEUE_CAP];
    int head;   /* indice du prochain élément à dépiler */
    int count;  /* nombre d'éléments en file */

    int has_stats;
    control_stats_t stats;
    time_t stats_time;

    /* Dernière tentative de sondage automatique CTRL_GET_STATS (cf.
     * control_registry_auto_stats_due), distincte de stats_time qui ne bouge
     * que sur un CTRL_STATS effectivement décodé. */
    time_t last_auto_poll_attempt;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
} control_session_t;

static control_session_t g_sessions[MAX_CONTROL_SESSIONS];

/* Protège l'allocation/libération de slot (in_use) et les parcours globaux
 * (count/snapshot/broadcast). Distinct du mutex par session : poster/attendre
 * une commande sur une session déjà enregistrée n'a pas besoin de bloquer les
 * autres sessions. */
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* État de pause "désiré" pour tout client qui se connectera APRÈS un
 * `pause`/`resume` console : mis à jour dans
 * `control_registry_broadcast_command` et appliqué à l'enregistrement d'une
 * nouvelle session (`control_registry_register`), pour qu'un client démarrant
 * après-coup n'ait pas besoin d'une commande ré-émise manuellement. Protégé
 * par g_registry_mutex, comme le reste des parcours globaux du registre. */
static int g_desired_pause_state = 0; /* 0 = résumé (défaut), 1 = en pause */

/* Compare le premier mot de `command_line` (délimité par un espace ou la fin
 * de chaîne) à `word`, sans retokeniser (même convention que
 * `control_command_allowed`, control_protocol.c). */
static int command_line_first_word_is(const char *command_line, const char *word)
{
    if (command_line == NULL) {
        return 0;
    }
    size_t len = strlen(word);
    return strncmp(command_line, word, len) == 0 &&
           (command_line[len] == '\0' || command_line[len] == ' ');
}

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void registry_init_once(void)
{
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        pthread_mutex_init(&g_sessions[i].mutex, NULL);
        pthread_cond_init(&g_sessions[i].cond, NULL);
        g_sessions[i].in_use = 0;
        g_sessions[i].socket_id = -1;
        g_sessions[i].head = 0;
        g_sessions[i].count = 0;
        g_sessions[i].has_stats = 0;
    }
}

int control_registry_register(int socket_id, const control_hello_t *hello)
{
    pthread_once(&g_init_once, registry_init_once);
    if (hello == NULL) {
        return -1;
    }

    int idx = -1;
    pthread_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        if (!g_sessions[i].in_use) {
            idx = i;
            break;
        }
    }
    if (idx >= 0) {
        control_session_t *s = &g_sessions[idx];
        pthread_mutex_lock(&s->mutex);
        s->in_use = 1;
        s->socket_id = socket_id;
        s->hello = *hello;
        s->last_activity = time(NULL);
        s->head = 0;
        s->count = 0;
        s->has_stats = 0;
        s->last_auto_poll_attempt = s->last_activity;
        if (g_desired_pause_state) {
            /* Pré-poste "pause" dans la file toute neuve, pour que le tout
             * premier `control_session_step` de cette session l'exécute avant
             * même le premier CTRL_PING — le client rejoint dans le même état
             * que les sessions déjà actives, sans commande console à rejouer. */
            s->queue[0].cmd = CTRL_COMMAND;
            strncpy(s->queue[0].command_line, "pause", CONTROL_COMMAND_LINE_MAX - 1);
            s->queue[0].command_line[CONTROL_COMMAND_LINE_MAX - 1] = '\0';
            s->count = 1;
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return idx;
}

void control_registry_unregister(int index)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS) {
        return;
    }
    pthread_mutex_lock(&g_registry_mutex);
    control_session_t *s = &g_sessions[index];
    pthread_mutex_lock(&s->mutex);
    s->in_use = 0;
    s->socket_id = -1;
    s->head = 0;
    s->count = 0;
    s->has_stats = 0;
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_unlock(&g_registry_mutex);
}

int control_registry_post_command(int index, uint8_t cmd, const char *command_line)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS) {
        return -1;
    }
    control_session_t *s = &g_sessions[index];
    int result = -1;
    pthread_mutex_lock(&s->mutex);
    if (s->in_use && s->count < CONTROL_SESSION_QUEUE_CAP) {
        int tail = (s->head + s->count) % CONTROL_SESSION_QUEUE_CAP;
        s->queue[tail].cmd = cmd;
        s->queue[tail].command_line[0] = '\0';
        if (command_line != NULL) {
            strncpy(s->queue[tail].command_line, command_line, CONTROL_COMMAND_LINE_MAX - 1);
            s->queue[tail].command_line[CONTROL_COMMAND_LINE_MAX - 1] = '\0';
        }
        s->count++;
        s->last_activity = time(NULL);
        result = 0;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
}

int control_registry_wait_command(int index, uint8_t *out_cmd, char *out_command_line,
                                   size_t bufsize, int timeout_ms)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS || out_cmd == NULL) {
        return -1;
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    control_session_t *s = &g_sessions[index];

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    pthread_mutex_lock(&s->mutex);
    if (!s->in_use) {
        pthread_mutex_unlock(&s->mutex);
        return -1;
    }
    while (s->count == 0 && s->in_use) {
        int wr = pthread_cond_timedwait(&s->cond, &s->mutex, &ts);
        if (wr == ETIMEDOUT) {
            rc = 1;
            break;
        }
        if (wr != 0) {
            rc = -1;
            break;
        }
    }
    if (rc == 0) {
        if (!s->in_use) {
            rc = -1;
        } else if (s->count > 0) {
            control_pending_command_t *front = &s->queue[s->head];
            *out_cmd = front->cmd;
            if (out_command_line != NULL && bufsize > 0) {
                strncpy(out_command_line, front->command_line, bufsize - 1);
                out_command_line[bufsize - 1] = '\0';
            }
            s->head = (s->head + 1) % CONTROL_SESSION_QUEUE_CAP;
            s->count--;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return rc;
}

void control_registry_touch(int index)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS) {
        return;
    }
    control_session_t *s = &g_sessions[index];
    pthread_mutex_lock(&s->mutex);
    if (s->in_use) {
        s->last_activity = time(NULL);
    }
    pthread_mutex_unlock(&s->mutex);
}

int control_registry_count(void)
{
    pthread_once(&g_init_once, registry_init_once);
    int n = 0;
    pthread_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        control_session_t *s = &g_sessions[i];
        pthread_mutex_lock(&s->mutex);
        if (s->in_use) {
            n++;
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return n;
}

int control_registry_snapshot(control_session_info_t *out, int max)
{
    pthread_once(&g_init_once, registry_init_once);
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    pthread_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < MAX_CONTROL_SESSIONS && n < max; i++) {
        control_session_t *s = &g_sessions[i];
        pthread_mutex_lock(&s->mutex);
        if (s->in_use) {
            out[n].pid = s->hello.pid;
            out[n].nb_forks = s->hello.nb_forks;
            out[n].mode = s->hello.mode;
            out[n].last_activity = s->last_activity;
            out[n].has_stats = s->has_stats;
            out[n].stats = s->stats;
            out[n].stats_time = s->stats_time;
            n++;
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return n;
}

void control_registry_record_stats(int index, const control_stats_t *stats)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS || stats == NULL) {
        return;
    }
    control_session_t *s = &g_sessions[index];
    pthread_mutex_lock(&s->mutex);
    if (s->in_use) {
        s->stats = *stats;
        s->stats_time = time(NULL);
        s->has_stats = 1;
    }
    pthread_mutex_unlock(&s->mutex);
}

int control_registry_broadcast_command(uint8_t cmd, const char *command_line)
{
    pthread_once(&g_init_once, registry_init_once);
    int n = 0;
    pthread_mutex_lock(&g_registry_mutex);
    if (cmd == CTRL_COMMAND) {
        if (command_line_first_word_is(command_line, "pause")) {
            g_desired_pause_state = 1;
        } else if (command_line_first_word_is(command_line, "resume")) {
            g_desired_pause_state = 0;
        }
    }
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        if (g_sessions[i].in_use) {
            if (control_registry_post_command(i, cmd, command_line) == 0) {
                n++;
            }
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return n;
}

int control_registry_broadcast_get_stats(void)
{
    return control_registry_broadcast_command(CTRL_GET_STATS, NULL);
}

int control_registry_auto_stats_due(int index, int interval_sec)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS || interval_sec <= 0) {
        return 0;
    }
    control_session_t *s = &g_sessions[index];
    int due = 0;
    pthread_mutex_lock(&s->mutex);
    if (s->in_use) {
        time_t now = time(NULL);
        if (difftime(now, s->last_auto_poll_attempt) >= (double)interval_sec) {
            s->last_auto_poll_attempt = now;
            due = 1;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return due;
}

int control_registry_desired_pause_state(void)
{
    pthread_once(&g_init_once, registry_init_once);
    int state;
    pthread_mutex_lock(&g_registry_mutex);
    state = g_desired_pause_state;
    pthread_mutex_unlock(&g_registry_mutex);
    return state;
}
