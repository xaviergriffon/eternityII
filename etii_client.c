#include "etii_client.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
                // Vérification rapide sous mutex, puis relâchement avant l'I/O réseau
                // pour ne pas bloquer autosearch sur pthread_mutex_lock(works_mutex)
                // pendant la durée des échanges TCP (send_possibility_analysed /
                // get_last_possibility peuvent attendre le serveur).
                pthread_mutex_lock(&thread_params[i].works_mutex);
                int need_work = (client_possibility->works == 0);
                pthread_mutex_unlock(&thread_params[i].works_mutex);

                if(need_work)
                {
                    // I/O réseau hors du mutex
                    send_possibility_analysed(client_possibility);
                    // Un pruner consomme vite : on demande un lot pour amortir les
                    // allers-retours TCP ; un client de recherche garde 1 racine
                    array_possibility_packet *aposs = get_last_possibility(client_possibility, pruner_mode ? PRUNER_BATCH_SIZE : 1);
                    if(aposs->size > 0)
                    {
                        // On alimente la pile des possibilités en étude
                        for (int p = 0; p < aposs->size; p++) {
                            add_possibility_analysed(&aposs->possibilities[p], i);
                        }
                        // Réacquisition du mutex uniquement pour la mise à jour de aposs/works
                        pthread_mutex_lock(&thread_params[i].works_mutex);
                        thread_params[i].aposs = aposs;
                        thread_params[i].works = 1;
                        pthread_mutex_unlock(&thread_params[i].works_mutex);
                    } else
                    {
                        free_array_possibility_packet(aposs);
                    }
                }
                else if (client_possibility->socket_id != -1)
                {
                    // Keepalive : un worker occupé sur son stock local ne parle pas
                    // au serveur. Sans ping, le serveur ferme la session après
                    // tcp_timeout secondes d'inactivité (SO_RCVTIMEO) → Broken pipe
                    // à la prochaine I/O. On pingue donc avant l'échéance.
                    time_t now = time(NULL);
                    int interval = (tcp_timeout > 2) ? (tcp_timeout / 2) : 1;
                    if (now - client_possibility->last_socket_activity >= interval)
                    {
                        pthread_mutex_lock(&client_possibility->socket_mutex);
                        if (client_possibility->socket_id != -1) {
                            if (is_connected(client_possibility->socket_id)) {
                                client_possibility->last_socket_activity = now;
                            } else {
                                // Déjà fermée : on oublie le socket, il sera rouvert
                                // au prochain besoin de travail.
                                client_possibility->socket_id = -1;
                            }
                        }
                        pthread_mutex_unlock(&client_possibility->socket_mutex);
                    }
                }
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

/**
 * @brief Thread de contrôle du débit de recherche.
 *
 * Compare le nombre de possibilités traitées par rapport à `max_search_by_sec`.
 * Si le débit dépasse la limite, passe `request` à REQUEST_PAUSE pour ralentir
 * les threads de recherche. Reprend dès que le débit redescend sous la limite.
 * Ne fait rien si `max_search_by_sec == 0` (mode illimité).
 *
 * @param param Tableau de `client_possibility_t` (un par thread de recherche).
 * @return      NULL.
 */
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
                if(thread->works == 1 && thread->aposs != NULL)
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
            if (nbCheck > 0) {
            long double divider = nbCheck / 1000.0L;
            unsigned long long simulationBySec = (unsigned long long)(*oneSecond / divider);
            if (request == REQUEST_CONTINUE && simulationBySec >= max_search_by_sec) {
                request = REQUEST_PAUSE;
            } else {
                if (request == REQUEST_PAUSE && simulationBySec < max_search_by_sec) {
                    request = REQUEST_CONTINUE;
                }
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

/**
 * @brief Démarre le thread de contrôle du débit de recherche en mode détaché.
 * @param thread_params Tableau de contextes de threads de recherche.
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

/**
 * @brief Lance le client en mode multi-thread avec `NB_THREADS` threads de recherche.
 *
 * Crée un thread `autosearch` par valeur de `NB_THREADS`, un thread d'alimentation
 * (`feed_thread_aposs`) et attend que tous les threads aient terminé avant de retourner.
 *
 * @param file Chemin du fichier CSV de définition des pièces.
 */
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
        pthread_mutex_t smutex = PTHREAD_MUTEX_INITIALIZER;
        thread_params[i].socket_mutex = smutex;
        thread_params[i].last_socket_activity = time(NULL);
        times(&thread_params[i].start_socket);
        if (0 != pthread_create((thread_params[i].tid), thread_attributes, pruner_mode ? autoprune : autosearch, &(thread_params[i])))
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
        pthread_mutex_lock(&thread_params[i].socket_mutex);
        if (thread_params[i].socket_id != -1 && is_connected(thread_params[i].socket_id)) {
            close_socket(thread_params[i].socket_id);
        }
        pthread_mutex_unlock(&thread_params[i].socket_mutex);
    }
}

/**
 * @brief Lance le client en mode mono-thread (un seul thread `autosearch`).
 *
 * Mode utilisé par les processus enfants issus du fork. Initialise le contexte
 * client, démarre les threads d'alimentation et de contrôle, puis exécute
 * `autosearch` dans le thread courant.
 *
 * @param file Chemin du fichier CSV de définition des pièces.
 */
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
    pthread_mutex_t smutex = PTHREAD_MUTEX_INITIALIZER;
    thread_params->socket_mutex = smutex;
    thread_params->last_socket_activity = time(NULL);
    times(&thread_params->start_socket);
    free_array_part(apart);
    
    build_feed_thread(thread_params);
    build_control_thread(thread_params);
    if (pruner_mode) {
        autoprune(thread_params);
    } else {
        autosearch(thread_params);
    }

    pthread_mutex_lock(&thread_params->socket_mutex);
    if (thread_params->socket_id != -1 && is_connected(thread_params->socket_id)) {
        close_socket(thread_params->socket_id);
    }
    pthread_mutex_unlock(&thread_params->socket_mutex);
}

/**
 * @brief Thread de statistiques du client (lancé par `run_checker`).
 *
 * Toutes les 10 secondes, collecte et formate dans `lastcheck` :
 * - la taille de chaque file de possibilités,
 * - les statistiques de chaque processus fork (vitesse, stock),
 * - le nombre global de traitements par seconde et le meilleur résultat atteint.
 *
 * @param param Non utilisé.
 * @return      NULL (boucle infinie).
 */
void *check_client_threads(void *param)
{
    int sleep_time = 10;
    int last_record = max_result;
    while(1)
    {
        free(lastcheck);
        lastcheck = calloc(4000, sizeof(char));
        
        // Côté client, le travail tourne dans les processus fork (mémoire séparée
        // après fork) : les files locales du parent sont vides. La donnée réelle
        // est dans fork_statistics[], par fork. On présente donc un tableau par
        // fork : stock local en cours d'étude et possibilités en cours d'analyse.
        unsigned long long fork_possibility_stock = 0;
        unsigned long long fork_analysed_stock = 0;
        char *table = calloc(2000, sizeof(char));
        int table_offset = sprintf(table,
            "Thread queues\n"
            "Fork |     In stock |     Analysed\n"
            "-----+--------------+-------------\n");
        unsigned long long bys = 0;
        int f;
        for(f=0; f < NB_THREADS; f++)
        {
            if (fork_statistics[f].max_result > max_result) {
                max_result = fork_statistics[f].max_result;
            }

            bys += fork_statistics[f].shots_per_second;
            unsigned long long in_stock = fork_statistics[f].possibilities_in_stock;
            unsigned long long analysed = fork_statistics[f].analyses_in_stock;
            table_offset += sprintf(table + table_offset, "%4i | %12llu | %12llu\n",
                                    f, in_stock, analysed);
            fork_possibility_stock += in_stock;
            fork_analysed_stock += analysed;
        }
        table_offset += sprintf(table + table_offset,
                                "-----+--------------+-------------\n"
                                "Total| %12llu | %12llu\n",
                                fork_possibility_stock, fork_analysed_stock);
        strcat(lastcheck, table);
        free(table);
                
#if FORWARD_CHECK_K > 0
        // Forward-checking : agrégat des forks + compteurs du processus courant
        // (ces derniers couvrent les modes test et DEBUG_IN_MONO_PROCESS).
        unsigned long long fca = __atomic_load_n(&fc_attempts, __ATOMIC_RELAXED);
        unsigned long long fcp = __atomic_load_n(&fc_pruned, __ATOMIC_RELAXED);
        unsigned long long fcd[FORWARD_CHECK_K + 1];
        for (int j = 1; j <= FORWARD_CHECK_K; j++) {
            fcd[j] = __atomic_load_n(&fc_pruned_at[j], __ATOMIC_RELAXED);
        }
        for (f = 0; f < NB_THREADS; f++) {
            fca += fork_statistics[f].fc_attempts;
            fcp += fork_statistics[f].fc_pruned;
            for (int j = 1; j <= FORWARD_CHECK_K; j++) {
                fcd[j] += fork_statistics[f].fc_pruned_at[j];
            }
        }
        if (fca > 0) {
            char *fctemp = calloc(1000, sizeof(char));
            int fcoff = sprintf(fctemp, "forward-check K=%d : pruned %llu/%llu (%.2f%%), par distance :",
                                FORWARD_CHECK_K, fcp, fca, 100.0 * (double)fcp / (double)fca);
            for (int j = 1; j <= FORWARD_CHECK_K; j++) {
                fcoff += sprintf(fctemp + fcoff, " d%d:%.1f%%", j,
                                 fcp > 0 ? 100.0 * (double)fcd[j] / (double)fcp : 0.0);
            }
            sprintf(fctemp + fcoff, "\n");
            strcat(lastcheck, fctemp);
            free(fctemp);
        }
#endif // FORWARD_CHECK_K > 0

        // Statistiques pruner : agrégat des forks + compteurs du processus courant
        unsigned long long prc = pruner_checked;
        unsigned long long prr = pruner_removed;
        for (f = 0; f < NB_THREADS; f++) {
            prc += fork_statistics[f].pruner_checked;
            prr += fork_statistics[f].pruner_removed;
        }
        if (prc + prr > 0) {
            char *prtemp = calloc(200, sizeof(char));
            sprintf(prtemp, "pruner : %llu mortes / %llu vérifiées (%.2f%%)\n",
                    prr, prc + prr, 100.0 * (double)prr / (double)(prc + prr));
            strcat(lastcheck, prtemp);
            free(prtemp);
        }

        char *temp = calloc(1000, sizeof(char));
        sprintf(temp, "active thread/s :%lli\npossibility in stock :%lli (analysed:%llu)\nmax search by sec : %lli\nmax stock by thread : %i\nmax result :%i\n",
            bys,
            fork_possibility_stock,
            fork_analysed_stock,
            max_search_by_sec, max_stock_by_thread, max_result);
#ifdef DEBUG_SOCKET
        sprintf(temp, "%ssocket opened :%i\n", temp, opened_tcp);
#endif // DEBUG_SOCKET
        strcat(lastcheck, temp);
        free(temp);

        /* Bandeau de stats « live » : résumé compact poussé à chaque tour.
           En mode ncurses il s'affiche en continu ; en mode ANSI, no-op. */
        {
            char limit_str[24];
            if (max_search_by_sec > 0) {
                snprintf(limit_str, sizeof limit_str, "%llu", (unsigned long long)max_search_by_sec);
            } else {
                snprintf(limit_str, sizeof limit_str, "-");
            }
            log_status(" coups/s:%llu  stock:%llu  analyse:%llu  record:%i/%i  limite:%s ",
                       bys, fork_possibility_stock, fork_analysed_stock,
                       max_result, ETERN_PARTS, limit_str);
        }

        if (max_result > last_record) {
            last_record = max_result;
            log_event("new record: %i pieces placed", max_result);
        }

        sleep(sleep_time);
    }

    return NULL;
}
