#include "console.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "logger.h"
#include "static_variables.h"
#include "command_lines.h"

#define EXIT_CMD "exit"

/**
 * @brief Lit une ligne depuis stdin avec buffer extensible.
 *
 * Commence avec un buffer de 100 octets, le double si nécessaire. Arrête la
 * lecture sur EOF ou '\\n'.
 *
 * @return Ligne lue (à libérer par l'appelant), ou NULL en cas d'erreur d'allocation.
 */
static char * getcmdline(void) {
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

/**
 * @brief Thread de la console interactive.
 *
 * Boucle infinie : affiche le prompt, lit une commande, la délègue à
 * `do_command_line`. Appelle `exit(EXIT_SUCCESS)` si `getcmdline` renvoie NULL
 * (EOF/erreur).
 *
 * @param param Non utilisé.
 * @return      Ne retourne pas (exit ou boucle infinie).
 */
void * console(void *param)
{
    status_zone_init();
    char *buffer = NULL;
    while(buffer == NULL)
    {
        log_console("commande :");
        buffer = getcmdline();
        log_console("\n");
        do_command_line(buffer);
        free(buffer);
        buffer= NULL;
    }
    exit(EXIT_SUCCESS);
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
        log_error("Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}
