#include "ui/console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "ui/logger.h"
#include "ui/command_lines.h"
#include "ui/command_history.h"
#include "ui/line_edit.h"
#include "app/fork_gate.h"
#include "app/fork_orchestrator.h"

#define EXIT_CMD "exit"

/* Persistance de l'historique des commandes entre sessions.
   history_persist_atexit() est enregistré via atexit() au démarrage de la
   console : il couvre la sortie propre par la commande `exit` (qui appelle
   exit()) dans les deux builds. Le chemin est mémorisé au chargement pour être
   réutilisé à la sauvegarde. */
static char history_path_buf[1200];
static int  history_path_ready = 0;

static void history_persist_atexit(void)
{
    if (history_path_ready) {
        history_save(history_path_buf);
    }
}

/** @brief Charge l'historique persistant et arme la sauvegarde à la sortie. */
static void history_persist_init(void)
{
    if (history_default_path(history_path_buf, sizeof history_path_buf) != NULL) {
        history_path_ready = 1;
        history_load(history_path_buf);
        atexit(history_persist_atexit);
    }
}

#ifndef USE_NCURSES
#include <termios.h>

/* ------------------------------------------------------------------------- */
/*  Mode raw + édition de ligne avec historique (flèches ↑ / ↓)              */
/* ------------------------------------------------------------------------- */

/* Sauvegarde des attributs terminal pour restauration via atexit. */
static struct termios saved_termios;
static int termios_saved = 0;

static void restore_termios_on_exit(void)
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        termios_saved = 0;
    }
}

/**
 * @brief Bascule stdin en mode non-canonique sans écho. Indispensable pour
 *        intercepter les flèches ↑/↓ (séquences \033[A et \033[B).
 *
 * On désactive `ICANON` (lecture char-par-char) et `ECHO` (on écho nous-mêmes
 * pour pouvoir filtrer les séquences d'échappement), mais on conserve `ISIG`
 * (Ctrl-C envoie toujours SIGINT). On force aussi explicitement `OPOST` et
 * `ONLCR` côté sortie : sur certains terminaux, ces bits ne sont pas garantis
 * sur le `c_oflag` retourné par `tcgetattr`, et sans eux les `\n` ne sont plus
 * traduits en `\r\n` → tous les `log_info`/`log_console` produisent un effet
 * « escalier » dans la zone de sortie. La version sauvegardée du termios reste
 * intacte et sera restaurée à l'identique par `restore_termios_on_exit`.
 *
 * @return 0 en cas de succès, -1 si stdin n'est pas un TTY ou si tcsetattr échoue.
 */
static int try_enable_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &saved_termios) == -1) return -1;

    struct termios raw = saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_oflag |= (OPOST | ONLCR);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) return -1;

    termios_saved = 1;
    atexit(restore_termios_on_exit);
    return 0;
}

/**
 * @brief Implémentation historique : lecture ligne-par-ligne en mode cooked
 *        (utilisée comme fallback si stdin n'est pas un TTY, par exemple si
 *         l'entrée est redirigée depuis un fichier).
 */
static char *getcmdline_cooked(void)
{
    char *line = malloc(100), *linep = line;
    size_t lenmax = 100, len = lenmax;
    int c;

    if (line == NULL) return NULL;

    for (;;) {
        c = fgetc(stdin);
        if (c == EOF) {
            if (line == linep) {
                /* EOF sans aucun caractère : stdin est épuisé/fermé (ex.
                   redirection depuis /dev/null, pipe refermé). On renvoie NULL
                   pour signaler la FIN d'entrée — distinct d'une ligne vide
                   ("") — afin que la console cesse de lire au lieu de boucler
                   à pleine vitesse sur un prompt « commande : ». */
                free(linep);
                return NULL;
            }
            break; /* EOF en fin de ligne non terminée : on rend la ligne lue */
        }
        if (c == '\n') break;

        if (--len == 0) {
            len = lenmax;
            /* On capture l'offset avant le realloc : linep peut être libéré, le
               réutiliser ensuite dans l'arithmétique déclenche -Wuse-after-free. */
            size_t offset = line - linep;
            char *linen = realloc(linep, lenmax *= 2);
            if (linen == NULL) { free(linep); return NULL; }
            line = linen + offset;
            linep = linen;
        }
        *line++ = (char)c;
    }
    *line = '\0';
    return linep;
}

/* Prompt de la console interactive (partagé entre le rendu initial et les
   redessins de ligne via console_input_render). */
#define CONSOLE_PROMPT "commande :"

/**
 * @brief Lecture en mode raw : on lit caractère par caractère et on délègue
 *        toute la logique d'édition (curseur, backspace, historique, Ctrl-A/
 *        E/U/W, ←/→, Home/End) au module commun ui/line_edit.c — partagé
 *        avec la variante ncurses (nc_console_loop, logger_ncurses.c).
 *
 * La ligne de saisie n'est plus échoée caractère par caractère : chaque touche
 * traitée repasse par console_input_render (logger.c), qui publie la ligne et
 * la position du curseur auprès du logger. Les logs asynchrones (thread de
 * statistiques, événements relayés des forks) peuvent ainsi effacer la ligne,
 * s'écrire, puis la redessiner en dessous — la saisie en cours n'est plus
 * corrompue.
 */
static char *getcmdline_raw(void)
{
    line_edit_t le;
    line_edit_reset(&le);
    console_input_render(CONSOLE_PROMPT, line_edit_text(&le), line_edit_cursor(&le));

    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            /* Fin d'entrée (Ctrl-D sur un TTY, ou stdin refermé) : on renvoie
               NULL pour que la console s'arrête proprement, au lieu de boucler
               sur une saisie vide. La protection de la ligne est levée. */
            console_input_end();
            return NULL;
        }
        // Toute frappe au clavier annule un décompte d'auto-démarrage en
        // cours (état COUNTDOWN de l'orchestrateur) : 5 s ne suffisent pas à
        // taper une commande complète, donc on ne peut pas attendre qu'une
        // ligne `config <clé> <valeur>` soit validée en entier pour annuler —
        // le simple fait de commencer à saisir (même juste "start") doit
        // déjà donner un temps illimité. No-op hors COUNTDOWN (self-loop).
        // Snapshot AVANT l'envoi de l'événement : un seul log au moment où le
        // décompte bascule réellement (COUNTDOWN -> CONFIGURING, une seule
        // fois), pas à chaque frappe suivante — sans quoi cette annulation
        // restait totalement invisible dans les logs (l'opérateur ne pouvait
        // que la déduire de l'absence des lignes "auto-démarrage dans …s").
        orch_state_t state_before_key;
        fork_orchestrator_snapshot(&state_before_key, NULL);
        if (state_before_key == ORCH_COUNTDOWN) {
            log_info("orchestrateur : décompte d'auto-démarrage annulé par une saisie clavier\n");
        }
        fork_orchestrator_post_event(EV_CONFIG_BEGUN, NULL);

        if (c == '\n' || c == '\r') {
            console_input_end();        /* écho de Entrée + fin de protection */
            history_add(line_edit_text(&le));
            int len = le.len;
            char *r = malloc(len + 1);
            if (r) memcpy(r, le.buf, len + 1);
            return r;
        }

        /* Ctrl-L : efface l'écran (le contenu part dans le scrollback natif)
           puis redessine la ligne de saisie en cours. */
        if (c == 0x0c) {
            clear_console();
            console_input_render(CONSOLE_PROMPT, line_edit_text(&le), line_edit_cursor(&le));
            continue;
        }

        line_edit_key key;
        int ch = c;
        switch (c) {
        case 0x7f: case 0x08: key = LE_KEY_BACKSPACE; break;  /* DEL / ^H     */
        case 0x01: key = LE_KEY_HOME;      break;             /* Ctrl-A       */
        case 0x05: key = LE_KEY_END;       break;             /* Ctrl-E       */
        case 0x15: key = LE_KEY_KILL_LINE; break;             /* Ctrl-U       */
        case 0x17: key = LE_KEY_KILL_WORD; break;             /* Ctrl-W       */
        case 0x1b: {                                          /* ESC [ ...    */
            int c2 = fgetc(stdin);
            if (c2 != '[') continue;
            int c3 = fgetc(stdin);
            if (c3 == 'A') { key = LE_KEY_HISTORY_PREV; break; }
            if (c3 == 'B') { key = LE_KEY_HISTORY_NEXT; break; }
            if (c3 == 'C') { key = LE_KEY_RIGHT; break; }
            if (c3 == 'D') { key = LE_KEY_LEFT;  break; }
            if (c3 == 'H') { key = LE_KEY_HOME;  break; }
            if (c3 == 'F') { key = LE_KEY_END;   break; }
            if (c3 == '3') {                    /* Delete : ESC [ 3 ~        */
                int c4 = fgetc(stdin);
                if (c4 != '~') continue;
                key = LE_KEY_DELETE;
                break;
            }
            /* Autres séquences (F1…) : ignorées pour l'instant. */
            continue;
        }
        default:
            if (c >= 32 && c < 127) { key = LE_KEY_CHAR; break; }
            continue; /* autre caractère de contrôle : ignoré */
        }

        if (line_edit_feed(&le, key, ch)) {
            console_input_render(CONSOLE_PROMPT, line_edit_text(&le), line_edit_cursor(&le));
        }
    }
}

/**
 * @brief Lit une commande depuis stdin avec gestion des flèches ↑/↓ pour
 *        l'historique (en mode raw si possible, sinon fallback ligne-par-ligne).
 *
 * Le prompt est géré ici : le chemin raw affiche et met à jour la ligne de
 * saisie via console_input_render ; le fallback cooked garde l'affichage
 * historique (prompt avant lecture, saut de ligne après).
 *
 * @return Chaîne malloc'ée (à libérer par l'appelant), ou NULL sur fin
 *         d'entrée / erreur d'alloc.
 */
static int raw_attempted = 0;
static int raw_ok = 0;

static char *getcmdline(void)
{
    if (!raw_attempted) {
        raw_attempted = 1;
        raw_ok = (try_enable_raw_mode() == 0);
    }
    if (raw_ok) {
        return getcmdline_raw();
    }
    log_console(CONSOLE_PROMPT);
    char *line = getcmdline_cooked();
    log_console("\n");
    return line;
}
#endif /* !USE_NCURSES */

/**
 * @brief Thread de la console interactive.
 *
 * En mode ANSI (par défaut), boucle infinie : affiche le prompt, lit une
 * commande via `getcmdline`, la délègue à `do_command_line`. En mode ncurses
 * (`USE_NCURSES`), délègue à `nc_console_loop` qui gère sa propre boucle de
 * saisie via ncurses.
 *
 * @param param Non utilisé.
 * @return      Ne retourne pas (exit ou boucle infinie).
 */
void * console(void *param)
{
    (void)param;
    status_zone_init();
    history_persist_init();
#ifdef USE_NCURSES
    nc_console_loop();
    exit(EXIT_SUCCESS);
#else
    // Enregistrement auprès du gate de quiescence coopérative : la console
    // est le cas particulier, bloquée dans read() (getcmdline). fork_gate_mark_blocked
    // la déclare quiescente pour la durée du read (elle n'y détient aucun
    // verrou) sans avoir besoin de se garer sur la condvar ; au retour, si une
    // quiescence a été demandée entre-temps, fork_gate_checkpoint la gare
    // pour de bon avant tout traitement.
    int gate_slot = fork_gate_register("console");
    for (;;)
    {
        fork_gate_mark_blocked(gate_slot, 1);
        char *buffer = getcmdline();
        fork_gate_mark_blocked(gate_slot, 0);
        fork_gate_checkpoint(gate_slot);
        if (buffer == NULL)
        {
            /* Fin de l'entrée standard (stdin épuisé/fermé : /dev/null, pipe,
               Ctrl-D) ou échec d'allocation. On NE doit PAS reboucler en
               affichant « commande : » à pleine vitesse, ni terminer le process
               (le serveur/client doit continuer à tourner sans console). On sort
               donc de la boucle de saisie et le thread console s'arrête. */
            break;
        }
        /* Pagination de la sortie de la commande (mode raw seulement : la
           pause « --Suite-- » lit une touche caractère par caractère). Les
           logs des autres threads ne sont ni paginés ni retenus. */
        if (raw_ok) {
            console_pager_begin();
        }
        do_command_line(buffer);
        if (raw_ok) {
            console_pager_end();
        }
        free(buffer);
    }
    fork_gate_unregister(gate_slot);
    /* Sortie propre par fin de stdin (Ctrl-D, pipe fermé) : le thread s'arrête
       sans passer par exit(), on persiste donc l'historique explicitement. */
    if (history_path_ready) {
        history_save(history_path_buf);
    }
    log_info("console : fin de l'entrée standard — console interactive arrêtée (le traitement continue)\n");
    return NULL;
#endif /* USE_NCURSES */
}

/**
 * @brief Démarre le thread de console interactive en mode détaché.
 * @param server 1 si mode serveur, 0 si mode client (passé en paramètre au thread, non utilisé actuellement).
 */
void run_console(int server)
{
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    /* Création du thread */
    
    int *param = malloc(sizeof(int));
    *param = server;
    if(0 != pthread_create(&thread, NULL, console, param))
    {
        // Non fatal : sous forte pression de ressources, on poursuit sans
        // console interactive plutôt que de planter l'application.
        log_error("run_console : pthread_create a échoué — console interactive indisponible\n");
        free(thread_attributes);
        free(param);
        return;
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}
