#include "command_lines.h"
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "logger.h"
#include "datamanager.h"
#include "local_socket.h"
#include "readdata.h"

#define DEF_FILE "./eternityII.back"
#define DEF_ANALYSE_FILE "./eternityII-in_analyse.back"
#define NB_COMMANDS 26

/**
 * @brief Définition d'une commande prise en charge
 */
typedef struct
{
    /// nom de l'instruction
    char *command;
    /// fonction à exectuer pour la commande
    int (*interpreter)(void);
    ///indique si la commande doit être transmise aux threads fils
    int8_t send_to_childs;
} command_description;

int sort_ascending_interpreter(void);
int sort_descending_interpreter(void);
int max_stock_by_thread_interpreter(void);
int limit_interpreter(void);
int exit_interpreter(void);
int check_interpreter(void);
int backup_interpreter(void);
int restore_interpreter(void);
int restoreOld_interpreter(void);
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
int printanalysed_interpreter(void);
int min_interpreter(void);
int help_interpreter(void);
int statistic_interpreter(void);

/**
 * @brief Commandes prises en charge.
 */
static command_description commands[NB_COMMANDS] = {
    {"sorta", sort_ascending_interpreter, 0},
    {"sortd", sort_descending_interpreter, 0},
    {"maxStockByThread", max_stock_by_thread_interpreter, 1},
    {"limit", limit_interpreter, 1},
    {"exit", exit_interpreter, 0},
    {"check", check_interpreter, 0},
    {"backup", backup_interpreter, 1},
    {"restore", restore_interpreter, 1},
    {"restoreOld", restoreOld_interpreter, 1},
    {"import", import_interpreter, 0},
    {"loadjson", loadjson_interpreter, 0},
    {"print", print_interpreter, 0},
    {"sortdm", sortdm_interpreter, 0},
    {"split", split_interpreter, 0},
    {"regroup", regroup_interpreter, 0},
    {"checkdatas", checkdatas_interpreter, 0},
    {"checkduplicate", check_duplicate_interpreter, 0},
    {"checkfiles", checkfiles_interpreter, 0},
    {"printfile", printfile_interpreter, 0},
    {"checkfile", checkfile_interpreter, 0},
    {"checkdirections", checkdirections_interpreter, 0},
    {"rmnonext", rmnonext_interpreter, 1},
    {"printanalysed", printanalysed_interpreter, 1},
    {"statistic", statistic_interpreter, 0},
    {"min", min_interpreter, 1},
    {"help", help_interpreter, 0}
};

/** @brief Interpréteur de la commande `sorta` : tri ascendant des possibilités. */
int sort_ascending_interpreter(void) {
    return sort_ascending();
}

/** @brief Interpréteur de la commande `sortd [n_file]` : tri descendant, optionnellement sur un seul fichier. */
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
    return -1;
}

/** @brief Interpréteur de `limit <n>` : fixe le débit maximum de recherche par seconde. */
int limit_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_search_by_sec = atoi(arguments);
        return 0;
    }
    
    return -1;
}

/** @brief Interpréteur de `check` : affiche le rapport de statistiques `lastcheck`. */
int check_interpreter(void) {
    log_info("%s\n",lastcheck);
    return 0;
}

/** @brief Interpréteur de `backup` : sauvegarde les files de possibilités dans les fichiers `.back`. */
int backup_interpreter(void) {
    log_info("start backup\n");
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    int isServer = server;
    if (isServer == 0) {
        char *temp = malloc(sizeof(char) *(strlen(def_file) + 11));
        sprintf(temp, "%s_%i", def_file, getpid());
        def_file = temp;
        temp = malloc(sizeof(char) * (strlen(def_analyse_file)+ 11));
        sprintf(temp, "%s_%i", def_analyse_file, getpid());
        def_analyse_file = temp;
    }
    backup(def_file);
    backup_analysed(def_analyse_file);
    log_info("backup ended\n");
    if (isServer == 0) {
        free(def_file);
        free(def_analyse_file);
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
            while (1)
            {
                if (cptloop == 10) {
                    log_console("\r            ");
                    log_console("\r");
                    cptloop = 0;
                }
                log_console("*");
                cptloop++;
                usleep(MICRO_SLEEP);
            }

            
        }
    } else  {
        exit(EXIT_SUCCESS);
    }
    
    return 0;
}

/** @brief Interpréteur de `restore` : restaure les possibilités depuis les fichiers `.back` par défaut. */
int restore_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    log_info("start restore\n");
    restore(def_file);
    restore_analysed(def_analyse_file);
    log_info("backup restore\n");
    return 0;
}

/** @brief Interpréteur de `restoreOld` : restaure depuis un format ancien (nécessite `FACES_USED_BITS`). */
int restoreOld_interpreter(void) {
#ifdef FACES_USED_BITS
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    log_info("start restore\n");
    restore_old_file(def_file);
    restore_analysed(def_analyse_file);
    log_info("backup restore\n");
    
#endif // FACES_USED_BITS
    return 0;
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

/** @brief Interpréteur de `loadjson` : importe une possibilité depuis une chaîne JSON (stdin/clipboard). */
int loadjson_interpreter(void) {
    log_info("load from json\n");
    import_json();
    log_info("backup json\n");
    
    return 0;
}

/** @brief Interpréteur de `print` : affiche l'état du data manager (files, tailles). */
int print_interpreter(void) {
    return printdatamanager();
}

/** @brief Interpréteur de `sortdm` : tri descendant multi-threadé des fichiers de possibilités. */
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

/** @brief Interpréteur de `checkdatas` : vérifie la cohérence des possibilités stockées. */
int checkdatas_interpreter(void) {
    return check_datas();
}

/** @brief Interpréteur de `checkduplicate` : recherche et supprime les doublons dans les files. */
int check_duplicate_interpreter(void) {
    return check_duplicate();
}
/** @brief Interpréteur de `statistic` : affiche les statistiques détaillées des données. */
int statistic_interpreter(void) {
    return statistic_datas();
}

/** @brief Interpréteur de `checkfiles` : vérifie la cohérence de toutes les files de possibilités. */
int checkfiles_interpreter(void) {
    return check_files();
}

/** @brief Interpréteur de `printfile <n>` : affiche le contenu du fichier de possibilités numéro n. */
int printfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        return print_file(atoi(arguments));
    }
    
    return -1;
}

/** @brief Interpréteur de `checkfile <n>` : vérifie la cohérence du fichier de possibilités numéro n. */
int checkfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        return check_file(atoi(arguments));
    }
    
    return -1;
}

/** @brief Interpréteur de `checkdirections` : vérifie la cohérence du tableau de traversée `directions`. */
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

/** @brief Interpréteur de `rmnonext` : supprime les possibilités sans continuation valide. */
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

/** @brief Interpréteur de `printanalysed` : affiche les possibilités en cours d'analyse. */
int printanalysed_interpreter(void) {
    return print_all_file_analysed();
}

/** @brief Interpréteur de `min` : affiche le nombre minimal de pièces placées parmi toutes les possibilités. */
int min_interpreter(void) {
    log_info("min : %i\n",search_min_datas());
    
    return 0;
}

/** @brief Interpréteur de `help` : affiche la liste des commandes disponibles. */
int help_interpreter(void) {
    char *help = calloc(NB_COMMANDS * 100, sizeof(char));
    strcat(help, "commands :\n");
    for (int c = 0; c < NB_COMMANDS; c++) {
        strcat(help, "  ");
        strcat(help, commands[c].command);
        strcat(help, "\n");
    }
    log_info("%s", help);
    free(help);
    return 0;
}

/**
 * @brief Recherche une `command_description` par son nom dans le tableau `commands`.
 * @param instruction Nom de la commande à chercher.
 * @return            Pointeur vers la commande trouvée, ou NULL si inconnue.
 */
command_description *find_command(const char *instruction) {
    for (int c =0; c < NB_COMMANDS; c++) {
        command_description *command = &commands[c];
        if (strcmp(command->command, instruction) == 0) {
            return command;
        }
    }
    return NULL;
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
        command_description *command_desc = find_command(instruction);
        if (command_desc != NULL) {
            result = command_desc->interpreter();
            if(result == 0 && command_desc->send_to_childs == 1) {
                send_command_to_childs(command);
            }
        } else {
            result = -1;
        }
        free(toSplit);
    }
    return result;
}
