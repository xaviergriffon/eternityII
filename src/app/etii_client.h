/**
 * @file etii_client.h
 * @brief Méthodes pour un client EternityII
 * 
 */
#ifndef etii_client_h
#define etii_client_h

#include <pthread.h>
#include <sys/times.h>
#include <sys/types.h>
#include "app/app_static_variables.h"
#include "core/possibility.h"
#include "core/part.h"

/**
 * @brief Structure représentant un thread de client EternityII
 * 
 */
typedef struct
{
    volatile int works;
    pthread_mutex_t works_mutex;
    /// Sérialise les échanges réseau sur socket_id : le thread d'alimentation et
    /// le thread de recherche partagent le même socket, et leurs échanges
    /// (send_instruction + send + recv ack) ne doivent pas s'entrelacer.
    pthread_mutex_t socket_mutex;
    pthread_t *tid;
    /**
     * @todo définir et renommer
     */
    array_possibility_packet *aposs;
    map_big_array *map_part;
    struct array_part *all_rotate_part;
    int compteur;
    int max_shots_per_second;
    int id;
    /// Rang de ce fork parmi les NB_THREADS forks de son process parent
    /// (0..N-1), capturé AVANT que le fork ne réduise sa propre vue de
    /// NB_THREADS à 1 (cf. handle_client, src/app/main.c). Distinct de `id`,
    /// qui indexe LOCALEMENT `file_possibility_analysed[]` (toujours 0 après
    /// fork : NE JAMAIS réutiliser `id` pour cet usage, cf.
    /// send_possibility_analysed/add_possibility_analysed, src/core/datamanager.c).
    /// Sert uniquement à peupler `client_identity_t.fork_seq` du hello envoyé
    /// sur la connexion de travail (INST_CLIENT_HELLO, check_and_connect_to_server).
    int fork_seq;
    pid_t pid;
    int socket_id;
    struct tms start_socket;
    /// Horodatage (wall-clock) du dernier échange réseau, pour le keepalive :
    /// un worker occupé sur son stock local doit pinguer le serveur avant son
    /// timeout d'inactivité (tcp_timeout), sinon le serveur ferme la session.
    time_t last_socket_activity;
    /// Buffer de délégation réutilisé par `bt_delegate_if_needed` (etii_search.c) :
    /// la boucle de recherche délègue le surplus de travail toutes les
    /// DELEGATE_MIN_INTERVAL_MS et matérialise alors jusqu'à `max_stock_by_thread`
    /// paquets. Pré-allouer ce tampon une fois par thread évite un
    /// malloc/free de ~max_stock_by_thread × sizeof(possibility_packet) à chaque
    /// délégation. Alloué paresseusement à la première délégation, agrandi si
    /// `max_stock_by_thread` augmente à chaud, libéré en fin de thread `autosearch`.
    struct possibility_packet *delegate_buf;
    /// Capacité courante (en paquets) de `delegate_buf` (0 si non alloué).
    int delegate_buf_capacity;
} client_possibility_t;

/**
 * @brief Pièces de recherche : tableau des rotations + map de lookup 4D.
 *
 * Les deux vont toujours ensemble (la map pointe sur les mêmes pièces) et ont
 * la même durée de vie ; les réunir permet de n'avoir qu'UN seul propriétaire
 * à suivre, qu'elles soient héritées du process parent ou construites
 * localement (cf. `acquire_search_parts`).
 */
typedef struct search_parts
{
    /// Toutes les rotations de toutes les pièces (`rotate_all_parts`).
    struct array_part *rotate_parts;
    /// Map de lookup (top, right, bottom, left) -> pièces (`prepare_map_part`).
    map_big_array *map;
} search_parts_t;

/**
 * @brief Construit les pièces de recherche à partir du fichier CSV de pièces.
 *
 * Enchaîne `read_parts` -> `rotate_all_parts` -> `prepare_map_part` et libère
 * le tableau intermédiaire. Comme `read_parts`, quitte le process si le fichier
 * est illisible ou mal formé.
 *
 * @param out  Structure à remplir (les deux champs sont écrits).
 * @param file Chemin du fichier CSV de définition des pièces.
 */
void build_search_parts(search_parts_t *out, const char *file);

/**
 * @brief Libère des pièces de recherche et remet les champs à NULL.
 *
 * Tolère `NULL` et les champs déjà NULL (donc idempotent : appelable deux fois
 * sans double libération).
 *
 * @param parts Structure à libérer (peut être NULL).
 */
void free_search_parts(search_parts_t *parts);

/**
 * @brief Publie les pièces que les process enfants doivent réutiliser.
 *
 * Appelé par le process PARENT du client AVANT sa boucle de `fork()`
 * (`handle_client`, src/app/main.c) : la map n'étant plus jamais écrite après
 * sa construction, les enfants la partagent physiquement par copy-on-write au
 * lieu d'en construire chacun une copie privée (5,06 Mo de `flat` + 1,27 Mo
 * d'index compact + 0,11 Mo d'arène par process). Le parent reste
 * propriétaire : il est le seul à appeler `free_search_parts` dessus.
 *
 * @param parts Pièces à publier, ou NULL pour effacer la publication.
 */
void set_inherited_search_parts(const search_parts_t *parts);

/**
 * @brief Récupère les pièces de recherche à utiliser : héritées ou construites.
 *
 * @param out  Structure à remplir.
 * @param file Fichier CSV utilisé si aucune map n'a été publiée par le parent.
 * @return     0 si les pièces sont HÉRITÉES (ne rien libérer : elles
 *             appartiennent au process parent), 1 si elles viennent d'être
 *             construites (l'appelant doit appeler `free_search_parts`).
 */
int acquire_search_parts(search_parts_t *out, const char *file);

/**
 * @brief Lance un client en mono-thread
 *
 * @param file     fichier contenant la définition des pieces
 * @param fork_seq rang de ce fork (0..N-1) parmi les forks de son process
 *                 parent, propagé jusqu'au hello de la connexion de travail
 *                 (INST_CLIENT_HELLO) ; 0 en mode `test` (pas de vrais forks).
 */
void run_mono_client(const char *file, int fork_seq);
/**
 * @brief Effectue un contrôle des threads client
 * 
 * @param param
 * @return void* null. Retourne un pointeur afin de respecter le format d'une méthode de thread.
 */
void *check_client_threads(void *param);

/**
 * @brief Un tour de la boucle de `check_client_threads` (rapport + record), sans
 *        le `sleep` de fin de tour.
 *
 * Extrait pour être testable hors thread. Voir etii_client.c pour le détail.
 *
 * @param last_record In/out : meilleur résultat déjà annoncé (détection de record).
 */
void check_client_threads_step(int *last_record);

/**
 * @brief Calcule la prochaine durée de back-off quand le serveur n'a rien à fournir.
 *
 * Fonction pure : double la valeur jusqu'au plafond `NO_WORK_SLEEP_MAX`.
 * Retourne `NO_WORK_SLEEP_START` si `current == 0` (premier cycle à vide).
 *
 * @param current Valeur de pause actuelle (en µs).
 * @return Prochaine valeur de pause (en µs).
 */
useconds_t next_no_work_sleep(useconds_t current);

/**
 * @brief Initialise les champs d'une structure `client_possibility_t`.
 *
 * Initialisation du contexte d'un thread de recherche (`run_mono_client`).
 * L'appelant fournit les tableaux de pièces et la map déjà construits ; cette
 * fonction ne fait aucun appel réseau ni appel système autre que `time()` et
 * `times()`.
 *
 * @param p          Structure à initialiser.
 * @param rotateParts Tableau de pièces en rotation (alloué par l'appelant).
 * @param map         Map de lookup (allouée par l'appelant via `prepare_map_part`).
 * @param id          Identifiant logique du thread (indice dans le pool).
 * @param compteur    Indice du compteur associé.
 * @param pid         PID du process propriétaire (0 si non utilisé).
 * @param fork_seq    Rang de ce fork parmi les forks du process parent
 *                    (0..N-1) — cf. `client_possibility_t.fork_seq`.
 */
void init_client_possibility(client_possibility_t *p, struct array_part *rotateParts,
                             map_big_array *map, int id, int compteur, pid_t pid, int fork_seq);

/**
 * @brief Compte le nombre de processus enfants effectivement créés.
 *
 * Un processus est considéré créé si `pids[i] > 0`.
 *
 * @param pids Tableau des PID (taille `nb`).
 * @param nb   Taille du tableau.
 * @return     Nombre d'entrées où `pids[i] > 0`.
 */
int count_created_forks(pid_t *pids, int nb);

/**
 * @brief Cherche l'indice du socket Unix enfant dont le `sun_path` correspond.
 *
 * @param sun_path Chemin à chercher.
 * @param forkIds  Tableau de chaînes (chemins socket de chaque fork).
 * @param nb       Taille du tableau.
 * @return         Indice du premier correspondant, ou -1 si absent.
 */
int find_fork_index(const char *sun_path, char **forkIds, int nb);

/**
 * @brief Construit le tableau « Thread queues » du rapport client (une ligne par
 *        fork + Total) dans une chaîne allouée ; renvoie via out-params (NULL
 *        accepté) le stock total, l'analysed total et la somme des coups/s, et
 *        met à jour max_result. Buffer dimensionné sur NB_THREADS. À libérer.
 */
char *build_thread_queues_table(unsigned long long *out_stock,
                                unsigned long long *out_analysed,
                                unsigned long long *out_shots_per_sec);

/**
 * @brief Construit le tableau « Search depth » de la commande console `min`
 *        côté client/pruner (une ligne par fork + Min) dans une chaîne
 *        allouée ; à libérer par l'appelant.
 *
 * Remplace `search_min_datas()` côté client : les files du datamanager
 * restent vides après fork (chaque fork explore en interne — voir AGENTS.md),
 * donc la seule source de vérité est ce que chaque fork remonte lui-même par
 * IPC (`client_statistics.root_depth`/`min_pending_depth`). Colonne
 * « Racine » : profondeur de la possibilité reçue du serveur, fixe pour
 * toute la durée de son étude. Colonne « Min » : profondeur minimale ENCORE
 * EN ATTENTE dans la pile de décisions de ce fork — PAS la profondeur du
 * chemin en cours d'exploration, qui ne fait que croître et peut donc être
 * bien plus profonde que ce que ce fork détient encore de plus superficiel
 * (voir `bt_min_pending_depth`, core/etii_search.c). `-1` (idle, ou rôle
 * pruner : pas de recherche en cours) s'affiche `-`.
 */
char *build_thread_depth_table(void);

/**
 * @brief Alimente un thread de recherche en travail (un tour de la boucle `for`
 *        de `feed_thread_aposs`).
 *
 * Extrait du corps de boucle pour être testable hors thread (en mode local,
 * `server_ip == NULL`, les échanges passent par le datamanager). No-op si
 * `request != REQUEST_CONTINUE`. Quand le thread `i` manque de travail
 * (`works == 0`), draine son « en analyse » puis tente d'obtenir une (ou un lot
 * de) possibilité(s) ; s'il en reçoit, les empile et passe `works = 1`. Sinon,
 * s'il a un socket ouvert, émet un keepalive. Incrémente en place `*needed_work`
 * (thread ayant réclamé) et `*got_work` (thread ayant reçu).
 *
 * @param thread_params Tableau des contextes de threads de recherche.
 * @param i             Indice du thread à alimenter.
 * @param needed_work   Compteur in/out des threads ayant réclamé du travail.
 * @param got_work      Compteur in/out des threads ayant reçu du travail.
 */
void feed_one_thread(client_possibility_t *thread_params, int i,
                     int *needed_work, int *got_work);

/**
 * @brief Exécute un tour de la boucle de régulation du débit (`control_thread`).
 *
 * Fonction pure (aucune I/O, ne dort pas) extraite du corps du `while` de
 * `control_thread` pour être testable hors du thread. Quand `max_search_by_sec`
 * est positif, accumule les coups joués depuis le dernier tour
 * (`counters[t] - lastCheck[t]`) sur les threads actifs, estime un débit par
 * seconde et bascule la globale `request` entre REQUEST_CONTINUE et REQUEST_PAUSE
 * selon ce débit. Réinitialise la fenêtre de mesure (`*oneSecond`, `*nbCheck`)
 * tous les 1000 tours. Sans effet sur `request` si `max_search_by_sec == 0`.
 *
 * @param thread_params Tableau des contextes de threads de recherche (≥ NB_THREADS).
 * @param lastCheck     Compteurs par thread du tour précédent (taille NB_THREADS).
 * @param oneSecond     Accumulateur de coups de la fenêtre courante (in/out).
 * @param nbCheck       Compteur de tours de la fenêtre courante (in/out).
 */
void control_step(client_possibility_t *thread_params,
                  unsigned long long *lastCheck,
                  unsigned long long *oneSecond,
                  int *nbCheck);

#endif /* etii_client_h */
