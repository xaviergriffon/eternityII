#ifndef static_variables_h
#define static_variables_h

#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/un.h>
#include "app/etii_statistic.h"

#define VERSION 6

#define NB_CONNECTIONS_PER_THREAD 1
// Temps d'attente de 100 microsecondes
#define MICRO_SLEEP 100
// Temps d'attente court de 10 microsecondes
#define MICRO_SHORT_SLEEP 10
// Temps d'attente pour les boucles de threads
#define THREAD_MICRO_SLEEP 10000
// Back-off du thread d'alimentation quand le serveur n'a AUCUNE possibilité à
// fournir (stock épuisé, ou serveur saturé qui ne répond pas au handshake) : au
// lieu de redemander toutes les THREAD_MICRO_SLEEP (≈ 100 req/s/thread, ce qui
// alimente la contention « all threads busy »), on attend de plus en plus
// longtemps (doublement) jusqu'à un plafond, puis on repart à zéro dès qu'un
// travail est obtenu. Bornes en microsecondes.
#define NO_WORK_SLEEP_START 50000    // 50 ms : première pause après un cycle à vide
#define NO_WORK_SLEEP_MAX  500000    // 0,5 s : plafond (sous la limite usleep POSIX de 1 s)
#define MAX_STOCK_BY_THREAD 300
// Intervalle minimal entre deux délégations de possibilités au serveur (ms).
// Une délégation coûte jusqu'à max_stock_by_thread aller-retours TCP synchrones
// exécutés par le thread de recherche : sa fréquence doit être bornée en temps,
// pas en nombre de nœuds explorés (sinon elle croît avec la vitesse du moteur).
#define DELEGATE_MIN_INTERVAL_MS 500
// Nombre de possibilités demandées au serveur par requête d'un client pruner
// (valeur PAR DÉFAUT de `pruner_batch_size`). Le contrôle d'une possibilité est
// rapide : sans lot, l'aller-retour TCP dominerait le coût.
#define PRUNER_BATCH_SIZE 100
// Borne supérieure de la taille de lot pruner configurable (`pruner_batch_size`).
// Plafonne la mémoire d'un échange par lot (côté serveur comme pruner) et la
// taille des tampons GPU managés. 65536 × ~0,5 Ko ≈ 36 Mo.
#define PRUNER_BATCH_MAX 65536

#define REQUEST_STOP 1
#define REQUEST_CONTINUE 0
#define REQUEST_PAUSE 2

#define DEFAULT_TCP_TIMEOUT 10

#define PART_SIZES 4
// Surchargeable via -DETERN_PARTS=16 (puzzle 4×4) sans éditer ce fichier : la CI
// compile les deux tailles. Défaut 256 (16×16). Cf. section Puzzle Configuration.
#ifndef ETERN_PARTS
#define ETERN_PARTS 256
#endif
#define ETERN_WITH_INDICES 1
#if ETERN_PARTS == 256
#define ETERN_SIZE 16
#define FACES_USED_SIZE 17// (ETERN_PARTS / 16) + 1;
#else
// 16 pieces
#define ETERN_SIZE 4
#define FACES_USED_SIZE 2// (ETERN_PARTS / 16) + 1;
#endif // ETERN_PARTS == 256

#define BUF_SIZE 300

/**
 * @brief Taille de la fenêtre de forward-checking.
 *
 * Après avoir placé une pièce à `directions[i]`, on vérifie que les
 * `FORWARD_CHECK_K` prochaines cases (`directions[i+1] ... directions[i+K]`)
 * possèdent encore au moins une pièce candidate compatible compte tenu de
 * l'état courant du plateau et du stock de pièces. Si l'une est « morte »,
 * la branche est abandonnée immédiatement sans la pousser dans la file.
 *
 * Une valeur plus élevée détecte les impasses plus tôt mais coûte plus de
 * lookups par placement. K=3 est un bon compromis par défaut.
 */
// Surchargeable via -DFORWARD_CHECK_K=0 (désactive le forward-checking) : la CI
// compile aussi cette variante. Défaut 6.
#ifndef FORWARD_CHECK_K
#define FORWARD_CHECK_K 6
#endif
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
// Passe en mono-process pour pouvoir débugger
//#define DEBUG_IN_MONO_PROCESS
// Trace des informations sur les commandes
//#define DEBUG_COMMANDS
// Trace des informations sur les threads
//#define DEBUG_THREAD
// ------------------------------------------------
#define FACES_USED_BITS

#if FORWARD_CHECK_K > 0
/**
 * @brief Compteur global du nombre de branches élaguées par forward-checking.
 *
 * Incrémenté à chaque fois qu'une pièce candidate placée dans le moteur de
 * recherche est rejetée parce qu'une des `FORWARD_CHECK_K` prochaines cases
 * est devenue « morte ». Utilise des additions atomiques relaxées pour
 * limiter la contention inter-threads.
 */
extern volatile unsigned long long fc_pruned;

/**
 * @brief Compteur global du nombre total d'appels au forward-checking.
 *
 * Sert de dénominateur pour calculer le taux d'élagage `fc_pruned / fc_attempts`.
 */
extern volatile unsigned long long fc_attempts;

#if FORWARD_CHECK_K > FC_STAT_MAX_K
#error "FORWARD_CHECK_K dépasse FC_STAT_MAX_K (voir etii_statistic.h)"
#endif

/**
 * @brief Cumul des élagages par distance de la première case morte.
 *
 * `fc_pruned_at[j]` compte les élagages dont la première case sans candidat
 * est à distance j (1..FORWARD_CHECK_K) du placement testé. La somme des
 * indices 1..K vaut `fc_pruned`. Permet d'estimer la taille des sous-arbres
 * économisés (croissance géométrique avec la distance) et donc le rapport
 * gain/coût de la valeur de K choisie.
 */
extern volatile unsigned long long fc_pruned_at[FORWARD_CHECK_K + 1];
#endif // FORWARD_CHECK_K > 0

extern uint8_t directions[ETERN_PARTS];

extern uint8_t dirx[ETERN_PARTS];

extern uint8_t diry[ETERN_PARTS];
/**
 * @brief  Nombre de threads clients
 */
extern int NB_THREADS;

/**
 * @brief 1 si le processus est un client pruner (mode `tcppruner`).
 *
 * Un client pruner ne cherche pas : il demande au serveur des possibilités non
 * vérifiées (INST_GET_TO_CHECK), contrôle que toutes leurs cases vides ont
 * encore au moins une pièce candidate (`possibility_all_has_a_next`), élimine
 * les mortes et renvoie les survivantes marquées `checked = 1`.
 */
extern int pruner_mode;

/**
 * @brief Nombre de possibilités qu'un client pruner demande/acquitte par lot.
 *
 * Configurable au démarrage (argument CLI de `tcppruner`/`gpupruner`) et à
 * l'exécution via la commande `prunerBatch <n>` (propagée aux process enfants).
 * Borne la mémoire de l'échange : le pruner ne détient jamais plus que ce lot,
 * la capacité mémoire n'a donc pas à être supposée illimitée. Défaut
 * `PRUNER_BATCH_SIZE`, plafonné à `PRUNER_BATCH_MAX`.
 */
extern int pruner_batch_size;

#ifdef WITH_CUDA
/**
 * @brief 1 si le processus est un client pruner GPU (mode `gpupruner`).
 *
 * Implique `pruner_mode == 1` (même plomberie réseau que `tcppruner`) mais le
 * contrôle des lots est délégué au GPU via `gpu_pruner_check_batch`. N'existe que
 * dans les builds CUDA (`make CUDA=1`).
 */
extern int gpu_pruner_mode;
#endif // WITH_CUDA

/** @brief Cumul des possibilités validées (et renvoyées) par ce processus pruner. */
extern volatile unsigned long long pruner_checked;

/** @brief Cumul des possibilités mortes éliminées par ce processus pruner. */
extern volatile unsigned long long pruner_removed;

extern unsigned long long *counters;
extern unsigned long long *lastfilesize;

extern volatile uint16_t max_result;
extern char *lastcheck;

// TODO : deplacer dans un parametre ?
extern char* parts_files;

extern unsigned long long non_null_possibilities;

extern volatile int request;

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
