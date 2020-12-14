#include "command_lines.h"
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "datamanager.h"
#include "local_socket.h"
#include "readdata.h"

#define DEF_FILE "./eternityII.back"
#define DEF_ANALYSE_FILE "./eternityII-in_analyse.back"
#define NB_COMMANDS 25

typedef struct
{
    char *command;
    int (*interpreter)(void);
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
int checkfiles_interpreter(void);
int printfile_interpreter(void);
int checkfile_interpreter(void);
int checkdirections_interpreter(void);
int rmnonext_interpreter(void);
int printanalysed_interpreter(void);
int min_interpreter(void);
int help_interpreter(void);
int statistic_interpreter(void);

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

int sort_ascending_interpreter(void) {
    return sort_ascending();
}

int sort_descending_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments == NULL) {
        return sort_descending();
    }
    
    int n_file = atoi(arguments);
    sort_d_mono(&n_file);
    return 0;
}

int max_stock_by_thread_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_stock_by_thread = atoi(arguments);
        return 0;
    }
    return -1;
}

int limit_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        max_search_by_sec = atoi(arguments);
        return 0;
    }
    
    return -1;
}

int check_interpreter(void) {
    printf("%s\n",lastcheck);
    return 0;
}

int backup_interpreter(void) {
    printf("start backup\n");
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
    printf("backup ended\n");
    if (isServer == 0) {
        free(def_file);
        free(def_analyse_file);
    }
    return 0;
}

int exit_interpreter(void) {
    request = REQUEST_STOP;
    if (server == 0) {
        if (parent_pid == getpid()) {
            if (childrens_pid != NULL) {
                for (int c = 0; c < NB_THREADS; c++) {
                    if (childrens_pid[c] > 0) {
                        kill(childrens_pid[c], SIGINT);
                    }
                }
            }
            
            int cptloop = 0;
            while (1)
            {
                if (cptloop == 10) {
                    printf("\r            ");
                    printf("\r");
                    cptloop = 0;
                }
                printf("*");
                cptloop++;
                usleep(MICRO_SLEEP);
            }
        }
    } else  {
        exit(EXIT_SUCCESS);
    }
    
    return 0;
}

int restore_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    printf("start restore\n");
    restore(def_file);
    restore_analysed(def_analyse_file);
    printf("backup restore\n");
    return 0;
}

int restoreOld_interpreter(void) {
#ifdef FACES_USED_BITS
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    printf("start restore\n");
    restore_old_file(def_file);
    restore_analysed(def_analyse_file);
    printf("backup restore\n");
    
#endif // FACES_USED_BITS
    return 0;
}

int import_interpreter(void) {
    char *def_file = DEF_FILE;
    char *def_analyse_file = DEF_ANALYSE_FILE;
    printf("start import\n");
    import(NULL, def_file);
    import_analysed(def_analyse_file);
    printf("backup restore\n");
    
    return 0;
}

int loadjson_interpreter(void) {
    printf("load from json\n");
    import_json();
    printf("backup json\n");
    
    return 0;
}

int print_interpreter(void) {
    return printdatamanager();
}

int sortdm_interpreter(void) {
    return sort_descending_mthread();
}

int split_interpreter(void) {
    return split_datas();
}

int regroup_interpreter(void) {
    return regroup_datas();
}

int checkdatas_interpreter(void) {
    return check_datas();
}

int statistic_interpreter(void) {
    return statistic_datas();
}

int checkfiles_interpreter(void) {
    return check_files();
}

int printfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        return print_file(atoi(arguments));
    }
    
    return -1;
}

int checkfile_interpreter(void) {
    char *arguments = strtok(NULL, " ");
    if (arguments != NULL) {
        return check_file(atoi(arguments));
    }
    
    return -1;
}

int checkdirections_interpreter(void) {
    if(test_directions() == 0)
    {
        printf("directions : ok\n");
    } else
    {
        printf("directions : NOK !\n");
    }
    
    return 0;
}

int rmnonext_interpreter(void) {
    struct array_part *apart= read_parts(partsFiles);
    
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map_parts = prepare_map_part(rotateParts);
    remove_possibilities_with_no_next(map_parts, rotateParts);
    free_bigarray(map_parts);
    free_array_part(rotateParts);
    free_array_part(apart);
    
    return 0;
}

int printanalysed_interpreter(void) {
    return print_all_file_analysed();
}

int min_interpreter(void) {
    printf("min : %i\n",search_min_datas());
    
    return 0;
}

int help_interpreter(void) {
    char *help = calloc(NB_COMMANDS * 100, sizeof(char));
    strcat(help, "commands :\n");
    for (int c = 0; c < NB_COMMANDS; c++) {
        strcat(help, "  ");
        strcat(help, commands[c].command);
        strcat(help, "\n");
    }
    printf("%s", help);
    free(help);
    return 0;
}

command_description *find_command(const char *instruction) {
    for (int c =0; c < NB_COMMANDS; c++) {
        command_description *command = &commands[c];
        if (strcmp(command->command, instruction) == 0) {
            return command;
        }
    }
    return NULL;
}

int do_command_line(char *command) {
    int result = 0;
    if (command != NULL && strlen(command) > 0) {
        size_t command_length = strlen(command) + 1;
        char *toSplit = malloc(sizeof(char) * command_length);
        strncpy(toSplit, command, command_length);
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
