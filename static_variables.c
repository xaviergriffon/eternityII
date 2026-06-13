#include "static_variables.h"
#include <stdio.h>

#if ETERN_PARTS == 256

uint8_t directions[ETERN_PARTS] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    31,47,63,79,95,111,127,143,159,175,191,207,223,239,255,
    254,253,252,251,250,249,248,247,246,245,244,243,242,241,240,
    224,208,192,176,160,144,128,112,96,80,64,48,32,16,
    225,226,209,210,
    238,237,222,221,
    17,33,18,34,
    30,29,46,45,
    193,227,
    228,211,194,177,
    161,178,195,212,229,
    230,213,196,179,162,145,
    129,146,163,180,197,214,231,
    215,198,181,164,147,130,
    131,148,165,182,199,
    183,166,149,132,
    133,150,167,
    151,134,
    135,
    206,236,
    235,220,205,190,
    174,189,204,219,234,
    233,218,203,188,173,158,
    142,157,172,187,202,217,232,
    216,201,186,171,156,141,
    140,155,170,185,200,
    184,169,154,139,
    138,153,168,
    152,137,
    136,
    19,49,
    65,50,35,20,
    21,36,51,66,81,
    97,82,67,52,37,22,
    23,38,53,68,83,98,113,
    114,99,84,69,54,39,
    55,70,85,100,115,
    116,101,86,71,
    87,102,117,
    118,103,
    119,
    126,
    125,110,
    94,109,124,
    123,108,93,78,
    62,77,92,107,122,
    121,106,91,76,61,
    60,75,90,105,120,
    104,89,74,59,44,
    28,43,58,73,88,
    72,57,42,27,
    26,41,56,
    40,25,
    24};

uint8_t dirx[ETERN_PARTS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 2, 14, 13, 14, 13, 1, 1, 2, 2, 14, 13, 14, 13, 1, 3, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 5, 6, 7, 7, 6, 7, 14, 12, 11, 12, 13, 14, 14, 13, 12, 11, 10, 9, 10, 11, 12, 13, 14, 14, 13, 12, 11, 10, 9, 8, 8, 9, 10, 11, 12, 13, 12, 11, 10, 9, 8, 8, 9, 10, 11, 10, 9, 8, 8, 9, 8, 3, 1, 1, 2, 3, 4, 5, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 4, 5, 6, 7, 7, 6, 5, 6, 7, 7, 14, 13, 14, 14, 13, 12, 11, 12, 13, 14, 14, 13, 12, 11, 10, 9, 10, 11, 12, 13, 12, 11, 10, 9, 8, 8, 9, 10, 11, 12, 12, 11, 10, 9, 8, 8, 9, 10, 11, 10, 9, 8, 8, 9, 8};

uint8_t diry[ETERN_PARTS] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 14, 14, 13, 13, 14, 14, 13, 13, 1, 2, 1, 2, 1, 1, 2, 2, 12, 14, 14, 13, 12, 11, 10, 11, 12, 13, 14, 14, 13, 12, 11, 10, 9, 8, 9, 10, 11, 12, 13, 14, 13, 12, 11, 10, 9, 8, 8, 9, 10, 11, 12, 11, 10, 9, 8, 8, 9, 10, 9, 8, 8, 12, 14, 14, 13, 12, 11, 10, 11, 12, 13, 14, 14, 13, 12, 11, 10, 9, 8, 9, 10, 11, 12, 13, 14, 13, 12, 11, 10, 9, 8, 8, 9, 10, 11, 12, 11, 10, 9, 8, 8, 9, 10, 9, 8, 8, 1, 3, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 5, 6, 7, 7, 6, 7, 7, 7, 6, 5, 6, 7, 7, 6, 5, 4, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1, 2, 3, 4, 5, 4, 3, 2, 1, 1, 2, 3, 2, 1, 1};

#else
uint8_t directions[ETERN_PARTS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

uint8_t dirx[ETERN_PARTS] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};

uint8_t diry[ETERN_PARTS] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};

#endif
#if FORWARD_CHECK_K > 0
volatile unsigned long long fc_pruned = 0;
volatile unsigned long long fc_attempts = 0;
volatile unsigned long long fc_pruned_at[FORWARD_CHECK_K + 1] = {0};
#endif // FORWARD_CHECK_K > 0

int pruner_mode = 0;

volatile unsigned long long pruner_checked = 0;

volatile unsigned long long pruner_removed = 0;

unsigned long long *counters = NULL;

struct client_statistics *fork_statistics = NULL;

unsigned long long *lastfilesize = NULL;
char *lastcheck = NULL;

// TODO : deplacer dans un parametre ?
#if ETERN_PARTS == 256
char* parts_files = "./pieces.csv";
#else
char* parts_files = "./pieces16.csv";
#endif // ETERN_PARTS == 256

unsigned long long non_null_possibilities = 0;
volatile uint16_t max_result = 0;
//int request = REQUEST_CONTINUE;


volatile int request = REQUEST_CONTINUE;

long inst_unknow = 0;

int NB_THREADS = 10;

int version = VERSION;

int parent_pid = -1;

pid_t *childrens_pid = NULL;

struct sockaddr_un *main_addr = NULL;

int *main_socket_id = NULL;

char **forkId = NULL;

int SERVER_PORT = 2020;

unsigned long long max_search_by_sec = 0;

int max_stock_by_thread = MAX_STOCK_BY_THREAD;

int communication_in_progress = 0;

#ifdef DEBUG_SOCKET
int opened_tcp = 0;
#endif // DEBUG_SOCKET
long nb_client = 0;

int tcp_timeout = DEFAULT_TCP_TIMEOUT;

int fork_checker_socket_id = -1;

int server = 0;

int server_rmnonext_timing = 30;
