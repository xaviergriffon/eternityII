#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "etii_client.h"
#include "readdata.h"
#include "datamanager.h"
#include "etii_search.h"

// Méthode chargée d'alimenter les threads quand lors file est à 0
void *feed_thread_aposs(void *param) {
    client_possibility_t *thread_params = param;
    while (request == REQUEST_CONTINUE) {
        for(int i = 0; i < NB_THREADS; i++)
        {
            if(request == REQUEST_CONTINUE)
            {
                if(thread_params[i].works == 0)
                {
                    send_possibility_analysed(i);

                    array_possibility_packet *aposs = get_last_possibility(1);
                    if(aposs->size > 0)
                    {
                        // On alimente la pile des poissiblités en étude
                        for (int p = 0; p < aposs->size; p++) {
                            add_possibility_analysed(&aposs->possibilities[p], i);
                        }
                        thread_params[i].aposs = aposs;
                        thread_params[i].works = 1;;
                        //printf("alimentation thread %i\n", i);
                    } else
                    {
                        free(aposs);
                    }
                }
            }
        }
        
        usleep(THREAD_MICRO_SLEEP);
    }
    
    return NULL;
}

// Construit un thread d'alimentation des files des threads de recherche
void build_feed_thread(client_possibility_t *thread_params) {
    /* création d'un nouveau thread */
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    
    /* Création du thread */
    if (0 != pthread_create(&thread, thread_attributes, feed_thread_aposs, thread_params))
    {
        fprintf(stderr, "Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}

void runThreadClient(const char *file)
{
    client_possibility_t *thread_params;
    int i;
    
    /* création du tableau de structures client_possibility_t avec un élément par thread */
    if(NULL == (thread_params = malloc(sizeof(*thread_params) * NB_THREADS)))
    {
        fprintf(stderr, "Problème avec malloc()\n");
        exit(EXIT_FAILURE);
    }
    struct array_part *apart= read_parts(file);
    for(i = 0; i < NB_THREADS; i++)
    {
        thread_params[i].works = 0;
        thread_params[i].aposs = NULL;
        struct array_part *rotateParts = rotate_all_parts(apart);
        thread_params[i].all_rotate_part =rotateParts;
        thread_params[i].map_part = prepare_map_part(rotateParts);
        thread_params[i].tid = NULL;
        thread_params[i].compteur = i;
        thread_params[i].max_shots_per_second = -1;
        
        /* création d'un nouveau thread */
        pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
        pthread_attr_init(thread_attributes);
        pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
        
        /* Création du thread */
        thread_params[i].tid = malloc(sizeof(pthread_t));
        thread_params[i].id = i;
        if (0 != pthread_create((thread_params[i].tid), thread_attributes, autosearch, &(thread_params[i])))
        {
            fprintf(stderr, "Problème avec pthread_create()\n");
            free(thread_attributes);
            exit(EXIT_FAILURE);
        }
        pthread_attr_destroy(thread_attributes);
        free(thread_attributes);
    }
    free_array_part(apart);
    
    build_feed_thread(thread_params);
    
    while (1)
    {
        int threadworking = 0;
        for(i = 0; i < NB_THREADS; i++)
        {
            if(thread_params[i].works == 1)
            {
                threadworking++;
            }
        }
        
        if(request == REQUEST_STOP && threadworking == 0)
        {
            break;
        }
        usleep(THREAD_MICRO_SLEEP);
    }
}

void runMonoClient(const char *file)
{
    client_possibility_t *thread_params = malloc(sizeof(*thread_params));
    
    struct array_part *apart= read_parts(file);
    thread_params->works = 0;
    thread_params->aposs = NULL;
    struct array_part *rotateParts = rotate_all_parts(apart);
    thread_params->all_rotate_part =rotateParts;
    thread_params->map_part = prepare_map_part(rotateParts);
    thread_params->tid = NULL;
    thread_params->compteur = 0;
    thread_params->max_shots_per_second = -1;
    free_array_part(apart);
    
    build_feed_thread(thread_params);
    autosearch(thread_params);    
}


void *check_client_threads(void *param)
{
    unsigned long long lastactive = 0;
    int sleep_time = 10;
    while(1)
    {
        free(lastcheck);
        lastcheck = calloc(1000, sizeof(char));
        unsigned long long currentactive = lastactive;
        int c;
        lastactive = 0;
        for(c=0; c < NB_THREADS;c++)
        {
            lastactive = lastactive + compteurs[c];
        }
        currentactive = lastactive - currentactive;
        getted_possibility_not_null = lastactive;
        
        unsigned long long file_possibility_stock = 0;
        int f;
        for(f=0; f < NB_FILE_POSSIBILITY; f++)
        {
            int f_size = file_size(f);
            char *temp = calloc(1000, sizeof(char));
            sprintf(temp, "file:%i stock:%i\n",f,f_size);
            strcat(lastcheck, temp);
            free(temp);
            file_possibility_stock = file_possibility_stock + f_size;
        }
        for(f=0; f < NB_THREADS; f++)
        {
            char *temp = calloc(1000, sizeof(char));
            sprintf(temp, "Thread %i file size:%i\n",f,lastfilesize[f]);
            strcat(lastcheck, temp);
            free(temp);
        }
        unsigned long long bys = currentactive / sleep_time;
        char *temp = calloc(1000, sizeof(char));
        sprintf(temp, "active thread last %isec :%lli\nactive thread/s :%lli\npossibility in stock :%lli\ngetted possibility not null :%lli\nmax result :%i\n",sleep_time,currentactive, bys,file_possibility_stock,getted_possibility_not_null, max_result);
        strcat(lastcheck, temp);
        free(temp);
        
        sleep(sleep_time);
    }
    
    return NULL;
}
