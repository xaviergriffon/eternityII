#include "app/known_clients_registry.h"

#include <string.h>
#include <pthread.h>

#include "ui/logger.h"

/**
 * @brief Une session active d'une machine connue, suivie pour calculer le
 *        cumul par ACCROISSEMENT (delta) plutôt que par simple somme des
 *        valeurs instantanées : `pruner_checked`/`pruner_removed`
 *        (`control_stats_t`) sont des compteurs PAR PROCESSUS, remis à 0 à
 *        chaque redémarrage d'un client — une session qui reconnecte
 *        redémarre donc `last_pruner_checked`/`last_pruner_removed` à 0
 *        (`known_clients_registry_on_connect`), et chaque `CTRL_STATS` reçu
 *        n'ajoute au total de la MACHINE que la croissance observée depuis le
 *        dernier relevé de CETTE session (`known_clients_registry_on_stats`).
 */
typedef struct {
    int in_use;
    uint8_t client_uid[CLIENT_UID_BYTES];
    time_t connect_time;
    uint64_t last_pruner_checked;
    uint64_t last_pruner_removed;
} known_client_session_t;

/**
 * @brief État complet d'une machine connue (non exposé, cf.
 *        known_clients_registry.h pour la vue légère `known_client_info_t`).
 */
typedef struct {
    int in_use;
    uint8_t machine_uid[MACHINE_UID_BYTES];
    char label[CLIENT_LABEL_MAX];
    char peer_ip[PEER_IP_MAX_LEN];
    uint8_t mode;
    int nb_active_sessions;
    int nb_connections_total;
    time_t first_seen;
    time_t last_seen;
    uint64_t total_pruner_checked;
    uint64_t total_pruner_removed;
    uint64_t best_max_result;
    uint64_t cumulative_uptime_seconds;

    known_client_session_t sessions[KNOWN_CLIENT_MAX_SESSIONS];
} known_client_t;

static known_client_t g_known_clients[MAX_KNOWN_CLIENTS];

/* Un seul mutex pour tout le registre : les opérations (connexion, stats,
 * déconnexion) sont des évènements ponctuels de contrôle, pas un chemin
 * chaud de recherche — même choix que g_registry_mutex dans control_registry.c,
 * pas de granularité par entrée. */
static pthread_mutex_t g_known_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void known_clients_init_once(void)
{
    memset(g_known_clients, 0, sizeof(g_known_clients));
}

/* Cherche l'entrée dont machine_uid correspond. Retourne -1 si absente.
 * Appelant : doit détenir g_known_clients_mutex. */
static int find_machine_index(const uint8_t *machine_uid)
{
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (g_known_clients[i].in_use &&
            memcmp(g_known_clients[i].machine_uid, machine_uid, MACHINE_UID_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

/* Cherche un slot libre, ou évince la plus ancienne (last_seen) entrée
 * DÉCONNECTÉE (nb_active_sessions == 0) si le registre est plein. Ne touche
 * JAMAIS une entrée dont une session est active. Retourne -1 si aucun slot
 * n'est disponible (registre plein de machines toutes actuellement
 * connectées) — l'appelant doit alors renoncer à suivre cette machine, sans
 * jamais faire échouer la session réseau qui a déclenché l'appel.
 * Appelant : doit détenir g_known_clients_mutex. */
static int allocate_machine_slot(void)
{
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (!g_known_clients[i].in_use) {
            return i;
        }
    }
    int victim = -1;
    time_t oldest = 0;
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (g_known_clients[i].nb_active_sessions == 0 &&
            (victim == -1 || g_known_clients[i].last_seen < oldest)) {
            victim = i;
            oldest = g_known_clients[i].last_seen;
        }
    }
    if (victim != -1) {
        memset(&g_known_clients[victim], 0, sizeof(g_known_clients[victim]));
        return victim;
    }
    return -1;
}

/* Cherche, parmi les sessions d'une machine, celle dont client_uid
 * correspond. Retourne -1 si absente. Appelant : doit détenir le mutex. */
static int find_session_index(const known_client_t *kc, const uint8_t *client_uid)
{
    for (int i = 0; i < KNOWN_CLIENT_MAX_SESSIONS; i++) {
        if (kc->sessions[i].in_use &&
            memcmp(kc->sessions[i].client_uid, client_uid, CLIENT_UID_BYTES) == 0) {
            return i;
        }
    }
    return -1;
}

void known_clients_registry_on_connect(const client_identity_t *identity, const char *peer_ip)
{
    pthread_once(&g_init_once, known_clients_init_once);
    if (identity == NULL) {
        return;
    }

    pthread_mutex_lock(&g_known_clients_mutex);
    time_t now = time(NULL);

    int idx = find_machine_index(identity->machine_uid);
    if (idx < 0) {
        idx = allocate_machine_slot();
        if (idx < 0) {
            pthread_mutex_unlock(&g_known_clients_mutex);
            log_error("known_clients_registry : registre plein (%d machines connectées), "
                      "cumul non tenu pour cette machine\n", MAX_KNOWN_CLIENTS);
            return;
        }
        known_client_t *kc = &g_known_clients[idx];
        kc->in_use = 1;
        memcpy(kc->machine_uid, identity->machine_uid, MACHINE_UID_BYTES);
        kc->first_seen = now;
    }

    known_client_t *kc = &g_known_clients[idx];
    strncpy(kc->label, identity->label, CLIENT_LABEL_MAX - 1);
    kc->label[CLIENT_LABEL_MAX - 1] = '\0';
    if (peer_ip != NULL) {
        strncpy(kc->peer_ip, peer_ip, PEER_IP_MAX_LEN - 1);
        kc->peer_ip[PEER_IP_MAX_LEN - 1] = '\0';
    } else {
        kc->peer_ip[0] = '\0';
    }
    kc->mode = identity->mode;
    kc->last_seen = now;
    kc->nb_connections_total++;

    // client_uid est censé être unique parmi les sessions actives (un seul
    // hello de contrôle par exécution de processus parent) : un doublon ne
    // devrait jamais se produire, mais s'il se produisait quand même (hello
    // rejoué), on rafraîchit la session existante plutôt que d'en ouvrir une
    // seconde — ce qui fausserait le cumul (deux bases de calcul de delta
    // pour le même flux de compteurs).
    int sidx = find_session_index(kc, identity->client_uid);
    if (sidx < 0) {
        for (int i = 0; i < KNOWN_CLIENT_MAX_SESSIONS; i++) {
            if (!kc->sessions[i].in_use) {
                sidx = i;
                break;
            }
        }
        if (sidx < 0) {
            log_error("known_clients_registry : trop de sessions simultanées pour une machine "
                      "(>%d), cumul non tenu pour cette session\n", KNOWN_CLIENT_MAX_SESSIONS);
            pthread_mutex_unlock(&g_known_clients_mutex);
            return;
        }
        kc->sessions[sidx].in_use = 1;
        memcpy(kc->sessions[sidx].client_uid, identity->client_uid, CLIENT_UID_BYTES);
        kc->sessions[sidx].last_pruner_checked = 0;
        kc->sessions[sidx].last_pruner_removed = 0;
        kc->nb_active_sessions++;
    }
    kc->sessions[sidx].connect_time = now;

    pthread_mutex_unlock(&g_known_clients_mutex);
}

void known_clients_registry_on_stats(const uint8_t *machine_uid, const uint8_t *client_uid,
                                      const control_stats_t *stats)
{
    pthread_once(&g_init_once, known_clients_init_once);
    if (machine_uid == NULL || client_uid == NULL || stats == NULL) {
        return;
    }

    pthread_mutex_lock(&g_known_clients_mutex);
    int idx = find_machine_index(machine_uid);
    if (idx >= 0) {
        known_client_t *kc = &g_known_clients[idx];
        int sidx = find_session_index(kc, client_uid);
        if (sidx >= 0) {
            known_client_session_t *s = &kc->sessions[sidx];
            // Delta borné à >= 0 : un compteur par processus ne peut que
            // croître entre deux relevés d'une même session. Une valeur plus
            // basse que le dernier relevé ne devrait jamais arriver (aucun
            // redémarrage n'a lieu sans passer par on_connect, qui réarme la
            // base à 0) ; on l'ignore sans faire régresser le cumul plutôt
            // que de risquer un sous-débordement uint64_t.
            if (stats->pruner_checked >= s->last_pruner_checked) {
                kc->total_pruner_checked += stats->pruner_checked - s->last_pruner_checked;
                s->last_pruner_checked = stats->pruner_checked;
            }
            if (stats->pruner_removed >= s->last_pruner_removed) {
                kc->total_pruner_removed += stats->pruner_removed - s->last_pruner_removed;
                s->last_pruner_removed = stats->pruner_removed;
            }
        }
        if (stats->max_result > kc->best_max_result) {
            kc->best_max_result = stats->max_result;
        }
        kc->last_seen = time(NULL);
    }
    pthread_mutex_unlock(&g_known_clients_mutex);
}

void known_clients_registry_on_disconnect(const uint8_t *machine_uid, const uint8_t *client_uid)
{
    pthread_once(&g_init_once, known_clients_init_once);
    if (machine_uid == NULL || client_uid == NULL) {
        return;
    }

    pthread_mutex_lock(&g_known_clients_mutex);
    int idx = find_machine_index(machine_uid);
    if (idx >= 0) {
        known_client_t *kc = &g_known_clients[idx];
        time_t now = time(NULL);
        int sidx = find_session_index(kc, client_uid);
        if (sidx >= 0) {
            double elapsed = difftime(now, kc->sessions[sidx].connect_time);
            if (elapsed > 0) {
                kc->cumulative_uptime_seconds += (uint64_t)elapsed;
            }
            kc->sessions[sidx].in_use = 0;
            kc->nb_active_sessions--;
        }
        kc->last_seen = now;
    }
    pthread_mutex_unlock(&g_known_clients_mutex);
}

int known_clients_registry_snapshot(known_client_info_t *out, int max)
{
    pthread_once(&g_init_once, known_clients_init_once);
    if (out == NULL || max <= 0) {
        return 0;
    }

    int n = 0;
    pthread_mutex_lock(&g_known_clients_mutex);
    for (int i = 0; i < MAX_KNOWN_CLIENTS && n < max; i++) {
        known_client_t *kc = &g_known_clients[i];
        if (!kc->in_use) {
            continue;
        }
        if (client_identity_hex_encode(kc->machine_uid, MACHINE_UID_BYTES,
                                        out[n].machine_uid_hex, sizeof(out[n].machine_uid_hex)) < 0) {
            out[n].machine_uid_hex[0] = '\0';
        }
        strncpy(out[n].label, kc->label, CLIENT_LABEL_MAX - 1);
        out[n].label[CLIENT_LABEL_MAX - 1] = '\0';
        strncpy(out[n].peer_ip, kc->peer_ip, PEER_IP_MAX_LEN - 1);
        out[n].peer_ip[PEER_IP_MAX_LEN - 1] = '\0';
        out[n].mode = kc->mode;
        out[n].connected = (kc->nb_active_sessions > 0) ? 1 : 0;
        out[n].nb_active_sessions = kc->nb_active_sessions;
        out[n].nb_connections_total = kc->nb_connections_total;
        out[n].first_seen = kc->first_seen;
        out[n].last_seen = kc->last_seen;
        out[n].total_pruner_checked = kc->total_pruner_checked;
        out[n].total_pruner_removed = kc->total_pruner_removed;
        out[n].best_max_result = kc->best_max_result;
        out[n].cumulative_uptime_seconds = kc->cumulative_uptime_seconds;
        n++;
    }
    pthread_mutex_unlock(&g_known_clients_mutex);
    return n;
}

int known_clients_registry_count(void)
{
    pthread_once(&g_init_once, known_clients_init_once);
    int n = 0;
    pthread_mutex_lock(&g_known_clients_mutex);
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (g_known_clients[i].in_use) {
            n++;
        }
    }
    pthread_mutex_unlock(&g_known_clients_mutex);
    return n;
}
