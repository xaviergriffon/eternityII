#include "ui/command_lines.h"
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "ui/logger.h"
#include "core/datamanager.h"
#include "net/local_socket.h"
#include "core/readdata.h"
#include "ui/command_match.h"
#include "app/static_variables.h"
#include "app/control_registry.h"
#include "net/control_protocol.h"

#define DEF_FILE "./eternityII.back"
#define DEF_ANALYSE_FILE "./eternityII-in_analyse.back"
#define NB_COMMANDS 35

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
int statistic_interpreter(void);
int pause_interpreter(void);
int resume_interpreter(void);
int clients_interpreter(void);
int clients_stats_interpreter(void);
int clients_cmd_interpreter(void);
int clients_pause_interpreter(void);
int clients_resume_interpreter(void);

/**
 * @brief Commandes prises en charge.
 */
static command_description commands[NB_COMMANDS] = {
    {"sorta", sort_ascending_interpreter, 0},
    {"sortd", sort_descending_interpreter, 0},
    {"maxStockByThread", max_stock_by_thread_interpreter, 1},
    {"prunerBatch", pruner_batch_interpreter, 1},
    {"limit", limit_interpreter, 1},
    {"exit", exit_interpreter, 0},
    {"check", check_interpreter, 0},
    {"backup", backup_interpreter, 1},
    {"restore", restore_interpreter, 1},
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
    {"expand", expand_interpreter, 0},
    {"printanalysed", printanalysed_interpreter, 1},
    {"restockanalysed", restockanalysed_interpreter, 0},
    {"statistic", statistic_interpreter, 0},
    {"min", min_interpreter, 1},
    {"help", help_interpreter, 0},
    {"pause", pause_interpreter, 1},
    {"resume", resume_interpreter, 1},
    {"clients", clients_interpreter, 0},
    {"clientsStats", clients_stats_interpreter, 0},
    {"clientsCmd", clients_cmd_interpreter, 0},
    {"clientsPause", clients_pause_interpreter, 0},
    {"clientsResume", clients_resume_interpreter, 0}
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

/**
 * @brief Interpréteur de `check` : réaffiche le rapport de statistiques `lastcheck` en place (sans défilement).
 *
 * `lastcheck` est republié toutes les 10 secondes par un thread de
 * statistiques (`check_server`/`check_client_threads`, via
 * `lastcheck_publish()`), potentiellement pendant que cette commande
 * s'exécute depuis le thread console. On prend donc `lastcheck_mutex` pour
 * copier le rapport dans un buffer local (`strdup`), et on logge cette copie
 * une fois le verrou relâché : sans cela, une lecture concurrente au swap
 * pointeur/free pourrait déréférencer un buffer déjà libéré (use-after-free)
 * ou encore en cours de remplissage.
 */
int check_interpreter(void) {
    pthread_mutex_lock(&lastcheck_mutex);
    char *report_copy = lastcheck != NULL ? strdup(lastcheck) : NULL;
    pthread_mutex_unlock(&lastcheck_mutex);

    clear_console();
    log_info("%s\n", report_copy != NULL ? report_copy : "");
    free(report_copy);
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
    log_info("start restore\n");

    // Suspension de la recherche pendant le remplacement du stock : sans cela,
    // les threads de recherche consomment et délèguent des possibilités au
    // milieu du vidage/réimport et mélangent ancien et nouvel état.
    int previous_request = request;
    if (previous_request == REQUEST_CONTINUE) {
        request = REQUEST_PAUSE;
        usleep(THREAD_MICRO_SLEEP);
    }

    int result = restore(def_file);
    if (result != 0) {
        log_error("restore impossible (%s) : stock conservé\n", def_file);
    } else if (restore_analysed(def_analyse_file) != 0) {
        log_error("restore analysed impossible (%s) : files analysées conservées\n", def_analyse_file);
        result = -1;
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

/**
 * @brief Commande `expand <niveau>` : développe le stock du serveur jusqu'au
 *        niveau de curseur demandé (anti-famine, cf. expand_datas_to_level).
 *
 * Reconstruit la map depuis `parts_files` (comme `rmnonext` : le serveur libère
 * la sienne après l'expansion de démarrage), la passe à `expand_datas_to_level`,
 * puis libère tout. Utile à chaud quand le stock distribuable s'est raréfié.
 */
int expand_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        log_error("expand : niveau manquant (usage : expand <niveau>)\n");
        return -1;
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
 * @brief Interpréteur de `pause` : pose une pause administrative (`REQUEST_ADMIN_PAUSE`).
 *
 * Contrairement à `REQUEST_PAUSE` (régulation de débit, levée automatiquement
 * par `control_step`), cette pause ne peut être levée que par la commande
 * `resume` — cf. la note dans static_variables.h. No-op si déjà en pause admin
 * ou si le processus est en cours d'arrêt (`REQUEST_STOP`).
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
    return 0;
}

/**
 * @brief Interpréteur de `resume` : lève une pause administrative (`REQUEST_ADMIN_PAUSE`).
 *
 * No-op si le processus n'est pas en pause administrative (par ex. déjà en
 * fonctionnement normal, ou en pause de régulation de débit — laissée à
 * `control_step`).
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
            result = ADMIN_CMD_OK;
        } else if (strcmp(word, "resume") == 0) {
            request = admin_pause_transition(request, 0);
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
        log_info("  pid=%d  mode=%u  forks=%d  derniere activite=%lld\n",
                  infos[i].pid, (unsigned)infos[i].mode, infos[i].nb_forks,
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
 * @brief Interpréteur de `clientsCmd <ligne...>` : diffuse une commande console
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
        log_error("clientsCmd : ligne de commande manquante (usage : clientsCmd <commande>)\n");
        return -1;
    }
    if (!control_command_allowed(rest)) {
        log_error("clientsCmd : commande non autorisée à distance : \"%s\"\n", rest);
        return -1;
    }
    int n = control_registry_broadcast_command(CTRL_COMMAND, rest);
    log_info("clientsCmd : \"%s\" diffusée à %d session(s)\n", rest, n);
    return 0;
}

/** @brief Interpréteur de `clientsPause` : sucre pour `clientsCmd pause`. */
int clients_pause_interpreter(void) {
    int n = control_registry_broadcast_command(CTRL_COMMAND, "pause");
    log_info("clientsPause : diffusée à %d session(s)\n", n);
    return 0;
}

/** @brief Interpréteur de `clientsResume` : sucre pour `clientsCmd resume`. */
int clients_resume_interpreter(void) {
    int n = control_registry_broadcast_command(CTRL_COMMAND, "resume");
    log_info("clientsResume : diffusée à %d session(s)\n", n);
    return 0;
}

/** @brief Interpréteur de `printanalysed` : affiche les possibilités en cours d'analyse. */
int printanalysed_interpreter(void) {
    return print_all_file_analysed();
}

/** @brief Interpréteur de `restockanalysed` : remet les possibilités en cours d'analyse dans le stock. */
int restockanalysed_interpreter(void) {
    return restock_analysed();
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
        if (instruction == NULL) {
            /* Ligne ne contenant que des espaces : rien à exécuter. */
            free(toSplit);
            return 0;
        }
        command_description *command_desc = find_command(instruction);
        if (command_desc != NULL) {
            result = command_desc->interpreter();
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
