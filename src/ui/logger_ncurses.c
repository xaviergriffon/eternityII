/*
 * Variante ncurses de logger.c.
 *
 * Compilée à la place de logger.c quand on build avec NCURSES=1 (voir Makefile).
 * Reprend exactement l'interface publique de logger.h, mais utilise ncurses
 * pour gérer trois zones distinctes du terminal :
 *
 *   ┌────────────────────────────────┐  ← rangée 0
 *   │  output_pad (pad ncurses)      │
 *   │  log_info / log_error / ...    │  ← scrollable via PgUp/PgDn/Home/End
 *   │  ...                           │
 *   ├────────────────────────────────┤  ← stats_win (bandeau live)
 *   │ coups/s … stock … record …     │  (vidéo inverse, MAJ par le checker)
 *   ├────────────────────────────────┤  ← début events_win
 *   │  Events  [+N pour PgDn/End]    │  (titre vidéo inverse + indicateur scroll)
 *   │  [hh:mm:ss] event 1            │
 *   │  ...                           │
 *   ├────────────────────────────────┤  ← input_win
 *   │  commande : _                  │
 *   └────────────────────────────────┘  ← dernière rangée
 *
 * Scroll de la zone output : pad ncurses (newpad) de OUTPUT_PAD_LINES lignes.
 * Touches :
 *   PgUp / PgDn   : scroll d'une page
 *   Home / End    : tout en haut / tout en bas (End réactive l'auto-suivi)
 * Quand on tape Entrée, on revient automatiquement en bas (l'auto-suivi est
 * réactivé) pour voir l'output de la commande qu'on vient d'exécuter.
 *
 * Pas de dépendance ncurses dans le build par défaut : ce fichier n'est
 * compilé que si -DUSE_NCURSES est défini.
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ui/logger.h"
#include "ui/command_lines.h"
#include "ui/command_history.h"
#include "app/static_variables.h"
#include "net/ipc_protocol.h"

/* ------------------------------------------------------------------------- */
/*  État partagé : buffer d'événements + fenêtres ncurses                    */
/* ------------------------------------------------------------------------- */

#define EVENT_ZONE_LINES 6
#define EVENT_MSG_MAX    200
#define EVENT_LOG_FILE   "events.log"
#define LOG_LINE_MAX     4096

/* Bandeau de statistiques « live » : une ligne d'état en vidéo inverse,
   rafraîchie en continu par le thread checker via log_status(). */
#define STATS_ZONE_LINES 1
#define STATUS_MSG_MAX   512

/* Taille du pad de sortie : nombre maximum de lignes d'historique
   conservées. ~3000 lignes × ~200 cols × ~8 octets ≈ 5 Mo. */
#define OUTPUT_PAD_LINES 3000

/* Sérialise toutes les écritures ncurses : la lib n'est pas thread-safe. */
static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Flag indiquant qu'un redimensionnement est en attente (depuis SIGWINCH). */
static volatile sig_atomic_t resize_pending = 0;


/* Ring buffer des derniers événements. */
static char            event_ring[EVENT_ZONE_LINES][EVENT_MSG_MAX];
static int             event_head = 0;
static int             event_count = 0;
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;

static WINDOW *output_pad = NULL;  /* pad scrollable, plus grand que l'écran */
static WINDOW *stats_win  = NULL;  /* bandeau de stats live (1 ligne)        */
static WINDOW *events_win = NULL;
static WINDOW *input_win  = NULL;
static int     nc_active  = 0;

/* Dernier texte du bandeau de stats (protégé par output_mutex). */
static char status_buf[STATUS_MSG_MAX] = " stats : en attente du premier rapport... ";

/* État de défilement du pad de sortie. */
static int output_screen_h = 0;    /* hauteur visible de la zone de sortie    */
static int pad_view_top    = 0;    /* index dans le pad de la ligne du haut   */
static int auto_stick      = 1;    /* 1 = la vue suit le bas du pad           */

/* Tampon de saisie courante (partagé entre nc_console_loop et l'affichage). */
#define INPUT_PROMPT "commande : "
#define INPUT_BUFSZ  1024
static char input_buf[INPUT_BUFSZ];
static int  input_len = 0;

static void nc_draw_events_locked(void);
static void nc_draw_input_locked(void);
static void nc_draw_status_locked(void);
static void nc_refresh_pad_locked(void);

/* ------------------------------------------------------------------------- */
/*  IPC : routage des logs des enfants forkés vers le parent                 */
/* ------------------------------------------------------------------------- */

/** @brief Vrai si on est un enfant forké avec un socket prêt vers le parent. */
static int log_should_route_to_parent(void)
{
    return parent_pid != 0
        && parent_pid != getpid()
        && fork_checker_socket_id > 0
        && main_addr != NULL;
}

/**
 * @brief Envoie au parent un datagramme UDP : 1 octet de type + texte.
 *        Best-effort (MSG_DONTWAIT) : un éventuel buffer plein ne bloque pas
 *        les threads de recherche.
 */
static void log_send_to_parent(int8_t type, const char *text)
{
    char buf[1 + IPC_LINE_MAX];
    size_t len = strlen(text);
    if (len > IPC_LINE_MAX - 1) len = IPC_LINE_MAX - 1;
    buf[0] = (char)type;
    memcpy(buf + 1, text, len);
    sendto(fork_checker_socket_id, buf, len + 1, MSG_DONTWAIT,
           (struct sockaddr *) main_addr, sizeof(struct sockaddr_un));
}

/* ------------------------------------------------------------------------- */
/*  Helpers : position du curseur dans le pad et bornes de la vue            */
/* ------------------------------------------------------------------------- */

/** @brief Renvoie la ligne courante du curseur du pad (où ira la prochaine écriture). */
static int pad_cursor_y(void)
{
    if (!output_pad) return 0;
    int y, x;
    getyx(output_pad, y, x);
    (void)x;
    return y;
}

/** @brief Index maximum de pad_view_top pour que la dernière ligne soit visible. */
static int pad_max_view_top(int visible_h)
{
    int y = pad_cursor_y();
    int max_top = y - visible_h + 1;
    if (max_top < 0) max_top = 0;
    return max_top;
}

/* ------------------------------------------------------------------------- */
/*  Layout / setup                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief (Re)dimensionne les fenêtres en fonction de la taille actuelle de
 *        l'écran ncurses. Le pad de sortie est créé une seule fois et
 *        redimensionné (wresize) sur les changements de taille pour
 *        préserver l'historique. À appeler sous `output_mutex`.
 */
static void nc_setup_layout_locked(void)
{
    /* Lorsqu'on est appelé suite à un KEY_RESIZE, ncurses a déjà redimensionné
       stdscr et mis à jour LINES/COLS : on lit donc directement la nouvelle
       taille. NB : ne PAS appeler resizeterm() ici — cela réinjecte un
       KEY_RESIZE dans la file d'entrée (boucle de resize infinie). */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (rows < 3)  rows = 3;
    if (cols < 40) cols = 40;

    int stats_h  = STATS_ZONE_LINES;
    int events_h = EVENT_ZONE_LINES + 1;
    int input_h  = 1;
    int output_h = rows - stats_h - events_h - input_h;
    if (output_h < 1) {
        stats_h = 0;
        output_h = rows - events_h - input_h;
        if (output_h < 1) {
            events_h = (rows >= 3) ? rows - 2 : 1;
            output_h = 1;
            input_h  = 1;
        }
    }
    output_screen_h = output_h;

    /* Nettoyer les anciennes fenêtres. */
    if (stats_win)  { delwin(stats_win);  stats_win  = NULL; }
    if (events_win) { delwin(events_win); events_win = NULL; }
    if (input_win)  { delwin(input_win);  input_win  = NULL; }

    /* Le pad d'output : créé une fois, redimensionné sur resize. */
    if (!output_pad) {
        output_pad = newpad(OUTPUT_PAD_LINES, cols);
        if (output_pad) {
            scrollok(output_pad, TRUE);
            idlok(output_pad, TRUE);
        }
    } else {
        /* wresize() peut échouer en cas de manque de mémoire ;
           on ignore l'erreur et on continue avec l'ancienne taille. */
        if (cols > 0) {
            wresize(output_pad, OUTPUT_PAD_LINES, cols);
        }
    }

    /* Nouvelles fenêtres : empilement vertical output / [stats] / events / input.
       NB : ne pas journaliser ici en cas d'échec de newwin() — log_error()
       reprend output_mutex (non récursif) que l'on détient déjà → deadlock.
       Toutes les routines de dessin tolèrent une fenêtre NULL. */
    int y = output_h;
    if (stats_h > 0) {
        stats_win = newwin(stats_h, cols, y, 0);
        y += stats_h;
    }

    events_win = newwin(events_h, cols, y, 0);
    y += events_h;

    input_win = newwin(input_h, cols, y, 0);

    if (input_win) {
        keypad(input_win, TRUE);
        nodelay(input_win, TRUE);
    }

    /* Repositionner la vue du pad si nécessaire. */
    if (auto_stick) {
        pad_view_top = pad_max_view_top(output_screen_h);
    } else {
        int max_top = pad_max_view_top(output_screen_h);
        if (pad_view_top > max_top) pad_view_top = max_top;
    }

    /* Forcer un redraw complet : invalide la copie interne de l'écran ncurses
       pour que doupdate() réécrive chaque cellule sans se fier à son cache. */
    clearok(curscr, TRUE);

    if (stdscr) {
        werase(stdscr);
        wnoutrefresh(stdscr);
    }
    if (stats_win) {
        werase(stats_win);
        wnoutrefresh(stats_win);
    }
    if (events_win) {
        werase(events_win);
        wnoutrefresh(events_win);
    }
    if (input_win) {
        werase(input_win);
        wnoutrefresh(input_win);
    }
    nc_refresh_pad_locked();
    nc_draw_status_locked();
    nc_draw_events_locked();
    doupdate();

    resize_pending = 0;
}

/* ------------------------------------------------------------------------- */
/*  Rendu : pad de sortie, Events, Input                                     */
/* ------------------------------------------------------------------------- */

/** @brief Affiche la portion visible du pad de sortie. Sous `output_mutex`. */
static void nc_refresh_pad_locked(void)
{
    if (!nc_active || !output_pad || output_screen_h <= 0) return;
    if (!stdscr) return;

    int cols = getmaxx(stdscr);
    if (cols <= 0) cols = 80;

    /* pnoutrefresh prépare la mise à jour ; doupdate la pousse à l'écran. */
    if (pnoutrefresh(output_pad, pad_view_top, 0, 0, 0,
                     output_screen_h - 1, cols - 1) != ERR) {
        if (input_win) wnoutrefresh(input_win);
        doupdate();
    }
}

static void nc_draw_events_locked(void)
{
    if (!nc_active || !events_win) return;

    char snapshot[EVENT_ZONE_LINES][EVENT_MSG_MAX];
    int n;
    pthread_mutex_lock(&event_mutex);
    n = event_count;
    for (int i = 0; i < n; i++) {
        int idx = (event_head - n + i + EVENT_ZONE_LINES) % EVENT_ZONE_LINES;
        memcpy(snapshot[i], event_ring[idx], EVENT_MSG_MAX);
    }
    pthread_mutex_unlock(&event_mutex);

    int cols = getmaxx(events_win);
    if (cols <= 0) cols = 80;

    werase(events_win);

    char title[128];
    int hidden = pad_max_view_top(output_screen_h) - pad_view_top;
    if (hidden > 0) {
        snprintf(title, sizeof title,
                 " Events  [+%d ligne%s sous la vue — PgDn/End pour revenir] ",
                 hidden, hidden > 1 ? "s" : "");
    } else {
        snprintf(title, sizeof title, " Events ");
    }
    int title_len = (int)strlen(title);
    if (title_len > cols) title_len = cols;

    wattron(events_win, A_REVERSE);
    mvwaddnstr(events_win, 0, 0, title, title_len);
    for (int c = title_len; c < cols; c++) {
        waddch(events_win, ' ');
    }
    wattroff(events_win, A_REVERSE);

    for (int i = 0; i < n && i < EVENT_ZONE_LINES; i++) {
        mvwaddnstr(events_win, i + 1, 0, snapshot[i], cols - 1);
    }
    wnoutrefresh(events_win);
    if (input_win) {
        wnoutrefresh(input_win);
    }
    doupdate();
}

static void nc_draw_input_locked(void)
{
    if (!nc_active || !input_win) return;
    int cols = getmaxx(input_win);
    if (cols <= 0) cols = 80;

    werase(input_win);
    mvwaddstr(input_win, 0, 0, INPUT_PROMPT);
    int promlen = (int)strlen(INPUT_PROMPT);
    int avail = cols - promlen - 1;
    if (avail < 0) avail = 0;
    int show_len = input_len > avail ? avail : input_len;
    int start = input_len - show_len;
    if (start < 0) start = 0;
    if (show_len > 0) {
        waddnstr(input_win, input_buf + start, show_len);
    }
    wmove(input_win, 0, promlen + show_len);
    wrefresh(input_win);
}

/**
 * @brief Affiche le bandeau de stats live (vidéo inverse, sur toute la largeur).
 *        Sous `output_mutex`. Le curseur est rendu à la zone de saisie.
 */
static void nc_draw_status_locked(void)
{
    if (!nc_active || !stats_win) return;
    int cols = getmaxx(stats_win);
    if (cols <= 0) cols = 80;

    werase(stats_win);
    int len = (int)strlen(status_buf);
    if (len > cols) len = cols;
    wattron(stats_win, A_REVERSE);
    mvwaddnstr(stats_win, 0, 0, status_buf, len);
    for (int c = len; c < cols; c++) {
        waddch(stats_win, ' ');
    }
    wattroff(stats_win, A_REVERSE);
    wnoutrefresh(stats_win);
    if (input_win) wnoutrefresh(input_win);
    doupdate();
}

/* ------------------------------------------------------------------------- */
/*  Logs classiques : redirigés vers output_pad                              */
/* ------------------------------------------------------------------------- */

static void nc_write_output_locked(const char *text)
{
    if (nc_active && output_pad) {
        waddstr(output_pad, text);
        if (auto_stick) {
            pad_view_top = pad_max_view_top(output_screen_h);
        }
        nc_refresh_pad_locked();
        if (!auto_stick && events_win) {
            nc_draw_events_locked();
        }
    } else if (!nc_active) {
        fputs(text, stdout);
        fflush(stdout);
    }
}

void log_errno(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    snprintf(buf + n, sizeof buf - n, "%i : %s \n", errno, strerror(errno));

    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_LOG_ERROR, buf);
        return;
    }
    pthread_mutex_lock(&output_mutex);
    nc_write_output_locked(buf);
    pthread_mutex_unlock(&output_mutex);
}

void log_error(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_LOG_ERROR, buf);
        return;
    }
    pthread_mutex_lock(&output_mutex);
    nc_write_output_locked(buf);
    pthread_mutex_unlock(&output_mutex);
}

void log_info(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_LOG_INFO, buf);
        return;
    }
    pthread_mutex_lock(&output_mutex);
    nc_write_output_locked(buf);
    pthread_mutex_unlock(&output_mutex);
}

void log_debug(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_LOG_DEBUG, buf);
        return;
    }
    pthread_mutex_lock(&output_mutex);
    nc_write_output_locked(buf);
    pthread_mutex_unlock(&output_mutex);
}

void log_console(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_LOG_CONSOLE, buf);
        return;
    }
    pthread_mutex_lock(&output_mutex);
    nc_write_output_locked(buf);
    pthread_mutex_unlock(&output_mutex);
}

/* Sous ncurses, le rafraîchissement est implicite (doupdate). Ces flush_*
   restent appelables sans effet pour préserver l'API publique. */
void flush_console(void) {}
void flush_debug(void)   {}
void flush_error(void)   {}
void flush_info(void)    {}

/* ------------------------------------------------------------------------- */
/*  Événements : buffer + fichier + redessin                                 */
/* ------------------------------------------------------------------------- */

void log_event(const char *format, ...)
{
    char msg[EVENT_MSG_MAX];
    char line[EVENT_MSG_MAX];

    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof msg, format, args);
    va_end(args);

    size_t l = strlen(msg);
    while (l > 0 && (msg[l - 1] == '\n' || msg[l - 1] == '\r')) {
        msg[--l] = '\0';
    }

    /* Si on est un enfant forké : on relaie au parent, qui horodatera,
       écrira dans events.log et redessinera la zone Events. */
    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_EVENT, msg);
        return;
    }

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[16];
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    snprintf(line, sizeof line, "[%s] %s", ts, msg);

    /* line est déjà NUL-terminé par snprintf et fait EVENT_MSG_MAX octets :
       un memcpy du buffer complet copie le terminateur (évite
       -Wstringop-truncation sur le strncpy précédent). */
    pthread_mutex_lock(&event_mutex);
    memcpy(event_ring[event_head], line, EVENT_MSG_MAX);
    event_head = (event_head + 1) % EVENT_ZONE_LINES;
    if (event_count < EVENT_ZONE_LINES) {
        event_count++;
    }
    pthread_mutex_unlock(&event_mutex);

    char fts[24];
    strftime(fts, sizeof fts, "%Y-%m-%d %H:%M:%S", &tmv);
    FILE *f = fopen(EVENT_LOG_FILE, "a");
    if (f != NULL) {
        fprintf(f, "[%s] %s\n", fts, msg);
        fclose(f);
    }

    pthread_mutex_lock(&output_mutex);
    if (nc_active) {
        nc_draw_events_locked();
    } else {
        fprintf(stdout, "%s\n", line);
        fflush(stdout);
    }
    pthread_mutex_unlock(&output_mutex);
}

void log_status(const char *format, ...)
{
    char buf[STATUS_MSG_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);

    /* Le thread checker qui appelle log_status() tourne dans le parent ; il n'y
       a pas de routage IPC à faire. Si un enfant forké appelait malgré tout,
       on ignore (il n'a pas de bandeau à mettre à jour). */
    if (log_should_route_to_parent()) {
        return;
    }

    pthread_mutex_lock(&output_mutex);
    strncpy(status_buf, buf, sizeof status_buf - 1);
    status_buf[sizeof status_buf - 1] = '\0';
    if (nc_active) {
        nc_draw_status_locked();
    }
    pthread_mutex_unlock(&output_mutex);
}

/* ------------------------------------------------------------------------- */
/*  Cycle de vie : init / teardown                                           */
/* ------------------------------------------------------------------------- */

void status_zone_init(void)
{
    if (!isatty(STDOUT_FILENO)) {
        return;
    }
    pthread_mutex_lock(&output_mutex);
    initscr();
    cbreak();
    noecho();
    nonl();
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);
    curs_set(1);
    nc_setup_layout_locked();
    nc_active = 1;
    nc_draw_status_locked();
    nc_draw_events_locked();
    nc_draw_input_locked();
    pthread_mutex_unlock(&output_mutex);

    atexit(status_zone_teardown);
}

void status_zone_teardown(void)
{
    if (!nc_active) {
        return;
    }
    pthread_mutex_lock(&output_mutex);
    nc_active = 0;
    resize_pending = 0;
    if (output_pad) { delwin(output_pad); output_pad = NULL; }
    if (stats_win)  { delwin(stats_win);  stats_win  = NULL; }
    if (events_win) { delwin(events_win); events_win = NULL; }
    if (input_win)  { delwin(input_win);  input_win  = NULL; }
    if (stdscr) {
        endwin();
    }
    pthread_mutex_unlock(&output_mutex);
}

void clear_console(void)
{
    pthread_mutex_lock(&output_mutex);
    if (nc_active && output_pad) {
        werase(output_pad);
        wmove(output_pad, 0, 0);   /* curseur en haut, prêt à réécrire        */
        pad_view_top = 0;
        auto_stick = 1;
        nc_refresh_pad_locked();
        nc_draw_events_locked();   /* met à jour l'indicateur de scroll       */
    }
    pthread_mutex_unlock(&output_mutex);
}

/* ------------------------------------------------------------------------- */
/*  Boucle interactive ncurses                                               */
/* ------------------------------------------------------------------------- */

void nc_console_loop(void)
{
    input_len = 0;
    input_buf[0] = '\0';

    /* État de navigation dans l'historique. hist_cursor = -1 → on édite le
       draft courant ; sinon index dans l'historique (0 = plus récente). */
    char draft_buf[INPUT_BUFSZ];
    int  draft_len = 0;
    int  hist_cursor = -1;

    pthread_mutex_lock(&output_mutex);
    nc_draw_input_locked();
    pthread_mutex_unlock(&output_mutex);

    int input_dirty = 0;

    while (1) {
        /* Vérifier si un redimensionnement a été signalé par SIGWINCH. */
        if (resize_pending) {
            pthread_mutex_lock(&output_mutex);
            nc_setup_layout_locked();   /* dessine déjà stats + events */
            if (nc_active) {
                nc_draw_input_locked(); /* repose le curseur sur la saisie */
            }
            pthread_mutex_unlock(&output_mutex);
            continue;
        }

        if (input_dirty) {
            pthread_mutex_lock(&output_mutex);
            if (nc_active) {
                nc_draw_input_locked();
            }
            pthread_mutex_unlock(&output_mutex);
            input_dirty = 0;
        }

        int ch;
        pthread_mutex_lock(&output_mutex);
        ch = (nc_active && input_win) ? wgetch(input_win) : ERR;
        pthread_mutex_unlock(&output_mutex);

        if (ch == ERR) {
            usleep(30000);
            continue;
        }

        /* KEY_RESIZE : ncurses a déjà mis à jour LINES/COLS. */
        if (ch == KEY_RESIZE) {
            resize_pending = 1;
            continue;
        }

        /* --- Scroll de l'historique de sortie --- */
        if (ch == KEY_PPAGE) {              /* PgUp */
            pthread_mutex_lock(&output_mutex);
            int step = output_screen_h - 1;
            if (step < 1) step = 1;
            pad_view_top = (pad_view_top >= step) ? (pad_view_top - step) : 0;
            auto_stick = 0;
            nc_refresh_pad_locked();
            nc_draw_events_locked();
            pthread_mutex_unlock(&output_mutex);
            continue;
        }
        if (ch == KEY_NPAGE) {              /* PgDn */
            pthread_mutex_lock(&output_mutex);
            int step = output_screen_h - 1;
            if (step < 1) step = 1;
            int max_top = pad_max_view_top(output_screen_h);
            pad_view_top += step;
            if (pad_view_top >= max_top) {
                pad_view_top = max_top;
                auto_stick = 1;
            }
            nc_refresh_pad_locked();
            nc_draw_events_locked();
            pthread_mutex_unlock(&output_mutex);
            continue;
        }
        if (ch == KEY_HOME) {
            pthread_mutex_lock(&output_mutex);
            pad_view_top = 0;
            auto_stick = 0;
            nc_refresh_pad_locked();
            nc_draw_events_locked();
            pthread_mutex_unlock(&output_mutex);
            continue;
        }
        if (ch == KEY_END) {
            pthread_mutex_lock(&output_mutex);
            pad_view_top = pad_max_view_top(output_screen_h);
            auto_stick = 1;
            nc_refresh_pad_locked();
            nc_draw_events_locked();
            pthread_mutex_unlock(&output_mutex);
            continue;
        }
        /* --- Fin scroll --- */

        /* --- Navigation dans l'historique des commandes (↑ / ↓) --- */
        if (ch == KEY_UP) {
            if (history_size() > 0) {
                if (hist_cursor == -1) {
                    /* On entre dans l'historique : on sauvegarde la saisie
                       en cours pour pouvoir y revenir avec ↓. */
                    memcpy(draft_buf, input_buf, input_len);
                    draft_buf[input_len] = '\0';
                    draft_len = input_len;
                    hist_cursor = 0;
                } else if (hist_cursor + 1 < history_size()) {
                    hist_cursor++;
                } else {
                    continue; /* déjà sur la plus ancienne */
                }
                const char *h = history_get(hist_cursor);
                if (h != NULL) {
                    int hlen = (int)strlen(h);
                    if (hlen >= (int)sizeof input_buf) hlen = (int)sizeof input_buf - 1;
                    memcpy(input_buf, h, hlen);
                    input_buf[hlen] = '\0';
                    input_len = hlen;
                    input_dirty = 1;
                }
            }
            continue;
        }
        if (ch == KEY_DOWN) {
            if (hist_cursor < 0) continue; /* déjà sur le draft */
            hist_cursor--;
            if (hist_cursor < 0) {
                memcpy(input_buf, draft_buf, draft_len);
                input_buf[draft_len] = '\0';
                input_len = draft_len;
            } else {
                const char *h = history_get(hist_cursor);
                if (h != NULL) {
                    int hlen = (int)strlen(h);
                    if (hlen >= (int)sizeof input_buf) hlen = (int)sizeof input_buf - 1;
                    memcpy(input_buf, h, hlen);
                    input_buf[hlen] = '\0';
                    input_len = hlen;
                }
            }
            input_dirty = 1;
            continue;
        }
        /* --- Fin navigation historique --- */

        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            input_buf[input_len] = '\0';

            /* Ajoute à l'historique (dédoublonné) et réinitialise le curseur. */
            history_add(input_buf);
            hist_cursor = -1;
            draft_len = 0;

            char echo[INPUT_BUFSZ + 32];
            snprintf(echo, sizeof echo, "commande : %s\n", input_buf);
            pthread_mutex_lock(&output_mutex);
            /* Taper Entrée réactive l'auto-suivi : on veut voir la sortie
               de la commande qu'on vient d'exécuter. */
            auto_stick = 1;
            nc_write_output_locked(echo);
            pthread_mutex_unlock(&output_mutex);

            char *copy = strdup(input_buf);
            input_len = 0;
            input_buf[0] = '\0';
            input_dirty = 1;
            if (copy) {
                do_command_line(copy);
                free(copy);
            }
            continue;
        }

        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (input_len > 0) {
                input_len--;
                input_buf[input_len] = '\0';
                input_dirty = 1;
            }
            continue;
        }

        if (ch >= 32 && ch < 127 && input_len + 1 < (int)sizeof input_buf) {
            input_buf[input_len++] = (char)ch;
            input_buf[input_len] = '\0';
            input_dirty = 1;
            continue;
        }
        /* Autres touches : ignorées. */
    }
}
