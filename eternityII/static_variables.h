#ifndef static_variables_h
#define static_variables_h

#ifdef WIN32
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
typedef unsigned char u_int8_t;
typedef unsigned short u_int16_t;
#else
#include <stdint.h>
#define PACK( __Declaration__ ) __Declaration__ __attribute__((__packed__))
#endif


#define NB_CONNECTIONS_PAR_THREAD 1
#define MICRO_SLEEP 100
#define THREAD_MICRO_SLEEP 10000
#define MAX_STOCK_BY_THREAD 300

#define REQUEST_STOP 1
#define REQUEST_CONTINUE 0

#define ETERN_SIZE 16
#define ETERN_PARTS 256
//#define CHECK_POSSIBILITY 1

extern u_int8_t directions[ETERN_PARTS];

extern u_int8_t dirx[ETERN_PARTS];

extern u_int8_t diry[ETERN_PARTS];

extern int NB_THREADS;

extern unsigned long long *compteurs;
extern int *lastfilesize;

extern u_int16_t max_result;
extern char *lastcheck;

// TODO : deplacer dans un parametre ?
extern char* partsFiles;

extern unsigned long long getted_possibility_not_null;

extern int request;

extern long inst_unknow;

extern int NB_THREADS;

#endif /* static_variables_h */
