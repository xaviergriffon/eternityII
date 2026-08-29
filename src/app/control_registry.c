#include "app/control_registry.h"

#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

#include "app/app_static_variables.h"   /* MAX_CONTROL_SESSIONS, MAX_KNOWN_CLIENTS */
#include "ui/logger.h"

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

/**
 * @brief Dosage recherche/contrôle « désiré » PAR MACHINE (PR3,
 *        docs/conception/pilotage_type_client.md), calqué sur
 *        `g_desired_pause_state` mais KEYÉ par `machine_uid` — contrairement à
 *        pause/resume (fleet-wide, un seul booléen suffit), le dosage est par
 *        construction une propriété PAR CLIENT.
 *
 * `machine_uid` (jamais `client_uid`/`session_no`, qui ne survivent pas à un
 * redémarrage de processus client) est la SEULE clé stable pour qu'une
 * machine qui redémarre reprenne le dosage voulu — même choix de clé que
 * `known_clients_registry` (cf. sa doc), mais table SÉPARÉE et volontairement
 * plus petite : ce registre-ci reste dans le domaine « piloter » de
 * control_registry, jamais « mesurer ». Mise à jour dans
 * `control_registry_apply_role_dosage`, consultée à l'enregistrement
 * (`control_registry_register`) pour pré-poster le dosage dans la file toute
 * neuve d'une session qui vient de s'enregistrer. Protégée par
 * `g_registry_mutex`, comme `g_desired_pause_state`.
 */
typedef struct {
    int in_use;
    uint8_t machine_uid[MACHINE_UID_BYTES];
    int pruner_forks;
} desired_role_t;

static desired_role_t g_desired_roles[MAX_KNOWN_CLIENTS];

/* Cherche l'entrée dont machine_uid correspond. Retourne -1 si absente.
 * Appelant : doit détenir g_registry_mutex. */
static int desired_role_find_locked(const uint8_t *machine_uid)
{
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (g_desired_roles[i].in_use &&
            memcmp(g_desired_roles[i].machine_uid, machine_uid, MACHINE_UID_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

/* Enregistre/actualise le dosage désiré de machine_uid. Purement
 * observationnel : une table pleine ET une machine encore inconnue ne doit
 * jamais faire échouer clientsRoles, seulement priver CETTE machine de
 * persistance (avertissement, comme known_clients_registry sur registre
 * plein). Appelant : doit détenir g_registry_mutex. */
static void desired_role_set_locked(const uint8_t *machine_uid, int pruner_forks)
{
    int idx = desired_role_find_locked(machine_uid);
    if (idx < 0) {
        for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
            if (!g_desired_roles[i].in_use) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            log_error("control_registry : table du dosage désiré pleine (>%d machines), "
                      "dosage non mémorisé pour cette machine\n", MAX_KNOWN_CLIENTS);
            return;
        }
        g_desired_roles[idx].in_use = 1;
        memcpy(g_desired_roles[idx].machine_uid, machine_uid, MACHINE_UID_BYTES);
    }
    g_desired_roles[idx].pruner_forks = pruner_forks;
}

/* Copie bornée, toujours NUL-terminée, d'une ligne de commande dans un slot de
 * file (`command_line[CONTROL_COMMAND_LINE_MAX]`). Équivalent de
 * `strncpy(out, source, CONTROL_COMMAND_LINE_MAX - 1); out[...] = '\0';`,
 * réécrit en strlen()+memcpy() explicite : gcc (-Wstringop-truncation, CI
 * Linux) signale l'idiome strncpy+troncature comme potentiellement non
 * terminé dès que `source` est un buffer construit à l'exécution (ex.
 * `snprintf`) plutôt qu'un littéral court — même correctif déjà appliqué dans
 * `known_clients_registry.c`/`app_runtime.c` (`resolve_client_label`). */
static void copy_bounded_command_line(char out[CONTROL_COMMAND_LINE_MAX], const char *source)
{
    size_t len = strlen(source);
    if (len >= CONTROL_COMMAND_LINE_MAX) {
        len = CONTROL_COMMAND_LINE_MAX - 1;
    }
    memcpy(out, source, len);
    out[len] = '\0';
}

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
            copy_bounded_command_line(s->queue[0].command_line, "pause");
            s->count = 1;
        }
        /* Dosage recherche/contrôle désiré (PR3) : même principe que la pause
         * ci-dessus, mais keyé par machine_uid (cf. la doc de
         * g_desired_roles) et donc en PLUS de la pause, jamais à sa place --
         * les deux peuvent coexister dans la même file toute neuve. Deux
         * commandes (config puis configApply), donc vérifie la place pour les
         * DEUX avant d'en écrire une seule : jamais un `config` pré-posté sans
         * son `configApply`, qui laisserait le dosage préparé mais jamais
         * appliqué. */
        int desired_idx = desired_role_find_locked(hello->identity.machine_uid);
        if (desired_idx >= 0 && s->count + 2 <= CONTROL_SESSION_QUEUE_CAP) {
            char line[CONTROL_COMMAND_LINE_MAX];
            snprintf(line, sizeof(line), "config pruner_forks %d", g_desired_roles[desired_idx].pruner_forks);

            int tail = (s->head + s->count) % CONTROL_SESSION_QUEUE_CAP;
            s->queue[tail].cmd = CTRL_COMMAND;
            copy_bounded_command_line(s->queue[tail].command_line, line);
            s->count++;

            tail = (s->head + s->count) % CONTROL_SESSION_QUEUE_CAP;
            s->queue[tail].cmd = CTRL_COMMAND;
            copy_bounded_command_line(s->queue[tail].command_line, "configApply");
            s->count++;
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

void control_registry_count_role_forks(const control_session_info_t *sessions, int n,
                                        int *out_nb_search_forks, int *out_nb_prune_forks)
{
    int nb_search_forks = 0;
    int nb_prune_forks = 0;
    for (int i = 0; i < n; i++) {
        if (sessions[i].mode == CLIENT_MODE_SEARCH) {
            nb_search_forks += sessions[i].nb_forks;
        } else {
            nb_prune_forks += sessions[i].nb_forks;
        }
    }
    if (out_nb_search_forks != NULL) {
        *out_nb_search_forks = nb_search_forks;
    }
    if (out_nb_prune_forks != NULL) {
        *out_nb_prune_forks = nb_prune_forks;
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

int control_registry_desired_pruner_forks(const uint8_t machine_uid[MACHINE_UID_BYTES])
{
    pthread_once(&g_init_once, registry_init_once);
    int result = -1;
    pthread_mutex_lock(&g_registry_mutex);
    int idx = desired_role_find_locked(machine_uid);
    if (idx >= 0) {
        result = g_desired_roles[idx].pruner_forks;
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return result;
}

/* Copie machine_uid du titulaire du slot `index` si toujours actif au moment
 * de l'appel. Appelant : doit détenir g_registry_mutex (mais PAS s->mutex du
 * slot). Retourne 1 si copié, 0 si le slot n'est plus actif (déconnexion
 * concurrente entre la résolution et cet appel -- ne peut arriver que si
 * l'appelant a relâché puis repris g_registry_mutex entre les deux, jamais le
 * cas dans ce fichier, mais vérifié quand même par défense en profondeur,
 * même style que control_registry_resolve_client_uid). */
static int copy_machine_uid_if_active_locked(int index, uint8_t out_machine_uid[MACHINE_UID_BYTES])
{
    control_session_t *s = &g_sessions[index];
    int result = 0;
    pthread_mutex_lock(&s->mutex);
    if (s->in_use) {
        memcpy(out_machine_uid, s->hello.identity.machine_uid, MACHINE_UID_BYTES);
        result = 1;
    }
    pthread_mutex_unlock(&s->mutex);
    return result;
}

int control_registry_apply_role_dosage(const char *target, int pruner_forks)
{
    pthread_once(&g_init_once, registry_init_once);

    char line_config[CONTROL_COMMAND_LINE_MAX];
    snprintf(line_config, sizeof(line_config), "config pruner_forks %d", pruner_forks);

    pthread_mutex_lock(&g_registry_mutex);

    if (target == NULL || *target == '\0') {
        /* Diffusion : même boucle que control_registry_broadcast_command,
         * mais deux commandes par session touchée plutôt qu'une, et le
         * dosage désiré mémorisé pour chacune. */
        int n = 0;
        for (int i = 0; i < MAX_CONTROL_SESSIONS; i++) {
            if (!g_sessions[i].in_use) {
                continue;
            }
            if (control_registry_post_command(i, CTRL_COMMAND, line_config) == 0 &&
                control_registry_post_command(i, CTRL_COMMAND, "configApply") == 0) {
                uint8_t machine_uid[MACHINE_UID_BYTES];
                if (copy_machine_uid_if_active_locked(i, machine_uid)) {
                    desired_role_set_locked(machine_uid, pruner_forks);
                    n++;
                }
            }
        }
        pthread_mutex_unlock(&g_registry_mutex);
        return n;
    }

    /* Cible unique : même résolution (session_no, client_uid, label, dans cet
     * ordre) que control_registry_send_command_to/resolve_client_uid. */
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
        if (control_registry_post_command(matched_index, CTRL_COMMAND, line_config) == 0 &&
            control_registry_post_command(matched_index, CTRL_COMMAND, "configApply") == 0) {
            uint8_t machine_uid[MACHINE_UID_BYTES];
            if (copy_machine_uid_if_active_locked(matched_index, machine_uid)) {
                desired_role_set_locked(machine_uid, pruner_forks);
                result = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return result;
}
