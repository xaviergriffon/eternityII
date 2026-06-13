/**
 * @file datamanager.h
 * @brief Méthodes pour gérer les files de possibilités
 */
#ifndef eternityII_datamanager_h
#define eternityII_datamanager_h

#include <pthread.h>
#include "etii_client.h"
#include "possibility.h"
#include "lifo.h"

/**
 * @todo Rendre configurable
 */
#define NB_FILE_POSSIBILITY 10

/**
 * @brief Structure représentant un file de possibilités
 * 
 * Cette structure permet d'indiquer qu'une file est "lockée" en mutli-thread
 */
typedef struct
{
    File file;
    pthread_mutex_t lock;
} file_possibility_t;

int add_possibility(client_possibility_t *client_possibility, array_possibility_packet *possibilities);
array_possibility_packet *get_last_possibility(client_possibility_t *client_possibility, int max_result);
/**
 * @brief Extrait des possibilités non vérifiées du datamanager local (côté serveur).
 *
 * @param max_result Nombre maximum de possibilités à extraire.
 * @return           Tableau alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *get_last_possibility_tocheck(int max_result);
int add_possibility_analysed(struct possibility_packet *possiblity, int thread);
void send_possibility_analysed(client_possibility_t *client_possibility);
int remove_possibility_analysed(struct possibility_packet *possiblity, int thread);
unsigned long long file_size(int nfile);
unsigned long long file_checked_size(int nfile);
unsigned long long file_analysed_size(int nfile);
unsigned long long datas_size(void);

/**
 * @brief Renseigne l'IP du serveur
 * 
 * @param server IP du serveur au format texte.
 */
void set_server_ip(const char *server);
/**
 * @return l'IP du serveur
 */
char *get_server_ip(void);
/**
 * @brief Effectue une sauvegarde fichier des files de possiblités
 * 
 * @param filename nom du fichier dans lequel faire la sauvegarde
 * @return 0 si OK ou -1 en cas d'erreur
 */
int backup(char *filename);
/**
 * @brief Effectue une sauvegarde fichier des possibilités en cours d'anlayse.
 * Les possibilités en cours d'analyse sont les possibilités fournies par le serveur.
 * 
 * @param filename nom du fichier dans lequel faire la sauvegarde
 * @return 0 si OK ou -1 en cas d'erreur 
 */
int backup_analysed(char *filename);
/**
 * @brief Reconstruit les files avec le contenu du fichier
 * 
 * @param filename nom du fichier contenant une sauvegarde de file de possibilité
 * @return 0 si OK ou -1 en cas d'erreur
 */
int restore(char *filename);
#ifdef FACES_USED_BITS
int restore_old_file(char *filename);
#endif // FACES_USED_BITS
/**
 * @brief Reconstruit les files de possibiltés en cours d'analyse avec le contenu du fichier
 * 
 * @param filename nom du fichier contenant une sauvegarde de file de possibilité en cours d'analyse
 * @return 0 si OK ou -1 en cas d'erreur
 */
int restore_analysed(char *filename);
/**
 * @brief Importe le contenu d'un fichier dans les files
 * 
 * @param client_possibility Thread client à qui sont destinées ces possibilités
 * @param filename nom du fichier contenant un ensemble de possibilité
 * @return 0 si OK ou -1 en cas d'erreur
 */
int import(client_possibility_t *client_possibility, char *filename);
/**
 * @brief Importe le contenu du fichier dans les possiblités en cours d'analyse
 * 
 * @param filename nom du fichier contenant une liste de possibilités en cours d'analyse
 * @return 0 si OK ou -1 en cas d'erreur
 */
int import_analysed(char *filename);
/**
 * @brief Effectue un import au format JSON
 * @deprecated Il s'agit d'une méthode pour débugguer
 * @return 0 si OK ou -1 en cas d'erreur
 */
int import_json(void);

/**
 * @brief Affiche dans la console les possibilités, au format JSON, de toutes les files.
 * 
 * @return 0 si OK ou -1 en cas d'erreur
 */
int printdatamanager(void);
/**
 * @brief Affiche dans la console les possibilités, au format JSON, contenu dans une file
 * 
 * @param fp numéro de la file à afficher
 * @return 0 si OK ou -1 en cas d'erreur
 */
int print_file(int fp);

int print_file_analysed(int fp);
int print_all_file_analysed(void);
int sort_ascending(void);
int sort_descending(void);
int sort_descending_mthread(void);
void *sort_d_mono(void *f);
int regroup_datas(void);
int split_datas(void);
int check_datas(void);
int check_duplicate(void);
int statistic_datas(void);
int check_file(int f);
int check_files(void);
int search_min_datas(void);

int remove_possibilities_with_no_next(map_big_array *mapParts, struct array_part *all_rotate_part);
#endif
