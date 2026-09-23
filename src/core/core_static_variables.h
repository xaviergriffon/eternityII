#ifndef core_static_variables_h
#define core_static_variables_h

#include <stdint.h>
#include <unistd.h>
#include "app/etii_statistic.h"

// Ce fichier porte le sous-ensemble de l'état global EFFECTIVEMENT référencé
// sous `src/core/` (vérifié par grep, pas reconstitué de mémoire) : géométrie
// du puzzle, forward-checking, machine à états `request`, compteurs du pruner
// et de la recherche. Tout le reste — CLI, identité client, API HTTP, options
// serveur, bancs — vit dans `src/app/app_static_variables.h`, que `src/core/`
// ne doit PAS inclure : « core/ ne dépend jamais de app/ ».
//
// Exception documentée : `app/etii_statistic.h` ci-dessus, pour `FC_STAT_MAX_K`
// (dimensionnement de `fc_pruned_at[]`, partagé avec `struct
// client_statistics`, le message IPC parent↔enfant). Ce header n'a lui-même
// aucune dépendance : c'est une constante de format de message, pas de la
// logique applicative.
//
// Couplage RÉEL et non résolu par ce découpage : `datamanager.c` et
// `etii_search.c` lisent directement de l'état genuinement applicatif
// (version de protocole, port serveur, identité machine, options
// d'expansion/rebalance/bail). Le résoudre demande de le faire remonter par
// injection, comme `owner_alive` dans `datamanager_reclaim_expired_leases` —
// chantier séparé.

// Historique du protocole — un bump force tous les clients à se resynchroniser
// (la poignée de main est en correspondance exacte). Détail des trames :
// docs/echanges_client_serveur.md.
//  v7  : réponse GET cadrée (int32 K + K paquets, send_all/recv_all) au lieu
//        d'un send/recv brut discriminé par la longueur — une lecture TCP
//        partielle désynchronisait le flux.
//  v8  : INST_NEED_WORK, sonde de faim du serveur (délégation anticipée).
//  v9  : INST_CONTROL_HELLO, canal de contrôle dédié où le serveur devient
//        l'initiateur (net/control_protocol.h). Protocole de travail inchangé.
//  v10 : CTRL_GET_BEST_BOARD / CTRL_BEST_BOARD — tirer le plateau complet d'un
//        client, pas seulement son compte (core/best_board.h).
//  v11 : nouveau parcours de plateau (directions[]/dirx[]/diry[] en 256) — même
//        `alloc`, cases différentes, donc plateau corrompu sans bump.
//  v12 : INST_CLIENT_HELLO sur la connexion de TRAVAIL, identité déclarée par
//        fork (net/client_identity.h) ; control_hello_t étendu des mêmes champs.
//  v13 : `possibility_packet.alloc` change de SENS sans changer de type ni de
//        position — curseur de parcours avant, nombre de cases NON VIDES depuis
//        (référentiel qu'exige le moteur MRV). Sans bump, deux versions se
//        comprendraient sur le fil en désynchronisant le plateau en silence.
//
// `VERSION` elle-même vit dans app_static_variables.h (seul le réseau la
// compare) ; cet historique reste ici parce qu'il documente aussi l'évolution
// du SENS de `alloc`, un invariant core/ (cf. possibility.h).

// Temps d'attente de 100 microsecondes
#define MICRO_SLEEP 100
// Temps d'attente court de 10 microsecondes
#define MICRO_SHORT_SLEEP 10
// Temps d'attente pour les boucles de threads
#define THREAD_MICRO_SLEEP 10000
// Nombre de tours (chacun un balayage complet des NB_FILE_POSSIBILITY files,
// ou une tentative isolée sur une file fixée) après lequel les boucles
// d'attente active de datamanager.c (put_to_pool, scroll_from_pool,
// add_possibility_analysed_impl) abandonnent au lieu de tourner indéfiniment
// quand AUCUNE file du pool visé n'est disponible — typiquement une file
// gelée par une opération de maintenance (sauvegarde, restore, tri...).
// Borne le pire cas à DATAMANAGER_TRYLOCK_MAX_SWEEPS * MICRO_SLEEP (µs)
// ≈ 500 ms, très en-deçà de tcp_timeout par défaut (10 s,
// app/app_static_variables.h) : le client reçoit un stock K=0 ou un
// INST_ERROR gracieux, déjà géré des deux côtés, plutôt qu'un timeout de
// connexion.
#define DATAMANAGER_TRYLOCK_MAX_SWEEPS 5000
// Cadence de la boucle d'attente active en pause de RÉGULATION (REQUEST_PAUSE,
// control_step) dans la boucle chaude de recherche (etii_search.c :
// search_packet_backtracking, autoprune_step, autoprune_gpu). Utilisait
// MICRO_SHORT_SLEEP (10 µs) : correct pour espacer des itérations de calcul,
// mais appliqué à une pure attente de reprise, ça revient à ~100 000
// réveils/s/thread (chaque usleep() est un aller-retour noyau) rien que pour
// relire `request` — un thread en pause sature un cœur pour ne rien faire.
// Reste volontairement court (10 ms) : cette pause est réévaluée à chaque tour
// de control_step selon le débit mesuré, une attente plus longue fausserait
// cette mesure. Voir ADMIN_PAUSE_POLL_SLEEP_US pour la pause administrative,
// où la précision ne compte pas.
#define PAUSE_POLL_SLEEP_US 10000
// Cadence de la même boucle d'attente, mais pour une pause ADMINISTRATIVE
// (REQUEST_ADMIN_PAUSE, manuelle ou distante via le canal de contrôle) :
// durée arbitraire (peut être longue), aucune mesure de débit n'en dépend —
// on peut donc se permettre une attente bien plus grossière pour réduire
// encore la charge CPU (2 réveils/s/thread au lieu de 100).
#define ADMIN_PAUSE_POLL_SLEEP_US 500000
#define MAX_STOCK_BY_THREAD 300
// Défaut de shallow_root_abandon_depth : 0 = mécanisme désactivé. Voir la doc
// de la variable elle-même pour le rationale (le seuil de stock seul ne se
// déclenche jamais sur une racine peu profonde à branchement MRV fin).
#define SHALLOW_ROOT_ABANDON_DEPTH 0
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
// Nombre maximum de possibilités déposées en UN aller-retour INST_ADD_BATCH
// (v14, chemin de retour client → serveur). Borne la trame réseau et
// l'allocation que le serveur fait en face : 1024 × 576 o ≈ 590 Ko, à comparer
// aux 36 Mo qu'autoriserait PRUNER_BATCH_MAX. `put_to_server` découpe tout
// tableau plus grand en autant de lots ; le gain visé (supprimer l'aller-retour
// PAR possibilité) est déjà entièrement acquis dès la première centaine.
#define ADD_BATCH_MAX 1024

// Budget de nœuds de la preuve de fermeture bornée du pruner CPU
// (`pruner_dfs_budget`) : ce qu'une possibilité jugée vivante par le contrôle
// superficiel (`possibility_all_has_a_next_counted`), mais pas encore
// `checked`, peut consommer en backtracking réel avant que le pruner renonce
// à prouver sa fermeture et la conserve.
//
// 10000 est un ARBITRAGE, pas un optimum dérivé : il capture 88 % de ce que
// ferme un budget de 1 000 000 pour 1,5 % de son coût. Ce n'est PAS un coude —
// sur le stock mesuré la courbe monte encore (56,4 % à 1 000, 66,4 % à 10 000,
// 73,2 % à 100 000) là où celle de §4.10, sur un autre stock, plafonnait dès
// 1 000. Le point de fonctionnement dépend donc du stock, et 1000 reste un
// choix conservateur défendable. Mesures : §4.6c de
// docs/conception/elagage_recherche.md.
//
// Configurable à l'exécution (console `prunerDfsBudget <n>`, clé
// `dfs_budget`) ; `<= 0` court-circuite avant tout backtracking, donc
// désactiver est strictement gratuit.
//
// ATTENTION : ne traite QUE le flux. Une possibilité déjà `checked` n'est
// jamais resoumise à la preuve (cf. `reset_checked_pool`, datamanager.c) —
// le passif d'un stock existant demande un `resetChecked` explicite.
#define PRUNER_DFS_BUDGET_DEFAULT 10000
// Plafond de sécurité du budget configurable : au-delà, un seul contrôle de
// possibilité cesse d'être une opération bornée bon marché (l'objet même de
// cette PR) et se rapproche d'une recherche non plafonnée. Ne borne pas la
// MÉMOIRE (le backtracking borné n'alloue rien de plus que la recherche
// réelle) mais le TEMPS qu'un seul contrôle peut engager.
#define PRUNER_DFS_BUDGET_MAX 10000000

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

#define PART_SIZES 4
// Surchargeable via -DETERN_PARTS=16 (puzzle 4×4) sans éditer ce fichier : la CI
// compile plusieurs tailles. Défaut 256 (16×16). Cf. section Puzzle Configuration.
#ifndef ETERN_PARTS
#define ETERN_PARTS 256
#endif
#define ETERN_WITH_INDICES 1
// Tailles supportées. 256 (le vrai puzzle) et 16 (le 4×4 de test) sont
// historiques ; les tailles intermédiaires servent aux CLONES à solution
// connue du banc « côté trouver » (tools/gen_clone.py,
// tests/bench/bench_solve.c, docs/conception/banc_resolution_clones.md).
// L'énumération est explicite plutôt que calculée : le préprocesseur ne sait
// pas extraire une racine carrée, et une taille non carrée doit être refusée
// à la COMPILATION, pas produire une grille silencieusement incohérente.
#if ETERN_PARTS == 256
#define ETERN_SIZE 16
#elif ETERN_PARTS == 196
#define ETERN_SIZE 14
#elif ETERN_PARTS == 144
#define ETERN_SIZE 12
#elif ETERN_PARTS == 100
#define ETERN_SIZE 10
#elif ETERN_PARTS == 64
#define ETERN_SIZE 8
#elif ETERN_PARTS == 16
#define ETERN_SIZE 4
#else
#error "ETERN_PARTS non supporté : attendu 16, 64, 100, 144, 196 ou 256 (cf. docs/compilation.md)"
#endif // ETERN_PARTS
// Nombre de mots de 16 bits du masque `b_faceused` : ceil(ETERN_PARTS/16),
// plus un mot de marge (la formule donne déjà ceil+1 quand ETERN_PARTS est
// multiple de 16, et ceil quand il ne l'est pas — les deux couvrent
// ETERN_PARTS bits). Écrite en dur (17 / 2) jusqu'ici, un chiffre par taille ;
// l'expression unique évite d'avoir à en ajouter un par taille de clone.
#define FACES_USED_SIZE ((ETERN_PARTS / 16) + 1)

#define BUF_SIZE 300

/** @brief Stringification d'une macro (double expansion obligatoire). */
#define ETII_STRINGIFY_(x) #x
#define ETII_STRINGIFY(x) ETII_STRINGIFY_(x)

/**
 * @brief Bascule d'activation du forward-checking, et taille de fenêtre du
 *        chemin froid `forward_check_next_k`.
 *
 * Sur la boucle chaude (`bt_forward_check`), cette valeur ne borne plus
 * aucune fenêtre depuis le passage aux voisines géométriques (au plus 4,
 * indépendant de `FORWARD_CHECK_K`). Seul le chemin froid
 * `forward_check_next_k` garde l'ancienne sémantique : après un placement,
 * vérifie que les `FORWARD_CHECK_K` prochaines cases du parcours ont encore
 * un candidat.
 *
 * `FORWARD_CHECK_K == 0` reste le seul interrupteur : compile tout le
 * forward-checking hors du binaire (les deux chemins).
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
 * @brief Cumul des élagages par position dans la fenêtre inspectée.
 *
 * `fc_pruned_at[j]` compte les élagages dont la première case sans candidat
 * est en position j (1..). Deux appelants contribuent avec des fenêtres de
 * nature différente : `bt_forward_check` (boucle chaude) inspecte au plus 4
 * voisines géométriques (j ∈ [1,4]) ; `forward_check_next_k` (chemins
 * froids) inspecte les `FORWARD_CHECK_K` prochaines cases du parcours
 * (j ∈ [1, FORWARD_CHECK_K]). Tableau dimensionné sur `FC_STAT_MAX_K`
 * (indépendant de `FORWARD_CHECK_K`) pour rester sûr quel que soit le
 * domaine. La somme de tous les indices vaut toujours `fc_pruned`.
 */
extern volatile unsigned long long fc_pruned_at[FC_STAT_MAX_K + 1];

/**
 * @brief Compteur du nombre d'élagages dus à un conflit de singletons —
 *        sous-ensemble de `fc_pruned`.
 *
 * Incrémenté par `bt_forward_check` quand deux voisines de la pièce posée
 * exigent chacune, comme seul candidat encore libre, la même pièce — cas
 * `|S| = 2` du théorème de Hall, structurellement invisible au forward-check
 * case-par-case (chaque voisine prise isolément a bien ≥ 1 candidat). Actif
 * seulement quand `singleton_conflict_check` est levé.
 */
extern volatile unsigned long long fc_singleton_conflict;
#endif // FORWARD_CHECK_K > 0

extern uint8_t directions[ETERN_PARTS];

extern uint8_t dirx[ETERN_PARTS];

extern uint8_t diry[ETERN_PARTS];

/**
 * @brief Cumul des cases inspectées par le forward-checking.
 *
 * Chaque case réellement inspectée (les cases déjà remplies ne comptent
 * pas) par `bt_forward_check` ou `forward_check_next_k`. Flux disjoint de
 * `counters`. Reste à 0 quand `FORWARD_CHECK_K == 0`.
 */
extern volatile unsigned long long fc_cells_studied;

/**
 * @brief 1 si l'on s'arrête à la première solution (`--stop-on-solution`).
 *
 * Défaut 0 : on continue après une solution, le serveur reste en service. À
 * 1 : le processus qui trouve une solution sort, et le serveur qui la reçoit
 * sauvegarde son stock puis s'arrête.
 *
 * Lue dans `main()` avant tout fork, donc héritée par les enfants ; d'où sa
 * place ici (`core/`) plutôt que dans app.
 */
extern int stop_on_solution;

/**
 * @brief Nombre de possibilités qu'un client pruner demande/acquitte par lot.
 *
 * Configurable au démarrage (CLI du mode `pruner`) et à l'exécution via
 * `prunerBatch <n>`. Borne la mémoire de l'échange : le pruner ne détient
 * jamais plus que ce lot. Défaut `PRUNER_BATCH_SIZE`, plafonné à
 * `PRUNER_BATCH_MAX`.
 */
extern int pruner_batch_size;

/**
 * @brief Budget de nœuds de la preuve de fermeture bornée du pruner CPU.
 *
 * Configurable au démarrage (clé `dfs_budget`) et via `prunerDfsBudget <n>`.
 * `<= 0` désactive ce contrôle supplémentaire — `autoprune_step` retombe sur
 * le seul contrôle superficiel. Défaut `PRUNER_DFS_BUDGET_DEFAULT` = 10000
 * (activé) : mesuré en conditions réelles, 67,8 % du stock éliminé (§4.6c).
 * Plafonné à `PRUNER_DFS_BUDGET_MAX` par `pruner_dfs_budget_clamp`.
 */
extern int pruner_dfs_budget;

/** @brief Cumul des possibilités validées (et renvoyées) par ce processus pruner. */
extern volatile unsigned long long pruner_checked;

/** @brief Cumul des possibilités mortes éliminées par ce processus pruner. */
extern volatile unsigned long long pruner_removed;

/**
 * @brief Cumul des cases étudiées par les contrôles de possibilité du prunage.
 *
 * Chaque contrôle d'une possibilité balaie plusieurs cases : ce cumul compte
 * chacune de ces études, dans un flux disjoint de `counters`. Avec
 * `fc_cells_studied`, alimente le débit « dont prunage/s » des rapports
 * `check`.
 */
extern volatile unsigned long long pruner_cells_studied;

/**
 * @brief Cumul des possibilités prouvées mortes par la preuve de fermeture
 *        bornée du pruner CPU, sous-ensemble de `pruner_removed`.
 *
 * Isole la contribution propre de ce mécanisme, par opposition au contrôle
 * superficiel qui incrémente `pruner_removed` sans le toucher. Compteur
 * purement local au process : contrairement à `pruner_checked`/
 * `pruner_removed`, n'est pas propagé au parent (diagnostic/tests
 * seulement).
 */
extern volatile unsigned long long pruner_dfs_closed;

/**
 * @brief Cumul des nœuds de backtracking explorés par la preuve de fermeture
 *        bornée du pruner CPU, qu'elle ait fermé le sous-arbre ou épuisé son
 *        budget sans conclure.
 *
 * Coût réel du mécanisme, complémentaire de `pruner_dfs_closed` : le nombre
 * de fermetures seul ne dit rien du prix des tentatives infructueuses.
 */
extern volatile unsigned long long pruner_dfs_nodes;

/**
 * @brief Nombre de possibilités déplacées de la file la plus pleine vers la
 *        plus vide à chaque tour de `check_server_step` (`--rebalance-budget`).
 */

extern unsigned long long *counters;
extern unsigned long long *lastfilesize;

/**
 * @brief Profondeur (pièces posées) de la racine REÇUE du serveur par chaque
 *        fil de recherche, un élément par indice `client->compteur`.
 *
 * Figée une fois par racine (jamais réévaluée en cours d'étude) — voir
 * `root_depth`, `search_packet_backtracking_mrv` (core/etii_search.c).
 * Sentinelle `-1` : aucune racine en cours d'étude par ce fil (idle, ou rôle
 * pruner). Alloué/remis à zéro par `init_counters` (app/app_runtime.c),
 * remonté au parent via `client_statistics.root_depth`.
 */
extern int *lastroot;

/**
 * @brief Profondeur minimale parmi les possibilités encore en attente dans
 *        la pile de décisions de chaque fil de recherche (`bt_min_pending_depth`,
 *        core/etii_search.c), un élément par indice `client->compteur`.
 *
 * PAS la profondeur du chemin courant (`placed_count`) : celle-ci ne fait
 * que croître le long d'une seule branche et peut donc être largement
 * supérieure à ce qu'un fil détient encore de plus superficiel — précisément
 * le stock implicite que `bt_count_pending`/`bt_materialize_pending` cèdent
 * au serveur. Sentinelle `-1` : rien en attente (idle, rôle pruner, ou
 * juste avant `BT_CORE_EXHAUSTED`). Alloué/remis à zéro par `init_counters`
 * (app/app_runtime.c), remonté au parent via `client_statistics.min_pending_depth`.
 */
extern int *lastdepth;

extern volatile uint16_t max_result;

/**
 * @brief Durée (µs) à attendre si `r` est l'une des deux valeurs de pause, 0 sinon.
 *
 * Deux durées distinctes plutôt qu'un booléen : `REQUEST_PAUSE` (régulation
 * de débit) doit rester précis — une attente trop longue fausserait la
 * mesure de débit réévaluée à chaque tour (`PAUSE_POLL_SLEEP_US`, 10 ms) ;
 * `REQUEST_ADMIN_PAUSE` (pause manuelle/distante, durée arbitraire) n'a
 * aucune contrainte de précision (`ADMIN_PAUSE_POLL_SLEEP_US`, 500 ms).
 */
useconds_t request_is_pause(int r);

/** @brief Vrai si `r` ne signale pas un arrêt (`REQUEST_STOP`) — continue, pause de régulation ou pause admin. */
int request_keeps_running(int r);

// TODO : deplacer dans un parametre ?
extern char* parts_files;

// Indices officiels du puzzle (voir data/indices.csv et first_possibility, possibility.c).
extern char* indices_file;

extern unsigned long long non_null_possibilities;

extern volatile int request;

extern int max_stock_by_thread;

/**
 * @brief Profondeur (pièces posées) à laquelle un fil abandonne une racine
 *        reçue trop peu profonde plutôt que de l'explorer seul. `0` (défaut) :
 *        désactivé, seul `max_stock_by_thread` régit la délégation
 *        (`bt_delegate_if_needed`, `src/core/etii_search.c`).
 *
 * Sous ce seuil, `pending` — le stock implicite visible sur la pile de
 * décisions — peut rester indéfiniment sous `max_stock_by_thread` alors que le
 * sous-arbre reste énorme : un branchement MRV très fin fait grimper
 * `placed_count` sans faire grimper `pending`. Le fil reste alors des heures
 * sur sa racine sans que rien ne se déclenche.
 *
 * Quand la racine REÇUE (profondeur fixée au `GET`, jamais réévaluée) est sous
 * le seuil et que la pile l'atteint, tout le travail restant (frères non
 * explorés + chemin courant) est rendu au serveur par `bt_flush_pending` —
 * même mécanisme que `REQUEST_STOP` — et le fil reprend une racine au `GET`
 * suivant. Un seul déclenchement par racine.
 *
 * Opt-in, à calibrer par la mesure (paires alternées, cf.
 * docs/conception/elagage_recherche.md) avant d'envisager un défaut actif :
 * deux heuristiques de profondeur/ordre très proches ont déjà perdu ici.
 */
extern int shallow_root_abandon_depth;

/**
 * @brief Compteur global du nombre de racines abandonnées par
 *        `shallow_root_abandon_depth` (cumul depuis le démarrage du fil).
 *
 * Remonté par `client_statistics.shallow_root_abandoned`, affiché par le
 * rapport périodique (console `statistic`) — sert à mesurer l'effet du
 * mécanisme ci-dessus plutôt qu'à le deviner.
 */
extern volatile unsigned long long shallow_root_abandoned;

/**
 * @brief Vrai (1) tant que ce fork est en train d'échanger avec le serveur
 *        (connexion, envoi/réception, sonde de faim) depuis n'importe lequel
 *        de ses deux threads réseau. Faux sinon.
 *
 * Basé sur le périmètre exact de `client_possibility->socket_mutex` (un seul
 * mutex par fork, déjà partagé entre les deux threads) : seuls
 * `server_socket_io_lock`/`_unlock` doivent le faire varier, jamais une
 * affectation directe ailleurs.
 *
 * Rapporté au parent via `client_statistics.server_io_active` — répond à
 * « ce fils encore vivant à l'arrêt parle-t-il au serveur, ou est-il
 * bloqué/inactif ? ». Écrit par `core/datamanager.c`, lu côté app pour le
 * reporting — d'où sa place ici plutôt que dans le fichier app.
 */
extern volatile int server_io_active;

// Dernière faim du serveur connue du processus (réponse INST_NEED_WORK) :
// écrite par le thread d'alimentation (sonde, app/etii_client.c), lue par les
// threads de recherche (core/etii_search.c) dans le bloc throttlé de
// délégation, décrémentée après une délégation anticipée. Toujours via
// __atomic_* (accès inter-threads sans mutex).
extern int server_hunger;

/**
 * @brief Arme la détection de conflit de singletons dans `bt_forward_check`
 *        — expérience de mesure, jamais un réglage d'exploitation (défaut 0).
 *
 * Pendant le balayage des voisines, compte jusqu'à 2 candidats libres au lieu
 * de s'arrêter au premier. Si exactement 1, compare son `id` aux singletons
 * déjà vus dans ce balayage : un `id` répété ⇒ deux cases exigent la même
 * pièce unique ⇒ branche morte. C'est le cas `|S| = 2` du théorème de Hall,
 * invisible à un test case par case.
 *
 * ÉCARTÉ du chemin par défaut : mesuré sur du stock réel, il se déclenche
 * réellement mais exclusivement dans des sous-arbres trop grands pour fermer
 * dans les budgets testés, pour −9,5 à −11,4 % de débit agrégé.
 *
 * Coût nul quand il vaut 0. Lu par `bt_forward_check` seul.
 */
extern int singleton_conflict_check;

#endif /* core_static_variables_h */
