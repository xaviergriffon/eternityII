#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ui/logger.h"
#include "app/static_variables.h"
#include "net/ipc_protocol.h"

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

/* Ligne de saisie interactive (prompt + saisie en cours), protégée par
   output_mutex. Tant que input_active vaut 1, les écritures de log terminées
   par '\n' effacent la ligne, écrivent le log puis la redessinent en dessous. */
#define INPUT_SNAPSHOT_MAX 1200
static int  input_active = 0;
static char input_snapshot[INPUT_SNAPSHOT_MAX];
/* Colonne terminal (1-based) où repositionner le curseur après un redessin de
   la ligne de saisie — prompt inclus. Calculée par console_input_render à
   partir de sa position dans la ligne éditée (0..strlen(line)). */
static int  input_cursor_col = 1;

/** @brief Repositionne le curseur terminal en colonne input_cursor_col
 *         (colonne absolue \033[<n>G, indépendante de la longueur déjà
 *         écrite). Appelant sous output_mutex, input_active vrai. */
static void reposition_input_cursor_locked(void)
{
    char buf[24];
    snprintf(buf, sizeof buf, "\033[%dG", input_cursor_col);
    fputs(buf, stdout);
}

static void redraw_event_zone_locked(void); /* appelant détient output_mutex    */

/* Pagination de la sortie des commandes console (voir console_pager_begin,
   logger.h). L'état n'est modifié que par le thread console (propriétaire) et
   lu sous output_mutex par les écrivains. */
static int       pager_engaged = 0;  /* 1 entre pager_begin et pager_end        */
static pthread_t pager_owner;        /* seul ce thread est paginé               */
static int       pager_page = 0;     /* lignes par page                         */
static int       pager_budget = 0;   /* lignes restantes avant la pause         */
static int       pager_snooze = 0;   /* 1 : « q » — dérouler le reste sans pause */

/**
 * @brief Écrit un bloc de log sur `stream` en préservant la ligne de saisie.
 *
 * Si une saisie interactive est active : efface la ligne (`\r\033[K`), écrit le
 * bloc, puis redessine la ligne de saisie — uniquement si le bloc se termine
 * par un saut de ligne (un bloc partiel sera complété par les écritures
 * suivantes, la ligne de saisie reviendra à ce moment-là). Les flush croisés
 * stdout/stderr garantissent l'ordre d'affichage quand les deux flux pointent
 * vers le même terminal. L'appelant doit détenir `output_mutex`.
 */
static void write_stream_locked(FILE *stream, const char *buf)
{
    size_t len = strlen(buf);
    if (input_active) {
        fputs("\r\033[K", stdout);
        if (stream != stdout) fflush(stdout);
    }
    fputs(buf, stream);
    if (input_active && len > 0 && buf[len - 1] == '\n') {
        if (stream != stdout) fflush(stream);
        fputs(input_snapshot, stdout);
        reposition_input_cursor_locked();
        fflush(stdout);
    }
}

/**
 * @brief Pause de pagination : affiche « --Suite-- », attend une touche.
 *
 * Le verrou `output_mutex` (détenu à l'appel) est RELÂCHÉ pendant l'attente de
 * la touche : les logs des autres threads continuent de s'afficher, seule la
 * commande paginée est suspendue. Espace (ou toute autre touche) : page
 * suivante ; entrée : une ligne ; q ou fin d'entrée : dérouler le reste sans
 * pause (rien n'est supprimé). Rend la main avec le verrou repris.
 */
static void pager_wait_locked(void)
{
    fputs("\033[7m--Suite-- (espace : page, entrée : ligne, q : dérouler)\033[0m", stdout);
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
    int c = fgetc(stdin);
    pthread_mutex_lock(&output_mutex);
    fputs("\r\033[K", stdout);
    fflush(stdout);
    if (c == 'q' || c == 'Q' || c == EOF) {
        pager_snooze = 1;
    } else if (c == '\n' || c == '\r') {
        pager_budget = 1;
    } else {
        pager_budget = pager_page;
    }
}

/**
 * @brief Écrit `buf` ligne par ligne en marquant une pause à chaque page pleine.
 *        Appelant sous `output_mutex` (relâché/repris pendant les pauses).
 */
static void write_paged_locked(FILE *stream, const char *buf)
{
    const char *p = buf;
    while (*p != '\0') {
        if (pager_snooze) {
            fputs(p, stream);   /* « q » : le reste défile sans pause         */
            return;
        }
        const char *nl = strchr(p, '\n');
        size_t chunk = (nl != NULL) ? (size_t)(nl - p + 1) : strlen(p);
        fwrite(p, 1, chunk, stream);
        p += chunk;
        if (nl != NULL && --pager_budget <= 0) {
            if (stream != stdout) fflush(stream);
            pager_wait_locked();
        }
    }
}

/**
 * @brief Point d'entrée commun des écritures de log : prend le verrou et route
 *        vers l'écriture paginée (thread console pendant une commande) ou
 *        l'écriture directe protégeant la ligne de saisie.
 */
static void write_output(FILE *stream, const char *buf, int do_flush)
{
    pthread_mutex_lock(&output_mutex);
    if (pager_engaged && pthread_equal(pager_owner, pthread_self())) {
        write_paged_locked(stream, buf);
    } else {
        write_stream_locked(stream, buf);
    }
    if (do_flush) {
        fflush(stream);
    }
    pthread_mutex_unlock(&output_mutex);
}

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
    write_output(stderr, buf, 1);
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
    write_output(stderr, buf, 1);
}

/**
 * @brief Journalise un message d'erreur fatal puis termine le process.
 *
 * Délègue le rendu à log_error (qui gère le routage vers le parent et le flush),
 * en passant le message déjà formaté via "%s" pour qu'un éventuel '%' littéral
 * ne soit pas réinterprété, puis exit(EXIT_FAILURE).
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
    write_output(stdout, buf, 0);
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
    write_output(stdout, buf, 0);
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
    write_output(stdout, buf, 1);
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

    /* Stockage dans le buffer circulaire. line est déjà NUL-terminé par
       snprintf et fait EVENT_MSG_MAX octets : un memcpy du buffer complet copie
       le terminateur (évite -Wstringop-truncation sur le strncpy précédent). */
    pthread_mutex_lock(&event_mutex);
    memcpy(event_ring[event_head], line, EVENT_MSG_MAX);
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
        /* Pas de zone fixe (sortie non interactive) : impression classique,
           via le helper pour préserver une éventuelle ligne de saisie. */
        char out[EVENT_MSG_MAX + 2];
        snprintf(out, sizeof out, "%s\n", line);
        write_stream_locked(stdout, out);
        fflush(stdout);
    }
    pthread_mutex_unlock(&output_mutex);
}

/* ------------------------------------------------------------------------- */
/*  Ligne de saisie interactive (voir logger.h)                              */
/* ------------------------------------------------------------------------- */

void console_input_render(const char *prompt, const char *line, int cursor)
{
    pthread_mutex_lock(&output_mutex);
    size_t plen = prompt != NULL ? strlen(prompt) : 0;
    snprintf(input_snapshot, sizeof input_snapshot, "%s%s",
             prompt != NULL ? prompt : "", line != NULL ? line : "");
    if (cursor < 0) cursor = 0;
    /* Colonne 1-based = 1 + (nb de caractères avant le curseur). Borne à la
       longueur réellement écrite (snprintf peut avoir tronqué). */
    size_t written = strlen(input_snapshot);
    size_t before_cursor = plen + (size_t)cursor;
    if (before_cursor > written) before_cursor = written;
    input_cursor_col = (int)before_cursor + 1;
    input_active = 1;
    fputs("\r\033[K", stdout);
    fputs(input_snapshot, stdout);
    reposition_input_cursor_locked();
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}

void console_input_end(void)
{
    pthread_mutex_lock(&output_mutex);
    if (input_active) {
        input_active = 0;
        fputs("\n", stdout);
        fflush(stdout);
    }
    pthread_mutex_unlock(&output_mutex);
}

void console_pager_begin(void)
{
    /* La pause lit une touche sur stdin et l'affichage cible un écran : les
       deux doivent être des terminaux (sinon : sortie redirigée, tests,
       console pilotée par pipe — aucune pagination). */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return;
    }
    int rows = query_terminal_rows();
    /* Hauteur utile : la région de défilement si la zone fixe est installée,
       tout l'écran sinon ; moins 1 ligne pour l'invite « --Suite-- ». */
    int page = (zone_active ? zone_rows - ZONE_RESERVED : rows) - 1;
    if (page < 3) {
        return;         /* écran trop petit : pagination plus gênante qu'utile */
    }
    pthread_mutex_lock(&output_mutex);
    pager_engaged = 1;
    pager_owner   = pthread_self();
    pager_page    = page;
    pager_budget  = page;
    pager_snooze  = 0;
    pthread_mutex_unlock(&output_mutex);
}

void console_pager_end(void)
{
    pthread_mutex_lock(&output_mutex);
    pager_engaged = 0;
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
 * @brief Efface la zone interactive en préservant le contenu dans le scrollback.
 *
 * Quand la zone fixe est active, le contenu de la région de défilement n'est pas
 * détruit : depuis le bas de la région, chaque saut de ligne fait défiler la
 * région d'une ligne vers le haut, et les lignes sorties par le haut partent
 * dans le scrollback natif du terminal (molette / Cmd+↑) — même mécanisme que le
 * défilement normal. Le curseur revient ensuite en haut de la région. Sans zone
 * fixe, efface tout l'écran. Sans effet hors terminal.
 */
void clear_console(void)
{
    if (!isatty(STDOUT_FILENO)) {
        return;
    }
    pthread_mutex_lock(&output_mutex);
    if (zone_active) {
        int region_bottom = zone_rows - ZONE_RESERVED;
        /* Assez large pour "\033[<int>;1H" quel que soit le nombre de chiffres
           (un int → 11 chiffres max) : évite -Wformat-truncation. */
        char buf[32];
        snprintf(buf, sizeof buf, "\033[%d;1H", region_bottom);
        fputs(buf, stdout);                /* curseur en bas de la région        */
        for (int r = 0; r < region_bottom; r++) {
            fputc('\n', stdout);           /* pousse une ligne dans le scrollback */
        }
        /* Curseur en haut de la région ; la sortie qui suivra remplira de haut en bas. */
        fputs("\033[1;1H", stdout);
    } else {
        fputs("\033[2J\033[H", stdout);
    }
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}
