/**
 * @file etii_client.h
 * @brief Méthodes pour un client EternityII
 * 
 */
#ifndef etii_client_h
#define etii_client_h

#include <stdio.h>
#include <pthread.h>
#include <sys/times.h>
#include "static_variables.h"
#include "possibility.h"

/**
 * @brief Structure représentant un thread de client EternityII
 * 
 */
typedef struct
{
    int works;
    pthread_t *tid;
    /**
     * @todo définir et renommer
     */
    array_possibility_packet *aposs;
    map_big_array *map_part;
    struct array_part *all_rotate_part;
    int compteur;
    int max_shots_per_second;
    int id;
    int socket_id;
    pthread_mutex_t socket_mutex;
    struct tms start_socket;
} client_possibility_t;

/**
 * @brief Lance un client en mode multi-thread
 * 
 * Le nombre de thread est en fonction de la variable globale NB_THREADS.
 * 
 * @param[in] file fichier contenant la définition des pieces
 */
void runThreadClient(const char *file);
/**
 * @brief Lance un client en mono-thread
 * 
 * @param file fichier contenant la définition des pieces
 */
void run_mono_client(const char *file);
/**
 * @brief Effectue un contrôle des threads client
 * 
 * @param param
 * @return void* null. Retourne un pointeur afin de respecter le format d'une méthode de thread.
 */
void *check_client_threads(void *param);

#endif /* etii_client_h */
