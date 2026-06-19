#include "ui/command_history.h"

#include <stdlib.h>
#include <string.h>

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
