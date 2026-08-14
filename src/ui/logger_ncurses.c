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
 *   │  [hh:mm:ss] event 1            │  (pas de titre : le bandeau de stats
 *   │  ...                           │   juste au-dessus fait déjà la
 *   │                                │   séparation ; la rangée ne s'affiche
 *   │                                │   en vidéo inverse que si on a remonté
 *   │                                │   dans l'historique du pad, cf. « +N
 *   │                                │   lignes sous la vue » ci-dessous)
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
#include "ui/line_edit.h"
#include "app/static_variables.h"
#include "app/fork_gate.h"
#include "app/fork_orchestrator.h"
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
   conservées. ~3000 lignes × ~200 cols × ~8 octets ≈ 5 Mo. Surchargeable à la
   compilation (make NCURSES=1 CPPFLAGS="-DOUTPUT_PAD_LINES=10000"), même
   convention que ETERN_PARTS / FORWARD_CHECK_K. */
#ifndef OUTPUT_PAD_LINES
#define OUTPUT_PAD_LINES 3000
#endif

/* Pas de défilement de la molette souris (lignes par cran). */
#define MOUSE_WHEEL_STEP 3

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

/* Saisie courante (partagée entre nc_console_loop et l'affichage). La logique
   d'édition (curseur, historique, Ctrl-A/E/U/W...) vit dans ui/line_edit.c,
   module commun avec la variante ANSI (console.c) : ce fichier ne fait plus
   que traduire les touches ncurses en touches abstraites et dessiner l'état. */
#define INPUT_PROMPT "commande : "
static line_edit_t input_le;

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

    /* Rangée 0 : plus de titre « Events » — le bandeau de stats juste
       au-dessus fait déjà la séparation visuelle, et le format horodaté des
       lignes qui suivent ("[hh:mm:ss] ...") suffit à les identifier comme
       des logs. Cette rangée reste néanmoins utile pour signaler qu'on a
       remonté dans l'historique du pad de sortie (PgUp) : sans indicateur,
       rien ne dirait qu'il y a du contenu plus récent hors vue. Elle ne
       s'affiche donc, en vidéo inverse, QUE dans ce cas — sinon elle reste
       simplement vide (déjà effacée par werase ci-dessus). */
    int hidden = pad_max_view_top(output_screen_h) - pad_view_top;
    if (hidden > 0) {
        char indicator[128];
        snprintf(indicator, sizeof indicator,
                 " +%d ligne%s sous la vue — PgDn/End pour revenir ",
                 hidden, hidden > 1 ? "s" : "");
        int indicator_len = (int)strlen(indicator);
        if (indicator_len > cols) indicator_len = cols;

        wattron(events_win, A_REVERSE);
        mvwaddnstr(events_win, 0, 0, indicator, indicator_len);
        for (int c = indicator_len; c < cols; c++) {
            waddch(events_win, ' ');
        }
        wattroff(events_win, A_REVERSE);
    }

    for (int i = 0; i < n && i < EVENT_ZONE_LINES; i++) {
        mvwaddnstr(events_win, i + 1, 0, snapshot[i], cols - 1);
    }
    wnoutrefresh(events_win);
    if (input_win) {
        wnoutrefresh(input_win);
    }
    doupdate();
}

/**
 * @brief Dessine la ligne de saisie et positionne le curseur ncurses.
 *
 * Le curseur du module line_edit peut être n'importe où dans la ligne (pas
 * seulement en fin) : la fenêtre visible [start, start+show_len) est calculée
 * pour toujours contenir le curseur, en faisant défiler horizontalement si la
 * ligne dépasse la largeur disponible (même principe que readline/bash).
 */
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

    int len    = input_le.len;
    int cursor = input_le.cursor;
    int start  = cursor - avail;
    if (start < 0) start = 0;
    int show_len = len - start;
    if (show_len > avail) show_len = avail;
    if (show_len < 0) show_len = 0;

    if (show_len > 0) {
        waddnstr(input_win, input_le.buf + start, show_len);
    }
    wmove(input_win, 0, promlen + (cursor - start));
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

/**
 * @brief Journalise un message d'erreur fatal puis termine le process.
 *
 * Variante ncurses : délègue à log_error (rendu dans le pad de sortie ou routage
 * vers le parent) puis exit(EXIT_FAILURE), ce qui déclenche le teardown ncurses
 * enregistré via atexit.
 */
__attribute__((noreturn))
void fatal_error(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    log_error("%s", buf);
    exit(EXIT_FAILURE);
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

/** @brief Prend `output_mutex` — cf. logger.h pour le contrat complet. */
void logger_lock_output(void)
{
    pthread_mutex_lock(&output_mutex);
}

/** @brief Relâche `output_mutex`. */
void logger_unlock_output(void)
{
    pthread_mutex_unlock(&output_mutex);
}

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
    /* Molette souris : scroll du pad de sortie. BUTTON4 = molette haut dans
       tous les cas. Molette bas : BUTTON5 n'existe qu'avec le protocole souris
       v2 (ncurses ABI 6) ; en v1 (ncurses ABI 5 — le ncurses système de macOS)
       le bouton 5 n'a pas de bit dans le bstate et l'événement molette-bas est
       délivré avec le bit REPORT_MOUSE_POSITION (constaté empiriquement : sans
       ce bit dans le masque, l'événement est filtré et getmouse rend ERR). Le
       protocole terminal reste le mode « clics seuls » (1000), donc demander
       REPORT_MOUSE_POSITION en v1 ne déclenche aucun flot d'événements de
       déplacement. NB : activer la souris fait intercepter les clics par le
       terminal — la sélection de texte demande alors Maj+clic (comportement
       standard des applications plein écran avec souris). */
#if NCURSES_MOUSE_VERSION > 1
    mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
#else
    mousemask(BUTTON4_PRESSED | REPORT_MOUSE_POSITION, NULL);
#endif
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

void status_zone_disown_child(void)
{
    /* Pas de verrou : appelée comme tout premier traitement d'un process
       fraîchement forké, mono-thread à cet instant (cf. logger.h). Écrit
       dans la copie COW du fils — sans effet sur le parent, et surtout
       n'appelle JAMAIS endwin() ici (ce fils ne possède pas le terminal). */
    nc_active = 0;
}

/**
 * @brief Efface la vue de sortie SANS détruire l'historique du pad.
 *
 * Avance le curseur du pad d'un écran complet de lignes vides : la vue devient
 * blanche (comme `clear` dans un shell) mais tout le contenu antérieur reste
 * accessible via PgUp/Home — contrairement à l'ancien werase() qui détruisait
 * les OUTPUT_PAD_LINES lignes d'historique.
 */
void clear_console(void)
{
    pthread_mutex_lock(&output_mutex);
    if (nc_active && output_pad) {
        int y, x;
        getyx(output_pad, y, x);
        (void)y;
        if (x > 0) {
            waddch(output_pad, '\n');  /* termine la ligne en cours            */
        }
        int h = output_screen_h > 0 ? output_screen_h : 1;
        for (int i = 1; i < h; i++) {
            waddch(output_pad, '\n');  /* un écran de lignes vides             */
        }
        pad_view_top = pad_max_view_top(output_screen_h);
        auto_stick = 1;
        nc_refresh_pad_locked();
        nc_draw_events_locked();       /* met à jour l'indicateur de scroll    */
    }
    pthread_mutex_unlock(&output_mutex);
}

/* Ligne de saisie interactive : spécifique au mode ANSI (voir logger.h). En
   ncurses la saisie vit dans input_win, redessinée par nc_draw_input_locked —
   ces deux fonctions sont des no-ops conservés pour l'interface commune. */
void console_input_render(const char *prompt, const char *line, int cursor)
{
    (void)prompt;
    (void)line;
    (void)cursor;
}

void console_input_end(void)
{
}

/* Pagination « --Suite-- » : spécifique au mode ANSI (voir logger.h). En
   ncurses le pad + PgUp/PgDn/molette couvrent déjà le besoin — no-ops. */
void console_pager_begin(void)
{
}

void console_pager_end(void)
{
}

/* ------------------------------------------------------------------------- */
/*  Boucle interactive ncurses                                               */
/* ------------------------------------------------------------------------- */

void nc_console_loop(void)
{
    line_edit_reset(&input_le);

    pthread_mutex_lock(&output_mutex);
    nc_draw_input_locked();
    pthread_mutex_unlock(&output_mutex);

    int input_dirty = 0;

    // Enregistrement auprès du gate de quiescence coopérative. Contrairement
    // à la variante ANSI (console.c), cette boucle est déjà non bloquante (wgetch en mode
    // nodelay, cf. plus bas) : un simple checkpoint en tête de boucle suffit,
    // pas besoin de fork_gate_mark_blocked.
    int gate_slot = fork_gate_register("console");

    while (1) {
        fork_gate_checkpoint(gate_slot);

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

        // Toute frappe au clavier annule un décompte d'auto-démarrage en
        // cours (état COUNTDOWN de l'orchestrateur) — cf. console.c pour le
        // même besoin côté ANSI : 5 s ne suffisent pas à taper une commande
        // complète. No-op hors COUNTDOWN. Snapshot avant l'événement, même
        // convention que console.c : un seul log au moment de la bascule
        // réelle, pas un par frappe.
        orch_state_t state_before_key;
        fork_orchestrator_snapshot(&state_before_key, NULL);
        if (state_before_key == ORCH_COUNTDOWN) {
            log_info("orchestrateur : décompte d'auto-démarrage annulé par une saisie clavier\n");
        }
        fork_orchestrator_post_event(EV_CONFIG_BEGUN, NULL);

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

        if (ch == 12) {                     /* Ctrl-L : efface la vue (l'historique
                                               du pad reste accessible via PgUp) */
            clear_console();
            continue;
        }

        if (ch == KEY_MOUSE) {              /* molette : scroll du pad          */
            MEVENT ev;
            if (getmouse(&ev) == OK) {
                pthread_mutex_lock(&output_mutex);
                if (ev.bstate & BUTTON4_PRESSED) {          /* molette haut  */
                    pad_view_top = (pad_view_top >= MOUSE_WHEEL_STEP)
                                 ? (pad_view_top - MOUSE_WHEEL_STEP) : 0;
                    auto_stick = 0;
                }
                /* Molette bas : BUTTON5 en protocole v2, REPORT_MOUSE_POSITION
                   en v1 (voir le commentaire du mousemask dans status_zone_init). */
#if NCURSES_MOUSE_VERSION > 1
                else if (ev.bstate & BUTTON5_PRESSED) {     /* molette bas   */
#else
                else if (ev.bstate & REPORT_MOUSE_POSITION) {
#endif
                    int max_top = pad_max_view_top(output_screen_h);
                    pad_view_top += MOUSE_WHEEL_STEP;
                    if (pad_view_top >= max_top) {
                        pad_view_top = max_top;
                        auto_stick = 1;
                    }
                }
                nc_refresh_pad_locked();
                nc_draw_events_locked();
                pthread_mutex_unlock(&output_mutex);
            }
            continue;
        }

        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            /* Ajoute à l'historique (dédoublonné) et réinitialise la
               navigation historique (mais pas le tampon : on en a encore
               besoin pour l'écho ci-dessous). */
            history_add(line_edit_text(&input_le));
            input_le.hist_cursor = -1;
            input_le.draft_len   = 0;

            char echo[LINE_EDIT_BUFSZ + 32];
            snprintf(echo, sizeof echo, "commande : %s\n", line_edit_text(&input_le));
            pthread_mutex_lock(&output_mutex);
            /* Taper Entrée réactive l'auto-suivi : on veut voir la sortie
               de la commande qu'on vient d'exécuter. */
            auto_stick = 1;
            nc_write_output_locked(echo);
            pthread_mutex_unlock(&output_mutex);

            char *copy = strdup(line_edit_text(&input_le));
            line_edit_reset(&input_le);
            input_dirty = 1;
            if (copy) {
                do_command_line(copy);
                free(copy);
            }
            continue;
        }

        /* --- Édition de la ligne : déléguée au module commun line_edit.c
           (curseur, backspace, delete, historique, Ctrl-A/E/U/W...), partagé
           avec la variante ANSI (getcmdline_raw, console.c). --- */
        /* NB : KEY_HOME/KEY_END sont déjà interceptées plus haut pour le
           scroll du pad de sortie (cf. docs/console.md) ; Ctrl-A/Ctrl-E
           couvrent donc le début/fin de LIGNE ici, sans ambiguïté de touche. */
        line_edit_key key;
        int fed_ch = ch;
        switch (ch) {
        case KEY_LEFT:      key = LE_KEY_LEFT;         break;
        case KEY_RIGHT:     key = LE_KEY_RIGHT;        break;
        case KEY_DC:        key = LE_KEY_DELETE;       break; /* Suppr        */
        case KEY_BACKSPACE: case 127: case 8:
                            key = LE_KEY_BACKSPACE;    break;
        case 0x01:          key = LE_KEY_HOME;         break; /* Ctrl-A       */
        case 0x05:          key = LE_KEY_END;          break; /* Ctrl-E       */
        case 0x15:          key = LE_KEY_KILL_LINE;    break; /* Ctrl-U       */
        case 0x17:          key = LE_KEY_KILL_WORD;    break; /* Ctrl-W       */
        case KEY_UP:        key = LE_KEY_HISTORY_PREV; break;
        case KEY_DOWN:      key = LE_KEY_HISTORY_NEXT; break;
        default:
            if (ch >= 32 && ch < 127) { key = LE_KEY_CHAR; break; }
            continue; /* touche non gérée : ignorée */
        }

        if (line_edit_feed(&input_le, key, fed_ch)) {
            input_dirty = 1;
        }
    }
}
