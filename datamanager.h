#ifndef eternityII_datamanager_h
#define eternityII_datamanager_h

#include <pthread.h>
#include "etii_client.h"
#include "possibility.h"
#include "lifo.h"

#define NB_FILE_POSSIBILITY 10

typedef struct
{
    File file;
    pthread_mutex_t lock;
} file_possibility_t;

int add_possibility(client_possibility_t *client_possibility, array_possibility_packet *possibilities);
array_possibility_packet *get_last_possibility(client_possibility_t *client_possibility, int max_result);
int add_possibility_analysed(struct possibility_packet *possiblity, int thread);
void send_possibility_analysed(client_possibility_t *client_possibility);
int remove_possibility_analysed(struct possibility_packet *possiblity, int thread);
int file_size(int nfile);
int file_analysed_size(int nfile);
int datas_size(void);

void set_server_ip(const char *server);
char *get_server_ip(void);

int backup(char *filename);
int backup_analysed(char *filename);
int restore(char *filename);
#ifdef FACES_USED_BITS
int restore_old_file(char *filename);
#endif // FACES_USED_BITS
int restore_analysed(char *filename);
int import(client_possibility_t *client_possibility, char *filename);
int import_analysed(char *filename);
int import_json(void);

int print_file(int fp);
int printdatamanager(void);
int print_file_analysed(int fp);
int print_all_file_analysed(void);
int sort_ascending(void);
int sort_descending(void);
int sort_descending_mthread(void);
void *sort_d_mono(void *f);
int regroup_datas(void);
int split_datas(void);
int check_datas(void);
int statistic_datas(void);
int check_file(int f);
int check_files(void);
int search_min_datas(void);

int remove_possibilities_with_no_next(map_big_array *mapParts, struct array_part *all_rotate_part);
#endif
