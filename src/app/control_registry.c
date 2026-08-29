#include "app/control_registry.h"

#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#include "app/app_static_variables.h"   /* MAX_CONTROL_SESSIONS */

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
    uint64_t session_no;
    char peer_ip[PEER_IP_MAX_LEN];
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

/* Compteur monotone de session_no, jamais réutilisé (contrairement à l'indice
 * de slot). Démarre à 1 (0 réservé à "non assigné") ; protégé par
 * g_registry_mutex, déjà détenu par tout appelant de control_registry_register. */
static uint64_t g_next_session_no = 1;

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
        g_sessions[i].peer_ip[0] = '\0';
        g_sessions[i].head = 0;
        g_sessions[i].count = 0;
        g_sessions[i].has_stats = 0;
    }
}

int control_registry_register(int socket_id, const char *peer_ip, const control_hello_t *hello)
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
        // session_no : identifiant monotone, jamais réutilisé, distinct de
        // l'indice de slot `idx` (réattribué au prochain client une fois ce
        // slot libéré). Assigné une seule fois ici, à l'enregistrement.
        s->session_no = g_next_session_no++;
        if (peer_ip != NULL) {
            strncpy(s->peer_ip, peer_ip, PEER_IP_MAX_LEN - 1);
            s->peer_ip[PEER_IP_MAX_LEN - 1] = '\0';
        } else {
            s->peer_ip[0] = '\0';
        }
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
            out[n].session_no = s->session_no;
            out[n].pid = s->hello.pid;
            out[n].nb_forks = s->hello.nb_forks;
            out[n].mode = s->hello.identity.mode;
            strncpy(out[n].label, s->hello.identity.label, CLIENT_LABEL_MAX - 1);
            out[n].label[CLIENT_LABEL_MAX - 1] = '\0';
            if (client_identity_hex_encode(s->hello.identity.machine_uid, MACHINE_UID_BYTES,
                                            out[n].machine_uid_hex, sizeof(out[n].machine_uid_hex)) < 0) {
                out[n].machine_uid_hex[0] = '\0';
            }
            if (client_identity_hex_encode(s->hello.identity.client_uid, CLIENT_UID_BYTES,
                                            out[n].client_uid_hex, sizeof(out[n].client_uid_hex)) < 0) {
                out[n].client_uid_hex[0] = '\0';
            }
            strncpy(out[n].peer_ip, s->peer_ip, PEER_IP_MAX_LEN - 1);
            out[n].peer_ip[PEER_IP_MAX_LEN - 1] = '\0';
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

void control_registry_count_roles(const control_session_info_t *sessions, int n,
                                   int *out_nb_search, int *out_nb_prune)
{
    int nb_search = 0;
    int nb_prune = 0;
    for (int i = 0; i < n; i++) {
        if (sessions[i].mode == CLIENT_MODE_SEARCH) {
            nb_search++;
        } else {
            // CLIENT_MODE_PRUNER ou CLIENT_MODE_GPU_PRUNER : les deux comptent
            // comme « contrôle » pour ce dosage binaire (cf. la doc du header).
            nb_prune++;
        }
    }
    if (out_nb_search != NULL) {
        *out_nb_search = nb_search;
    }
    if (out_nb_prune != NULL) {
        *out_nb_prune = nb_prune;
    }
}

int control_registry_get_identity(int index, client_identity_t *out)
{
    pthread_once(&g_init_once, registry_init_once);
    if (index < 0 || index >= MAX_CONTROL_SESSIONS || out == NULL) {
        return -1;
    }
    control_session_t *s = &g_sessions[index];
    int result = -1;
    pthread_mutex_lock(&s->mutex);
    if (s->in_use) {
        *out = s->hello.identity;
        result = 0;
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
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

/* `target` est un `session_no` décimal si et seulement s'il est intégralement
 * composé de chiffres (au moins un) : strtoull s'arrêterait silencieusement
 * au premier caractère non numérique, ce qui accepterait à tort un label
 * comme "3abc" comme s'il valait le session_no 3 -- vérifier `*end == '\0'`
 * empêche cette confusion. */
static int target_matches_session_no(const char *target, uint64_t session_no)
{
    if (target == NULL || target[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(target, &end, 10);
    if (errno != 0 || end == target || *end != '\0') {
        return 0;
    }
    return (uint64_t)v == session_no;
}

static int target_matches_client_uid(const char *target, const uint8_t *client_uid)
{
    if (target == NULL || strlen(target) != 2 * CLIENT_UID_BYTES) {
        return 0;
    }
    uint8_t decoded[CLIENT_UID_BYTES];
    if (client_identity_hex_decode(target, decoded, CLIENT_UID_BYTES) != 0) {
        return 0;
    }
    return memcmp(decoded, client_uid, CLIENT_UID_BYTES) == 0;
}

static int target_matches_label(const char *target, const char *label)
{
    return target != NULL && label != NULL && strcmp(target, label) == 0;
}

int control_registry_send_command_to(const char *target, uint8_t cmd, const char *command_line)
{
    pthread_once(&g_init_once, registry_init_once);
    if (target == NULL) {
        return -1;
    }

    pthread_mutex_lock(&g_registry_mutex);
    int matched_index = -1;
    int nb_matches = 0;
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        control_session_t *s = &g_sessions[i];
        pthread_mutex_lock(&s->mutex);
        if (s->in_use) {
            int hit = target_matches_session_no(target, s->session_no) ||
                      target_matches_client_uid(target, s->hello.identity.client_uid) ||
                      target_matches_label(target, s->hello.identity.label);
            if (hit) {
                nb_matches++;
                if (nb_matches == 1) {
                    matched_index = i;
                }
            }
        }
        pthread_mutex_unlock(&s->mutex);
    }

    int result = 0;
    if (nb_matches == 1) {
        /* Un seul titulaire trouvé sous cette identité : poste directement
         * sur son slot, toujours sous g_registry_mutex (même schéma que
         * control_registry_broadcast_command) pour qu'aucune déconnexion
         * concurrente ne puisse réattribuer ce slot entre la résolution et
         * l'envoi. */
        if (control_registry_post_command(matched_index, cmd, command_line) == 0) {
            result = 1;
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return result;
}

int control_registry_resolve_client_uid(const char *target, uint8_t out_client_uid[CLIENT_UID_BYTES])
{
    pthread_once(&g_init_once, registry_init_once);
    if (target == NULL) {
        return -1;
    }

    /* Même résolution que control_registry_send_command_to (session_no,
     * client_uid, label, dans cet ordre), mais lecture pure : rien n'est
     * posté, out_client_uid n'est rempli qu'en cas de titulaire unique. */
    pthread_mutex_lock(&g_registry_mutex);
    int matched_index = -1;
    int nb_matches = 0;
    for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
        control_session_t *s = &g_sessions[i];
        pthread_mutex_lock(&s->mutex);
        if (s->in_use) {
            int hit = target_matches_session_no(target, s->session_no) ||
                      target_matches_client_uid(target, s->hello.identity.client_uid) ||
                      target_matches_label(target, s->hello.identity.label);
            if (hit) {
                nb_matches++;
                if (nb_matches == 1) {
                    matched_index = i;
                }
            }
        }
        pthread_mutex_unlock(&s->mutex);
    }

    int result = 0;
    if (nb_matches == 1) {
        control_session_t *s = &g_sessions[matched_index];
        pthread_mutex_lock(&s->mutex);
        if (s->in_use) {
            memcpy(out_client_uid, s->hello.identity.client_uid, CLIENT_UID_BYTES);
            result = 1;
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return result;
}

int control_registry_has_active_client(const uint8_t client_uid[CLIENT_UID_BYTES])
{
    pthread_once(&g_init_once, registry_init_once);
    if (client_uid == NULL) {
        return 0;
    }

    pthread_mutex_lock(&g_registry_mutex);
    int found = 0;
    for (int i = 0; i < MAX_CONTROL_SESSIONS && !found; i++) {
        control_session_t *s = &g_sessions[i];
        pthread_mutex_lock(&s->mutex);
        if (s->in_use && memcmp(s->hello.identity.client_uid, client_uid, CLIENT_UID_BYTES) == 0) {
            found = 1;
        }
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return found;
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
