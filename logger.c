#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "logger.h"
#include "static_variables.h"
#include "ipc_protocol.h"

/* Taille max d'une ligne de log formatée (avant routage IPC ou affichage). */
#define LOG_LINE_MAX 4096

/* ------------------------------------------------------------------------- */
/*  Zone d'affichage fixe (événements) + journal fichier                     */
/*                                                                            */
/*  Layout :                                                                  */
/*    rangées 1 .. zone_rows - ZONE_RESERVED       → région de défilement     */
/*                                                   (output des commandes,   */
/*                                                    prompt en bas)          */
/*    rangée  zone_rows - ZONE_RESERVED + 1        → titre « Events »         */
/*    rangées zone_rows - EVENT_ZONE_LINES + 1 ..  → derniers événements      */
/*            zone_rows                                                       */
/*                                                                            */
/*  Comme la région de défilement commence à la rangée 1, les lignes qui     */
/*  sortent par le haut sont envoyées dans le scrollback natif du terminal.   */
/* ------------------------------------------------------------------------- */

#define EVENT_ZONE_LINES 6           /* nombre d'événements affichés dans la zone */
#define EVENT_MSG_MAX    200         /* taille max d'un message (avec horodatage)  */
#define EVENT_LOG_FILE   "events.log"
#define ZONE_RESERVED    (EVENT_ZONE_LINES + 1) /* +1 pour la ligne de titre        */

/* Sérialise toutes les écritures sur le terminal (logs + redessin de la zone). */
static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Buffer circulaire des derniers événements. */
static char            event_ring[EVENT_ZONE_LINES][EVENT_MSG_MAX];
static int             event_head = 0;   /* prochaine position d'écriture       */
static int             event_count = 0;  /* nombre d'événements stockés         */
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;

static int zone_active = 0;  /* 1 si la zone fixe ANSI est installée            */
static int zone_rows = 0;    /* hauteur du terminal au dernier réglage          */

static void redraw_event_zone_locked(void); /* appelant détient output_mutex    */

/* ------------------------------------------------------------------------- */
/*  IPC : routage des logs des enfants forkés vers le parent                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Vrai si le processus courant est un enfant forké en mode client
 *        et qu'il dispose d'un socket pour parler au parent.
 *
 * Le processus parent (mode client/serveur/test) a `parent_pid == getpid()`
 * et écrit ses logs localement comme avant. Les enfants forkés ont un
 * `getpid()` différent et envoient leurs logs au parent par IPC pour qu'ils
 * apparaissent dans l'unique console (sinon plusieurs processus écriraient
 * en concurrence dans le terminal).
 */
static int log_should_route_to_parent(void)
{
    return parent_pid != 0
        && parent_pid != getpid()
        && fork_checker_socket_id > 0
        && main_addr != NULL;
}

/**
 * @brief Envoie au parent un datagramme UDP : 1 octet de type + texte.
 *        Best-effort (MSG_DONTWAIT) : si le tampon est plein le message est
 *        perdu sans bloquer le thread appelant.
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

/**
 * @brief Renvoie le nombre de lignes du terminal, ou 0 si indéterminable.
 */
static int query_terminal_rows(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        return ws.ws_row;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/*  Logs classiques (sérialisés par output_mutex)                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Affiche un message d'erreur suivi du message système correspondant à `errno`.
 */
void log_errno(const char *format, ...)
{
    char buf[LOG_LINE_MAX];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    snprintf(buf + n, sizeof buf - n, "%i : %s \n", errno, strerror(errno));

    if (log_should_route_to_parent()) {
        log_send_to_parent(IPC_MSG_LOG_ERROR, buf);
        return;
    }
    pthread_mutex_lock(&output_mutex);
    fputs(buf, stderr);
    fflush(stderr);
    pthread_mutex_unlock(&output_mutex);
}

/**
 * @brief Affiche un message d'erreur sur stderr et vide le tampon.
 */
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
    fputs(buf, stderr);
    fflush(stderr);
    pthread_mutex_unlock(&output_mutex);
}

/**
 * @brief Affiche un message informatif sur stdout.
 */
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
    fputs(buf, stdout);
    pthread_mutex_unlock(&output_mutex);
}

/**
 * @brief Affiche un message de débogage sur stdout.
 */
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
    fputs(buf, stdout);
    pthread_mutex_unlock(&output_mutex);
}

/**
 * @brief Affiche un message destiné à l'affichage interactif de la console.
 */
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
    fputs(buf, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}

/** @brief Vide le tampon de sortie standard (pour `log_console`). */
void flush_console(void)
{
    fflush(stdout);
}

/** @brief Vide le tampon de sortie standard (pour `log_debug`). */
void flush_debug(void)
{
    fflush(stdout);
}

/** @brief Vide le tampon d'erreur standard (pour `log_error` / `log_errno`). */
void flush_error(void)
{
    fflush(stderr);
}

/** @brief Vide le tampon de sortie standard (pour `log_info`). */
void flush_info(void)
{
    fflush(stdout);
}

/* ------------------------------------------------------------------------- */
/*  Événements : buffer + journal + redessin                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief Redessine la zone fixe (placée en bas de l'écran) à partir du buffer.
 *
 * Sauvegarde la position du curseur, écrit le titre puis les derniers événements
 * en positionnant chaque ligne en absolu (rangées `zone_rows - ZONE_RESERVED + 1`
 * à `zone_rows`), puis restaure le curseur. L'appelant doit détenir `output_mutex`.
 */
static void redraw_event_zone_locked(void)
{
    if (!zone_active) {
        return;
    }

    char snapshot[EVENT_ZONE_LINES][EVENT_MSG_MAX];
    int n;
    pthread_mutex_lock(&event_mutex);
    n = event_count;
    for (int i = 0; i < n; i++) {
        int idx = (event_head - n + i + EVENT_ZONE_LINES) % EVENT_ZONE_LINES;
        memcpy(snapshot[i], event_ring[idx], EVENT_MSG_MAX);
    }
    pthread_mutex_unlock(&event_mutex);

    int title_row = zone_rows - ZONE_RESERVED + 1;
    char buf[32];

    fputs("\0337", stdout);                            /* sauvegarde curseur (DECSC) */
    snprintf(buf, sizeof buf, "\033[%d;1H", title_row);
    fputs(buf, stdout);
    fputs("\033[7m\033[K Events \033[0m", stdout);     /* titre, vidéo inverse       */
    for (int i = 0; i < EVENT_ZONE_LINES; i++) {
        snprintf(buf, sizeof buf, "\033[%d;1H", title_row + 1 + i);
        fputs(buf, stdout);
        fputs("\033[K", stdout);                       /* efface la ligne            */
        if (i < n) {
            fputs(snapshot[i], stdout);
        }
    }
    fputs("\0338", stdout);                            /* restaure curseur (DECRC)   */
    fflush(stdout);
}

/**
 * @brief Bandeau de statistiques « live » : sans objet en mode ANSI.
 *
 * Le mode ANSI ne réserve pas de ligne d'état dédiée (seule la zone Events est
 * fixe). Les statistiques agrégées restent consultables via la commande `check`.
 * Cette fonction existe pour préserver l'interface commune avec la variante
 * ncurses (`logger_ncurses.c`).
 */
void log_status(const char *format, ...)
{
    (void)format;
}

void log_event(const char *format, ...)
{
    char msg[EVENT_MSG_MAX];
    char line[EVENT_MSG_MAX];

    va_list args;
    va_start(args, format);
    vsnprintf(msg, sizeof msg, format, args);
    va_end(args);

    /* On retire un éventuel saut de ligne final (affichage sur une seule ligne). */
    size_t l = strlen(msg);
    while (l > 0 && (msg[l - 1] == '\n' || msg[l - 1] == '\r')) {
        msg[--l] = '\0';
    }

    /* Si on est un enfant forké : on relaie au parent qui se chargera de
       l'horodatage, du journal fichier et de l'affichage. Évite plusieurs
       écrivains sur events.log et garde un seul affichage cohérent. */
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

    /* Stockage dans le buffer circulaire. */
    pthread_mutex_lock(&event_mutex);
    strncpy(event_ring[event_head], line, EVENT_MSG_MAX - 1);
    event_ring[event_head][EVENT_MSG_MAX - 1] = '\0';
    event_head = (event_head + 1) % EVENT_ZONE_LINES;
    if (event_count < EVENT_ZONE_LINES) {
        event_count++;
    }
    pthread_mutex_unlock(&event_mutex);

    /* Journal fichier (date complète). */
    char fts[24];
    strftime(fts, sizeof fts, "%Y-%m-%d %H:%M:%S", &tmv);
    FILE *f = fopen(EVENT_LOG_FILE, "a");
    if (f != NULL) {
        fprintf(f, "[%s] %s\n", fts, msg);
        fclose(f);
    }

    pthread_mutex_lock(&output_mutex);
    if (zone_active) {
        redraw_event_zone_locked();
    } else {
        /* Pas de zone fixe (sortie non interactive) : impression classique. */
        fprintf(stdout, "%s\n", line);
        fflush(stdout);
    }
    pthread_mutex_unlock(&output_mutex);
}

/* ------------------------------------------------------------------------- */
/*  Gestion de la zone fixe et de la région de défilement                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief Thread de rafraîchissement de la zone fixe (heartbeat + redimensionnement).
 *
 * Toutes les secondes : si la taille du terminal a changé, réajuste la région
 * de défilement ; redessine ensuite la zone des événements.
 */
static void *event_zone_loop(void *arg)
{
    (void)arg;
    while (zone_active) {
        int rows = query_terminal_rows();
        pthread_mutex_lock(&output_mutex);
        if (zone_active) {
            if (rows > ZONE_RESERVED + 1 && rows != zone_rows) {
                zone_rows = rows;
                int region_bottom = zone_rows - ZONE_RESERVED;
                char buf[32];
                snprintf(buf, sizeof buf, "\033[1;%dr", region_bottom);
                fputs(buf, stdout);              /* nouvelle région de défilement */
                snprintf(buf, sizeof buf, "\033[%d;1H", region_bottom);
                fputs(buf, stdout);              /* curseur en bas de la région   */
                fflush(stdout);
            }
            redraw_event_zone_locked();
        }
        pthread_mutex_unlock(&output_mutex);
        sleep(1);
    }
    return NULL;
}

void status_zone_init(void)
{
    if (!isatty(STDOUT_FILENO)) {
        return;     /* sortie redirigée : on garde l'affichage classique */
    }
    int rows = query_terminal_rows();
    if (rows <= ZONE_RESERVED + 1) {
        return;     /* terminal trop petit pour réserver la zone */
    }

    pthread_mutex_lock(&output_mutex);
    zone_rows = rows;
    int region_bottom = zone_rows - ZONE_RESERVED;
    char buf[32];
    fputs("\033[2J", stdout);                          /* efface tout l'écran          */
    snprintf(buf, sizeof buf, "\033[1;%dr", region_bottom);
    fputs(buf, stdout);                                /* région de défilement (haut)  */
    snprintf(buf, sizeof buf, "\033[%d;1H", region_bottom);
    fputs(buf, stdout);                                /* curseur en bas de la région  */
    zone_active = 1;
    redraw_event_zone_locked();
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);

    atexit(status_zone_teardown);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, event_zone_loop, NULL) != 0) {
        log_error("pthread_create() failed (event_zone)\n");
    }
    pthread_attr_destroy(&attr);
}

void status_zone_teardown(void)
{
    if (!zone_active) {
        return;
    }
    pthread_mutex_lock(&output_mutex);
    zone_active = 0;
    fputs("\033[r", stdout);                           /* région de défilement = écran complet */
    char buf[16];
    snprintf(buf, sizeof buf, "\033[%d;1H", zone_rows > 0 ? zone_rows : 1);
    fputs(buf, stdout);                                /* curseur en bas               */
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}

/**
 * @brief Efface la zone interactive et y replace le curseur (réaffichage en place).
 *
 * Quand la zone fixe est active, n'efface que la région de défilement (au-dessus
 * de la zone des événements) et redessine la zone (elle est inchangée mais on la
 * réécrit pour la cohérence). Sinon, efface tout l'écran. Sans effet hors terminal.
 */
void clear_console(void)
{
    if (!isatty(STDOUT_FILENO)) {
        return;
    }
    pthread_mutex_lock(&output_mutex);
    if (zone_active) {
        int region_bottom = zone_rows - ZONE_RESERVED;
        char buf[16];
        /* Efface ligne par ligne la région de défilement (sans toucher la zone). */
        for (int r = 1; r <= region_bottom; r++) {
            snprintf(buf, sizeof buf, "\033[%d;1H\033[K", r);
            fputs(buf, stdout);
        }
        /* Curseur en haut de la région ; la sortie qui suivra remplira de haut en bas. */
        fputs("\033[1;1H", stdout);
    } else {
        fputs("\033[2J\033[H", stdout);
    }
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}
