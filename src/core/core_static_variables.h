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
 * Sur la boucle chaude (`bt_forward_check`, `etii_search.c`), CETTE VALEUR NE
 * BORNE PLUS AUCUNE FENÊTRE depuis le passage aux voisines géométriques : le
 * nombre de cases inspectées après un placement est une propriété de la
 * grille (au plus 4 voisines), indépendante de `FORWARD_CHECK_K`. Seul le
 * chemin froid `forward_check_next_k` (`possibility.c` — matérialisation de
 * délégation, tests) garde l'ancienne sémantique de fenêtre : après avoir
 * placé une pièce à `directions[i]`, il vérifie que les `FORWARD_CHECK_K`
 * prochaines cases (`directions[i+1] ... directions[i+K]`) possèdent encore
 * au moins une pièce candidate (39 % des relations de
 * voisinage jamais couvertes par l'ancienne fenêtre K=6).
 *
 * `FORWARD_CHECK_K == 0` reste le seul interrupteur : il compile TOUT le
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
 * est en position j (1..) dans la fenêtre inspectée par l'appelant. Deux
 * appelants y contribuent, avec des fenêtres de nature différente :
 * - `bt_forward_check` (`etii_search.c`, boucle chaude) inspecte les VOISINES
 *   géométriques de la pièce qu'on vient de placer — au plus 4, donc j ∈ [1,4] ;
 * - `forward_check_next_k` (`possibility.c`, chemins froids : matérialisation
 *   de délégation, tests) inspecte encore les `FORWARD_CHECK_K` prochaines
 *   cases du PARCOURS — j ∈ [1, FORWARD_CHECK_K].
 * Le tableau est dimensionné sur `FC_STAT_MAX_K` (borne indépendante de
 * `FORWARD_CHECK_K`, cf. `etii_statistic.h`) pour rester sûr quel que soit le
 * plus petit des deux domaines. La somme de tous les indices vaut toujours
 * `fc_pruned` ; sa répartition n'estime plus une distance de parcours
 * uniforme.
 */
extern volatile unsigned long long fc_pruned_at[FC_STAT_MAX_K + 1];

/**
 * @brief Compteur du nombre d'élagages dus à un CONFLIT DE SINGLETONS —
 *        sous-ensemble de `fc_pruned`.
 *
 * Incrémenté par `bt_forward_check` quand deux voisines de la pièce posée
 * exigent chacune, comme seul candidat encore libre, la MÊME pièce — cas
 * `|S| = 2` du théorème de Hall, que le forward-check case-par-case ne peut
 * structurellement pas voir (chaque voisine prise isolément a bien ≥ 1
 * candidat). Compteur DÉDIÉ plutôt que replié dans `fc_pruned_at[]` :
 * répond à « ce mécanisme se déclenche-t-il, indépendamment du débit
 * agrégé ? » — la question tranchée en §4.4. Actif seulement quand
 * `singleton_conflict_check` est levé (cf. sa doc).
 */
extern volatile unsigned long long fc_singleton_conflict;
#endif // FORWARD_CHECK_K > 0

extern uint8_t directions[ETERN_PARTS];

extern uint8_t dirx[ETERN_PARTS];

extern uint8_t diry[ETERN_PARTS];

/**
 * @brief Cumul des cases inspectées par le forward-checking.
 *
 * Chaque appel à `bt_forward_check` (boucle chaude : au plus 4 voisines
 * géométriques) ou `forward_check_next_k` (chemin froid : jusqu'à
 * `FORWARD_CHECK_K` cases du parcours) ajoute au cumul chaque case
 * RÉELLEMENT inspectée (cases déjà remplies sautées non comptées). Même
 * unité qu'un coup de la recherche, flux disjoint de `counters`. Reste à 0
 * quand `FORWARD_CHECK_K == 0`. Incrémenté par ajout atomique (boucle chaude
 * multi-thread), comme `fc_attempts`.
 */
extern volatile unsigned long long fc_cells_studied;

/**
 * @brief 1 si l'on s'arrête à la première solution (option `--stop-on-solution`).
 *
 * Défaut 0 : on continue après une solution — le processus de recherche backtrack
 * pour en chercher d'autres, et le serveur reste en service pour que les clients
 * continuent d'explorer. À 1 : le processus de recherche qui trouve une solution
 * sort, et le serveur qui en reçoit une sauvegarde son stock puis s'arrête.
 *
 * Lue dans `main()` AVANT tout fork → héritée par les processus enfants
 * (fixée par `parse_cli_options`, app/app_static_variables.c), et consultée
 * directement par la boucle chaude de recherche (`core/etii_search.c`) et par
 * `core/datamanager.c` (sauvegarde/arrêt serveur à la réception d'une
 * solution) — d'où sa place ici plutôt que dans le fichier app.
 */
extern int stop_on_solution;

/**
 * @brief Nombre de possibilités qu'un client pruner demande/acquitte par lot.
 *
 * Configurable au démarrage (argument CLI du mode `pruner`, app/client_config.c)
 * et à l'exécution via la commande `prunerBatch <n>` (propagée aux process
 * enfants). Borne la mémoire de l'échange : le pruner ne détient jamais plus
 * que ce lot, la capacité mémoire n'a donc pas à être supposée illimitée.
 * Défaut `PRUNER_BATCH_SIZE`, plafonné à `PRUNER_BATCH_MAX`. Lu directement
 * par `core/etii_search.c`/`core/datamanager.c` (taille de lot GET_TO_CHECK).
 */
extern int pruner_batch_size;

/**
 * @brief Budget de nœuds de la preuve de fermeture bornée du pruner CPU (§4.6b).
 *
 * Configurable au démarrage (fichier de configuration client, clé
 * `dfs_budget`) et à l'exécution via la commande `prunerDfsBudget <n>`
 * (propagée aux process enfants). `<= 0` désactive entièrement ce contrôle
 * supplémentaire — `autoprune_step` retombe alors sur le seul contrôle
 * superficiel (`possibility_all_has_a_next_counted`), comportement d'avant
 * cette PR. Défaut `PRUNER_DFS_BUDGET_DEFAULT` = 0 (DÉSACTIVÉ, voir sa doc) :
 * mesuré sans gain sur le stock réel actuel, mur structurel `max_result` ≈ 74
 * oblige — un opt-in délibéré, pas un défaut prudent en attendant mieux.
 * Plafonné à `PRUNER_DFS_BUDGET_MAX` par `pruner_dfs_budget_clamp`
 * (`src/ui/command_lines.{h,c}`).
 */
extern int pruner_dfs_budget;

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
 * @brief Cumul des possibilités prouvées mortes par la preuve de fermeture
 *        bornée du pruner CPU (§4.6b), sous-ensemble de `pruner_removed`.
 *
 * Isole la contribution PROPRE de ce mécanisme (par opposition au contrôle
 * superficiel `possibility_all_has_a_next_counted`, qui incrémente
 * `pruner_removed` sans jamais toucher ce compteur) — même discipline de
 * mesure que `fc_singleton_conflict` pour §4.4 : répondre à « rentable ou
 * non » exige de ne pas se fier au seul débit agrégé. Compteur PUREMENT
 * LOCAL au process : contrairement à `pruner_checked`/`pruner_removed`, il
 * n'est PAS propagé au parent par `client_statistics`/IPC ni exposé sur le
 * canal de contrôle ou l'API HTTP — un choix délibéré pour ne pas faire
 * grossir le format de ces échanges pour un compteur de diagnostic
 * (mesure/tests), qui reste lisible en local (débogueur, tests unitaires).
 */
extern volatile unsigned long long pruner_dfs_closed;

/**
 * @brief Cumul des nœuds de backtracking explorés par la preuve de fermeture
 *        bornée du pruner CPU (§4.6b), qu'elle ait fermé le sous-arbre ou
 *        épuisé son budget sans conclure.
 *
 * Coût réel du mécanisme, complémentaire de `pruner_dfs_closed` : le nombre
 * de fermetures prouvées seul ne dit rien du prix payé pour les tentatives
 * infructueuses (budget épuisé). Même flux purement local que
 * `pruner_dfs_closed`, pour la même raison (voir sa doc).
 */
extern volatile unsigned long long pruner_dfs_nodes;

/**
 * @brief Nombre de possibilités déplacées de la file la plus pleine vers la
 *        plus vide à chaque tour de `check_server_step` (option CLI
 *        `--rebalance-budget <n>`) — NON, voir app_static_variables.h :
 *        `rebalance_budget` lui-même reste app (seul `check_server_step`, un
 *        fichier app/, l'utilise).
 */

extern unsigned long long *counters;
extern unsigned long long *lastfilesize;

extern volatile uint16_t max_result;

/**
 * @brief Durée (µs) à attendre si `r` est l'une des deux valeurs de pause, 0 sinon.
 *
 * Regroupe `REQUEST_PAUSE` et `REQUEST_ADMIN_PAUSE` : les boucles chaudes qui
 * doivent juste attendre (usleep + continue) sans traiter cela comme un arrêt
 * n'ont pas à connaître la distinction entre les deux origines de pause — mais
 * l'objectif de chacune diffère, d'où deux durées distinctes plutôt qu'un
 * simple booléen :
 * - `REQUEST_PAUSE` (régulation de débit, `control_step`) doit rester précis :
 *   une attente trop longue fausserait la mesure de débit que ce même
 *   mécanisme réévalue à chaque tour. `PAUSE_POLL_SLEEP_US` (10 ms).
 * - `REQUEST_ADMIN_PAUSE` (pause manuelle/distante, durée arbitraire, parfois
 *   longue) n'a aucune contrainte de précision : on peut attendre bien plus
 *   longtemps pour économiser du CPU. `ADMIN_PAUSE_POLL_SLEEP_US` (500 ms).
 *
 * @param r Valeur de `request` à tester.
 * @return  `PAUSE_POLL_SLEEP_US` si `r == REQUEST_PAUSE`,
 *          `ADMIN_PAUSE_POLL_SLEEP_US` si `r == REQUEST_ADMIN_PAUSE`, 0 sinon.
 */
useconds_t request_is_pause(int r);

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

// Indices officiels du puzzle (voir data/indices.csv et first_possibility, possibility.c).
extern char* indices_file;

extern unsigned long long non_null_possibilities;

extern volatile int request;

extern int max_stock_by_thread;

/**
 * @brief Vrai (1) tant que CE fork (process courant) est en train d'échanger
 *        avec le serveur — connexion, envoi ou réception d'un paquet, sonde
 *        de faim (`INST_NEED_WORK`) — depuis N'IMPORTE LEQUEL de ses deux
 *        threads réseau (le thread d'alimentation `feed_one_thread`, ou le
 *        thread de recherche via `add_possibility`/délégation). Faux (0)
 *        sinon. Basé sur le périmètre exact de `client_possibility->socket_mutex`
 *        (un seul `client_possibility_t` par fork, donc un seul mutex,
 *        déjà partagé entre ces deux threads — aucune notion de « par
 *        thread » n'est nécessaire) : `server_socket_io_lock`/
 *        `server_socket_io_unlock` (`core/datamanager.h`) sont les seuls
 *        points qui doivent le faire varier, jamais une affectation directe
 *        ailleurs.
 *
 * Rapporté au parent via `client_statistics.server_io_active` (IPC_MSG_STATS,
 * même cadence que le reste des stats) — répond directement à « ce fils
 * encore vivant à l'arrêt est-il en train de PARLER au serveur, ou juste
 * bloqué/inactif ? » (cf. `fork_diagnostic_summary`). Écrit par
 * `core/datamanager.c` (propriétaire réel de l'état), lu côté app pour le
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
 * @brief Arme la détection de CONFLIT DE SINGLETONS dans `bt_forward_check` —
 *        expérience de mesure, jamais un réglage d'exploitation (défaut 0).
 *
 * Réimplémentation ultérieure :
 * pendant le balayage des voisines, au lieu de s'arrêter au premier candidat
 * libre trouvé (comportement par défaut), compte jusqu'à 2 candidats libres
 * — assez pour distinguer « singleton » de « pas singleton », inutile
 * d'aller plus loin. Si exactement 1, compare son `id` à celui des
 * singletons déjà rencontrés dans CE balayage (au plus 4 voisines par
 * appel) : un `id` répété ⇒ deux cases exigent la même pièce unique ⇒
 * branche morte — le cas `|S| = 2` du théorème de Hall, structurellement
 * invisible à un test case-par-case.
 *
 * Mesuré une première fois (§4.4, avant PR 10) sur le protocole du banc de
 * débit (`bench_search.sh`, mono-processus depuis la genèse) : **0
 * déclenchement** sur 500 M nœuds, code reverté. Cette conclusion souffrait
 * de la même erreur de méthode que le « mur à `max_result` ≈ 74 » corrigé en
 * §4.7 : protocole non représentatif du stock réel d'un serveur.
 *
 * REMESURÉ sur du VRAI stock via l'engin `fixe+singleton` du banc de
 * réfutation (`tests/bench/bench_refutation.c --engines`, jamais
 * `--pruner-profile` — ce mode-là ne touche pas `bt_forward_check`). Verdict :
 * le mécanisme se déclenche RÉELLEMENT (35 056 à 134 565 fois sur des
 * échantillons de 120 racines, compteur `fc_singleton_conflict`), mais
 * EXCLUSIVEMENT dans des sous-arbres trop grands pour fermer dans les
 * budgets testés — jamais dans un sous-arbre effectivement fermé. Coût
 * confirmé sur trois mesures indépendantes : −9,5 à −11,4 % de débit agrégé,
 * cohérent avec la mesure originale. Décision inchangée (ne pas fusionner),
 * diagnostic corrigé (« ne se déclenche jamais » → « se déclenche mais
 * jamais là où ça compte ») — voir §4.4 pour la trace complète.
 *
 * Coût nul quand il vaut 0 (le chemin historique, un seul candidat cherché,
 * est inchangé). Lu par `bt_forward_check` uniquement — MRV étant le seul
 * moteur (cf. docs/autosearch_step.md), ce drapeau ne s'applique plus qu'à
 * lui (autrefois partagé avec le moteur à ordre fixe, qui utilisait aussi
 * `bt_forward_check`).
 */
extern int singleton_conflict_check;

#endif /* core_static_variables_h */
