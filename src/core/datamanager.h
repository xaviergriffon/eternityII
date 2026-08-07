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
 * @brief Comme `add_possibility_analysed`, mais enregistre en plus le
 *        `client_uid` du client à qui LE SERVEUR sert cette possibilité
 *        (PR6, docs/conception/identification_clients.md, section 4.3 :
 *        attribution des analyses en cours). Réservé au côté serveur — côté
 *        client, `thread` est un index de fork local, sans rapport avec
 *        cette notion d'attribution, et `add_possibility_analysed` reste
 *        l'appel à utiliser (aucune attribution enregistrée).
 *
 * @param possiblity Paquet à enregistrer.
 * @param thread     Index du thread (−1 = choix automatique).
 * @param owner_uid  `client_uid` (16 octets) du client servi, jamais NULL
 *                    (utiliser `add_possibility_analysed` sinon).
 * @return           0.
 */
int add_possibility_analysed_owned(struct possibility_packet *possiblity, int thread,
                                    const uint8_t owner_uid[CLIENT_UID_BYTES]);

/**
 * @brief Résume ce qu'un client (`client_uid`) détient actuellement dans le
 *        pool « analysed » — consultation « que travaille X ? » (PR6).
 *
 * Balaye l'index latéral d'attribution adossé à `analysed_index` (jamais
 * `possibility_packet` lui-même, cf. arbitrage C du document de conception) :
 * une possibilité restaurée depuis un backup, ou servie côté client (jamais
 * via `add_possibility_analysed_owned`), n'a pas de propriétaire connu et
 * n'est donc jamais comptée ici, pour aucun `client_uid`.
 *
 * @param owner_uid     `client_uid` recherché (16 octets, jamais NULL).
 * @param out_count     Out : nombre de possibilités actuellement attribuées.
 * @param out_max_alloc Out : le plus grand `alloc` parmi elles, -1 si `*out_count == 0`.
 * @return              0 si OK, -1 si un paramètre est NULL.
 */
int datamanager_analysed_owned_by(const uint8_t owner_uid[CLIENT_UID_BYTES],
                                   unsigned long long *out_count, int *out_max_alloc);

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

/** @brief Code de retour de `backup`/`backup_analysed` : sauvegarde effectuée. */
#define BACKUP_OK 0
/** @brief Code de retour de `backup`/`backup_analysed` : échec (I/O, fopen, fwrite, rename…). */
#define BACKUP_ERROR (-1)
/**
 * @brief Code de retour de `backup`/`backup_analysed` : sauvegarde sautée car une
 * maintenance (tri, regroup, autre backup…) était déjà en cours (`maintenance` != 0).
 * Le fichier cible n'est ni créé ni modifié : l'ancienne sauvegarde reste intacte.
 * Les appelants critiques (arrêt sur solution, autobackup, commande console) doivent
 * journaliser ce cas : il ne s'agit PAS d'un succès silencieux.
 */
#define BACKUP_SKIPPED_MAINTENANCE 1

/**
 * @brief Effectue une sauvegarde fichier des files de possiblités.
 *
 * Écrit dans un fichier temporaire (`<filename>.tmp`, même répertoire) puis le
 * bascule atomiquement (`rename`) vers `filename` : un crash pendant l'écriture
 * ne corrompt jamais la sauvegarde précédente. `fwrite`/`fclose` sont contrôlés ;
 * en cas d'échec le `.tmp` est supprimé et `filename` reste inchangé.
 *
 * @param filename nom du fichier dans lequel faire la sauvegarde
 * @return BACKUP_OK (0) si la sauvegarde a été écrite et publiée,
 *         BACKUP_SKIPPED_MAINTENANCE (1) si elle a été sautée (maintenance en cours,
 *         fichier cible non touché), BACKUP_ERROR (-1) en cas d'erreur d'E/S.
 */
int backup(char *filename);
/**
 * @brief Effectue une sauvegarde fichier des possibilités en cours d'anlayse.
 * Les possibilités en cours d'analyse sont les possibilités fournies par le serveur.
 *
 * Mêmes garanties atomiques que `backup` (fichier temporaire + rename).
 *
 * @param filename nom du fichier dans lequel faire la sauvegarde
 * @return BACKUP_OK (0), BACKUP_SKIPPED_MAINTENANCE (1) ou BACKUP_ERROR (-1) — voir `backup`.
 */
int backup_analysed(char *filename);
/**
 * @brief Reconstruit les files avec le contenu du fichier
 * 
 * @param filename nom du fichier contenant une sauvegarde de file de possibilité
 * @return 0 si OK ou -1 en cas d'erreur
 */
int restore(char *filename);
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
 * @brief Variante fichier de printdatamanager : exporte toutes les files au
 *        format JSON dans @p out au lieu des logs (commande console
 *        `print [fichier]`).
 * @param out   Flux ouvert en écriture (ouverture/fermeture à la charge de l'appelant).
 * @param count Décompte cumulé des possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 si une écriture a échoué (export interrompu).
 */
int fprint_datamanager(FILE *out, size_t *count);

/**
 * @brief Variante fichier de print_file : exporte la file `fp` au format
 *        JSON dans @p out (commande console `printFile <n> [fichier]`).
 * @param out   Flux ouvert en écriture.
 * @param fp    Numéro de la file à exporter.
 * @param count Accumulateur du nombre de possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 dès la première écriture en échec.
 */
int fprint_file(FILE *out, int fp, size_t *count);

/**
 * @brief Affiche au format JSON les possibilités en cours d'analyse dans la file `fp`.
 * @param fp Numéro de la file analysée à afficher.
 * @return   0 si OK ou -1 en cas d'erreur.
 */
int print_file_analysed(int fp);

/**
 * @brief Variante fichier de print_file_analysed (commande console
 *        `printAnalysed [fichier]`, cette fonction traite une seule file).
 * @param out   Flux ouvert en écriture.
 * @param fp    Numéro de la file d'analyse à exporter.
 * @param count Accumulateur du nombre de possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 dès la première écriture en échec.
 */
int fprint_file_analysed(FILE *out, int fp, size_t *count);

/** @brief Affiche au format JSON toutes les files d'analyse en cours. */
int print_all_file_analysed(void);

/**
 * @brief Variante fichier de print_all_file_analysed : exporte toutes les
 *        files d'analyse dans @p out (commande console `printAnalysed [fichier]`).
 * @param out   Flux ouvert en écriture.
 * @param count Décompte cumulé des possibilités écrites, NULL si inutile.
 * @return      0 en cas de succès, -1 si une file a échoué à s'écrire.
 */
int fprint_all_file_analysed(FILE *out, size_t *count);

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
 * @brief Vérifie la cohérence structurelle d'une `File` (taille, chaînage, fin).
 *
 * Exposée (plutôt que `static`) pour être testable en isolation : aucune API
 * publique ne permet de fabriquer une `File` incohérente via les pools internes,
 * donc les tests construisent des `File`/`Element` à la main.
 *
 * @param file  File à contrôler.
 * @param f     Indice de la file (pour les messages de log).
 * @param label Nom du pool (« unchecked » / « checked ») pour les messages.
 * @return      0 si cohérent, -1 sinon.
 */
int check_one_file(File *file, int f, const char *label);

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

/**
 * @brief Développe le stock du serveur jusqu'à un niveau de curseur cible.
 *
 * Transforme un stock maigre (typiquement le paquet genèse et ses premiers
 * enfants) en de nombreuses possibilités distribuables, en développant chaque
 * possibilité case par case (une pièce candidate par successeur, via
 * `search_possiblity_light`) jusqu'à ce que son curseur `alloc` atteigne
 * `target_level`. But : supprimer la famine du démarrage, où un seul client
 * retient tout l'arbre pendant que le serveur n'a rien à servir aux autres.
 * Calcul purement serveur (avant toute connexion) : impact client nul.
 *
 * Bornée sur deux axes pour ne pas mettre le serveur au travail trop longtemps :
 *  - `EXPAND_MAX_LEVELS` passes maximum (borne en profondeur), quelle que soit
 *    la consigne `target_level` ;
 *  - `EXPAND_MAX_STOCK` possibilités (borne en nombre, contrôlée entre passes) —
 *    garde-fou contre un facteur de branchement élevé.
 *
 * Les branches mortes (aucun successeur) sont élaguées au passage. À appeler
 * pendant que la `map_big_array` est encore vivante (dans `runserver`, avant
 * `free_bigarray`, ou après reconstruction pour la commande console `expand`).
 *
 * @param target_level    Niveau de curseur `alloc` minimal visé (≤ 0 : no-op).
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return                Nombre de passes d'expansion réellement effectuées.
 */
int expand_datas_to_level(int target_level, map_big_array *mapParts, struct array_part *all_rotate_part);
#endif
