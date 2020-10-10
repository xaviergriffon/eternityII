//
//  etii_client.c
//  eternityII
//
//  Created by Xavier GRIFFON on 04/10/2020.
//  Copyright © 2020 Xavier GRIFFON. All rights reserved.
//
#include <unistd.h>
#include <stdlib.h>
#include "etii_client.h"
#include "readdata.h"
#include "datamanager.h"
#include "etii_search.h"

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
        thread_params[i].request = 0;
        
        /* création d'un nouveau thread */
        pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
        pthread_attr_init(thread_attributes);
        pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
        
        /* Création du thread */
        thread_params[i].tid = malloc(sizeof(pthread_t));
        // searchOpenCL
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
    
    while (1)
    {
        int threadworking = 0;
        for(i = 0; i < NB_THREADS; i++)
        {
            if(request == 0)
            {
                if(thread_params[i].works == 0)
                {
                    array_possibility_packet *aposs = get_last_possibility(1);
                    if(aposs->size > 0)
                    {
                        thread_params[i].aposs = aposs;
                        thread_params[i].works = 1;;
                    } else
                    {
                        free(aposs);
                    }
                }
            } else
            {
                if(thread_params[i].works == 1)
                {
                    thread_params[i].request = request;
                    threadworking++;
                }
            }
        }
        
        if(request == REQUEST_STOP && threadworking == 0)
        {
            exit(EXIT_SUCCESS);
        }
        usleep(THREAD_MICRO_SLEEP);
    }
}
