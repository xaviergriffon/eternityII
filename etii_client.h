#ifndef etii_client_h
#define etii_client_h

#include <stdio.h>
#include <pthread.h>
#include <sys/times.h>
#include "static_variables.h"
#include "possibility.h"

typedef struct
{
    int works;
    pthread_t *tid;
    array_possibility_packet *aposs;
    map_big_array *map_part;
    struct array_part *all_rotate_part;
    int compteur;
    //int request;
    int max_shots_per_second;
    int id;
    int socket_id;
    pthread_mutex_t socket_mutex;
    struct tms start_socket;
} client_possibility_t;

void runThreadClient(const char *file);
void runMonoClient(const char *file);
void *check_client_threads(void *param);

#endif /* etii_client_h */
