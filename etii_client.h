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
#include <time.h>
#include "static_variables.h"
#include "possibility.h"

/**
 * @brief Structure représentant un thread de client EternityII
 * 
 */
typedef struct
{
    volatile int works;
    pthread_mutex_t works_mutex;
    /// Sérialise les échanges réseau sur socket_id : le thread d'alimentation et
    /// le thread de recherche partagent le même socket, et leurs échanges
    /// (send_instruction + send + recv ack) ne doivent pas s'entrelacer.
    pthread_mutex_t socket_mutex;
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
    pid_t pid;
    int socket_id;
    struct tms start_socket;
    /// Horodatage (wall-clock) du dernier échange réseau, pour le keepalive :
    /// un worker occupé sur son stock local doit pinguer le serveur avant son
    /// timeout d'inactivité (tcp_timeout), sinon le serveur ferme la session.
    time_t last_socket_activity;
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
