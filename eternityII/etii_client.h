//
//  etii_client.h
//  eternityII
//
//  Created by Xavier GRIFFON on 04/10/2020.
//  Copyright © 2020 Xavier GRIFFON. All rights reserved.
//

#ifndef etii_client_h
#define etii_client_h

#include <stdio.h>
#include <pthread.h>
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
    int request;
} client_possibility_t;

void runThreadClient(const char *file);
#endif /* etii_client_h */
