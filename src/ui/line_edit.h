#ifndef line_edit_h
#define line_edit_h

#include <stddef.h>

/*
 * Module d'édition de ligne commun aux deux consoles (ANSI et ncurses).
 *
 * Sans I/O : expose un état (tampon + position du curseur) muté en réponse à
 * des touches ABSTRAITES. Chaque frontend traduit son entrée brute (séquences
 * \033[... côté ANSI ; KEY_LEFT/KEY_HOME/... côté ncurses) en touche
 * abstraite, appelle line_edit_feed(), puis redessine à partir de l'état
 * (line_edit_text() + line_edit_cursor()). Sans I/O, le module est testable
 * unitairement sans PTY ni fork (voir tests/ui/test_line_edit.c).
 *
 * Intègre la navigation dans l'historique (command_history.h) : ↑/↓
 * chargent une entrée de l'historique, en sauvegardant/restaurant la saisie
 * en cours (le « draft ») — logique auparavant dupliquée dans console.c et
 * logger_ncurses.c.
 */

#define LINE_EDIT_BUFSZ 1024

typedef enum {
    LE_KEY_CHAR,          /* insertion d'un caractère imprimable au curseur   */
    LE_KEY_LEFT,          /* curseur d'un caractère vers la gauche           */
    LE_KEY_RIGHT,         /* curseur d'un caractère vers la droite           */
    LE_KEY_HOME,          /* curseur en début de ligne (aussi : Ctrl-A)      */
    LE_KEY_END,           /* curseur en fin de ligne (aussi : Ctrl-E)        */
    LE_KEY_BACKSPACE,     /* efface le caractère avant le curseur            */
    LE_KEY_DELETE,        /* efface le caractère sous le curseur             */
    LE_KEY_KILL_LINE,     /* efface toute la ligne (Ctrl-U)                  */
    LE_KEY_KILL_WORD,     /* efface le mot précédant le curseur (Ctrl-W)     */
    LE_KEY_HISTORY_PREV,  /* rappelle la commande précédente (flèche ↑)      */
    LE_KEY_HISTORY_NEXT   /* avance vers une commande plus récente (flèche ↓) */
} line_edit_key;

typedef struct {
    char buf[LINE_EDIT_BUFSZ];   /* tampon de la ligne, NUL-terminé          */
    int  len;                   /* longueur courante (hors NUL)             */
    int  cursor;                /* position du curseur, 0..len              */

    /* Navigation historique : -1 = on édite la saisie courante (draft) ;
       sinon index dans l'historique (0 = commande la plus récente). */
    int  hist_cursor;
    char draft[LINE_EDIT_BUFSZ];
    int  draft_len;
} line_edit_t;

/** @brief Réinitialise l'état à une ligne vide, hors navigation historique. */
void line_edit_reset(line_edit_t *le);

/**
 * @brief Applique une touche abstraite à l'état.
 *
 * @param le    État à muter.
 * @param key   Touche abstraite.
 * @param ch    Caractère à insérer, uniquement significatif pour LE_KEY_CHAR
 *              (imprimable, 32..126 ; toute autre valeur est ignorée).
 * @return 1 si l'état a changé (le frontend doit redessiner), 0 sinon (ex. :
 *         LEFT en butée gauche, HISTORY_PREV sans historique disponible).
 */
int line_edit_feed(line_edit_t *le, line_edit_key key, int ch);

/** @brief Contenu courant de la ligne (toujours NUL-terminé). */
static inline const char *line_edit_text(const line_edit_t *le) { return le->buf; }

/** @brief Position courante du curseur (0..strlen(texte)). */
static inline int line_edit_cursor(const line_edit_t *le) { return le->cursor; }

#endif /* line_edit_h */
