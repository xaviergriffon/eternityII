#include "console.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "static_variables.h"
#include "datamanager.h"
#include "readdata.h"


#define EXIT_CMD "exit"

static char * getcmdline() {
    char * line = malloc(100), *linep = line;
    size_t lenmax = 100, len = lenmax;
    int c;
    
    if (line == NULL)
        return NULL;
    
    for (;;) {
        c = fgetc(stdin);
        if (c == EOF || c == '\n')
            break;
        
        if (--len == 0) {
            len = lenmax;
            char * linen = realloc(linep, lenmax *= 2);
            
            if (linen == NULL) {
                free(linep);
                return NULL;
            }
            line = linen + (line - linep);
            linep = linen;
        }
        
        if ((*line++ = c) == '\n')
            break;
    }
    *line = '\0';
    return linep;
}

void * console(void *param)
{
    int server = *(int *)param;
    // TODO : externaliser ces valeurs
    char *def_file = "./eternityII.back";
    char *def_analyse_file = "./eternityII-in_analyse.back";
    char *buffer = NULL;
    while(buffer == NULL)
    {
        printf("commande :");
        buffer = getcmdline();
        printf("\n");
        if(strcmp(buffer, EXIT_CMD) == 0)
        {
            if(server == 0)
            {
                request = REQUEST_STOP;
                free(buffer);
                buffer= NULL;
                while (1)
                {
                    printf("*");
                    usleep(MICRO_SLEEP);
                }
                
            } else
            {
                break;
            }
        }
        if(strcmp(buffer, "check") == 0)
        {
            printf("%s\n",lastcheck);
        }
        if(strcmp(buffer, "backup") == 0)
        {
            printf("start backup\n");
            backup(def_file);
            backup_analysed(def_analyse_file);
            printf("backup ended\n");
        }
        if(strcmp(buffer, "restore") == 0)
        {
            printf("start restore\n");
            restore(def_file);
            restore_analysed(def_analyse_file);
            printf("backup restore\n");
        }
        if(strcmp(buffer, "import") == 0)
        {
            printf("start import\n");
            import(def_file);
            import_analysed(def_analyse_file);
            printf("backup restore\n");
        }
        if (strcmp(buffer, "loadjson") == 0) {
            printf("load from json\n");
            import_json();
            printf("backup json\n");
        }
        if(strcmp(buffer, "print") == 0)
        {
            printdatamanager();
        }
        if(strcmp(buffer, "sorta") == 0)
        {
            sort_ascending();
        }
        if(strcmp(buffer, "sortd") == 0)
        {
            sort_descending();
        }
        if(strcmp(buffer, "sortdm") == 0)
        {
            sort_descending_mthread();
        }
        if(strcmp(buffer, "split") == 0)
        {
            split_datas();
        }
        if(strcmp(buffer, "regroup") == 0)
        {
            regroup_datas();
        }
        if(strcmp(buffer, "checkdatas") == 0)
        {
            check_datas();
        }
        if(strcmp(buffer, "checkfiles") == 0)
        {
            check_files();
        }
        // TODO : check all_parts pour vérifier calcul rotation
        if(strncmp(buffer, "sortd ", 6) == 0)
        {
            int f = atoi(&buffer[6]);
            sort_d_mono(&f);
        }
        if(strncmp(buffer, "printfile ", 10) == 0)
        {
            int f = atoi(&buffer[10]);
            print_file(f);
        }
        if(strncmp(buffer, "checkfile ", 10) == 0)
        {
            int f = atoi(&buffer[10]);
            check_file(f);
        }
        if(strncmp(buffer, "limit ", 6) == 0)
        {
            int f = atoi(&buffer[6]);
            max_search_by_sec = f;
        }
        if(strcmp(buffer, "checkdirections") == 0)
        {
            if(test_directions() == 0)
            {
                printf("directions : ok\n");
            } else
            {
                printf("directions : NOK !\n");
            }
        }
        if(strcmp(buffer, "rmnonext") == 0)
        {
            struct array_part *apart= read_parts(partsFiles);
            
            struct array_part *rotateParts = rotate_all_parts(apart);
            map_big_array *map_parts = prepare_map_part(rotateParts);
            remove_possibilities_with_no_next(map_parts, rotateParts);
            free_bigarray(map_parts);
            free_array_part(rotateParts);
            free_array_part(apart);
        }
        if(strcmp(buffer, "printanalysed") == 0) {
            print_all_file_analysed();
        }
        if(strcmp(buffer, "min") == 0)
        {
            printf("min :%i\n",search_min_datas());
        }
        if(strcmp(buffer, "help") == 0)
        {
            printf("commands:\n  help\n  check\n  backup\n  restore\n  import\n  print\n  regroup\n  sorta\n  sortd\n  split\n  checkdatas\n  checkfiles\n  checkdirections\n  min\n  rmnonext\n limit n\n exit\n");
        }
        free(buffer);
        buffer= NULL;
    }
    exit(EXIT_SUCCESS);
}


int run_console(int server)
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
        fprintf(stderr, "Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
    return 0;
}
