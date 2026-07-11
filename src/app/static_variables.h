#ifndef static_variables_h
#define static_variables_h

#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/un.h>
#include <pthread.h>
#include "app/etii_statistic.h"

// v7 : réponse GET unitaire cadrée (int32 K + K paquets, send_all/recv_all)
// au lieu du send()/recv() brut discriminé par la longueur (INST_NULL 1 octet
// vs paquet ~520 octets) — une lecture TCP partielle désynchronisait le flux.
// v8 : INST_NEED_WORK (sonde de faim du serveur, réponse int32) — permet la
// délégation anticipée quand le stock serveur ne suffit plus à nourrir les
// autres clients (famine du démarrage).
// v9 : INST_CONTROL_HELLO — un canal de contrôle TCP dédié où le serveur
// devient l'initiateur, transportant des trames cadrées CTRL_* (cf.
// control_protocol.h). N'affecte pas le protocole de travail existant
// (GET/ADD/ANALYSED), qui reste inchangé.
#define VERSION 9

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
// Cadence (secondes) de la sonde de faim du serveur (INST_NEED_WORK) émise par
// le thread d'alimentation pour chaque thread occupé disposant d'un socket.
// Elle remplace le keepalive INST_TEST_CONNECTED (un échange réussi prouve la
// session vivante) : l'intervalle effectif est min(tcp_timeout/2, cette valeur).
#define NEED_WORK_POLL_INTERVAL_S 2
// Faim du serveur par client actif : le serveur vise un stock d'au moins
// SERVER_HUNGER_PER_CLIENT × sessions connectées (marge pour que chaque GET
// trouve une possibilité), et publie le manque via INST_NEED_WORK.
#define SERVER_HUNGER_PER_CLIENT 2
// Plafond de la faim annoncée par le serveur : borne la matérialisation et
// l'envoi demandés aux clients occupés (chaque thread cède déjà au plus la
// moitié de son stock implicite, mais tous peuvent répondre en même temps).
#define SERVER_HUNGER_CAP 1000
#define MAX_STOCK_BY_THREAD 300
// Intervalle minimal entre deux délégations de possibilités au serveur (ms).
// Une délégation coûte jusqu'à max_stock_by_thread aller-retours TCP synchrones
// exécutés par le thread de recherche : sa fréquence doit être bornée en temps,
// pas en nombre de nœuds explorés (sinon elle croît avec la vitesse du moteur).
#define DELEGATE_MIN_INTERVAL_MS 500
// Nombre de nœuds explorés entre deux consultations de l'horloge par la boucle
// chaude de backtracking (search_packet_backtracking). Un clock_gettime par
// nœud coûterait plus cher que le nœud lui-même : on n'évalue la fenêtre
// DELEGATE_MIN_INTERVAL_MS qu'une fois tous les N nœuds.
#define DELEGATE_CHECK_INTERVAL_NODES 1000000
// Nombre de possibilités demandées au serveur par requête d'un client pruner
// (valeur PAR DÉFAUT de `pruner_batch_size`). Le contrôle d'une possibilité est
// rapide : sans lot, l'aller-retour TCP dominerait le coût.
#define PRUNER_BATCH_SIZE 100
// Borne supérieure de la taille de lot pruner configurable (`pruner_batch_size`).
// Plafonne la mémoire d'un échange par lot (côté serveur comme pruner) et la
// taille des tampons GPU managés. 65536 × ~0,5 Ko ≈ 36 Mo.
#define PRUNER_BATCH_MAX 65536
// Expansion du stock au démarrage du serveur (option `--expand-level`, commande
// console `expand`). Le serveur développe lui-même les possibilités du stock
// (une pièce candidate par case suivante) jusqu'à ce que leur curseur `alloc`
// atteigne le niveau demandé, ce qui transforme le paquet genèse en des
// milliers de possibilités distribuables — supprimant la famine du démarrage où
// un seul client détient tout l'arbre pendant que le serveur n'a rien à servir.
// L'impact client est nul (calcul purement serveur, avant toute connexion).
//
// EXPAND_MAX_LEVELS : nombre maximal de passes d'expansion, quelle que soit la
// consigne de niveau — garde-fou en PROFONDEUR pour ne pas mettre le serveur au
// travail trop longtemps.
#define EXPAND_MAX_LEVELS 4
// EXPAND_MAX_STOCK : plafond de sécurité en NOMBRE de possibilités. Le facteur
// de branchement du puzzle étant inconnu et variable, la seule borne en
// profondeur ne borne pas le travail réel ; on arrête donc l'expansion entre
// deux passes dès que le stock dépasse ce seuil. ~100000 × ~0,5 Ko ≈ 54 Mo.
#define EXPAND_MAX_STOCK 100000

#define REQUEST_STOP 1
#define REQUEST_CONTINUE 0
#define REQUEST_PAUSE 2
// Pause « administrative », déclenchée par la commande console `pause` (et,
// plus tard, un canal de contrôle distant) — PAR OPPOSITION à REQUEST_PAUSE,
// posée puis levée automatiquement par le régulateur de débit (`control_step`,
// src/app/etii_client.c) dès que le débit repasse sous `max_search_by_sec` ou
// qu'un thread devient inactif. Si l'on réutilisait REQUEST_PAUSE pour une
// pause opérateur, ce même mécanisme de régulation la lèverait involontairement
// dès le tour suivant. REQUEST_ADMIN_PAUSE n'est donc jamais touchée par
// `control_step` (comparaisons strictes à REQUEST_PAUSE) : seule la commande
// console `resume` (ou son équivalent distant futur) peut la lever.
#define REQUEST_ADMIN_PAUSE 3

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
 * @brief 1 si l'on s'arrête à la première solution (option `--stop-on-solution`).
 *
 * Défaut 0 : on continue après une solution — le processus de recherche backtrack
 * pour en chercher d'autres, et le serveur reste en service pour que les clients
 * continuent d'explorer. À 1 : le processus de recherche qui trouve une solution
 * sort, et le serveur qui en reçoit une sauvegarde son stock puis s'arrête.
 *
 * Lue dans `main()` AVANT tout fork → héritée par les processus enfants.
 */
extern int stop_on_solution;

/**
 * @brief Niveau de curseur (`alloc`) minimal visé par l'expansion du stock au
 *        démarrage du serveur (option CLI `--expand-level <n>`).
 *
 * 0 (défaut) : pas d'expansion. Sinon, `runserver` développe le stock genèse
 * jusqu'à ce que chaque possibilité atteigne ce niveau, borné par
 * `EXPAND_MAX_LEVELS` passes et `EXPAND_MAX_STOCK` possibilités. Lu côté serveur
 * uniquement (les autres modes l'ignorent). Position-indépendant, retiré d'argv
 * par `parse_cli_options` avant le parsing positionnel.
 */
extern int expand_min_level;

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

/**
 * @brief Cumul des cases étudiées par les contrôles de possibilité du prunage.
 *
 * Chaque contrôle d'une possibilité (`possibility_all_has_a_next`, client
 * pruner ou élagage `rmnonext`) balaie plusieurs cases du plateau : ce cumul
 * compte chacune de ces études de case — la même unité qu'un coup de la
 * recherche, mais dans un flux DISJOINT de `counters` (pas de double compte).
 * Avec `fc_cells_studied`, il alimente le débit « dont prunage/s » et
 * l'indice « études/s (recherche+prunage) » des rapports `check`.
 */
extern volatile unsigned long long pruner_cells_studied;

/**
 * @brief Cumul des cases inspectées par le forward-checking.
 *
 * Chaque appel à `forward_check_next_k` / `bt_forward_check` inspecte jusqu'à
 * `FORWARD_CHECK_K` cases : ce cumul compte chaque case réellement inspectée
 * (cases déjà remplies sautées non comptées). Même unité qu'un coup de la
 * recherche, flux disjoint de `counters`. Reste à 0 quand
 * `FORWARD_CHECK_K == 0`. Incrémenté par ajout atomique (boucle chaude
 * multi-thread), comme `fc_attempts`.
 */
extern volatile unsigned long long fc_cells_studied;

extern unsigned long long *counters;
extern unsigned long long *lastfilesize;

extern volatile uint16_t max_result;

/**
 * @brief Dernier rapport de statistiques formaté (commande console `check`).
 *
 * Republié toutes les 10 secondes par les threads de statistiques
 * (`check_server` / `check_client_threads`) : ceux-ci font free() de l'ancien
 * buffer puis calloc()+strcat() un nouveau rapport, pendant que le thread
 * console peut concurremment lire `lastcheck` à tout moment (commande
 * `check`). Sans synchronisation, cette lecture peut tomber pendant le swap
 * -> use-after-free (lecture d'un buffer déjà libéré) ou lecture d'un buffer
 * encore partiellement rempli. `lastcheck_mutex` protège l'écriture ET la
 * lecture ; voir `lastcheck_publish()`.
 */
extern char *lastcheck;

/**
 * @brief Mutex protégeant toutes les lectures/écritures de `lastcheck`.
 *
 * Toujours utiliser `lastcheck_publish()` pour publier un nouveau rapport
 * (construit dans un buffer local par l'appelant) : la section critique se
 * limite alors à l'échange de pointeur + free() de l'ancien buffer, ce qui
 * garde le verrou détenu le moins longtemps possible. Les lecteurs (ex. la
 * commande console `check`) doivent prendre ce même mutex avant de déréférencer
 * `lastcheck`.
 */
extern pthread_mutex_t lastcheck_mutex;

/**
 * @brief Publie atomiquement un nouveau rapport `lastcheck`.
 *
 * Prend `lastcheck_mutex`, libère l'ancien buffer, installe `new_report` à sa
 * place, puis relâche le mutex. `new_report` doit avoir été alloué (ex.
 * calloc/malloc) par l'appelant, qui construit tout son contenu (les
 * strcat/sprintf successifs) AVANT d'appeler cette fonction : la section
 * critique reste ainsi réduite au seul échange de pointeur, jamais à la
 * construction du rapport.
 *
 * @param new_report Nouveau buffer à publier (peut être NULL).
 */
void lastcheck_publish(char *new_report);

/**
 * @brief Vrai si `r` est l'une des deux valeurs de pause (régulation OU admin).
 *
 * Regroupe `REQUEST_PAUSE` et `REQUEST_ADMIN_PAUSE` : les boucles chaudes qui
 * doivent juste attendre (usleep + continue) sans traiter cela comme un arrêt
 * n'ont pas à connaître la distinction entre les deux origines de pause.
 *
 * @param r Valeur de `request` à tester.
 * @return  1 si `r == REQUEST_PAUSE || r == REQUEST_ADMIN_PAUSE`, 0 sinon.
 */
int request_is_pause(int r);

/**
 * @brief Vrai si `r` ne signale pas un arrêt (`REQUEST_STOP`).
 *
 * Regroupe l'idée « on continue de tourner », que ce soit en fonctionnement
 * normal (`REQUEST_CONTINUE`), en pause de régulation (`REQUEST_PAUSE`) ou en
 * pause administrative (`REQUEST_ADMIN_PAUSE`) : seules les boucles d'attente
 * de travail doivent rester actives dans ces trois cas et se terminer sur
 * `REQUEST_STOP`.
 *
 * @param r Valeur de `request` à tester.
 * @return  1 si `r != REQUEST_STOP`, 0 sinon.
 */
int request_keeps_running(int r);

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

// Dernière faim du serveur connue du processus (réponse INST_NEED_WORK) :
// écrite par le thread d'alimentation (sonde), lue par les threads de recherche
// dans le bloc throttlé de délégation, décrémentée après une délégation
// anticipée. Toujours via __atomic_* (accès inter-threads sans mutex).
extern int server_hunger;

extern int communication_in_progress;

#ifdef DEBUG_SOCKET
extern int opened_tcp;
#endif // DEBUG_SOCKET

extern long nb_client;

extern int tcp_timeout;

extern int server;

extern int server_rmnonext_timing;

/**
 * @brief Extrait les options globales de `argv` et les retire du tableau.
 *
 * Reconnaît `--stop-on-solution` (positionne `stop_on_solution`). Compacte
 * `argv` en place pour supprimer les options reconnues, afin de ne pas perturber
 * le parsing positionnel des modes. Appelée AVANT tout fork.
 *
 * @param argc Nombre d'arguments.
 * @param argv Tableau d'arguments (modifié en place : options retirées).
 * @return     Le nouveau nombre d'arguments (sans les options reconnues).
 */
int parse_cli_options(int argc, const char *argv[]);
#endif /* static_variables_h */
