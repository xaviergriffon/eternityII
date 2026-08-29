#include "app/client_config.h"
#include "app/app_static_variables.h"
#include "ui/command_lines.h"
#include "ui/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

const char *g_client_server_host = NULL;

void client_config_init(client_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
}

void client_config_free(client_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    free(cfg->server_host);
    free(cfg->parts_file);
    cfg->server_host = NULL;
    cfg->parts_file = NULL;
    cfg->has_server_host = 0;
    cfg->has_parts_file = 0;
}

/**
 * @brief Recule @p end tant qu'il pointe juste après un espace/tab, sans
 *        jamais reculer avant @p start. Utilisé pour trimmer clé et valeur.
 */
static const char *rtrim(const char *start, const char *end)
{
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    return end;
}

client_config_line_status_t client_config_parse_line(const char *line, client_config_t *cfg)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
        return CLIENT_CONFIG_LINE_IGNORED;
    }

    const char *eq = strchr(p, '=');
    if (eq == NULL) {
        return CLIENT_CONFIG_LINE_UNKNOWN_KEY;
    }

    const char *key_end = rtrim(p, eq);
    size_t key_len = (size_t)(key_end - p);
    char key[64];
    if (key_len == 0 || key_len >= sizeof(key)) {
        return CLIENT_CONFIG_LINE_UNKNOWN_KEY;
    }
    memcpy(key, p, key_len);
    key[key_len] = '\0';

    const char *v = eq + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    const char *v_end = v;
    while (*v_end != '\0' && *v_end != '\n' && *v_end != '\r' && *v_end != '#') {
        v_end++;
    }
    v_end = rtrim(v, v_end);
    size_t val_len = (size_t)(v_end - v);
    char value[512];
    if (val_len >= sizeof(value)) {
        return CLIENT_CONFIG_LINE_INVALID_VALUE;
    }
    memcpy(value, v, val_len);
    value[val_len] = '\0';

    if (strcmp(key, "nb_forks") == 0) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n <= 0 || n > INT_MAX) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_nb_forks = 1;
        cfg->nb_forks = (int)n;
    } else if (strcmp(key, "server_host") == 0) {
        if (val_len == 0) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        free(cfg->server_host);
        cfg->server_host = strdup(value);
        cfg->has_server_host = 1;
    } else if (strcmp(key, "parts_file") == 0) {
        if (val_len == 0) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        free(cfg->parts_file);
        cfg->parts_file = strdup(value);
        cfg->has_parts_file = 1;
    } else if (strcmp(key, "max_stock_by_thread") == 0) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n < 0 || n > INT_MAX) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_max_stock_by_thread = 1;
        cfg->max_stock_by_thread = (int)n;
    } else if (strcmp(key, "limit") == 0) {
        /* strtoull accepte un '-' de tête et enroule silencieusement (norme
           C) au lieu d'échouer : un signe négatif est donc rejeté avant même
           d'appeler strtoull, sans quoi "limit = -1" produirait une valeur
           positive énorme au lieu d'une erreur. */
        if (value[0] == '-') {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        char *end = NULL;
        unsigned long long n = strtoull(value, &end, 10);
        if (end == value || *end != '\0') {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_limit = 1;
        cfg->limit = n;
    } else if (strcmp(key, "pruner_batch") == 0) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n < INT_MIN || n > INT_MAX) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_pruner_batch = 1;
        cfg->pruner_batch = pruner_batch_clamp((int)n);
    } else if (strcmp(key, "pruner_forks") == 0) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n < 0 || n > INT_MAX) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_pruner_forks = 1;
        cfg->pruner_forks = (int)n;
    } else if (strcmp(key, "dfs_budget") == 0) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n < INT_MIN || n > INT_MAX) {
            return CLIENT_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_dfs_budget = 1;
        cfg->dfs_budget = pruner_dfs_budget_clamp((int)n);
    } else {
        return CLIENT_CONFIG_LINE_UNKNOWN_KEY;
    }
    return CLIENT_CONFIG_LINE_SET;
}

client_config_load_status_t client_config_load(const char *path, client_config_t *cfg)
{
    if (path == NULL) {
        return CLIENT_CONFIG_ABSENT;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return CLIENT_CONFIG_ABSENT;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int line_no = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        line_no++;
        client_config_line_status_t st = client_config_parse_line(line, cfg);
        if (st == CLIENT_CONFIG_LINE_UNKNOWN_KEY || st == CLIENT_CONFIG_LINE_INVALID_VALUE) {
            /* Retire un éventuel \r\n final avant de journaliser, pour un
               message d'avertissement sur une seule ligne. */
            size_t l = (size_t)len;
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
                line[--l] = '\0';
            }
            log_error("configuration (%s:%d) : %s, ligne ignorée : \"%s\"\n",
                      path, line_no,
                      st == CLIENT_CONFIG_LINE_UNKNOWN_KEY ? "clé inconnue ou ligne malformée"
                                                            : "valeur invalide",
                      line);
        }
    }
    free(line);
    fclose(f);
    return CLIENT_CONFIG_LOADED;
}

int client_config_format(const client_config_t *cfg, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    size_t off = 0;
    int truncated = 0;

#define APPEND(...) \
    do { \
        if (off < out_size) { \
            int n = snprintf(out + off, out_size - off, __VA_ARGS__); \
            if (n < 0) { \
                truncated = 1; \
            } else { \
                off += (size_t)n; \
            } \
        } else { \
            truncated = 1; \
        } \
    } while (0)

    if (cfg->has_nb_forks) {
        APPEND("nb_forks            = %d\n", cfg->nb_forks);
    }
    if (cfg->has_pruner_forks) {
        APPEND("pruner_forks        = %d\n", cfg->pruner_forks);
    }
    if (cfg->has_server_host && cfg->server_host != NULL) {
        APPEND("server_host         = %s\n", cfg->server_host);
    }
    if (cfg->has_parts_file && cfg->parts_file != NULL) {
        APPEND("parts_file          = %s\n", cfg->parts_file);
    }
    if (cfg->has_max_stock_by_thread) {
        APPEND("max_stock_by_thread = %d\n", cfg->max_stock_by_thread);
    }
    if (cfg->has_limit) {
        APPEND("limit               = %llu\n", cfg->limit);
    }
    if (cfg->has_pruner_batch) {
        APPEND("pruner_batch        = %d\n", cfg->pruner_batch);
    }
    if (cfg->has_dfs_budget) {
        APPEND("dfs_budget          = %d\n", cfg->dfs_budget);
    }
#undef APPEND

    if (off >= out_size) {
        out[out_size - 1] = '\0';
        return -1;
    }
    out[off] = '\0';
    return truncated ? -1 : (int)off;
}

int client_config_save(const char *path, const client_config_t *cfg)
{
    char buf[1024];
    if (client_config_format(cfg, buf, sizeof(buf)) < 0) {
        log_error("configSave (%s) : configuration trop volumineuse pour le tampon interne\n", path);
        return -1;
    }

    size_t len = strlen(path);
    char *tmp_path = malloc(len + 5); /* ".tmp" + '\0' */
    if (tmp_path == NULL) {
        log_error("configSave (%s) : malloc échoué\n", path);
        return -1;
    }
    memcpy(tmp_path, path, len);
    memcpy(tmp_path + len, ".tmp", 5);

    FILE *f = fopen(tmp_path, "w");
    if (f == NULL) {
        log_errno("configSave (%s) ", tmp_path);
        free(tmp_path);
        return -1;
    }

    size_t out_len = strlen(buf);
    size_t wrote = fwrite(buf, 1, out_len, f);
    int close_result = fclose(f);
    if (wrote != out_len || close_result != 0) {
        log_error("configSave (%s) : écriture incomplète\n", tmp_path);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    if (rename(tmp_path, path) != 0) {
        log_errno("configSave (%s -> %s) ", tmp_path, path);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    free(tmp_path);
    return 0;
}

void client_config_apply_to_globals(const client_config_t *cfg, int argc, const char **server_host)
{
    if (cfg->has_nb_forks && argc < 4) {
        NB_THREADS = cfg->nb_forks;
    }
    if (cfg->has_pruner_forks && pruner_forks_requested < 0) {
        /* Aucun équivalent positionnel : `--pruner-forks` est un DRAPEAU, pas
           un argument positionnel — la priorité CLI > fichier se lit donc
           directement sur `pruner_forks_requested` (déjà écrit par
           `parse_cli_options` si l'option a été fournie) plutôt que sur un
           seuil `argc`, à la différence de `nb_forks` ci-dessus. */
        pruner_forks_requested = cfg->pruner_forks;
    }
    if (cfg->has_server_host && cfg->server_host != NULL && server_host != NULL && argc < 3) {
        *server_host = strdup(cfg->server_host);
    }

    int cli_gave_parts_file = pruner_mode ? (argc >= 5) : (argc >= 6);
    if (cfg->has_parts_file && cfg->parts_file != NULL && !cli_gave_parts_file) {
        parts_files = strdup(cfg->parts_file);
    }

    if (!pruner_mode && cfg->has_max_stock_by_thread && argc < 5) {
        max_stock_by_thread = cfg->max_stock_by_thread;
    }

    if (cfg->has_limit) {
        /* Aucun équivalent positionnel au démarrage : rien ne peut être
           "déjà fourni par la CLI" pour cette clé. */
        max_search_by_sec = cfg->limit;
    }

    if (pruner_mode && cfg->has_pruner_batch && argc < 6) {
        pruner_batch_size = cfg->pruner_batch;
    }

    if (cfg->has_dfs_budget) {
        /* Aucun équivalent positionnel au démarrage, comme `limit`. */
        pruner_dfs_budget = cfg->dfs_budget;
    }
}

void client_config_apply_direct(const client_config_t *cfg, const char **server_host)
{
    if (cfg->has_nb_forks) {
        NB_THREADS = cfg->nb_forks;
    }
    if (cfg->has_pruner_forks) {
        pruner_forks_requested = cfg->pruner_forks;
    }
    if (cfg->has_server_host && cfg->server_host != NULL && server_host != NULL) {
        *server_host = strdup(cfg->server_host);
    }
    if (cfg->has_parts_file && cfg->parts_file != NULL) {
        parts_files = strdup(cfg->parts_file);
    }
    if (cfg->has_max_stock_by_thread) {
        max_stock_by_thread = cfg->max_stock_by_thread;
    }
    if (cfg->has_limit) {
        max_search_by_sec = cfg->limit;
    }
    if (cfg->has_pruner_batch) {
        pruner_batch_size = cfg->pruner_batch;
    }
    if (cfg->has_dfs_budget) {
        pruner_dfs_budget = cfg->dfs_budget;
    }
}

client_config_diff_t client_config_diff(const client_config_t *current, const client_config_t *staged)
{
    if (staged->has_nb_forks &&
        (!current->has_nb_forks || staged->nb_forks != current->nb_forks)) {
        return CLIENT_CONFIG_DIFF_NEEDS_RESTART;
    }
    if (staged->has_pruner_forks &&
        (!current->has_pruner_forks || staged->pruner_forks != current->pruner_forks)) {
        return CLIENT_CONFIG_DIFF_NEEDS_RESTART;
    }
    if (staged->has_server_host &&
        (!current->has_server_host || current->server_host == NULL ||
         staged->server_host == NULL || strcmp(staged->server_host, current->server_host) != 0)) {
        return CLIENT_CONFIG_DIFF_NEEDS_RESTART;
    }
    if (staged->has_parts_file &&
        (!current->has_parts_file || current->parts_file == NULL ||
         staged->parts_file == NULL || strcmp(staged->parts_file, current->parts_file) != 0)) {
        return CLIENT_CONFIG_DIFF_NEEDS_RESTART;
    }
    return CLIENT_CONFIG_DIFF_HOT_ONLY;
}

void client_config_capture_effective(client_config_t *out, const char *server_host)
{
    client_config_init(out);

    out->has_nb_forks = 1;
    out->nb_forks = NB_THREADS;

    if (pruner_forks_requested >= 0) {
        /* Omis (comme `server_host`/`parts_file` ci-dessous) tant qu'aucun
           dosage n'a été explicitement demandé : le sentinel -1 ne doit
           jamais être écrit dans un fichier de configuration ni réaffiché par
           `config` — le rôle par fork reste alors implicitement dérivé de
           `pruner_mode` (cf. `resolve_pruner_forks`), jamais une valeur
           inventée ici. */
        out->has_pruner_forks = 1;
        out->pruner_forks = pruner_forks_requested;
    }

    if (server_host != NULL && server_host[0] != '\0') {
        out->has_server_host = 1;
        out->server_host = strdup(server_host);
    }

    if (parts_files != NULL) {
        out->has_parts_file = 1;
        out->parts_file = strdup(parts_files);
    }

    out->has_max_stock_by_thread = 1;
    out->max_stock_by_thread = max_stock_by_thread;

    out->has_limit = 1;
    out->limit = max_search_by_sec;

    out->has_pruner_batch = 1;
    out->pruner_batch = pruner_batch_size;

    out->has_dfs_budget = 1;
    out->dfs_budget = pruner_dfs_budget;
}
