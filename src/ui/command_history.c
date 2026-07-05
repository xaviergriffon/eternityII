#include "ui/command_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/logger.h"

#define HISTORY_MAX 100

/* Ring buffer : history_buf[history_head] est la prochaine case à écrire.
   La dernière commande ajoutée est donc à (history_head - 1) modulo HISTORY_MAX. */
static char *history_buf[HISTORY_MAX];
static int   history_head = 0;
static int   history_count = 0;

void history_add(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }
    /* Ignore les doublons consécutifs (comportement de la plupart des shells). */
    if (history_count > 0) {
        int last_idx = (history_head - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (history_buf[last_idx] != NULL
            && strcmp(history_buf[last_idx], line) == 0) {
            return;
        }
    }

    if (history_buf[history_head] != NULL) {
        free(history_buf[history_head]);
    }
    history_buf[history_head] = strdup(line);

    history_head = (history_head + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) {
        history_count++;
    }
}

const char *history_get(int index)
{
    if (index < 0 || index >= history_count) {
        return NULL;
    }
    int actual = (history_head - 1 - index + HISTORY_MAX) % HISTORY_MAX;
    return history_buf[actual];
}

int history_size(void)
{
    return history_count;
}

#define HISTORY_FILE_NAME ".eternityII_history"

char *history_default_path(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return NULL;
    }
    const char *home = getenv("HOME");
    int n;
    if (home != NULL && home[0] != '\0') {
        n = snprintf(buf, size, "%s/%s", home, HISTORY_FILE_NAME);
    } else {
        /* Repli sur le répertoire courant si HOME est absent. */
        n = snprintf(buf, size, "./%s", HISTORY_FILE_NAME);
    }
    if (n < 0 || (size_t)n >= size) {
        return NULL; /* chemin tronqué : on ne renvoie pas de chemin partiel */
    }
    return buf;
}

void history_load(const char *path)
{
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        /* Absence du fichier (premier lancement) : ce n'est pas une erreur. */
        return;
    }
    char line[1024];
    while (fgets(line, sizeof line, f) != NULL) {
        /* Retire le saut de ligne final éventuel (\n ou \r\n). */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        /* history_add ignore les chaînes vides, dédoublonne et plafonne à
           HISTORY_MAX : le respect de ces règles est donc automatique. */
        history_add(line);
    }
    fclose(f);
}

int history_save(const char *path)
{
    if (path == NULL) {
        return -1;
    }
    /* Écriture atomique : on écrit dans un fichier temporaire puis on le
       renomme, afin de ne jamais corrompre l'historique existant si l'écriture
       échoue en cours de route. */
    char tmp[1200];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        log_error("history_save : chemin temporaire trop long pour « %s »\n", path);
        return -1;
    }

    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        log_error("history_save : impossible d'ouvrir « %s » en écriture\n", tmp);
        return -1;
    }

    /* Ordre chronologique : de la plus ancienne (index le plus grand) à la plus
       récente (index 0). */
    for (int i = history_count - 1; i >= 0; i--) {
        const char *entry = history_get(i);
        if (entry == NULL) {
            continue;
        }
        if (fprintf(f, "%s\n", entry) < 0) {
            log_error("history_save : échec d'écriture dans « %s »\n", tmp);
            fclose(f);
            remove(tmp);
            return -1;
        }
    }

    if (fclose(f) != 0) {
        log_error("history_save : échec de fermeture de « %s »\n", tmp);
        remove(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        log_error("history_save : échec du rename « %s » → « %s »\n", tmp, path);
        remove(tmp);
        return -1;
    }
    return 0;
}
