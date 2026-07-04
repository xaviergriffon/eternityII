#include "app/etii_client.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include "ui/logger.h"
#include "core/readdata.h"
#include "core/datamanager.h"
#include "core/etii_search.h"
#include "net/etii_protocol.h"

#ifdef WITH_CUDA
#include "app/gpu_pruner.h"
#endif // WITH_CUDA

useconds_t next_no_work_sleep(useconds_t current) {
    if (current == 0) return NO_WORK_SLEEP_START;
    if (current >= NO_WORK_SLEEP_MAX) return NO_WORK_SLEEP_MAX;
    useconds_t doubled = current * 2;
    return doubled > NO_WORK_SLEEP_MAX ? NO_WORK_SLEEP_MAX : doubled;
}

void init_client_possibility(client_possibility_t *p, struct array_part *rotateParts,
                             map_big_array *map, int id, int compteur, pid_t pid) {
    p->works = 0;
    p->aposs = NULL;
    p->all_rotate_part = rotateParts;
    p->map_part = map;
    p->tid = NULL;
    p->id = id;
    p->pid = pid;
    p->compteur = compteur;
    p->max_shots_per_second = -1;
    p->socket_id = -1;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    p->works_mutex = mutex;
    pthread_mutex_t smutex = PTHREAD_MUTEX_INITIALIZER;
    p->socket_mutex = smutex;
    p->last_socket_activity = time(NULL);
    p->delegate_buf = NULL;
    p->delegate_buf_capacity = 0;
    times(&p->start_socket);
}

int count_created_forks(pid_t *pids, int nb) {
    int created = 0;
    for (int c = 0; c < nb; c++) {
        if (pids[c] > 0) created++;
    }
    return created;
}

int find_fork_index(const char *sun_path, char **forkIds, int nb) {
    for (int i = 0; i < nb; i++) {
        if (strcmp(sun_path, forkIds[i]) == 0) return i;
    }
    return -1;
}

/**
 * @brief Méthode chargée d'alimenter les threads quand lors file est à 0
 */
/**
 * @brief Alimente un thread de recherche en travail (un tour de la boucle `for`
 *        de `feed_thread_aposs`).
 *
 * Extrait du corps de boucle pour être testable hors thread (en mode local,
 * `server_ip == NULL`, les échanges passent par le datamanager). Ne fait rien si
 * `request != REQUEST_CONTINUE`. Si le thread `i` n'a plus de travail
 * (`works == 0`), draine son « en analyse » puis tente d'obtenir une (ou un lot
 * de) possibilité(s) ; s'il en reçoit, les empile et passe `works = 1`. Sinon,
 * s'il a un socket ouvert, émet un keepalive avant le timeout serveur. Les
 * compteurs `*needed_work` / `*got_work` (threads ayant demandé / reçu du
 * travail) sont incrémentés en place pour piloter le back-off de l'appelant.
 *
 * @param thread_params Tableau des contextes de threads de recherche.
 * @param i             Indice du thread à alimenter.
 * @param needed_work   Compteur in/out des threads ayant réclamé du travail.
 * @param got_work      Compteur in/out des threads ayant reçu du travail.
 */
void feed_one_thread(client_possibility_t *thread_params, int i,
                     int *needed_work, int *got_work)
{
    if (request != REQUEST_CONTINUE)
    {
        return;
    }
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
        (*needed_work)++;
        // I/O réseau hors du mutex
        send_possibility_analysed(client_possibility);
        // Un pruner consomme vite : on demande un lot pour amortir les
        // allers-retours TCP ; un client de recherche garde 1 racine
        array_possibility_packet *aposs = get_last_possibility(client_possibility, pruner_mode ? pruner_batch_size : 1);
        if(aposs->size > 0)
        {
            (*got_work)++;
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

void *feed_thread_aposs(void *param) {
    client_possibility_t *thread_params = param;
#ifdef DEBUG_THREAD
    log_info("START aposs thread %i\n", getpid());
#endif // DEBUG_THREAD
    // Pause courante quand le serveur n'a rien à fournir (0 = pas de back-off
    // en cours, on utilise alors la cadence normale THREAD_MICRO_SLEEP).
    useconds_t no_work_sleep = 0;
    while (request == REQUEST_CONTINUE || request == REQUEST_PAUSE) {
        int needed_work = 0; // threads ayant demandé du travail ce tour
        int got_work = 0;    // threads ayant effectivement reçu du travail
        for(int i = 0; i < NB_THREADS; i++)
        {
            feed_one_thread(thread_params, i, &needed_work, &got_work);
        }

        // Pause adaptative : si des threads attendaient du travail mais que le
        // serveur n'a RIEN fourni à aucun (stock épuisé, ou serveur saturé qui
        // ne répond pas), on cède plus longtemps le CPU et on lève la pression
        // réseau qui nourrit la contention « all threads busy » côté serveur. La
        // pause double à chaque cycle à vide jusqu'à NO_WORK_SLEEP_MAX, et se
        // réarme dès qu'un travail est obtenu (ou qu'aucun thread n'en réclame).
        if (needed_work > 0 && got_work == 0)
        {
            no_work_sleep = next_no_work_sleep(no_work_sleep);
            // Découpage en tranches pour que REQUEST_STOP interrompe le back-off
            // sans attendre la totalité de no_work_sleep (jusqu'à 500 ms).
            useconds_t remaining = no_work_sleep;
            while (remaining > 0 && (request == REQUEST_CONTINUE || request == REQUEST_PAUSE))
            {
                useconds_t step = remaining < THREAD_MICRO_SLEEP ? remaining : THREAD_MICRO_SLEEP;
                usleep(step);
                remaining -= step;
            }
        }
        else
        {
            no_work_sleep = 0;
            usleep(THREAD_MICRO_SLEEP);
        }
    }
#ifdef DEBUG_THREAD
    log_info("END aposs thread %i\n", getpid());
#endif // DEBUG_THREAD
    return NULL;
}

/**
 * @brief Construit un thread d'alimentation des files des threads de recherche.
 *
 * Le thread est créé JOIGNABLE (pas détaché) : l'appelant DOIT le joindre à
 * l'arrêt (cf. run_mono_client). Sans cela, ce thread continuait de tourner
 * pendant que `run_mono_client` revenait et que le process se démontait —
 * accès concurrents aux structures partagées en cours de libération, source du
 * « double free » intermittent observé à l'arrêt.
 *
 * @return Identifiant du thread créé (à passer à pthread_join).
 */
pthread_t build_feed_thread(client_possibility_t *thread_params) {
    /* création d'un nouveau thread */
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
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
    return thread;
}

/**
 * @brief Un tour de régulation du débit (corps extrait de control_thread).
 *
 * Voir etii_client.h pour le contrat.
 */
void control_step(client_possibility_t *thread_params,
                  unsigned long long *lastCheck,
                  unsigned long long *oneSecond,
                  int *nbCheck)
{
    if (max_search_by_sec > 0) {
        for (int t = 0; t < NB_THREADS; t++)
        {
            client_possibility_t *thread = &thread_params[t];
            if (thread->works == 1 && thread->aposs != NULL)
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
                /* Un thread inactif (pas de possibilité en cours) ne contribue
                   pas au débit mesuré : le freiner n'a aucun effet régulateur.
                   Le garder en pause serait même contre-productif —
                   feed_one_thread() refuse de réclamer du travail au serveur
                   tant que request != REQUEST_CONTINUE, donc la pause bloque
                   aussi la demande réseau, pas seulement la recherche. On lève
                   donc la pause pour que le thread d'alimentation puisse lui
                   récupérer une possibilité dès le prochain cycle, sans
                   attendre la prochaine régulation. */
                if (request == REQUEST_PAUSE) {
                    request = REQUEST_CONTINUE;
                }
            }
        }
        if (*nbCheck > 0) {
            long double divider = *nbCheck / 1000.0L;
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

    if (*nbCheck > 1000) {
        *nbCheck = 0;
        *oneSecond = 0;
        if (request == REQUEST_PAUSE) {
            request = REQUEST_CONTINUE;
        }
    } else {
        (*nbCheck)++;
    }
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
        control_step(thread_params, lastCheck, oneSecond, &nbCheck);
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
 * @brief Démarre le thread de contrôle du débit de recherche (joignable).
 *
 * Comme le thread d'alimentation, il est créé JOIGNABLE : l'appelant le joint à
 * l'arrêt pour garantir qu'il ne tourne plus quand le process se démonte.
 *
 * @param thread_params Tableau de contextes de threads de recherche.
 * @return Identifiant du thread créé (à passer à pthread_join).
 */
pthread_t build_control_thread(client_possibility_t *thread_params) {
    /* création d'un nouveau thread */
    pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
    pthread_attr_init(thread_attributes);
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
    return thread;
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
        struct array_part *rotateParts = rotate_all_parts(apart);
        map_big_array *map = prepare_map_part(rotateParts);
        init_client_possibility(&thread_params[i], rotateParts, map, i, i, 0);

        /* création d'un nouveau thread */
        pthread_attr_t *thread_attributes = malloc(sizeof *thread_attributes);
        pthread_attr_init(thread_attributes);
        pthread_attr_setdetachstate(thread_attributes, PTHREAD_CREATE_DETACHED);

        thread_params[i].tid = malloc(sizeof(pthread_t));
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

    pthread_t feed_tid = build_feed_thread(thread_params);

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

    // Le thread d'alimentation est joignable : on attend sa fin avant de fermer
    // les connexions, pour qu'il ne touche plus aux sockets qu'on va fermer.
    pthread_join(feed_tid, NULL);

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

    struct array_part *apart = read_parts(file);
    struct array_part *rotateParts = rotate_all_parts(apart);
    map_big_array *map = prepare_map_part(rotateParts);
    init_client_possibility(thread_params, rotateParts, map, 0, 0, getpid());
    free_array_part(apart);

    pthread_t feed_tid = build_feed_thread(thread_params);
    pthread_t control_tid = build_control_thread(thread_params);
#ifdef WITH_CUDA
    if (gpu_pruner_mode) {
        // Les contextes CUDA ne sont pas hérités par fork() : l'init GPU doit
        // avoir lieu ici, dans le processus enfant, après construction de la map.
        if (gpu_pruner_init(thread_params->map_part, thread_params->all_rotate_part) != 0) {
            log_error("gpu_pruner_init a échoué — abandon du processus pruner GPU\n");
            request = REQUEST_STOP;
        } else {
            autoprune_gpu(thread_params);
            gpu_pruner_shutdown();
        }
    } else
#endif // WITH_CUDA
    if (pruner_mode) {
        autoprune(thread_params);
    } else {
        autosearch(thread_params);
    }

    // autosearch/autoprune ne reviennent que sur REQUEST_STOP. On joint d'abord
    // les threads auxiliaires : sans cela ils continuaient de tourner pendant
    // que ce process se démontait, accédant aux structures partagées (files,
    // aposs, sockets) en cours de libération — l'origine du « double free »
    // intermittent à l'arrêt. Le join est borné : le thread d'alimentation finit
    // au pire après le timeout de réception TCP (tcp_timeout), le thread de
    // contrôle boucle en usleep court.
    pthread_join(feed_tid, NULL);
    pthread_join(control_tid, NULL);

    pthread_mutex_lock(&thread_params->socket_mutex);
    if (thread_params->socket_id != -1 && is_connected(thread_params->socket_id)) {
        close_socket(thread_params->socket_id);
    }
    pthread_mutex_unlock(&thread_params->socket_mutex);
}

/**
 * @brief Construit le tableau « Thread queues » du rapport client (une ligne par
 *        fork : in-stock / analysed, plus la ligne Total) dans une chaîne
 *        fraîchement allouée. Renvoie via out-params (NULL accepté) le stock
 *        total, l'analysed total et la somme des coups/s ; met à jour la globale
 *        max_result avec le meilleur résultat parmi les forks.
 *
 * Le buffer est dimensionné sur NB_THREADS (256 + NB_THREADS*80) — régression
 * d'un débordement de tas observé avec un buffer fixe sur un NB_THREADS élevé.
 * Extrait du corps de boucle de check_client_threads pour être testable hors
 * thread (pur : lit l'instantané fork_statistics[]).
 *
 * @return Chaîne allouée (à libérer par l'appelant).
 */
char *build_thread_queues_table(unsigned long long *out_stock,
                                unsigned long long *out_analysed,
                                unsigned long long *out_shots_per_sec)
{
    unsigned long long stock = 0, analysed = 0, bys = 0;
    size_t size = 256 + (size_t)NB_THREADS * 80;
    char *table = calloc(size, sizeof(char));
    int off = snprintf(table, size,
        "Thread queues\n"
        "Fork |     In stock |     Analysed\n"
        "-----+--------------+-------------\n");
    for (int f = 0; f < NB_THREADS; f++) {
        if (fork_statistics[f].max_result > max_result) {
            max_result = fork_statistics[f].max_result;
        }
        bys += fork_statistics[f].shots_per_second;
        unsigned long long in_stock = fork_statistics[f].possibilities_in_stock;
        unsigned long long an = fork_statistics[f].analyses_in_stock;
        off += snprintf(table + off, size - off,
                        "%4i | %12llu | %12llu\n", f, in_stock, an);
        stock += in_stock; analysed += an;
    }
    snprintf(table + off, size - off,
             "-----+--------------+-------------\n"
             "Total| %12llu | %12llu\n", stock, analysed);
    if (out_stock)         *out_stock         = stock;
    if (out_analysed)      *out_analysed      = analysed;
    if (out_shots_per_sec) *out_shots_per_sec = bys;
    return table;
}

/**
 * @brief Un tour de la boucle de `check_client_threads` (corps extrait pour être
 *        testable hors thread, comme `check_server_step`).
 *
 * Construit et publie le rapport de statistiques client (files par fork,
 * forward-check, pruner), met à jour le bandeau `log_status` et détecte un
 * nouveau record. Ne contient PAS le `sleep(sleep_time)` de fin de tour :
 * c'est l'appelant (la boucle `while(1)` de `check_client_threads`) qui
 * rythme les tours.
 *
 * @param last_record In/out : meilleur résultat déjà annoncé (détection de record).
 */
void check_client_threads_step(int *last_record)
{
    // Les buffers de stats sont dimensionnés selon NB_THREADS : une ligne
    // de tableau par fork. Avec un buffer FIXE, un NB_THREADS élevé (ex.
    // 100) débordait et corrompait le tas ("double free or corruption" /
    // abort). On alloue donc en fonction du nombre de forks (+ marge pour
    // l'en-tête, le pied de tableau et les lignes forward-check / pruner /
    // résumé concaténées dans le rapport).
    //
    // Rapport construit dans un buffer LOCAL (jamais dans `lastcheck`
    // directement) : les strcat/sprintf qui suivent ne touchent aucun état
    // partagé, donc aucun besoin de tenir un verrou pendant leur exécution.
    // `lastcheck_publish()` n'est appelée qu'une fois le rapport complet,
    // ce qui réduit la section critique au seul échange de pointeur (voir
    // static_variables.h pour le détail de la race corrigée).
    size_t table_size = 256 + (size_t)NB_THREADS * 80;
    size_t lastcheck_size = table_size + 4096;
    char *report = calloc(lastcheck_size, sizeof(char));

        // Côté client, le travail tourne dans les processus fork (mémoire séparée
        // après fork) : les files locales du parent sont vides. La donnée réelle
        // est dans fork_statistics[], par fork. On présente donc un tableau par
        // fork : stock local en cours d'étude et possibilités en cours d'analyse.
        unsigned long long fork_possibility_stock = 0;
        unsigned long long fork_analysed_stock = 0;
        unsigned long long bys = 0;
        int f;
        char *table = build_thread_queues_table(&fork_possibility_stock,
                                                &fork_analysed_stock, &bys);
        strcat(report, table);
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
            strcat(report, fctemp);
            free(fctemp);
        }
#endif // FORWARD_CHECK_K > 0

        // Statistiques pruner : agrégat des forks + compteurs du processus courant
        unsigned long long prc = pruner_checked;
        unsigned long long prr = pruner_removed;
        unsigned long long prcells = pruner_cells_studied;
        unsigned long long prune_bys = 0;
        for (f = 0; f < NB_THREADS; f++) {
            prc += fork_statistics[f].pruner_checked;
            prr += fork_statistics[f].pruner_removed;
            prcells += fork_statistics[f].pruner_cells_studied;
            prune_bys += fork_statistics[f].pruner_cells_per_second;
        }
        if (prc + prr > 0) {
            char *prtemp = calloc(200, sizeof(char));
            sprintf(prtemp, "pruner : %llu mortes / %llu vérifiées (%.2f%%), %llu cases étudiées\n",
                    prr, prc + prr, 100.0 * (double)prr / (double)(prc + prr), prcells);
            strcat(report, prtemp);
            free(prtemp);
        }

        // Indice cumulé « études/s » : le compteur de coups crédite déjà les
        // études de prunage case par case (autoprune_step, rmnonext), donc
        // `bys` cumule recherche + prunage ; la ligne dédiée rend l'indice
        // explicite et « dont prunage/s » en isole la part.
        char *temp = calloc(1000, sizeof(char));
        sprintf(temp, "active thread/s :%lli\nétudes/s (recherche+prunage) :%llu\ndont prunage/s :%llu\npossibility in stock :%lli (analysed:%llu)\nmax search by sec : %lli\nmax stock by thread : %i\nmax result :%i\n",
            bys,
            (unsigned long long)bys,
            prune_bys,
            fork_possibility_stock,
            fork_analysed_stock,
            max_search_by_sec, max_stock_by_thread, max_result);
#ifdef DEBUG_SOCKET
        // Ajout à la suite : écrire à partir de la fin courante de `temp` plutôt
        // que de le repasser en argument %s (destination/source qui se
        // chevauchent → comportement indéfini, -Wrestrict).
        sprintf(temp + strlen(temp), "socket opened :%i\n", opened_tcp);
#endif // DEBUG_SOCKET
        strcat(report, temp);
        free(temp);

        lastcheck_publish(report);

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

        if (max_result > *last_record) {
            *last_record = max_result;
            log_event("new record: %i pieces placed", max_result);
        }
}

/**
 * @brief Thread de statistiques du client (lancé par `run_checker`).
 *
 * Toutes les 10 secondes, appelle `check_client_threads_step` puis dort.
 *
 * @param param Non utilisé.
 * @return      NULL (boucle infinie).
 */
void *check_client_threads(void *param)
{
    (void)param;
    int sleep_time = 10;
    int last_record = max_result;
    while(1)
    {
        check_client_threads_step(&last_record);
        sleep(sleep_time);
    }

    return NULL;
}
