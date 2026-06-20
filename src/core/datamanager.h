/**
 * @file datamanager.h
 * @brief Méthodes pour gérer les files de possibilités
 */
#ifndef eternityII_datamanager_h
#define eternityII_datamanager_h

#include <pthread.h>
#include "app/etii_client.h"
#include "core/possibility.h"
#include "core/lifo.h"

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

/**
 * @brief Ajoute des possibilités dans le datamanager (local ou serveur distant).
 *
 * Si une IP serveur est configurée et que `client_possibility` est non NULL,
 * les possibilités sont envoyées au serveur TCP ; sinon elles sont insérées dans
 * les files locales.
 *
 * @param client_possibility Contexte du thread client (peut être NULL en mode local).
 * @param possibilities      Tableau de paquets à ajouter.
 * @return                   0 si OK, valeur négative en cas d'erreur.
 */
int add_possibility(client_possibility_t *client_possibility, array_possibility_packet *possibilities);

/**
 * @brief Extrait des possibilités à traiter pour un thread de recherche.
 *
 * Essaie d'abord les files locales, puis le serveur TCP si disponible.
 *
 * @param client_possibility Contexte du thread client.
 * @param max_result         Nombre maximum de possibilités à extraire.
 * @return                   Tableau alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *get_last_possibility(client_possibility_t *client_possibility, int max_result);
/**
 * @brief Extrait des possibilités non vérifiées du datamanager local (côté serveur).
 *
 * @param max_result Nombre maximum de possibilités à extraire.
 * @return           Tableau alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *get_last_possibility_tocheck(int max_result);
/**
 * @brief Enregistre une possibilité en cours d'analyse dans la file dédiée.
 *
 * Utilise `thread` comme index de file cible (rotation automatique si `thread < 0`).
 * Met à jour `max_result` si la possibilité établit un nouveau record.
 *
 * @param possiblity Paquet à enregistrer.
 * @param thread     Index du thread (−1 = choix automatique).
 * @return           0.
 */
int add_possibility_analysed(struct possibility_packet *possiblity, int thread);

/**
 * @brief Renvoie au serveur les possibilités analysées depuis les files locales.
 *
 * Extrait les paquets de la file d'analyse et les transmet via le socket TCP
 * du thread client (instructions INST_ADD / INST_POSSIBILITY_ANALYSED).
 *
 * @param client_possibility Contexte du thread client.
 */
void send_possibility_analysed(client_possibility_t *client_possibility);

/**
 * @brief Signale une solution complète au serveur (instruction INST_SOLUTION).
 *
 * Envoie le paquet solution sur le socket TCP du thread client, sous
 * `socket_mutex`, et attend l'acquittement `INST_CONSIDERED`. Sans serveur
 * configuré (mode local) ou si la connexion échoue, journalise et renvoie -1
 * sans bloquer : la solution reste sauvegardée localement par `log_solution`.
 *
 * @param client_possibility Contexte du thread client (socket vers le serveur).
 * @param possibility        Paquet solution à transmettre.
 * @return                   0 si le serveur a acquitté, -1 sinon.
 */
int send_solution(client_possibility_t *client_possibility, struct possibility_packet *possibility);

/**
 * @brief Retire une possibilité de la file des analyses en cours.
 *
 * Parcourt les files d'analyse en cherchant le paquet correspondant et le supprime.
 *
 * @param possiblity Paquet à retirer.
 * @param thread     Index de file préférentiel (−1 = recherche dans toutes les files).
 * @return           1 si le paquet a été trouvé et retiré, 0 sinon.
 */
int remove_possibility_analysed(struct possibility_packet *possiblity, int thread);
/** @brief Nombre de possibilités dans la file `nfile` du pool principal (non vérifiées). */
unsigned long long file_size(int nfile);
/** @brief Nombre de possibilités dans la file `nfile` du pool vérifié (validées par un pruner). */
unsigned long long file_checked_size(int nfile);
/** @brief Nombre de possibilités en cours d'analyse dans la file `nfile`. */
unsigned long long file_analysed_size(int nfile);
/** @brief Taille totale des deux pools de possibilités (non vérifiées + vérifiées). */
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
/**
 * @brief Restaure les files depuis un fichier de sauvegarde au format pre-bitmask.
 *
 * Utilisé pour migrer d'anciens fichiers `.back` vers le format FACES_USED_BITS.
 *
 * @param filename Chemin du fichier à importer.
 * @return         0 si OK ou -1 en cas d'erreur.
 */
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

/**
 * @brief Affiche au format JSON les possibilités en cours d'analyse dans la file `fp`.
 * @param fp Numéro de la file analysée à afficher.
 * @return   0 si OK ou -1 en cas d'erreur.
 */
int print_file_analysed(int fp);

/** @brief Affiche au format JSON toutes les files d'analyse en cours. */
int print_all_file_analysed(void);

/**
 * @brief Remet dans le stock toutes les possibilités en cours d'analyse.
 *
 * Vide les files `file_possibility_analysed` et réinjecte les paquets dans le
 * stock non vérifié. À appeler quand des clients sont morts sans signaler la
 * fin de leur traitement.
 *
 * @return 0.
 */
int restock_analysed(void);

/** @brief Trie toutes les files de possibilités par ordre croissant de `alloc`. */
int sort_ascending(void);

/** @brief Trie toutes les files de possibilités par ordre décroissant de `alloc`. */
int sort_descending(void);

/** @brief Trie les files par ordre décroissant en parallèle (multi-thread). */
int sort_descending_mthread(void);

/**
 * @brief Fonction de tri décroissant exécutée par un thread (entrée `pthread_create`).
 * @param f Pointeur vers la `file_possibility_t` à trier.
 * @return  NULL.
 */
void *sort_d_mono(void *f);

/**
 * @brief Regroupe toutes les files de possibilités dans la file 0.
 *
 * Utile avant une opération globale (export, split) pour rassembler toutes
 * les possibilités en un seul endroit.
 *
 * @return 0.
 */
int regroup_datas(void);

/**
 * @brief Répartit équitablement les possibilités de la file 0 vers toutes les files.
 *
 * Inverse de `regroup_datas` ; permet de paralléliser à nouveau le traitement
 * entre les threads après un regroupement.
 *
 * @return 0.
 */
int split_datas(void);

/**
 * @brief Vérifie la cohérence de toutes les possibilités des files.
 *
 * Appelle `check_possibility` sur chaque paquet et rapporte les erreurs.
 *
 * @return 0 si aucune erreur, -1 si des paquets invalides ont été détectés.
 */
int check_datas(void);

/**
 * @brief Détecte les paquets en double dans les files de possibilités.
 *
 * Compare chaque paire de paquets et log les doublons détectés.
 *
 * @return 0.
 */
int check_duplicate(void);

/**
 * @brief Affiche des statistiques sur la distribution des possibilités par `alloc`.
 * @return 0.
 */
int statistic_datas(void);

/**
 * @brief Vérifie l'intégrité d'une file individuelle de possibilités.
 * @param f Numéro de la file à vérifier.
 * @return  0 si OK, -1 si une incohérence est détectée.
 */
int check_file(int f);

/**
 * @brief Vérifie l'intégrité de toutes les files de possibilités.
 * @return 0 si OK, -1 si une incohérence est détectée dans au moins une file.
 */
int check_files(void);

/**
 * @brief Identifie et affiche la possibilité ayant le plus petit `alloc` dans les files.
 * @return 0.
 */
int search_min_datas(void);

/**
 * @brief Supprime des files les possibilités sans suite (impasses garanties).
 *
 * Parcourt chaque paquet et appelle `possibility_has_a_next` : si la case
 * courante n'admet plus aucune pièce compatible, le paquet est retiré des files.
 *
 * @param mapParts         Tableau 4D de lookup.
 * @param all_rotate_part  Tableau de toutes les rotations.
 * @return                 Nombre de possibilités supprimées.
 */
int remove_possibilities_with_no_next(map_big_array *mapParts, struct array_part *all_rotate_part);
#endif
