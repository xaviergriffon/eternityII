//
//  static_variables.h
//  eternityII
//
//  Created by Xavier GRIFFON on 04/10/2020.
//  Copyright © 2020 Xavier GRIFFON. All rights reserved.
//

#ifndef static_variables_h
#define static_variables_h

#define NB_CONNECTIONS_PAR_THREAD 1
#define MICRO_SLEEP 100
#define THREAD_MICRO_SLEEP 10000
#define MAX_STOCK_BY_THREAD 100

#define REQUEST_STOP 1
#define REQUEST_CONTINUE 0

#define ETERN_SIZE 16
#define ETERN_PARTS 256

extern int NB_THREADS;
extern int request;

// TODO : déplacer dans une classe regroupant les statistiques


extern unsigned long long *compteurs;
extern int *lastfilesize;

#endif /* static_variables_h */
