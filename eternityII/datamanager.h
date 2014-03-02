#ifndef eternityII_datamanager_h
#define eternityII_datamanager_h

#include <pthread.h>
#include "possibility.h"
#include "lifo.h"

#define NB_FILE_POSSIBILITY 10
static uint16_t max_result = 0;

typedef struct
{
    File file;
    pthread_mutex_t lock;
} file_possibility_t;

int add_possibility(array_possibility_packet *possibilities);
array_possibility_packet *get_last_possibility(int max_result);
int file_size(int nfile);
int datas_size();

void set_server_ip(const char *server);
char *get_server_ip();

int backup(char *filename);
int restore(char *filename);
int import(char *filename);

int print_file(int fp);
int printdatamanager();
int sort_ascending();
int sort_descending();
int sort_descending_mthread();
void *sort_d_mono(void *f);
int regroup_datas();
int split_datas();
int check_datas();
int check_file(int f);
int check_files();
int search_min_datas();

int remove_possibilities_with_no_next(map_big_array *mapParts);
#endif