#include "ui/line_edit.h"
#include "ui/command_history.h"

#include <string.h>

void line_edit_reset(line_edit_t *le)
{
    le->buf[0]  = '\0';
    le->len     = 0;
    le->cursor  = 0;
    le->hist_cursor = -1;
    le->draft[0]    = '\0';
    le->draft_len   = 0;
}

/** @brief Charge l'entrée d'historique @p index dans le tampon (remplace tout). */
static void load_history_entry(line_edit_t *le, int index)
{
    const char *h = history_get(index);
    if (h == NULL) return;
    int hlen = (int)strlen(h);
    if (hlen >= LINE_EDIT_BUFSZ) hlen = LINE_EDIT_BUFSZ - 1;
    memcpy(le->buf, h, hlen);
    le->buf[hlen] = '\0';
    le->len    = hlen;
    le->cursor = hlen;
}

int line_edit_feed(line_edit_t *le, line_edit_key key, int ch)
{
    switch (key) {
    case LE_KEY_CHAR:
        if (ch < 32 || ch >= 127) return 0;
        if (le->len + 1 >= LINE_EDIT_BUFSZ) return 0;
        /* Décale la queue d'un cran pour insérer au curseur (pas seulement en
           fin de ligne) ; memmove couvre le cas curseur == len (déplacement
           de longueur nulle). */
        memmove(le->buf + le->cursor + 1, le->buf + le->cursor,
                (size_t)(le->len - le->cursor + 1)); /* +1 : inclut le NUL */
        le->buf[le->cursor] = (char)ch;
        le->cursor++;
        le->len++;
        return 1;

    case LE_KEY_LEFT:
        if (le->cursor == 0) return 0;
        le->cursor--;
        return 1;

    case LE_KEY_RIGHT:
        if (le->cursor >= le->len) return 0;
        le->cursor++;
        return 1;

    case LE_KEY_HOME:
        if (le->cursor == 0) return 0;
        le->cursor = 0;
        return 1;

    case LE_KEY_END:
        if (le->cursor == le->len) return 0;
        le->cursor = le->len;
        return 1;

    case LE_KEY_BACKSPACE:
        if (le->cursor == 0) return 0;
        memmove(le->buf + le->cursor - 1, le->buf + le->cursor,
                (size_t)(le->len - le->cursor + 1));
        le->cursor--;
        le->len--;
        return 1;

    case LE_KEY_DELETE:
        if (le->cursor >= le->len) return 0;
        memmove(le->buf + le->cursor, le->buf + le->cursor + 1,
                (size_t)(le->len - le->cursor)); /* inclut le NUL */
        le->len--;
        return 1;

    case LE_KEY_KILL_LINE:
        if (le->len == 0) return 0;
        le->buf[0] = '\0';
        le->len    = 0;
        le->cursor = 0;
        return 1;

    case LE_KEY_KILL_WORD: {
        if (le->cursor == 0) return 0;
        int end = le->cursor;
        int start = end;
        /* Saute les espaces immédiatement avant le curseur, puis le mot. */
        while (start > 0 && le->buf[start - 1] == ' ') start--;
        while (start > 0 && le->buf[start - 1] != ' ') start--;
        if (start == end) return 0;
        memmove(le->buf + start, le->buf + end, (size_t)(le->len - end + 1));
        le->len   -= (end - start);
        le->cursor = start;
        return 1;
    }

    case LE_KEY_HISTORY_PREV:
        if (history_size() == 0) return 0;
        if (le->hist_cursor == -1) {
            /* Sauvegarde la saisie courante avant d'entrer dans l'histo. */
            memcpy(le->draft, le->buf, (size_t)le->len + 1);
            le->draft_len = le->len;
            le->hist_cursor = 0;
        } else if (le->hist_cursor + 1 < history_size()) {
            le->hist_cursor++;
        } else {
            return 0; /* déjà sur la plus ancienne */
        }
        load_history_entry(le, le->hist_cursor);
        return 1;

    case LE_KEY_HISTORY_NEXT:
        if (le->hist_cursor < 0) return 0; /* déjà sur le draft */
        le->hist_cursor--;
        if (le->hist_cursor < 0) {
            memcpy(le->buf, le->draft, (size_t)le->draft_len + 1);
            le->len    = le->draft_len;
            le->cursor = le->draft_len;
        } else {
            load_history_entry(le, le->hist_cursor);
        }
        return 1;
    }

    return 0;
}
