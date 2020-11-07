#ifndef static_variables_h
#define static_variables_h

#ifdef WIN32
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#else
#include <stdint.h>
#define PACK( __Declaration__ ) __Declaration__ __attribute__((__packed__))
#endif

#define VERSION 1

#define NB_CONNECTIONS_PAR_THREAD 1
#define MICRO_SLEEP 100
#define MICRO_PAUSE 10
#define THREAD_MICRO_SLEEP 10000
#define MAX_STOCK_BY_THREAD 300

#define REQUEST_STOP 1
#define REQUEST_CONTINUE 0
#define REQUEST_PAUSE 2

#define ETERN_PARTS 256
#if ETERN_PARTS == 256
#define ETERN_SIZE 16
#else
// 16 pieces
#define ETERN_SIZE 4
#endif // ETERN_PARTS == 256

//#define CHECK_POSSIBILITY 1

extern uint8_t directions[ETERN_PARTS];

extern uint8_t dirx[ETERN_PARTS];

extern uint8_t diry[ETERN_PARTS];

extern int NB_THREADS;

extern unsigned long long *compteurs;
extern int *lastfilesize;

extern uint16_t max_result;
extern char *lastcheck;

// TODO : deplacer dans un parametre ?
extern char* partsFiles;

extern unsigned long long getted_possibility_not_null;

extern int request;

extern long inst_unknow;

extern int NB_THREADS;

extern int SERVER_PORT;

extern unsigned long long max_search_by_sec;

extern int max_stock_by_thread;

#endif /* static_variables_h */
