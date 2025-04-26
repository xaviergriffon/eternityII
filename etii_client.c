#include "etii_client.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "readdata.h"
#include "datamanager.h"
#include "etii_search.h"
#include "etii_protocol.h"

/**
 * @brief Méthode chargée d'alimenter les threads quand lors file est à 0
 */
void *feed_thread_aposs(void *param) {
    client_possibility_t *thread_params = param;
#ifdef DEBUG_THREAD
    log_info("START aposs thread %i\n", getpid());
#endif // DEBUG_THREAD
    while (request == REQUEST_CONTINUE || request == REQUEST_PAUSE) {
        for(int i = 0; i < NB_THREADS; i++)
        {
            if(request == REQUEST_CONTINUE)
            {
                client_possibility_t *client_possibility = &thread_params[i];
                pthread_mutex_lock(&thread_params[i].works_mutex);
                if(client_possibility->works == 0)
                {
                    send_possibility_analysed(client_possibility);
                    array_possibility_packet *aposs = get_last_possibility(client_possibility, 1);
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
                        free_array_possibility_packet(aposs);
                    }
                }
                pthread_mutex_unlock(&thread_params[i].works_mutex);
            }
        }
        
        usleep(THREAD_MICRO_SLEEP);
    }
#ifdef DEBUG_THREAD
    log_info("END aposs thread %i\n", getpid());
#endif // DEBUG_THREAD
    return NULL;
}

/**
 * @brief Construit un thread d'alimentation des files des threads de recherche
 */
void build_feed_thread(client_possibility_t *thread_params) {
    /* création d'un nouveau thread */
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    
    /* Création du thread */
    if (0 != pthread_create(&thread, thread_attributes, feed_thread_aposs, thread_params))
    {
        log_error("Problème avec pthread_create()\n");
        free(thread_attributes);
        exit(EXIT_FAILURE);
    }
    pthread_attr_destroy(thread_attributes);
    free(thread_attributes);
}

void *control_thread(void *param) {
    if (NB_THREADS <= 0) {
        return NULL;
    }
#ifdef DEBUG_THREAD
    log_info("START control thread %i\n", getpid());
#endif // DEBUG_THREAD
    client_possibility_t *thread_params = param;
    unsigned long long *lastCheck = malloc(sizeof(unsigned long long) * NB_THREADS);
    int t;
    for (t = 0; t < NB_THREADS; t++)
    {
        lastCheck[t] = 0;
    }
    
    unsigned long long *oneSecond = malloc(sizeof(unsigned long long));
    *oneSecond = 0;
    int nbCheck = 0;
    while (request == REQUEST_CONTINUE || request == REQUEST_PAUSE) {
        if(max_search_by_sec > 0) {
            for(t = 0; t < NB_THREADS; t++)
            {
                client_possibility_t *thread = &thread_params[t];
                if(thread->works == 1 && thread->aposs > 0)
                {
                    unsigned long long inMillis = 0;
                    if (counters[t] >= lastCheck[t]) {
                        inMillis = counters[t] - lastCheck[t];
                    } else {
                        // le compteur a fait un tour
                        inMillis = ((inMillis - 1) - lastCheck[t]) + counters[t];
                    }
                    
                    lastCheck[t] = counters[t];
                    *oneSecond = *oneSecond + inMillis;
                } else {
                    // TODO : pourquoi révéiller les threads ici ?
                    if (request == REQUEST_PAUSE) {
                        request = REQUEST_CONTINUE;
                    }
                }
            }
            long double divider = nbCheck / 1000.0;
            unsigned long long simulationBySec = *oneSecond / divider;
            if (request == REQUEST_CONTINUE && simulationBySec >= max_search_by_sec) {
                request = REQUEST_PAUSE;
            } else {
                if (request == REQUEST_PAUSE && simulationBySec < max_search_by_sec) {
                    request = REQUEST_CONTINUE;
                }
            }
        }
        
        if (nbCheck > 1000) {
            nbCheck = 0;
            *oneSecond = 0;
            if (request == REQUEST_PAUSE) {
                request = REQUEST_CONTINUE;
            }
        } else {
            nbCheck++;
        }
        // La priorité est au traitement lors on effectue des controles espacés.
        usleep(1000);
    }
#ifdef DEBUG_THREAD
    log_info("END control thread %i\n", getpid());
#endif // DEBUG_THREAD
    free(lastCheck);
    free(oneSecond);
    return NULL;
}

/* 
 * Construit un thread chargé de controler le nombre de recherche par seconde
 */
void build_control_thread(client_possibility_t *thread_params) {
    /* création d'un nouveau thread */
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
    pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    
    /* Création du thread */
    if (0 != pthread_create(&thread, thread_attributes, control_thread, thread_params))
    {
        log_error("Problème avec pthread_create()\n");
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
        log_error("Problème avec malloc()\n");
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
        thread_params[i].socket_id = -1;
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        thread_params[i].works_mutex = mutex;
        times(&thread_params[i].start_socket);
        if (0 != pthread_create((thread_params[i].tid), thread_attributes, autosearch, &(thread_params[i])))
        {
            log_error("Problème avec pthread_create()\n");
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
        // Lorsque l'instruction est stop, on attend que tous les threads soient terminés
        if(request == REQUEST_STOP && threadworking == 0)
        {
            break;
        }
        usleep(THREAD_MICRO_SLEEP);
    }

    // Fermeture des connections
    for (i = 0; i < NB_THREADS; i++) {
        if (thread_params[i].socket_id != -1 && is_connected(thread_params[i].socket_id)) {
            close_socket(thread_params[i].socket_id);
        }
    }
}

void run_mono_client(const char *file)
{
    client_possibility_t *thread_params = malloc(sizeof(*thread_params));
    
    struct array_part *apart= read_parts(file);
    thread_params->works = 0;
    thread_params->aposs = NULL;
    struct array_part *rotateParts = rotate_all_parts(apart);
    thread_params->all_rotate_part =rotateParts;
    thread_params->map_part = prepare_map_part(rotateParts);
    thread_params->tid = NULL;
    thread_params->id = 0;
    thread_params->pid = getpid();
    thread_params->compteur = 0;
    thread_params->max_shots_per_second = -1;
    thread_params->socket_id = -1;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    thread_params->works_mutex = mutex;
    times(&thread_params->start_socket);
    free_array_part(apart);
    
    build_feed_thread(thread_params);
    build_control_thread(thread_params);
    autosearch(thread_params);

    if (thread_params->socket_id != -1 && is_connected(thread_params->socket_id)) {
        close_socket(thread_params->socket_id);
    }
}

void *check_client_threads(void *param)
{
    int sleep_time = 10;
    while(1)
    {
        free(lastcheck);
        lastcheck = calloc(2000, sizeof(char));
        
        unsigned long long file_possibility_stock = 0;
        int f;
        for(f=0; f < NB_FILE_POSSIBILITY; f++)
        {
            unsigned long long f_size = file_size(f);
            char *temp = calloc(1000, sizeof(char));
            sprintf(temp, "file:%i stock:%llu\n", f, f_size);
            strcat(lastcheck, temp);
            free(temp);
            file_possibility_stock = file_possibility_stock + f_size;
        }
        
        unsigned long long bys = 0;
        for(f=0; f < NB_THREADS; f++)
        {
            char *temp = calloc(1000, sizeof(char));
            sprintf(temp, "Fork %i file size:%llu\n", f, fork_statistics[f].possibilities_in_stock);
            strcat(lastcheck, temp);
            free(temp);
            if (fork_statistics[f].max_result > max_result) {
                max_result = fork_statistics[f].max_result;
            }
            
            bys += fork_statistics[f].shots_per_second;
        }
                
        char *temp = calloc(1000, sizeof(char));
        sprintf(temp, "active thread/s :%lli\npossibility in stock :%lli\nmax search by sec : %lli\nmax stock by thread : %i\nmax result :%i\n",
            bys, file_possibility_stock, max_search_by_sec, max_stock_by_thread, max_result);
#ifdef DEBUG_SOCKET
        sprintf(temp, "%ssocket opened :%i\n", temp, opened_tcp);
#endif // DEBUG_SOCKET
        strcat(lastcheck, temp);
        free(temp);
        
        sleep(sleep_time);
    }
    
    return NULL;
}
