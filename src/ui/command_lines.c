#include "ui/command_lines.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>

#include "ui/logger.h"
#include "core/datamanager.h"
#include "net/local_socket.h"
#include "core/readdata.h"
#include "ui/command_match.h"
#include "app/static_variables.h"
#include "app/control_registry.h"
#include "net/control_protocol.h"
#include "core/best_board.h"

#define DEF_FILE "./eternityII.back"
#define DEF_ANALYSE_FILE "./eternityII-in_analyse.back"
#define DEF_BEST_BOARD_FILE "./eternityII-best_board.back"
#define NB_COMMANDS 44
/// Taille du tampon de construction des textes d'aide (aide générale comprise).
#define HELP_BUFFER_SIZE 16384

/**
 * @brief Catégories de commandes, pour l'aide (`help`, `help <catégorie>`).
 */
typedef enum {
    CMD_CAT_GENERAL = 0,
    CMD_CAT_SEARCH,
    CMD_CAT_STOCK,
    CMD_CAT_BACKUP,
    CMD_CAT_DIAG,
    CMD_CAT_CLIENTS,
    CMD_CAT_COUNT
} command_category;

/// Libellés affichés comme titres de section dans l'aide.
static const char *category_labels[CMD_CAT_COUNT] = {
    "Général",
    "Recherche & régulation",
    "Stock & files",
    "Sauvegarde & restauration",
    "Diagnostic & vérification",
    "Pilotage des clients (serveur)"
};

/// Mots-clés acceptés par `help <catégorie>` (comparés sans tenir compte de la casse).
static const char *category_keywords[CMD_CAT_COUNT] = {
    "general",
    "recherche",
    "stock",
    "sauvegarde",
    "diagnostic",
    "clients"
};

/**
 * @brief Définition d'une commande prise en charge
 */
typedef struct
{
    /// nom de l'instruction
    char *command;
    /// fonction à exectuer pour la commande (NULL pour un alias)
    int (*interpreter)(void);
    ///indique si la commande doit être transmise aux threads fils
    int8_t send_to_childs;
    /// catégorie d'aide (command_category)
    uint8_t category;
    /// 1 si la commande n'a d'effet utile que côté serveur
    uint8_t server_only;
    /// syntaxe avec arguments (NULL : la commande n'en prend pas)
    const char *usage;
    /// description en une ligne, affichée dans l'aide générale
    const char *summary;
    /// complément affiché par `help <commande>` (NULL : rien de plus)
    const char *details;
    /// nom canonique si cette entrée est un alias (NULL : entrée canonique)
    const char *alias_of;
} command_description;

int sort_ascending_interpreter(void);
int sort_descending_interpreter(void);
int max_stock_by_thread_interpreter(void);
int pruner_batch_interpreter(void);
int limit_interpreter(void);
int exit_interpreter(void);
int check_interpreter(void);
int backup_interpreter(void);
int restore_interpreter(void);
int import_interpreter(void);
int loadjson_interpreter(void);
int print_interpreter(void);
int sortdm_interpreter(void);
int split_interpreter(void);
int regroup_interpreter(void);
int checkdatas_interpreter(void);
int check_duplicate_interpreter(void);
int checkfiles_interpreter(void);
int printfile_interpreter(void);
int checkfile_interpreter(void);
int checkdirections_interpreter(void);
int rmnonext_interpreter(void);
int expand_interpreter(void);
int printanalysed_interpreter(void);
int restockanalysed_interpreter(void);
int min_interpreter(void);
int help_interpreter(void);
int clear_interpreter(void);
int statistic_interpreter(void);
int pause_interpreter(void);
int resume_interpreter(void);
int clients_interpreter(void);
int clients_stats_interpreter(void);
int clients_cmd_interpreter(void);

/**
 * @brief Commandes prises en charge (entrées canoniques puis alias).
 *
 * Source unique de vérité de l'aide interactive : catégorie, usage, résumé et
 * complément sont lus par `help`/`help <sujet>` et par le rappel d'usage
 * automatique de `do_command_line` (retour CMD_ERR_USAGE). docs/console.md
 * reprend le même contenu — tenir les deux en cohérence.
 */
static command_description commands[NB_COMMANDS] = {
    {"help", help_interpreter, 0, CMD_CAT_GENERAL, 0, "help [commande|catégorie]",
     "affiche l'aide (générale, d'une commande ou d'une catégorie)", NULL, NULL},
    {"exit", exit_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "arrête proprement le programme",
     "En mode client, attend la fin de tous les processus de recherche avant de quitter.", NULL},
    {"clear", clear_interpreter, 0, CMD_CAT_GENERAL, 0, NULL,
     "efface l'écran de la console (raccourci : Ctrl-L)",
     "Aucune autre commande n'efface l'écran : l'affichage ne défile que par les sorties.\n"
     "Le contenu n'est pas perdu : en mode ANSI il part dans le scrollback natif du\n"
     "terminal (molette / Cmd+↑), en mode ncurses il reste accessible via PgUp.", NULL},

    {"pause", pause_interpreter, 1, CMD_CAT_SEARCH, 0, NULL,
     "met la recherche en pause administrative (locale + clients connectés)",
     "Ne se lève que par « resume » (distincte de la pause de régulation posée par « limit »).\n"
     "Côté serveur : diffusée à tous les clients connectés et mémorisée pour ceux qui se\n"
     "connecteront ensuite.", NULL},
    {"resume", resume_interpreter, 1, CMD_CAT_SEARCH, 0, NULL,
     "lève la pause administrative",
     "Ne lève que la pause posée par « pause » ; la pause de régulation de débit reste\n"
     "gérée automatiquement par le régulateur.", NULL},
    {"limit", limit_interpreter, 1, CMD_CAT_SEARCH, 0, "limit <n>",
     "borne le débit de recherche à <n> coups/s (0 = illimité)", NULL, NULL},
    {"maxStockByThread", max_stock_by_thread_interpreter, 1, CMD_CAT_SEARCH, 0, "maxStockByThread <n>",
     "fixe le stock maximum de possibilités par thread", NULL, NULL},
    {"prunerBatch", pruner_batch_interpreter, 1, CMD_CAT_SEARCH, 0, "prunerBatch <n>",
     "fixe la taille de lot d'échange du pruner",
     "Bornée à [1, PRUNER_BATCH_MAX] pour maîtriser la mémoire du pruner et les tampons GPU.", NULL},

    {"sortAsc", sort_ascending_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie le stock par ordre croissant (moins avancées d'abord)", NULL, NULL},
    {"sortDesc", sort_descending_interpreter, 0, CMD_CAT_STOCK, 0, "sortDesc [n]",
     "trie par ordre décroissant, toutes les files ou la file <n>", NULL, NULL},
    {"sortDescMulti", sortdm_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "trie toutes les files en parallèle (multi-thread)", NULL, NULL},
    {"split", split_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "répartit les possibilités entre les différentes files", NULL, NULL},
    {"regroup", regroup_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "regroupe toutes les possibilités dans une seule file", NULL, NULL},
    {"removeNoNext", rmnonext_interpreter, 1, CMD_CAT_STOCK, 0, NULL,
     "supprime les possibilités sans continuation possible (élagage)", NULL, NULL},
    {"expand", expand_interpreter, 0, CMD_CAT_STOCK, 0, "expand <niveau>",
     "développe le stock jusqu'au niveau de curseur <niveau> (anti-famine)",
     "Place une pièce candidate sur la case suivante de chaque possibilité, jusqu'au niveau\n"
     "demandé — utile côté serveur quand le stock distribuable s'est raréfié. Borné en\n"
     "profondeur (EXPAND_MAX_LEVELS) et en volume (EXPAND_MAX_STOCK) ; niveau 3-4 recommandé.", NULL},
    {"restockAnalysed", restockanalysed_interpreter, 0, CMD_CAT_STOCK, 0, NULL,
     "remet les possibilités en cours d'analyse dans le stock", NULL, NULL},
    {"min", min_interpreter, 1, CMD_CAT_STOCK, 0, NULL,
     "affiche le niveau minimal de pièces placées dans les files", NULL, NULL},

    {"backup", backup_interpreter, 1, CMD_CAT_BACKUP, 0, NULL,
     "sauvegarde les files dans les fichiers .back",
     "Écrit ./eternityII.back, ./eternityII-in_analyse.back et ./eternityII-best_board.back\n"
     "(noms suffixés du pid côté client).", NULL},
    {"restore", restore_interpreter, 1, CMD_CAT_BACKUP, 0, "restore [fichier [fichier_analyse]]",
     "restaure les files depuis les fichiers .back (remplace le stock)",
     "La recherche est suspendue pendant le remplacement. Sans argument :\n"
     "./eternityII.back et ./eternityII-in_analyse.back.", NULL},
    {"import", import_interpreter, 0, CMD_CAT_BACKUP, 0, NULL,
     "importe les fichiers .back en plus du stock courant",
     "Contrairement à « restore », le stock courant n'est pas vidé.", NULL},
    {"loadJson", loadjson_interpreter, 0, CMD_CAT_BACKUP, 0, NULL,
     "importe une possibilité depuis une chaîne JSON", NULL, NULL},

    {"check", check_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "affiche le dernier rapport de statistiques", NULL, NULL},
    {"print", print_interpreter, 0, CMD_CAT_DIAG, 0, "print [fichier]",
     "affiche l'état du data manager (files, tailles)",
     "Avec [fichier] : exporte le dump JSON complet dans ce fichier au lieu de la\n"
     "console (évite qu'un gros stock ne déborde le pad de sortie en ncurses).", NULL},
    {"printFile", printfile_interpreter, 0, CMD_CAT_DIAG, 0, "printFile <n> [fichier]",
     "affiche le contenu de la file numéro <n>",
     "Avec [fichier] : exporte cette file au format JSON dans ce fichier au lieu de\n"
     "la console.", NULL},
    {"printAnalysed", printanalysed_interpreter, 1, CMD_CAT_DIAG, 0, "printAnalysed [fichier]",
     "affiche les possibilités en cours d'analyse",
     "Avec [fichier] : exporte le dump JSON complet dans ce fichier au lieu de la\n"
     "console. Propagée aux process fils : en mode client, le nom est suffixé du\n"
     "pid (comme « backup ») pour que chaque process écrive dans son propre fichier.", NULL},
    {"statistic", statistic_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "affiche des statistiques détaillées sur le contenu des files", NULL, NULL},
    {"checkDatas", checkdatas_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "vérifie l'intégrité des possibilités stockées", NULL, NULL},
    {"checkDuplicate", check_duplicate_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "recherche les doublons dans les files", NULL, NULL},
    {"checkFiles", checkfiles_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "vérifie l'intégrité de toutes les files", NULL, NULL},
    {"checkFile", checkfile_interpreter, 0, CMD_CAT_DIAG, 0, "checkFile <n>",
     "vérifie l'intégrité de la file numéro <n>", NULL, NULL},
    {"checkDirections", checkdirections_interpreter, 0, CMD_CAT_DIAG, 0, NULL,
     "vérifie la cohérence du tableau de parcours", NULL, NULL},

    {"clients", clients_interpreter, 0, CMD_CAT_CLIENTS, 1, NULL,
     "liste les sessions de contrôle actives (pid, ip, mode, forks)", NULL, NULL},
    {"clientsStats", clients_stats_interpreter, 0, CMD_CAT_CLIENTS, 1, NULL,
     "demande leurs statistiques à tous les clients connectés", NULL, NULL},
    {"clientsCommand", clients_cmd_interpreter, 0, CMD_CAT_CLIENTS, 1, "clientsCommand <commande...>",
     "pousse une commande à tous les clients connectés",
     "Liste blanche : pause, resume, limit, maxStockByThread, prunerBatch.\n"
     "Toute autre commande est refusée sans être diffusée.", NULL},

    /* Alias : résolus vers l'entrée canonique par find_command. Les noms
       historiques abrégés (sorta, rmnonext, …) restent acceptés ici ; les
       anciens noms tout-minuscule (printfile, checkdatas, …) n'ont pas besoin
       d'alias, la correspondance ignorant déjà la casse. */
    {"?", NULL, 0, CMD_CAT_GENERAL, 0, NULL, NULL, NULL, "help"},
    {"quit", NULL, 0, CMD_CAT_GENERAL, 0, NULL, NULL, NULL, "exit"},
    {"cls", NULL, 0, CMD_CAT_GENERAL, 0, NULL, NULL, NULL, "clear"},
    {"stats", NULL, 0, CMD_CAT_DIAG, 0, NULL, NULL, NULL, "statistic"},
    {"sorta", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortAsc"},
    {"sortd", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortDesc"},
    {"sortdm", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "sortDescMulti"},
    {"rmnonext", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "removeNoNext"},
    {"prune", NULL, 0, CMD_CAT_STOCK, 0, NULL, NULL, NULL, "removeNoNext"},
    {"clientsCmd", NULL, 0, CMD_CAT_CLIENTS, 1, NULL, NULL, NULL, "clientsCommand"}
};

/** @brief Interpréteur de la commande `sortAsc` (alias `sorta`) : tri ascendant des possibilités. */
int sort_ascending_interpreter(void) {
    return sort_ascending();
}

/** @brief Interpréteur de la commande `sortDesc [n_file]` (alias `sortd`) : tri descendant, optionnellement sur un seul fichier. */
int sort_descending_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return sort_descending();
    }
    
    int n_file = atoi(arguments);
    sort_d_mono(&n_file);
    return 0;
}

/** @brief Interpréteur de `maxStockByThread <n>` : fixe la limite de possibilités par thread. */
int max_stock_by_thread_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_stock_by_thread = atoi(arguments);
        return 0;
    }
    return CMD_ERR_USAGE;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int pruner_batch_clamp(int v) {
    if (v < 1) {
        return 1;
    }
    if (v > PRUNER_BATCH_MAX) {
        return PRUNER_BATCH_MAX;
    }
    return v;
}

/**
 * @brief Interpréteur de `prunerBatch <n>` : fixe la taille de lot d'échange du
 *        pruner (nombre de possibilités demandées/acquittées par aller-retour).
 *
 * Propagée aux process enfants (send_to_childs = 1). Bornée à [1, PRUNER_BATCH_MAX]
 * pour maîtriser la mémoire du pruner et les tampons GPU.
 */
int pruner_batch_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        pruner_batch_size = pruner_batch_clamp(atoi(arguments));
        return 0;
    }
    return CMD_ERR_USAGE;
}

/** @brief Interpréteur de `limit <n>` : fixe le débit maximum de recherche par seconde. */
int limit_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_search_by_sec = atoi(arguments);
        return 0;
    }

    return CMD_ERR_USAGE;
}

/**
 * @brief Interpréteur de `check` : affiche le dernier rapport de statistiques `lastcheck`.
 *
 * `lastcheck` est republié toutes les 10 secondes par un thread de
 * statistiques (`check_server`/`check_client_threads`, via
 * `lastcheck_publish()`), potentiellement pendant que cette commande
 * s'exécute depuis le thread console. On prend donc `lastcheck_mutex` pour
 * copier le rapport dans un buffer local (`strdup`), et on logge cette copie
 * une fois le verrou relâché : sans cela, une lecture concurrente au swap
 * pointeur/free pourrait déréférencer un buffer déjà libéré (use-after-free)
 * ou encore en cours de remplissage.
 *
 * N'efface plus l'écran : l'effacement est réservé à la commande `clear`
 * (aucune commande ne doit effacer implicitement — politique d'affichage).
 */
int check_interpreter(void) {
    pthread_mutex_lock(&lastcheck_mutex);
    char *report_copy = lastcheck != NULL ? strdup(lastcheck) : NULL;
    pthread_mutex_unlock(&lastcheck_mutex);

    log_info("%s\n", report_copy != NULL ? report_copy : "");
    free(report_copy);
    return 0;
}

/** @brief Interpréteur de `clear` (alias `cls`, raccourci Ctrl-L) : efface l'écran, le contenu reste dans le scrollback (ANSI) ou le pad (ncurses). */
int clear_interpreter(void) {
    clear_console();
    return 0;
}

/** @brief Interpréteur de `backup` : sauvegarde les files de possibilités dans les fichiers `.back`. */
int backup_interpreter(void) {
    log_info("start backup\n");
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    char *def_best_board_file = DEF_BEST_BOARD_FILE;
    int isServer = server;
    if (isServer == 0) {
        char *temp = malloc(sizeof(char) *(strlen(def_file) + 11));
        sprintf(temp, "%s_%i", def_file, getpid());
        def_file = temp;
        temp = malloc(sizeof(char) * (strlen(def_analyse_file)+ 11));
        sprintf(temp, "%s_%i", def_analyse_file, getpid());
        def_analyse_file = temp;
        temp = malloc(sizeof(char) * (strlen(def_best_board_file)+ 11));
        sprintf(temp, "%s_%i", def_best_board_file, getpid());
        def_best_board_file = temp;
    }
    int rb = backup(def_file);
    if (rb == BACKUP_SKIPPED_MAINTENANCE) {
        log_info("backup de %s sauté (maintenance en cours)\n", def_file);
    } else if (rb != BACKUP_OK) {
        log_info("backup de %s échoué\n", def_file);
    }
    int rba = backup_analysed(def_analyse_file);
    if (rba == BACKUP_SKIPPED_MAINTENANCE) {
        log_info("backup de %s sauté (maintenance en cours)\n", def_analyse_file);
    } else if (rba != BACKUP_OK) {
        log_info("backup de %s échoué\n", def_analyse_file);
    }
    // Représentation du meilleur plateau connu (pas seulement max_result) :
    // même commande console, fichier dédié (cf. core/best_board.h). Absent des
    // codes BACKUP_SKIPPED_MAINTENANCE/BACKUP_OK (best_board_save n'a pas de
    // section « maintenance » à sauter, cf. best_board.c) : 0 = succès.
    if (best_board_save(&g_server_best_board, def_best_board_file) != 0) {
        log_info("backup de %s échoué\n", def_best_board_file);
    }
    log_info("backup ended\n");
    if (isServer == 0) {
        free(def_file);
        free(def_analyse_file);
        free(def_best_board_file);
    }
    return 0;
}

/** @brief Interpréteur de `exit` : arrête proprement le programme (signal SIGINT aux enfants en mode client). */
int exit_interpreter(void) {
    request = REQUEST_STOP;
    if (server == 0) {
        if (parent_pid == getpid()) {
            // On attend que les threads enfants se terminent
            if (childrens_pid != NULL) {
#ifdef DEBUG_SIGNAL
                log_info("send signal to child\n");
#endif // DEBUG_SIGNAL
                for (int c = 0; c < NB_THREADS; c++) {
                    // On attend que le thread se termine
                    pid_t childrenPid = childrens_pid[c];
                    // On envoie le signal d'interruption
                    if (childrenPid > 0) {
                        kill(childrenPid, SIGINT);
                    }
                }
#ifdef DEBUG_SIGNAL
                log_info("send signal to child done\n");
#endif // DEBUG_SIGNAL
            }

            int cptloop = 0;
            int remaining;
            // On attend que tous les enfants soient réellement terminés.
            // kill(pid, 0) renvoie 0 tant que le process existe, -1 (ESRCH)
            // une fois qu'il a été récolté par wait_child / sigchld_handler.
            do {
                remaining = 0;
                if (childrens_pid != NULL) {
                    for (int c = 0; c < NB_THREADS; c++) {
                        if (childrens_pid[c] > 0 && kill(childrens_pid[c], 0) == 0) {
                            remaining++;
                        }
                    }
                }
                if (cptloop == 10) {
                    log_console("\r            ");
                    log_console("\r");
                    cptloop = 0;
                }
                log_console("*");
                flush_console();
                cptloop++;
                usleep(MICRO_SLEEP);
            } while (remaining > 0);
            log_console("\n");
            flush_console();
            exit(EXIT_SUCCESS);
        }
    } else  {
        exit(EXIT_SUCCESS);
    }
    
    return 0;
}

/**
 * @brief Cœur réentrant de `restore [fichier [fichier_analyse]]`, extrait de
 *        `restore_interpreter` pour être appelable sans passer par le
 *        curseur global `strtok` (cf. `admin_apply_privileged_command`, qui
 *        tokenise déjà la ligne via `strtok_r` et ne doit jamais toucher à
 *        l'état de `strtok` — même raison d'être que `admin_apply_remote_command`).
 *
 * @param file         Chemin du fichier de stock à restaurer.
 * @param analyse_file Chemin du fichier de possibilités analysées à restaurer.
 * @return             0 si la restauration a réussi, une valeur négative sinon.
 */
static int restore_apply(char *file, char *analyse_file) {
    log_info("start restore\n");

    // Suspension de la recherche pendant le remplacement du stock : sans cela,
    // les threads de recherche consomment et délèguent des possibilités au
    // milieu du vidage/réimport et mélangent ancien et nouvel état.
    int previous_request = request;
    if (previous_request == REQUEST_CONTINUE) {
        request = REQUEST_PAUSE;
        usleep(THREAD_MICRO_SLEEP);
    }

    int result = restore(file);
    if (result != 0) {
        log_error("restore impossible (%s) : stock conservé\n", file);
    } else if (restore_analysed(analyse_file) != 0) {
        log_error("restore analysed impossible (%s) : files analysées conservées\n", analyse_file);
        result = -1;
    }
    // Non bloquant : un backup plus ancien peut ne pas avoir ce fichier (feature
    // ajoutée après coup) — le stock/analysed restaurés ci-dessus restent valides
    // sans lui, seule la représentation du meilleur plateau reste vide.
    if (result == 0) {
        if (best_board_load(&g_server_best_board, DEF_BEST_BOARD_FILE) != 0) {
            log_error("restore best board impossible (%s) : aucun plateau record connu\n", DEF_BEST_BOARD_FILE);
        } else {
            // Le stock restauré ne reflète que la profondeur du curseur des
            // possibilités en attente, pas le meilleur plateau jamais atteint :
            // sans cette resynchronisation, max_result affiché reste sous-évalué
            // par rapport au vrai record connu du serveur.
            uint16_t recorded = best_board_result(&g_server_best_board);
            if (recorded > max_result) {
                max_result = recorded;
            }
        }
    }

    // On ne reprend que si aucun arrêt n'a été demandé entre-temps
    if (request == REQUEST_PAUSE) {
        request = previous_request;
    }

    if (result == 0) {
        log_info("backup restore\n");
    }
    return result;
}

/** @brief Interpréteur de `restore [fichier [fichier_analyse]]` : restaure les possibilités depuis les fichiers `.back`. */
int restore_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        def_file = arguments;
        arguments = strtok(NULL, " ");
        if (arguments != NULL) {
            def_analyse_file = arguments;
        }
    }
    return restore_apply(def_file, def_analyse_file);
}

/** @brief Interpréteur de `import` : importe les possibilités depuis les fichiers `.back` sans effacer les files actuelles. */
int import_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    log_info("start import\n");
    import(NULL, def_file);
    import_analysed(def_analyse_file);
    log_info("backup restore\n");
    
    return 0;
}

/** @brief Interpréteur de `loadJson` : importe une possibilité depuis une chaîne JSON (stdin/clipboard). */
int loadjson_interpreter(void) {
    log_info("load from json\n");
    import_json();
    log_info("backup json\n");
    
    return 0;
}

/**
 * @brief Ouvre @p path en écriture pour un export console ; journalise un
 *        message homogène et clair en cas d'échec (le fichier n'existe pas
 *        forcément, le répertoire parent peut être en lecture seule, etc.).
 */
static FILE *open_export_file(const char *cmd, const char *path)
{
    FILE *out = fopen(path, "w");
    if (out == NULL) {
        log_error("%s : impossible d'ouvrir « %s » en écriture\n", cmd, path);
    }
    return out;
}

/**
 * @brief Referme un export ouvert par open_export_file et journalise le
 *        résultat : décompte des possibilités écrites en cas de succès,
 *        message d'erreur homogène sinon (export alors incomplet).
 */
static void close_export_file(const char *cmd, const char *path, FILE *out, int rc, size_t count)
{
    fclose(out);
    if (rc == 0) {
        log_info("%s : export : %zu possibilité%s écrite%s dans %s\n",
                  cmd, count, count != 1 ? "s" : "", count != 1 ? "s" : "", path);
    } else {
        log_error("%s : échec de l'écriture dans « %s » (export incomplet)\n", cmd, path);
    }
}

/**
 * @brief Interpréteur de `print [fichier]` : affiche l'état du data manager
 *        (files, tailles), ou exporte le dump JSON complet dans [fichier]
 *        si l'argument est fourni.
 */
int print_interpreter(void) {
    char *path = strtok(NULL, " ");
    if (path == NULL) {
        return printdatamanager();
    }
    FILE *out = open_export_file("print", path);
    if (out == NULL) return -1;
    size_t count = 0;
    int rc = fprint_datamanager(out, &count);
    close_export_file("print", path, out, rc, count);
    return rc;
}

/** @brief Interpréteur de `sortDescMulti` (alias `sortdm`) : tri descendant multi-threadé des fichiers de possibilités. */
int sortdm_interpreter(void) {
    return sort_descending_mthread();
}

/** @brief Interpréteur de `split` : répartit les possibilités entre les différentes files. */
int split_interpreter(void) {
    return split_datas();
}

/** @brief Interpréteur de `regroup` : regroupe toutes les possibilités dans une seule file. */
int regroup_interpreter(void) {
    return regroup_datas();
}

/** @brief Interpréteur de `checkDatas` : vérifie la cohérence des possibilités stockées. */
int checkdatas_interpreter(void) {
    return check_datas();
}

/** @brief Interpréteur de `checkDuplicate` : recherche et supprime les doublons dans les files. */
int check_duplicate_interpreter(void) {
    return check_duplicate();
}
/** @brief Interpréteur de `statistic` : affiche les statistiques détaillées des données. */
int statistic_interpreter(void) {
    return statistic_datas();
}

/** @brief Interpréteur de `checkFiles` : vérifie la cohérence de toutes les files de possibilités. */
int checkfiles_interpreter(void) {
    return check_files();
}

/**
 * @brief Interpréteur de `printFile <n> [fichier]` : affiche le contenu de la
 *        file de possibilités numéro <n>, ou l'exporte en JSON dans [fichier]
 *        si l'argument est fourni. <n> reste obligatoire.
 */
int printfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return CMD_ERR_USAGE;
    }
    int n = atoi(arguments);
    char *path = strtok(NULL, " ");
    if (path == NULL) {
        return print_file(n);
    }
    FILE *out = open_export_file("printFile", path);
    if (out == NULL) return -1;
    size_t count = 0;
    int rc = fprint_file(out, n, &count);
    close_export_file("printFile", path, out, rc, count);
    return rc;
}

/** @brief Interpréteur de `checkFile <n>` : vérifie la cohérence du fichier de possibilités numéro n. */
int checkfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        return check_file(atoi(arguments));
    }

    return CMD_ERR_USAGE;
}

/** @brief Interpréteur de `checkDirections` : vérifie la cohérence du tableau de traversée `directions`. */
int checkdirections_interpreter(void) {
    if(test_directions() == 0)
    {
        log_info("directions : ok\n");
    } else
    {
        log_info("directions : NOK !\n");
    }
    
    return 0;
}

/** @brief Interpréteur de `removeNoNext` (alias `rmnonext`, `prune`) : supprime les possibilités sans continuation valide. */
int rmnonext_interpreter(void) {
    struct array_part *apart= read_parts(parts_files);

    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    remove_possibilities_with_no_next(map_parts, rotateParts);
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);

    return 0;
}

/**
 * @brief Commande `expand <niveau>` : développe le stock du serveur jusqu'au
 *        niveau de curseur demandé (anti-famine, cf. expand_datas_to_level).
 *
 * Reconstruit la map depuis `parts_files` (comme `removeNoNext` : le serveur libère
 * la sienne après l'expansion de démarrage), la passe à `expand_datas_to_level`,
 * puis libère tout. Utile à chaud quand le stock distribuable s'est raréfié.
 */
int expand_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return CMD_ERR_USAGE;
    }
    int level = atoi(arguments);
    if (level <= 0) {
        log_error("expand : niveau invalide (\"%s\") — attendu un entier > 0\n", arguments);
        return -1;
    }

    struct array_part *apart = read_parts(parts_files);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    expand_datas_to_level(level, map_parts, rotateParts);
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);

    return 0;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int admin_pause_transition(int current, int want_pause) {
    if (want_pause) {
        if (current == REQUEST_CONTINUE || current == REQUEST_PAUSE) {
            return REQUEST_ADMIN_PAUSE;
        }
        // Déjà en pause admin, ou en arrêt : inchangé.
        return current;
    }
    if (current == REQUEST_ADMIN_PAUSE) {
        return REQUEST_CONTINUE;
    }
    // Pas en pause admin (continue, pause de régulation, ou arrêt) : inchangé.
    return current;
}

/**
 * @brief Interpréteur de `pause` : pose une pause administrative (`REQUEST_ADMIN_PAUSE`)
 *        ET diffuse `CTRL_COMMAND "pause"` à toutes les sessions de contrôle actives.
 *
 * Contrairement à `REQUEST_PAUSE` (régulation de débit, levée automatiquement
 * par `control_step`), cette pause ne peut être levée que par la commande
 * `resume` — cf. la note dans static_variables.h. No-op côté local si déjà en
 * pause admin ou si le processus est en cours d'arrêt (`REQUEST_STOP`).
 *
 * Le volet diffusion (`control_registry_broadcast_command`) est ce qui rend
 * cette commande utile sur le SERVEUR : celui-ci ne lance jamais lui-même de
 * recherche (`request` n'y est jamais consulté par `autosearch`), donc une
 * pause purement locale y serait un no-op. En diffusant systématiquement —
 * sans distinguer serveur/client — la commande reste correcte des deux côtés :
 * sur un client, `control_registry` est toujours vide (rempli uniquement côté
 * serveur par `INST_CONTROL_HELLO`), donc la diffusion y est un no-op
 * silencieux (`n == 0`). Ancien comportement de `clientsPause`, désormais
 * fusionné ici (y compris la persistance de l'état désiré pour les futurs
 * clients, `control_registry_desired_pause_state`).
 */
int pause_interpreter(void) {
    int previous = request;
    int updated = admin_pause_transition(previous, 1);
    request = updated;
    if (updated == REQUEST_ADMIN_PAUSE && previous != REQUEST_ADMIN_PAUSE) {
        log_event("pause administrative demandée : recherche mise en pause\n");
    } else if (previous == REQUEST_ADMIN_PAUSE) {
        log_info("pause : déjà en pause administrative\n");
    } else {
        log_error("pause : impossible (arrêt en cours)\n");
    }
    int n = control_registry_broadcast_command(CTRL_COMMAND, "pause");
    if (n > 0) {
        log_info("pause : diffusée à %d session(s) client(s)\n", n);
    }
    return 0;
}

/**
 * @brief Interpréteur de `resume` : lève une pause administrative (`REQUEST_ADMIN_PAUSE`)
 *        ET diffuse `CTRL_COMMAND "resume"` à toutes les sessions de contrôle actives.
 *
 * No-op côté local si le processus n'est pas en pause administrative (par ex.
 * déjà en fonctionnement normal, ou en pause de régulation de débit — laissée
 * à `control_step`). Voir `pause_interpreter` pour le volet diffusion, fusionné
 * depuis l'ancien `clientsResume`.
 */
int resume_interpreter(void) {
    int previous = request;
    int updated = admin_pause_transition(previous, 0);
    request = updated;
    if (updated == REQUEST_CONTINUE && previous == REQUEST_ADMIN_PAUSE) {
        log_event("pause administrative levée : reprise de la recherche\n");
    } else {
        log_info("resume : pas de pause administrative en cours\n");
    }
    int n = control_registry_broadcast_command(CTRL_COMMAND, "resume");
    if (n > 0) {
        log_info("resume : diffusée à %d session(s) client(s)\n", n);
    }
    return 0;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int admin_apply_remote_command(const char *line) {
    if (!control_command_allowed(line)) {
        return ADMIN_CMD_FORBIDDEN;
    }

    size_t length = strlen(line) + 1;
    char *copy = malloc(sizeof(char) * length);
    memcpy(copy, line, length);

    char *save = NULL;
    char *word = strtok_r(copy, " ", &save);
    int result = ADMIN_CMD_BAD_ARGS;
    if (word != NULL) {
        char *arg = strtok_r(NULL, " ", &save);
        if (strcmp(word, "pause") == 0) {
            request = admin_pause_transition(request, 1);
            control_registry_broadcast_command(CTRL_COMMAND, "pause");
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "resume") == 0) {
            request = admin_pause_transition(request, 0);
            control_registry_broadcast_command(CTRL_COMMAND, "resume");
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "limit") == 0 && arg != NULL) {
            max_search_by_sec = atoi(arg);
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "maxStockByThread") == 0 && arg != NULL) {
            max_stock_by_thread = atoi(arg);
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "prunerBatch") == 0 && arg != NULL) {
            pruner_batch_size = pruner_batch_clamp(atoi(arg));
            result = ADMIN_CMD_OK;
        }
    }

    free(copy);
    return result;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int admin_apply_privileged_command(const char *line) {
    if (control_command_allowed(line)) {
        // Commande "standard" (pause/resume/limit/...) : identique, authentifiée
        // ou non, au chemin déjà validé par admin_apply_remote_command. Pas de
        // duplication de logique ici.
        return admin_apply_remote_command(line);
    }
    if (!control_command_privileged(line)) {
        return ADMIN_CMD_FORBIDDEN;
    }

    // À ce stade, line est reconnue par control_command_privileged (restore ou
    // backup) : l'appelant (handle_command_route, src/net/http_server.c) a déjà
    // vérifié le jeton Bearer AVANT cet appel — cette fonction n'authentifie rien
    // elle-même, elle exécute une commande déjà autorisée.
    size_t length = strlen(line) + 1;
    char *copy = malloc(sizeof(char) * length);
    memcpy(copy, line, length);

    char *save = NULL;
    char *word = strtok_r(copy, " ", &save);
    int result = ADMIN_CMD_BAD_ARGS;
    if (word != NULL) {
        if (strcmp(word, "backup") == 0) {
            // backup_interpreter ne prend aucun argument et n'appelle jamais
            // strtok : réentrant tel quel (contrairement à restore_interpreter).
            backup_interpreter();
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "restore") == 0) {
            char *file = strtok_r(NULL, " ", &save);
            char *analyse_file = (file != NULL) ? strtok_r(NULL, " ", &save) : NULL;
            // Comme les autres commandes admin (pause/resume/limit/...), le
            // résultat HTTP reflète que la commande a été exécutée, pas que
            // restore() a trouvé un fichier valide : un échec est déjà journalisé
            // par restore_apply (log_error), au même niveau que backup_interpreter.
            restore_apply(file != NULL ? file : DEF_FILE,
                           analyse_file != NULL ? analyse_file : DEF_ANALYSE_FILE);
            result = ADMIN_CMD_OK;
        }
    }

    free(copy);
    return result;
}

/**
 * @brief Interpréteur de `clients` : liste les sessions de contrôle actives
 *        (canal `INST_CONTROL_HELLO`, v9) via `control_registry_snapshot`.
 *
 * Commande SERVEUR pure (send_to_childs = 0) : les process forkés du client
 * ne connaissent pas ce registre.
 */
int clients_interpreter(void) {
    control_session_info_t infos[MAX_CONTROL_SESSIONS];
    int n = control_registry_snapshot(infos, MAX_CONTROL_SESSIONS);
    if (n == 0) {
        log_info("clients : aucune session de contrôle active\n");
        return 0;
    }
    log_info("clients : %d session(s) de contrôle active(s)\n", n);
    for (int i = 0; i < n; i++) {
        log_info("  pid=%d  ip=%s  mode=%u  forks=%d  derniere activite=%lld\n",
                  infos[i].pid, infos[i].peer_ip[0] != '\0' ? infos[i].peer_ip : "?",
                  (unsigned)infos[i].mode, infos[i].nb_forks,
                  (long long)infos[i].last_activity);
    }
    return 0;
}

/**
 * @brief Interpréteur de `clientsStats` : demande les statistiques agrégées
 *        (`CTRL_GET_STATS`) à toutes les sessions de contrôle actives.
 */
int clients_stats_interpreter(void) {
    int n = control_registry_broadcast_get_stats();
    log_info("clientsStats : demande envoyée à %d session(s)\n", n);
    return 0;
}

/**
 * @brief Interpréteur de `clientsCommand <ligne...>` (alias `clientsCmd`) : diffuse une commande console
 *        à distance (`CTRL_COMMAND`) à toutes les sessions de contrôle actives.
 *
 * `<ligne...>` est reprise TELLE QUELLE après le premier mot (pas retokenisée :
 * elle peut contenir plusieurs arguments, ex. "limit 500"). Avant diffusion,
 * son premier mot est vérifié par `control_command_allowed` (liste blanche
 * définie dans control_protocol.h) : une commande non autorisée est refusée
 * SANS être diffusée.
 */
int clients_cmd_interpreter(void) {
    char *rest = strtok(NULL, "");
    if (rest != NULL) {
        while (*rest == ' ') {
            rest++;
        }
    }
    if (rest == NULL || *rest == '\0') {
        return CMD_ERR_USAGE;
    }
    if (!control_command_allowed(rest)) {
        log_error("clientsCommand : commande non autorisée à distance (liste blanche : pause, resume, limit, maxStockByThread, prunerBatch) : \"%s\"\n", rest);
        return -1;
    }
    int n = control_registry_broadcast_command(CTRL_COMMAND, rest);
    log_info("clientsCommand : \"%s\" diffusée à %d session(s)\n", rest, n);
    return 0;
}

/**
 * @brief Interpréteur de `printAnalysed [fichier]` : affiche les possibilités
 *        en cours d'analyse, ou les exporte en JSON dans [fichier] si
 *        l'argument est fourni.
 *
 * `printAnalysed` est propagée aux process fils (send_to_childs = 1,
 * `send_command_to_childs`) : le TEXTE de la commande, [fichier] compris, est
 * rejoué tel quel par le parent ET par chaque fork de recherche — des
 * processus séparés. Sans précaution, tous écriraient dans LE MÊME fichier en
 * concurrence (écritures entrelacées / clobbering). On suffixe donc le chemin
 * par le pid en mode client — même convention que `backup_interpreter` pour
 * DEF_FILE/DEF_ANALYSE_FILE — pour que chaque process écrive dans son propre
 * fichier. Le serveur ne force aucun processus de recherche, donc pas de
 * collision possible côté serveur : chemin utilisé tel quel.
 */
int printanalysed_interpreter(void) {
    char *path = strtok(NULL, " ");
    if (path == NULL) {
        return print_all_file_analysed();
    }

    char *final_path = path;
    char *suffixed = NULL;
    if (server == 0) {
        suffixed = malloc(strlen(path) + 11);
        if (suffixed == NULL) {
            log_error("printAnalysed : allocation échouée pour le suffixe de pid\n");
            return -1;
        }
        sprintf(suffixed, "%s_%i", path, getpid());
        final_path = suffixed;
    }

    FILE *out = open_export_file("printAnalysed", final_path);
    if (out == NULL) {
        free(suffixed);
        return -1;
    }
    size_t count = 0;
    int rc = fprint_all_file_analysed(out, &count);
    close_export_file("printAnalysed", final_path, out, rc, count);
    free(suffixed);
    return rc;
}

/** @brief Interpréteur de `restockAnalysed` : remet les possibilités en cours d'analyse dans le stock. */
int restockanalysed_interpreter(void) {
    return restock_analysed();
}

/** @brief Interpréteur de `min` : affiche le nombre minimal de pièces placées parmi toutes les possibilités. */
int min_interpreter(void) {
    log_info("min : %i\n",search_min_datas());
    
    return 0;
}

/**
 * @brief Recherche une `command_description` par son nom dans le tableau `commands`.
 *
 * La comparaison ignore la casse (`maxstockbythread` trouve `maxStockByThread`)
 * et un alias est résolu vers son entrée canonique (un seul niveau : les alias
 * pointent toujours un nom canonique, jamais un autre alias).
 *
 * @param instruction Nom de la commande à chercher.
 * @return            Pointeur vers la commande canonique trouvée, ou NULL si inconnue.
 */
command_description *find_command(const char *instruction) {
    for (int c =0; c < NB_COMMANDS; c++) {
        command_description *command = &commands[c];
        if (strcasecmp(command->command, instruction) == 0) {
            if (command->alias_of != NULL) {
                return find_command(command->alias_of);
            }
            return command;
        }
    }
    return NULL;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
const char *command_canonical_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    command_description *command = find_command(name);
    return command != NULL ? command->command : NULL;
}

/**
 * @brief Ajoute du texte formaté à la fin de `out` (borné à `cap`, offset suivi par `*len`).
 */
static void help_append(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(out + *len, cap - *len, fmt, args);
    va_end(args);
    if (written > 0) {
        *len += (size_t)written > cap - *len ? cap - *len : (size_t)written;
    }
}

/**
 * @brief Ajoute la liste des alias d'une entrée canonique : ` (alias : a, b)`.
 */
static void help_append_aliases(const command_description *canonical,
                                char *out, size_t cap, size_t *len) {
    int first = 1;
    for (int c = 0; c < NB_COMMANDS; c++) {
        if (commands[c].alias_of != NULL &&
            strcmp(commands[c].alias_of, canonical->command) == 0) {
            help_append(out, cap, len, "%s%s", first ? " (alias : " : ", ", commands[c].command);
            first = 0;
        }
    }
    if (!first) {
        help_append(out, cap, len, ")");
    }
}

/**
 * @brief Nombre de caractères affichés d'une chaîne UTF-8 (points de code, pas octets).
 *
 * Nécessaire pour aligner la colonne des résumés : `%-36s` compte les octets,
 * donc chaque accent ("catégorie") décalerait la colonne d'un caractère.
 */
static size_t utf8_display_length(const char *s) {
    size_t n = 0;
    for (; *s != '\0'; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/**
 * @brief Ajoute la section d'une catégorie : titre + une ligne `usage  résumé` par commande.
 */
static void help_append_category(int category, char *out, size_t cap, size_t *len) {
    help_append(out, cap, len, "%s\n", category_labels[category]);
    for (int c = 0; c < NB_COMMANDS; c++) {
        const command_description *command = &commands[c];
        if (command->alias_of != NULL || command->category != category) {
            continue;
        }
        const char *shown = command->usage != NULL ? command->usage : command->command;
        size_t shown_length = utf8_display_length(shown);
        int padding = shown_length < 36 ? (int)(36 - shown_length) : 1;
        help_append(out, cap, len, "  %s%*s%s", shown, padding, " ", command->summary);
        if (command->server_only) {
            help_append(out, cap, len, " [serveur]");
        }
        help_append_aliases(command, out, cap, len);
        help_append(out, cap, len, "\n");
    }
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int help_format_general(char *out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    help_append(out, out_size, &len,
                "Commandes — « help <commande> » pour le détail, « help <catégorie> » pour filtrer.\n");
    help_append(out, out_size, &len, "Catégories :");
    for (int cat = 0; cat < CMD_CAT_COUNT; cat++) {
        help_append(out, out_size, &len, "%s %s", cat == 0 ? "" : ",", category_keywords[cat]);
    }
    help_append(out, out_size, &len, "\n\n");
    for (int cat = 0; cat < CMD_CAT_COUNT; cat++) {
        help_append_category(cat, out, out_size, &len);
        if (cat < CMD_CAT_COUNT - 1) {
            help_append(out, out_size, &len, "\n");
        }
    }
    return 0;
}

/**
 * @brief Voir la doc dans command_lines.h.
 */
int help_format_topic(const char *topic, char *out, size_t out_size) {
    size_t len = 0;
    out[0] = '\0';
    if (topic == NULL) {
        return -1;
    }

    command_description *command = find_command(topic);
    if (command != NULL) {
        help_append(out, out_size, &len, "%s — %s",
                    command->usage != NULL ? command->usage : command->command,
                    command->summary);
        help_append_aliases(command, out, out_size, &len);
        help_append(out, out_size, &len, "\n  catégorie   : %s\n",
                    category_labels[command->category]);
        help_append(out, out_size, &len, "  portée      : %s\n",
                    command->server_only ? "serveur uniquement" : "serveur et client");
        help_append(out, out_size, &len, "  propagation : %s\n",
                    command->send_to_childs ? "transmise aux processus fils"
                                            : "processus courant uniquement");
        if (command->details != NULL) {
            help_append(out, out_size, &len, "\n%s\n", command->details);
        }
        return 0;
    }

    for (int cat = 0; cat < CMD_CAT_COUNT; cat++) {
        if (strcasecmp(category_keywords[cat], topic) == 0) {
            help_append_category(cat, out, out_size, &len);
            return 0;
        }
    }
    return -1;
}

/** @brief Interpréteur de `help [commande|catégorie]` : aide générale, d'une commande ou d'une catégorie. */
int help_interpreter(void) {
    char *arg = strtok(NULL, " ");
    char *help = calloc(HELP_BUFFER_SIZE, sizeof(char));
    int result = arg == NULL ? help_format_general(help, HELP_BUFFER_SIZE)
                             : help_format_topic(arg, help, HELP_BUFFER_SIZE);
    if (result == 0) {
        log_info("%s", help);
    } else {
        const char *command_names[NB_COMMANDS];
        for (int c = 0; c < NB_COMMANDS; c++) {
            command_names[c] = commands[c].command;
        }
        const char *suggestion = closest_command(arg, command_names, NB_COMMANDS);
        if (suggestion != NULL) {
            log_error("help : commande ou catégorie inconnue : %s -- vouliez-vous dire \"help %s\" ?\n",
                      arg, suggestion);
        } else {
            log_error("help : commande ou catégorie inconnue : %s\n", arg);
        }
    }
    free(help);
    return result;
}

/**
 * @brief Parse et exécute une ligne de commande.
 *
 * Tokenize `command` sur les espaces, trouve la commande via `find_command`,
 * l'exécute, et la propage aux processus enfants si `send_to_childs == 1`.
 *
 * @param command Ligne de commande saisie par l'utilisateur (modifiée par `strtok`).
 * @return        0 en cas de succès, -1 si commande inconnue ou erreur d'interpréteur.
 */
int do_command_line(char *command) {
    int result = 0;
    if (command != NULL && strlen(command) > 0) {
        size_t command_length = strlen(command) + 1;
        char *toSplit = malloc(sizeof(char) * command_length);
        memcpy(toSplit, command, command_length);
        char *instruction = strtok(toSplit, " ");
        if (instruction == NULL) {
            /* Ligne ne contenant que des espaces : rien à exécuter. */
            free(toSplit);
            return 0;
        }
        command_description *command_desc = find_command(instruction);
        if (command_desc != NULL) {
            result = command_desc->interpreter();
            if (result == CMD_ERR_USAGE) {
                /* Argument manquant/invalide : rappel d'usage uniforme depuis la
                   table, au lieu d'un échec silencieux propre à chaque commande. */
                if (command_desc->usage != NULL) {
                    log_error("usage : %s — %s\n", command_desc->usage, command_desc->summary);
                }
                result = -1;
            }
            if(result == 0 && command_desc->send_to_childs == 1) {
                send_command_to_childs(command);
            }
        } else {
            /* Commande inconnue : on le signale (au lieu d'échouer en silence)
               et on propose la commande la plus proche en cas de faute de frappe.
               On projette les noms de la table `commands` pour les passer au
               module d'appariement (qui ignore le type command_description). */
            const char *command_names[NB_COMMANDS];
            for (int c = 0; c < NB_COMMANDS; c++) {
                command_names[c] = commands[c].command;
            }
            const char *suggestion = closest_command(instruction, command_names, NB_COMMANDS);
            if (suggestion != NULL) {
                log_error("commande inconnue : %s -- vouliez-vous dire \"%s\" ? (tapez \"help\")\n",
                          instruction, suggestion);
            } else {
                log_error("commande inconnue : %s (tapez \"help\" pour la liste)\n", instruction);
            }
            result = -1;
        }
        free(toSplit);
    }
    return result;
}
