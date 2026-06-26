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
#include "app/static_variables.h"
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
    pid_t pid;
    int socket_id;
    struct tms start_socket;
    /// Horodatage (wall-clock) du dernier échange réseau, pour le keepalive :
    /// un worker occupé sur son stock local doit pinguer le serveur avant son
    /// timeout d'inactivité (tcp_timeout), sinon le serveur ferme la session.
    time_t last_socket_activity;
} client_possibility_t;

/**
 * @brief Lance un client en mode multi-thread
 * 
 * Le nombre de thread est en fonction de la variable globale NB_THREADS.
 * 
 * @param[in] file fichier contenant la définition des pieces
 */
void runThreadClient(const char *file);
/**
 * @brief Lance un client en mono-thread
 * 
 * @param file fichier contenant la définition des pieces
 */
void run_mono_client(const char *file);
/**
 * @brief Effectue un contrôle des threads client
 * 
 * @param param
 * @return void* null. Retourne un pointeur afin de respecter le format d'une méthode de thread.
 */
void *check_client_threads(void *param);

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
 * Factorise l'initialisation commune à `runThreadClient` et `run_mono_client`.
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
 */
void init_client_possibility(client_possibility_t *p, struct array_part *rotateParts,
                             map_big_array *map, int id, int compteur, pid_t pid);

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
