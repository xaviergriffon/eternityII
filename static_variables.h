#ifndef static_variables_h
#define static_variables_h

#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/un.h>
#include "etii_statistic.h"

#define VERSION 4

#define NB_CONNECTIONS_PER_THREAD 1
// Temps d'attente de 100 microsecondes
#define MICRO_SLEEP 100
// Temps d'attente court de 10 microsecondes
#define MICRO_SHORT_SLEEP 10
// Temps d'attente pour les boucles de threads
#define THREAD_MICRO_SLEEP 10000
#define MAX_STOCK_BY_THREAD 300

#define REQUEST_STOP 1
#define REQUEST_CONTINUE 0
#define REQUEST_PAUSE 2

#define DEFAULT_TCP_TIMEOUT 10

#define PART_SIZES 4
#define ETERN_PARTS 256
//#define ETERN_WITH_INDICES 1
#if ETERN_PARTS == 256
#define ETERN_SIZE 16
#define FACES_USED_SIZE 17// (ETERN_PARTS / 16) + 1;
#else
// 16 pieces
#define ETERN_SIZE 4
#define FACES_USED_SIZE 2// (ETERN_PARTS / 16) + 1;
#endif // ETERN_PARTS == 256

#define BUF_SIZE 300
// ------------- Flags pour Debug -----------------
// Permet de contrôler les données des possibilités générés ou reçus
//#define DEBUG_CHECK_POSSIBILITY 1
// Trace des informations lors d'un rmnonext
//#define DEBUG_RM_NO_NEXT
// Trace des informations de la socket lors des déconnexions etc...
//#define DEBUG_SOCKET
// Trace les informations dans les signaux
//#define DEBUG_SIGNAL
// Trace les informations pour les sockets locale
//#define DEBUG_LOCAL_SOCKET
// Passe en mono-process pour pouvoir débeugger
#define DEBUG_IN_MONO_PROCESS
// ------------------------------------------------
#define FACES_USED_BITS
extern uint8_t directions[ETERN_PARTS];

extern uint8_t dirx[ETERN_PARTS];

extern uint8_t diry[ETERN_PARTS];
/**
 * @brief  Nombre de threads clients
 */
extern int NB_THREADS;

extern unsigned long long *counters;
extern unsigned long long *lastfilesize;

extern uint16_t max_result;
extern char *lastcheck;

// TODO : deplacer dans un parametre ?
extern char* parts_files;

extern unsigned long long non_null_possibilities;

extern int request;

extern long inst_unknow;

extern int version;

extern pid_t parent_pid;

extern pid_t *childrens_pid;

extern char **forkId;

extern struct client_statistics *fork_statistics;

extern int fork_checker_socket_id;

extern struct sockaddr_un *main_addr;

extern int *main_socket_id;

extern int SERVER_PORT;

extern unsigned long long max_search_by_sec;

extern int max_stock_by_thread;

extern int communication_in_progress;

#ifdef DEBUG_SOCKET
extern int opened_tcp;
#endif // DEBUG_SOCKET

extern long nb_client;

extern int tcp_timeout;

extern int server;

extern int server_rmnonext_timing;
#endif /* static_variables_h */
