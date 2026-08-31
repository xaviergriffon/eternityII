#include "app/known_clients_registry.h"

#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
    /// Rôle déclaré par CETTE session (`CLIENT_MODE_*`), figé à la connexion —
    /// nécessaire pour décrémenter le bon compteur `nb_active_search`/
    /// `nb_active_prune` de la machine à la déconnexion, puisque
    /// `known_client_t.mode` (dernière valeur déclarée, toutes sessions
    /// confondues) peut ne plus correspondre à CETTE session précise dans un
    /// dosage mixte.
    uint8_t mode;
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
    int nb_active_search;
    int nb_active_prune;
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

// Compteur de mutations persistées: voir la doc de
// known_clients_registry_mutation_count (known_clients_registry.h).
static unsigned long long g_known_clients_mutation_count = 0;

static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void known_clients_init_once(void)
{
    memset(g_known_clients, 0, sizeof(g_known_clients));
}

/* Copie bornée, toujours NUL-terminée. Équivalent de
 * `strncpy(out, source, out_size - 1); out[out_size - 1] = '\0';`, réécrit en
 * strlen()+memcpy() explicite : gcc (-Wstringop-truncation, CI Linux) peut
 * repérer, au sein de ce fichier, que `label`/`peer_ip` sont relus plus loin
 * comme chaînes NUL-terminées et signaler l'idiome strncpy+troncature comme
 * potentiellement non terminé — même correctif que `resolve_client_label`
 * (src/app/app_runtime.c). */
static void copy_bounded_string(char *out, size_t out_size, const char *source)
{
    if (source == NULL) {
        source = "";
    }
    size_t len = strlen(source);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, source, len);
    out[len] = '\0';
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
    copy_bounded_string(kc->label, sizeof(kc->label), identity->label);
    copy_bounded_string(kc->peer_ip, sizeof(kc->peer_ip), peer_ip);
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
        kc->sessions[sidx].mode = identity->mode;
        kc->sessions[sidx].last_pruner_checked = 0;
        kc->sessions[sidx].last_pruner_removed = 0;
        kc->nb_active_sessions++;
        // Ventilation par rôle : la même distinction binaire que
        // control_registry_count_roles (control_registry.h) — GPU_PRUNER
        // compte comme « contrôle ».
        if (identity->mode == CLIENT_MODE_SEARCH) {
            kc->nb_active_search++;
        } else {
            kc->nb_active_prune++;
        }
    }
    kc->sessions[sidx].connect_time = now;

    g_known_clients_mutation_count++;
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
        g_known_clients_mutation_count++;
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
            // Symétrique de l'incrément côté on_connect : décrémente le
            // compteur du rôle que CETTE session avait déclaré (figé dans
            // sessions[sidx].mode à la connexion), jamais kc->mode (« dernière
            // valeur déclarée », qui peut appartenir à une AUTRE session
            // encore active dans un dosage mixte).
            if (kc->sessions[sidx].mode == CLIENT_MODE_SEARCH) {
                kc->nb_active_search--;
            } else {
                kc->nb_active_prune--;
            }
        }
        kc->last_seen = now;
        g_known_clients_mutation_count++;
    }
    pthread_mutex_unlock(&g_known_clients_mutex);
}

/* Remplit un enregistrement persisté à partir d'une entrée en mémoire.
 * N'écrit QUE les champs cumulés — jamais l'état de session vivante, cf.
 * known_client_record_t. Appelant : doit détenir le mutex (lecture seule). */
static void fill_record_from_entry(known_client_record_t *rec, const known_client_t *kc)
{
    memset(rec, 0, sizeof(*rec));
    memcpy(rec->machine_uid, kc->machine_uid, MACHINE_UID_BYTES);
    copy_bounded_string(rec->label, sizeof(rec->label), kc->label);
    copy_bounded_string(rec->peer_ip, sizeof(rec->peer_ip), kc->peer_ip);
    rec->mode = kc->mode;
    rec->nb_connections_total = (uint32_t)kc->nb_connections_total;
    rec->first_seen = (int64_t)kc->first_seen;
    rec->last_seen = (int64_t)kc->last_seen;
    rec->total_pruner_checked = kc->total_pruner_checked;
    rec->total_pruner_removed = kc->total_pruner_removed;
    rec->best_max_result = kc->best_max_result;
    rec->cumulative_uptime_seconds = kc->cumulative_uptime_seconds;
}

int known_clients_registry_save(const char *filename)
{
    pthread_once(&g_init_once, known_clients_init_once);
    if (filename == NULL) {
        return -1;
    }

    known_client_record_t *records = malloc(sizeof(known_client_record_t) * MAX_KNOWN_CLIENTS);
    if (records == NULL) {
        return -1;
    }
    uint32_t count = 0;

    pthread_mutex_lock(&g_known_clients_mutex);
    for (int i = 0; i < MAX_KNOWN_CLIENTS; i++) {
        if (g_known_clients[i].in_use) {
            fill_record_from_entry(&records[count], &g_known_clients[i]);
            count++;
        }
    }
    pthread_mutex_unlock(&g_known_clients_mutex);

    // Écriture atomique : fichier temporaire puis rename, même convention que
    // best_board_save/backup() (src/core/best_board.c, src/core/datamanager.c).
    size_t len = strlen(filename);
    char *tmp_filename = malloc(len + 5); // ".tmp" + '\0'
    if (tmp_filename == NULL) {
        free(records);
        return -1;
    }
    memcpy(tmp_filename, filename, len);
    memcpy(tmp_filename + len, ".tmp", 5);

    int failed = 0;
    FILE *f = fopen(tmp_filename, "wb");
    if (f == NULL) {
        failed = 1;
    } else {
        known_clients_file_header_t header = { KNOWN_CLIENTS_FILE_MAGIC, count };
        if (fwrite(&header, sizeof(header), 1, f) != 1) {
            failed = 1;
        } else if (count > 0 && fwrite(records, sizeof(known_client_record_t), count, f) != count) {
            failed = 1;
        }
        if (fclose(f) != 0) {
            failed = 1;
        }
    }
    if (!failed && rename(tmp_filename, filename) != 0) {
        failed = 1;
    }
    if (failed) {
        unlink(tmp_filename);
    }
    free(tmp_filename);
    free(records);
    return failed ? -1 : 0;
}

/* Applique un enregistrement persisté au registre en mémoire : fusion
 * additive si la machine est déjà connue, nouvelle entrée déconnectée sinon.
 * Voir la doc de known_clients_registry_load pour la justification. Appelant
 * : doit détenir g_known_clients_mutex. */
static void apply_persisted_record(const known_client_record_t *rec)
{
    int idx = find_machine_index(rec->machine_uid);
    if (idx < 0) {
        idx = allocate_machine_slot();
        if (idx < 0) {
            log_error("known_clients_registry : registre plein, cumul persisté ignoré pour une "
                      "machine\n");
            return;
        }
        known_client_t *kc = &g_known_clients[idx];
        kc->in_use = 1;
        memcpy(kc->machine_uid, rec->machine_uid, MACHINE_UID_BYTES);
        copy_bounded_string(kc->label, sizeof(kc->label), rec->label);
        copy_bounded_string(kc->peer_ip, sizeof(kc->peer_ip), rec->peer_ip);
        kc->mode = rec->mode;
        kc->nb_connections_total = (int)rec->nb_connections_total;
        kc->first_seen = (time_t)rec->first_seen;
        kc->last_seen = (time_t)rec->last_seen;
        kc->total_pruner_checked = rec->total_pruner_checked;
        kc->total_pruner_removed = rec->total_pruner_removed;
        kc->best_max_result = rec->best_max_result;
        kc->cumulative_uptime_seconds = rec->cumulative_uptime_seconds;
        return;
    }

    known_client_t *kc = &g_known_clients[idx];
    kc->nb_connections_total += (int)rec->nb_connections_total;
    if (kc->first_seen == 0 || (time_t)rec->first_seen < kc->first_seen) {
        kc->first_seen = (time_t)rec->first_seen;
    }
    if ((time_t)rec->last_seen > kc->last_seen) {
        kc->last_seen = (time_t)rec->last_seen;
    }
    kc->total_pruner_checked += rec->total_pruner_checked;
    kc->total_pruner_removed += rec->total_pruner_removed;
    if (rec->best_max_result > kc->best_max_result) {
        kc->best_max_result = rec->best_max_result;
    }
    kc->cumulative_uptime_seconds += rec->cumulative_uptime_seconds;
    // label/peer_ip/mode/statut connecté : la valeur déjà en mémoire est plus
    // récente qu'un fichier persisté potentiellement ancien, on ne l'écrase pas.
}

int known_clients_registry_load(const char *filename)
{
    pthread_once(&g_init_once, known_clients_init_once);
    if (filename == NULL) {
        return -1;
    }

    FILE *f = fopen(filename, "rb");
    if (f == NULL) {
        return -1;
    }

    known_clients_file_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1 || header.magic != KNOWN_CLIENTS_FILE_MAGIC) {
        fclose(f);
        return -1;
    }

    pthread_mutex_lock(&g_known_clients_mutex);
    known_client_record_t rec;
    for (uint32_t i = 0; i < header.count; i++) {
        // Fichier tronqué au milieu des enregistrements : on applique ce qui a
        // pu être lu et on s'arrête là, plutôt que d'échouer tout le chargement
        // — tolérance en lecture, cf. la doc de cette fonction.
        if (fread(&rec, sizeof(rec), 1, f) != 1) {
            break;
        }
        apply_persisted_record(&rec);
    }
    pthread_mutex_unlock(&g_known_clients_mutex);

    fclose(f);
    return 0;
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
        copy_bounded_string(out[n].label, sizeof(out[n].label), kc->label);
        copy_bounded_string(out[n].peer_ip, sizeof(out[n].peer_ip), kc->peer_ip);
        out[n].mode = kc->mode;
        out[n].connected = (kc->nb_active_sessions > 0) ? 1 : 0;
        out[n].nb_active_sessions = kc->nb_active_sessions;
        out[n].nb_active_search = kc->nb_active_search;
        out[n].nb_active_prune = kc->nb_active_prune;
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

unsigned long long known_clients_registry_mutation_count(void)
{
    pthread_once(&g_init_once, known_clients_init_once);
    pthread_mutex_lock(&g_known_clients_mutex);
    unsigned long long n = g_known_clients_mutation_count;
    pthread_mutex_unlock(&g_known_clients_mutex);
    return n;
}
