#ifndef core_static_variables_h
#define core_static_variables_h

#include <stdint.h>
#include <unistd.h>
#include "app/etii_statistic.h"

// Ce fichier et app/app_static_variables.h formaient à l'origine un seul
// app/static_variables.h : `core/` en dépendait pour des constantes/globales
// qui n'ont pourtant rien d'applicatif (géométrie du puzzle, compteurs de
// recherche, machine à états de pause) — une violation directe de la règle
// "core/ ne doit jamais dépendre de app/". Ce fichier-ci
// contient le sous-ensemble EFFECTIVEMENT référencé par du code sous
// `src/core/` (vérifié par grep, pas reconstitué de mémoire) : puzzle/
// géométrie, forward-checking, machine à états `request`, compteurs du
// pruner/de la recherche. Tout le reste (CLI, identité client, API HTTP,
// options serveur, bancs de mesure...) reste dans
// `src/app/app_static_variables.h`, que `src/core/` ne doit PAS inclure.
//
// Exception documentée : l'unique dépendance restante vers `src/app/` est
// `app/etii_statistic.h` ci-dessus, pour `FC_STAT_MAX_K` (dimensionnement de
// `fc_pruned_at[]`, partagé avec `struct client_statistics`, le message IPC
// parent↔enfant). Ce header n'a lui-même AUCUNE dépendance (ni sur ce
// fichier, ni sur app_static_variables.h) : c'est une constante de format de
// message, pas de la logique applicative — accepté comme le pendant, côté
// "wire format", du principe qui autorise déjà `core/stock_spill.c` à
// dépendre de `core/datamanager.h` mais jamais l'inverse.
//
// Certains modules de `core/` (`datamanager.c`, `etii_search.c`) restent
// néanmoins tributaires de `app/app_static_variables.h` pour de l'état
// GENUINEMENT applicatif qu'ils lisent directement (version de protocole,
// port serveur, identité machine, options d'expansion/rebalance/bail...) —
// ce couplage-là est réel et PAS résolu par ce découpage : le résoudre
// demanderait de faire remonter cet état via injection (comme `owner_alive`
// dans `datamanager_reclaim_expired_leases`), un chantier séparé et plus
// large que ce simple éclatement de fichier.

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
// v10 : CTRL_GET_BEST_BOARD / CTRL_BEST_BOARD — nouvelles trames du canal de
// contrôle v9, permettant au serveur de tirer la représentation complète
// (pas seulement le compte) du meilleur plateau connu d'un client quand
// celui-ci rapporte un nouveau record via CTRL_STATS (cf. core/best_board.h).
// v11 : nouveau parcours de plateau (directions[]/dirx[]/diry[] en 256,
// core/core_static_variables.c) pensé pour éliminer des possibilités plus tôt
// dans la recherche. Un possibility_packet échangé entre un client v10 et un
// serveur v11 (ou l'inverse) désignerait des cases différentes pour le même
// indice de curseur (alloc) — bump de version pour forcer tous les clients
// à se resynchroniser sur le nouveau parcours plutôt que corrompre le board.
// v12 : identité déclarée des clients.
// Nouveau INST_CLIENT_HELLO sur la connexion de TRAVAIL (net/etii_protocol.h) :
// chaque fork l'envoie une fois, juste après le handshake de version, avec son
// identité (machine_uid, client_uid, fork_seq, label, mode — net/client_identity.h).
// control_hello_t (net/control_protocol.h) est étendu des mêmes champs. Bump
// nécessaire : un serveur v11 recevrait INST_CLIENT_HELLO comme une instruction
// inconnue (branche "else" de communicate_with_client_step) et fermerait la
// connexion au lieu de simplement l'ignorer — l'exact-match du handshake evite
// ce désync silencieux en le refusant explicitement à la place.
// v13 : `possibility_packet.alloc` change de SENS sans changer de type ni de
// position sur le fil (cf. docs/autosearch_step.md). Avant v13 : curseur de
// position dans directions[]/dirx[]/diry[] (« prochaine case à traiter »).
// Depuis v13 : nombre de cases non vides de la grille
// (possibility_placed_count), le référentiel qu'exige le moteur MRV (le
// curseur de parcours n'a plus de rapport avec l'état réel du plateau une
// fois l'ordre de variable rendu dynamique). Un client v12 et un serveur v13
// (ou l'inverse) se comprendraient sur le fil tout en désynchronisant
// silencieusement l'état du plateau — bump pour un refus explicite au
// handshake plutôt qu'une corruption silencieuse.
//
// NOTE : la constante `VERSION` elle-même (et le compteur `version` qui en
// hérite au démarrage) reste dans app_static_variables.h — seul le code
// réseau (net/, app/) la compare réellement ; ce commentaire d'historique
// reste ici parce qu'il documente aussi l'évolution du SENS de `alloc`, une
// invariant core/ (cf. possibility.h).

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

// Budget de nœuds par défaut de la preuve de fermeture bornée du pruner CPU
// (`pruner_dfs_budget`) :
// nombre de nœuds de backtracking RÉEL (search_packet_backtracking_budgeted)
// qu'une possibilité jugée vivante par le contrôle superficiel
// (`possibility_all_has_a_next_counted`) mais pas encore `checked` peut encore
// consommer avant que le pruner renonce à prouver sa fermeture et la
// conserve, comme avant cette PR.
//
// DÉSACTIVÉ PAR DÉFAUT (0) — décision de DÉPLOIEMENT, pas verdict de mesure.
// Une mesure initiale (stock synthétique trop peu profond, même erreur de
// méthode que corrigée pour MRV) avait conclu à 0 % de fermeture à tout
// budget testé jusqu'à 1 000 000 de nœuds. REMESURÉ depuis sur du VRAI stock
// serveur (`--pruner-profile`, tests/bench/bench_refutation.c, rejouant le
// pipeline réel `autoprune_step`) : la preuve DFS ferme bien +4,6 à +5,6
// points de pourcentage de possibilités au-delà du contrôle superficiel
// gratuit (lui-même à 50,2 % sur ce stock), reproduit sur un second stock.
// NE PAS reprendre l'affirmation « 0 % de fermeture, mécanisme inutile » —
// elle est fausse. Le défaut reste 0 malgré tout : basculer par défaut change
// le coût CPU de tout pruner déployé, décision laissée à l'opérateur, pas
// encore prise. Valeur recommandée par la mesure si activé : 10000 (capture
// 82 % du gain mesuré à 1 000 000 pour 1 % du coût CPU, rendements
// décroissants nets au-delà). Reste configurable à l'exécution (console
// `prunerDfsBudget <n>`, fichier de configuration client `dfs_budget`) : le
// mécanisme est correct et sans coût quand désactivé (`pruner_dfs_budget <= 0`
// court-circuite avant tout backtracking).
#define PRUNER_DFS_BUDGET_DEFAULT 0
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
 * @brief Nombre d'angles du plateau (mesure §4.9).
 *
 * Défini inconditionnellement : les masques et le prédicat de zone d'angle
 * sont toujours compilés (donc toujours testés), seuls les compteurs et leur
 * incrément dans la boucle chaude sont derrière `ETII_STAT_CORNER_ZONES`.
 */
#define CZ_CORNERS 4

#ifdef ETII_STAT_CORNER_ZONES
/**
 * @brief Instrumentation §4.9 : nœuds où une zone d'angle 3×3 est entièrement
 *        entourée alors qu'elle est encore incomplète.
 *
 * `cz_surrounded_incomplete[k]` compte, pour l'angle `k`, les nœuds du moteur
 * MRV où les 6 cases de l'anneau extérieur de la zone 3×3 sont TOUTES posées
 * alors qu'au moins une des 9 cases de la zone est encore vide. C'est la
 * seule situation dans laquelle une table de région (« pattern database »)
 * pourrait être interrogée utilement — cf.
 * docs/conception/elagage_recherche.md §4.9, mesure 3 : sous l'ordre fixe
 * `directions[]` elle est impossible par construction, mais l'ordre MRV étant
 * dynamique, seule la mesure peut trancher.
 *
 * Compteurs de MESURE, jamais compilés en production : tout le mécanisme est
 * derrière `ETII_STAT_CORNER_ZONES` parce que la boucle chaude est limitée par
 * le débit d'émission (IPC ≈ 3,15, cf. docs/autosearch_step.md §1.3 quater) —
 * une instrumentation inconditionnelle perturberait le débit qu'elle mesure.
 */
extern volatile unsigned long long cz_surrounded_incomplete[CZ_CORNERS];

/**
 * @brief Dénominateur de `cz_surrounded_incomplete` : nœuds MRV inspectés.
 */
extern volatile unsigned long long cz_nodes;

/**
 * @brief Nombre de cases d'une zone d'angle 3×3 (borne du histogramme de trous).
 */
#define CZ_ZONE_CELLS 9

/**
 * @brief Répartition du nombre de cases encore vides quand la zone se
 *        retrouve entourée.
 *
 * Indicateur décisif, et pas un simple détail : à UN seul trou, la « table de
 * région » dégénère exactement en la recherche par clé 4D que le moteur fait
 * déjà à chaque nœud — elle n'apporte alors rigoureusement aucun pouvoir
 * d'élagage supplémentaire. Seuls les déclenchements à ≥ 2 trous comptent.
 */
extern volatile unsigned long long cz_holes_hist[CZ_ZONE_CELLS + 1];

/** @brief Somme des profondeurs (pièces posées) des déclenchements. */
extern volatile unsigned long long cz_depth_sum;
/** @brief Profondeur minimale observée sur un déclenchement (0 si aucun). */
extern volatile unsigned long long cz_depth_min;
/** @brief Profondeur maximale observée sur un déclenchement. */
extern volatile unsigned long long cz_depth_max;

/**
 * @brief Nœuds explorés SOUS les nœuds déclencheurs (union des sous-arbres).
 *
 * Borne supérieure de ce qu'une table de région pourrait économiser : même en
 * supposant qu'elle réfute 100 % des déclenchements, elle ne peut pas faire
 * l'économie de plus que ces nœuds-là. À comparer au coût du seul DÉTECTEUR,
 * mesuré indépendamment (cf. §4.9). Les déclenchements imbriqués ne sont
 * comptés qu'une fois : un déclenchement survenu alors qu'un autre est encore
 * ouvert est déjà inclus dans le sous-arbre de celui-ci.
 */
extern volatile unsigned long long cz_subtree_nodes;
#endif // ETII_STAT_CORNER_ZONES

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
 * le seul contrôle superficiel. Défaut `PRUNER_DFS_BUDGET_DEFAULT` = 0
 * (désactivé) : mesuré sans gain sur le stock actuel, opt-in délibéré.
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
 * @brief Profondeur (nombre de pièces posées) à partir de laquelle un fil
 *        abandonne une racine reçue trop peu profonde plutôt que de
 *        continuer à l'explorer seul.
 *
 * `0` (défaut) : mécanisme désactivé — seul `max_stock_by_thread` régit la
 * délégation (cf. `bt_delegate_if_needed`, `src/core/etii_search.c`).
 *
 * Sous ce seuil, `pending` (le stock implicite visible sur la pile de
 * décisions) peut rester indéfiniment sous `max_stock_by_thread` alors que le
 * sous-arbre total reste énorme — un branchement MRV très fin (peu de
 * candidats par case) fait grimper `placed_count` sans jamais faire grimper
 * `pending`. `max_stock_by_thread` seul ne se déclenche donc jamais dans ce
 * cas, et le fil reste des heures sur une racine reçue à faible profondeur.
 *
 * Quand la racine REÇUE (profondeur fixée au moment du `GET`, jamais
 * réévaluée en cours d'étude) est sous ce seuil et que la profondeur COURANTE
 * de la pile l'atteint, tout le travail restant (frères non explorés à tous
 * les niveaux + chemin courant) est rendu au serveur via `bt_flush_pending`
 * — même mécanisme que l'arrêt propre (`REQUEST_STOP`) — et le fil se
 * repositionne sur une nouvelle racine au prochain `GET`. Un seul
 * déclenchement possible par racine : une fois rendue, il n'y a plus rien à
 * réévaluer sur cette racine.
 *
 * Opt-in, à calibrer par la mesure (protocole de paires alternées déjà
 * employé dans ce dépôt, cf. docs/conception/elagage_recherche.md) avant
 * d'envisager un défaut actif — deux heuristiques de profondeur/ordre très
 * proches de celle-ci ont déjà perdu à la mesure ici (départage MRV par
 * nombre de côtés contraints, sens de cession `--split-shallow-first`).
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
 * Pendant le balayage des voisines, au lieu de s'arrêter au premier candidat
 * libre (comportement par défaut), compte jusqu'à 2 candidats libres — assez
 * pour distinguer « singleton » de « pas singleton ». Si exactement 1,
 * compare son `id` à celui des singletons déjà rencontrés dans ce balayage :
 * un `id` répété ⇒ deux cases exigent la même pièce unique ⇒ branche morte
 * — le cas `|S| = 2` du théorème de Hall, invisible à un test case-par-case.
 *
 * Mesuré sur du stock réel (banc de réfutation, engin `fixe+singleton`) : le
 * mécanisme se déclenche réellement, mais exclusivement dans des sous-arbres
 * trop grands pour fermer dans les budgets testés — jamais là où ça compte.
 * Coût confirmé : −9,5 à −11,4 % de débit agrégé. Décision : ne pas
 * fusionner dans le chemin par défaut.
 *
 * Coût nul quand il vaut 0. Lu par `bt_forward_check` uniquement, seul
 * moteur MRV.
 */
extern int singleton_conflict_check;

#endif /* core_static_variables_h */
