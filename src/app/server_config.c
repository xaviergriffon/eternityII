#include "app/server_config.h"
#include "app/app_static_variables.h"
#include "ui/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

void server_config_init(server_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
}

void server_config_free(server_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    free(cfg->parts_file);
    free(cfg->http_token_file);
    free(cfg->stock_spill_dir);
    cfg->parts_file = NULL;
    cfg->http_token_file = NULL;
    cfg->stock_spill_dir = NULL;
    cfg->has_parts_file = 0;
    cfg->has_http_token_file = 0;
    cfg->has_stock_spill_dir = 0;
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

/** @brief Parse une valeur entière stricte (`strtol`), rejette tout suffixe non numérique. */
static int parse_int(const char *value, long min, long max, int *out)
{
    char *end = NULL;
    long n = strtol(value, &end, 10);
    if (end == value || *end != '\0' || n < min || n > max) {
        return -1;
    }
    *out = (int)n;
    return 0;
}

server_config_line_status_t server_config_parse_line(const char *line, server_config_t *cfg)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') {
        return SERVER_CONFIG_LINE_IGNORED;
    }

    const char *eq = strchr(p, '=');
    if (eq == NULL) {
        return SERVER_CONFIG_LINE_UNKNOWN_KEY;
    }

    const char *key_end = rtrim(p, eq);
    size_t key_len = (size_t)(key_end - p);
    char key[64];
    if (key_len == 0 || key_len >= sizeof(key)) {
        return SERVER_CONFIG_LINE_UNKNOWN_KEY;
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
        return SERVER_CONFIG_LINE_INVALID_VALUE;
    }
    memcpy(value, v, val_len);
    value[val_len] = '\0';

    int n;
    if (strcmp(key, "nb_threads") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_nb_threads = 1;
        cfg->nb_threads = n;
    } else if (strcmp(key, "parts_file") == 0) {
        if (val_len == 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        free(cfg->parts_file);
        cfg->parts_file = strdup(value);
        cfg->has_parts_file = 1;
    } else if (strcmp(key, "expand_level") == 0) {
        if (parse_int(value, 0, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_expand_level = 1;
        cfg->expand_level = n;
    } else if (strcmp(key, "expand_max_stock") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_expand_max_stock = 1;
        cfg->expand_max_stock = n;
    } else if (strcmp(key, "expand_max_levels") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_expand_max_levels = 1;
        cfg->expand_max_levels = n;
    } else if (strcmp(key, "http_port") == 0) {
        if (parse_int(value, 1, 65535, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_http_port = 1;
        cfg->http_port = n;
    } else if (strcmp(key, "http_token_file") == 0) {
        if (val_len == 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        free(cfg->http_token_file);
        cfg->http_token_file = strdup(value);
        cfg->has_http_token_file = 1;
    } else if (strcmp(key, "stock_files") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_stock_files = 1;
        cfg->stock_files = n;
    } else if (strcmp(key, "stock_max_ram") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_stock_max_ram = 1;
        cfg->stock_max_ram = n;
    } else if (strcmp(key, "stock_spill_dir") == 0) {
        if (val_len == 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        free(cfg->stock_spill_dir);
        cfg->stock_spill_dir = strdup(value);
        cfg->has_stock_spill_dir = 1;
    } else if (strcmp(key, "rebalance_budget") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_rebalance_budget = 1;
        cfg->rebalance_budget = n;
    } else if (strcmp(key, "tcp_timeout") == 0) {
        if (parse_int(value, 1, INT_MAX, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_tcp_timeout = 1;
        cfg->tcp_timeout = n;
    } else if (strcmp(key, "auto_roles") == 0) {
        if (parse_int(value, 0, 1, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_auto_roles = 1;
        cfg->auto_roles = n;
    } else if (strcmp(key, "stop_on_solution") == 0) {
        if (parse_int(value, 0, 1, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_stop_on_solution = 1;
        cfg->stop_on_solution = n;
    } else if (strcmp(key, "headless") == 0) {
        if (parse_int(value, 0, 1, &n) != 0) {
            return SERVER_CONFIG_LINE_INVALID_VALUE;
        }
        cfg->has_headless = 1;
        cfg->headless = n;
    } else {
        return SERVER_CONFIG_LINE_UNKNOWN_KEY;
    }
    return SERVER_CONFIG_LINE_SET;
}

server_config_load_status_t server_config_load(const char *path, server_config_t *cfg)
{
    if (path == NULL) {
        return SERVER_CONFIG_ABSENT;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return SERVER_CONFIG_ABSENT;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int line_no = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        line_no++;
        server_config_line_status_t st = server_config_parse_line(line, cfg);
        if (st == SERVER_CONFIG_LINE_UNKNOWN_KEY || st == SERVER_CONFIG_LINE_INVALID_VALUE) {
            size_t l = (size_t)len;
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
                line[--l] = '\0';
            }
            log_error("configuration serveur (%s:%d) : %s, ligne ignorée : \"%s\"\n",
                      path, line_no,
                      st == SERVER_CONFIG_LINE_UNKNOWN_KEY ? "clé inconnue ou ligne malformée"
                                                            : "valeur invalide",
                      line);
        }
    }
    free(line);
    fclose(f);
    return SERVER_CONFIG_LOADED;
}

int server_config_format(const server_config_t *cfg, char *out, size_t out_size)
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

    if (cfg->has_nb_threads) {
        APPEND("nb_threads         = %d\n", cfg->nb_threads);
    }
    if (cfg->has_parts_file && cfg->parts_file != NULL) {
        APPEND("parts_file         = %s\n", cfg->parts_file);
    }
    if (cfg->has_expand_level) {
        APPEND("expand_level       = %d\n", cfg->expand_level);
    }
    if (cfg->has_expand_max_stock) {
        APPEND("expand_max_stock   = %d\n", cfg->expand_max_stock);
    }
    if (cfg->has_expand_max_levels) {
        APPEND("expand_max_levels  = %d\n", cfg->expand_max_levels);
    }
    if (cfg->has_http_port) {
        APPEND("http_port          = %d\n", cfg->http_port);
    }
    if (cfg->has_http_token_file && cfg->http_token_file != NULL) {
        APPEND("http_token_file    = %s\n", cfg->http_token_file);
    }
    if (cfg->has_stock_files) {
        APPEND("stock_files        = %d\n", cfg->stock_files);
    }
    if (cfg->has_stock_max_ram) {
        APPEND("stock_max_ram      = %d\n", cfg->stock_max_ram);
    }
    if (cfg->has_stock_spill_dir && cfg->stock_spill_dir != NULL) {
        APPEND("stock_spill_dir    = %s\n", cfg->stock_spill_dir);
    }
    if (cfg->has_rebalance_budget) {
        APPEND("rebalance_budget   = %d\n", cfg->rebalance_budget);
    }
    if (cfg->has_tcp_timeout) {
        APPEND("tcp_timeout        = %d\n", cfg->tcp_timeout);
    }
    if (cfg->has_auto_roles) {
        APPEND("auto_roles         = %d\n", cfg->auto_roles);
    }
    if (cfg->has_stop_on_solution) {
        APPEND("stop_on_solution   = %d\n", cfg->stop_on_solution);
    }
    if (cfg->has_headless) {
        APPEND("headless           = %d\n", cfg->headless);
    }
#undef APPEND

    if (off >= out_size) {
        out[out_size - 1] = '\0';
        return -1;
    }
    out[off] = '\0';
    return truncated ? -1 : (int)off;
}

int server_config_save(const char *path, const server_config_t *cfg)
{
    char buf[1024];
    if (server_config_format(cfg, buf, sizeof(buf)) < 0) {
        log_error("configuration serveur (%s) : configuration trop volumineuse pour le tampon interne\n", path);
        return -1;
    }

    size_t len = strlen(path);
    char *tmp_path = malloc(len + 5); /* ".tmp" + '\0' */
    if (tmp_path == NULL) {
        log_error("configuration serveur (%s) : malloc échoué\n", path);
        return -1;
    }
    memcpy(tmp_path, path, len);
    memcpy(tmp_path + len, ".tmp", 5);

    FILE *f = fopen(tmp_path, "w");
    if (f == NULL) {
        log_errno("configuration serveur (%s) ", tmp_path);
        free(tmp_path);
        return -1;
    }

    size_t out_len = strlen(buf);
    size_t wrote = fwrite(buf, 1, out_len, f);
    int close_result = fclose(f);
    if (wrote != out_len || close_result != 0) {
        log_error("configuration serveur (%s) : écriture incomplète\n", tmp_path);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    if (rename(tmp_path, path) != 0) {
        log_errno("configuration serveur (%s -> %s) ", tmp_path, path);
        unlink(tmp_path);
        free(tmp_path);
        return -1;
    }

    free(tmp_path);
    return 0;
}

void server_config_apply_pre_dispatch(const server_config_t *cfg)
{
    // Ces quatre clés sont consommées par main() de façon INCONDITIONNELLE
    // (avant même de savoir quel mode s'exécute) : HTTP_TOKEN_FILE est chargé
    // juste après ce point, stock_files_requested/stock_max_ram_mb sont
    // appliqués via datamanager_configure_* un peu plus loin. Doivent donc
    // être résolues ICI. Aucune de ces globales n'a d'équivalent positionnel,
    // donc "encore à sa valeur par défaut" ⇔ "non fournie par la CLI" — même
    // convention que `parse_cli_options` pour ces options.
    if (cfg->has_http_port && HTTP_PORT == 0) {
        HTTP_PORT = cfg->http_port;
    }
    if (cfg->has_http_token_file && cfg->http_token_file != NULL && HTTP_TOKEN_FILE == NULL) {
        HTTP_TOKEN_FILE = strdup(cfg->http_token_file);
    }
    if (cfg->has_stock_files && stock_files_requested == 0) {
        stock_files_requested = cfg->stock_files;
    }
    if (cfg->has_stock_max_ram && stock_max_ram_mb == 0) {
        stock_max_ram_mb = cfg->stock_max_ram;
    }

    // Les clés suivantes n'ont, elles non plus, aucun équivalent positionnel
    // et ne sont consommées que plus tard (`handle_server`/`runserver`) :
    // rien n'empêche de les résoudre dès maintenant, avant même le dispatch
    // de mode — ce qui a aussi l'avantage de rendre les logs d'options de
    // `main()` (stop_on_solution/headless) cohérents avec une valeur venue du
    // fichier, pas seulement de la CLI.
    if (cfg->has_expand_level && expand_min_level == 0) {
        expand_min_level = cfg->expand_level;
    }
    if (cfg->has_expand_max_stock && expand_max_stock == EXPAND_MAX_STOCK) {
        expand_max_stock = cfg->expand_max_stock;
    }
    if (cfg->has_expand_max_levels && expand_max_levels == EXPAND_MAX_LEVELS) {
        expand_max_levels = cfg->expand_max_levels;
    }
    if (cfg->has_stock_spill_dir && cfg->stock_spill_dir != NULL &&
        strcmp(stock_spill_dir, "./eternityii-spill") == 0) {
        stock_spill_dir = strdup(cfg->stock_spill_dir);
    }
    if (cfg->has_rebalance_budget && rebalance_budget == REBALANCE_BUDGET_DEFAULT) {
        rebalance_budget = cfg->rebalance_budget;
    }
    if (cfg->has_tcp_timeout && tcp_timeout == DEFAULT_TCP_TIMEOUT) {
        tcp_timeout = cfg->tcp_timeout;
    }
    if (cfg->has_auto_roles && auto_roles_requested == 0) {
        auto_roles_requested = cfg->auto_roles;
    }
    if (cfg->has_stop_on_solution && stop_on_solution == 0) {
        stop_on_solution = cfg->stop_on_solution;
    }
    if (cfg->has_headless && headless_mode == 0) {
        headless_mode = cfg->headless;
    }
}

void server_config_apply_to_globals(const server_config_t *cfg, int cli_gave_nb_threads, int cli_gave_parts_file)
{
    if (cfg->has_nb_threads && !cli_gave_nb_threads) {
        NB_THREADS = cfg->nb_threads;
    }
    if (cfg->has_parts_file && cfg->parts_file != NULL && !cli_gave_parts_file) {
        parts_files = strdup(cfg->parts_file);
    }
}
