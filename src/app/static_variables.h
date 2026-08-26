#ifndef static_variables_h
#define static_variables_h

#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/un.h>
#include <pthread.h>
#include "app/etii_statistic.h"
#include "net/client_identity.h"

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
// app/static_variables.c) pensé pour éliminer des possibilités plus tôt dans
// la recherche. Un possibility_packet échangé entre un client v10 et un
// serveur v11 (ou l'inverse) désignerait des cases différentes pour le même
// indice de curseur (alloc) — bump de version pour forcer tous les clients
// à se resynchroniser sur le nouveau parcours plutôt que corrompre le board.
// v12 : identité déclarée des clients (PR2).
// Nouveau INST_CLIENT_HELLO sur la connexion de TRAVAIL (net/etii_protocol.h) :
// chaque fork l'envoie une fois, juste après le handshake de version, avec son
// identité (machine_uid, client_uid, fork_seq, label, mode — net/client_identity.h).
// control_hello_t (net/control_protocol.h) est étendu des mêmes champs. Bump
// nécessaire : un serveur v11 recevrait INST_CLIENT_HELLO comme une instruction
// inconnue (branche "else" de communicate_with_client_step) et fermerait la
// connexion au lieu de simplement l'ignorer — l'exact-match du handshake evite
// ce désync silencieux en le refusant explicitement à la place.
#define VERSION 12

#define NB_CONNECTIONS_PER_THREAD 1
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
// ≈ 500 ms, très en-deçà de tcp_timeout par défaut (10 s, DEFAULT_TCP_TIMEOUT
// ci-dessous) : le client reçoit un stock K=0 ou un INST_ERROR gracieux,
// déjà géré des deux côtés, plutôt qu'un timeout de connexion.
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

// Budget de nœuds par défaut de la preuve de fermeture bornée du pruner CPU
// (§4.6b de docs/conception/elagage_recherche.md, `pruner_dfs_budget`) :
// nombre de nœuds de backtracking RÉEL (search_packet_backtracking_budgeted)
// qu'une possibilité jugée vivante par le contrôle superficiel
// (`possibility_all_has_a_next_counted`) mais pas encore `checked` peut encore
// consommer avant que le pruner renonce à prouver sa fermeture et la
// conserve, comme avant cette PR.
//
// DÉSACTIVÉ PAR DÉFAUT (0) — décision de DÉPLOIEMENT, pas verdict de mesure
// (même raisonnement que MRV_DEFAULT_ENABLED, cf. sa doc dans ce fichier).
// Une mesure initiale (stock synthétique trop peu profond, même erreur de
// méthode que corrigée pour MRV) avait conclu à 0 % de fermeture à tout
// budget testé jusqu'à 1 000 000 de nœuds. REMESURÉ depuis sur du VRAI stock
// serveur (`--pruner-profile`, tests/bench/bench_refutation.c, rejouant le
// pipeline réel `autoprune_step`) : la preuve DFS ferme bien +4,6 à +5,6
// points de pourcentage de possibilités au-delà du contrôle superficiel
// gratuit (lui-même à 50,2 % sur ce stock), reproduit sur un second stock.
// Voir §4.6b de docs/conception/elagage_recherche.md pour la table complète.
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
// Moteur par défaut de la preuve de fermeture bornée du pruner CPU
// (§4.6b/§4.10 de docs/conception/elagage_recherche.md, `pruner_dfs_mrv`) :
// 0 = ordre FIXE (`search_packet_backtracking_core`, comportement historique),
// 1 = ordre DYNAMIQUE (`search_packet_backtracking_mrv`).
//
// Défaut 0 pour la même raison que MRV_DEFAULT_ENABLED et
// PRUNER_DFS_BUDGET_DEFAULT : basculer un défaut change le coût CPU de toute
// une flotte déployée, décision d'opérateur. Le levier est pourtant mesuré
// nettement favorable POUR CE TRAVAIL PRÉCIS : le banc de réfutation
// (tests/bench/bench_refutation.c) donne, à temps CPU égal sur du vrai stock
// serveur, 3,48 fermetures/s pour MRV contre 0,91 pour l'ordre fixe, et 40
// nœuds contre 295 339 sur les racines fermées par les deux — or fermer un
// sous-arbre est EXACTEMENT le métier du pruner, pas un effet de bord.
// Le risque connu de MRV (60× plus cher que l'ordre fixe sur des racines
// encore vivantes, cf. §4.7) est ici BORNÉ PAR CONSTRUCTION par
// `pruner_dfs_budget` : au pire la preuve échoue après B nœuds, exactement
// comme aujourd'hui. C'est le seul contexte du document où le risque de MRV
// est plafonné.
//
// Mesuré à l'A/B (`--pruner-profile --pruner-dfs-mrv`) sur un stock de
// PRODUCTION de 126 287 possibilités produites par de vrais clients,
// échantillon de 2 000 : ×3 à ×4 de fermetures à budget égal — 8,3 % → 34,8 %
// à budget 1 000, 10,0 % → 35,6 % à 10 000, 11,7 % → 36,0 % à 100 000 ; le
// stock éliminé passe de 32 % à 58 %. Aucun budget ne comble l'écart : LES
// DEUX moteurs plafonnent (l'ordre fixe ne gagne que 3,4 points en payant ×100
// de budget), à des niveaux différents — ce que MRV achète est un niveau
// d'élimination inatteignable autrement, pas de la vitesse. `MRV@1000` domine
// d'ailleurs strictement `fixe@100000` (56,8 % contre 33,8 % de stock éliminé,
// pour 10,7× moins de CPU), d'où le budget d'exploitation recommandé : 1 000.
// NE PAS lire le débit de fermetures isolément : à budget égal MRV coûte ~2×
// PLUS par fermeture sur ce stock (1,56 ms contre 0,77 ms) — il ferme aussi
// les sous-arbres que l'ordre fixe ne ferme jamais, qui sont les plus chers.
// Détail complet : §4.10 du document de conception.
#define PRUNER_DFS_MRV_DEFAULT 0
// Expansion du stock au démarrage du serveur (option `--expand-level`, commande
// console `expand`). Le serveur développe lui-même les possibilités du stock
// (une pièce candidate par case suivante) jusqu'à ce que leur curseur `alloc`
// atteigne le niveau demandé, ce qui transforme le paquet genèse en des
// milliers de possibilités distribuables — supprimant la famine du démarrage où
// un seul client détient tout l'arbre pendant que le serveur n'a rien à servir.
// L'impact client est nul (calcul purement serveur, avant toute connexion).
//
// EXPAND_MAX_LEVELS : valeur par DÉFAUT du plafond en NOMBRE DE PASSES
// d'expansion (variable globale `expand_max_levels`, configurable via
// l'option CLI `--expand-max-levels <n>` — voir static_variables.h), quelle
// que soit la consigne de niveau — garde-fou en PROFONDEUR pour ne pas mettre
// le serveur au travail trop longtemps.
#define EXPAND_MAX_LEVELS 4
// EXPAND_MAX_STOCK : valeur par DÉFAUT du plafond de sécurité en NOMBRE de
// possibilités (variable globale `expand_max_stock`, configurable à chaud via
// l'option CLI `--expand-max-stock <n>` — voir static_variables.h). Le facteur
// de branchement du puzzle étant inconnu et variable, la seule borne en
// profondeur ne borne pas le travail réel ; on arrête donc l'expansion entre
// deux passes dès que le stock dépasse ce seuil. ~100000 × ~0,5 Ko ≈ 54 Mo —
// un serveur à plus grosse capacité peut relever ce plafond.
#define EXPAND_MAX_STOCK 100000

// Rééquilibrage incrémental du stock entre files : valeur par DÉFAUT du nombre
// de possibilités déplacées de la file la plus pleine vers la plus vide à
// chaque tour de `check_server_step` (variable globale `rebalance_budget`,
// configurable via l'option CLI `--rebalance-budget <n>`). Ce qui rend le
// « temps de blocage ≤ 1 s par file » de la sauvegarde cohérente (PR2) vrai :
// des files de tailles comparables. Un budget modeste par tour (comme
// `expand_max_stock`, un plafond nul n'a pas de sens utile) répartit
// progressivement la charge sur plusieurs tours plutôt que de bloquer un
// tour entier sur un rééquilibrage complet.
#define REBALANCE_BUDGET_DEFAULT 1000

// Bail à expiration des analyses en cours (PR7) : durée par défaut, en
// secondes, au-delà de laquelle une possibilité attribuée à un client
// (owner_uid connu, cf. add_possibility_analysed_owned) et jamais acquittée
// devient ÉLIGIBLE à être rendue au stock non vérifié.
//
// Ce n'est qu'un MINORANT, pas un budget de temps garanti : rien ne prouve
// qu'une analyse tienne dans ce délai, donc `datamanager_reclaim_expired_leases`
// exige EN PLUS une preuve d'absence (callback `owner_alive`, fourni côté
// serveur par `owner_control_session_alive`/`control_registry_has_active_client`
// — tant que le canal de contrôle du client reste enregistré, son travail
// n'expire jamais, quelle que soit la durée depuis l'attribution). Un premier
// essai réel avec l'échéance seule a montré qu'un client occupé mais vivant se
// faisait réclamer son travail dès ce budget dépassé, avec pour conséquence
// une double exploration de la même branche quand ce client finissait par
// soumettre ses résultats pour une possibilité déjà réattribuée ailleurs.
// Configurable à l'exécution via la commande console `leaseDuration <n>` ;
// <n> ≤ 0 désactive le bail entièrement (même convention que `limit 0` pour la
// régulation de débit) — utile pour un déploiement qui préfère geler un
// stock indéfiniment plutôt que risquer un double travail.
#define ANALYSED_LEASE_DEFAULT_SECONDS 300

// Nombre maximal de sessions de contrôle (canal INST_CONTROL_HELLO, cf.
// control_registry.h) suivies simultanément par le serveur. Une session de
// contrôle réutilise un slot déjà présent du pool `client_t` (même connexion
// TCP acceptée, juste un comportement différent après le hello) — ce registre
// ne dimensionne donc PAS de nouvelles sockets, seulement l'état "session de
// contrôle" associé. 64 est large au regard du nombre de PROCESS parents
// client réellement déployés (un par machine/process, pas un par thread de
// recherche) : NB_THREADS peut être bien plus grand, la borne du registre est
// donc volontairement indépendante et fixe plutôt qu'indexée sur NB_THREADS.
#define MAX_CONTROL_SESSIONS 64

// Nombre maximal de MACHINES distinctes (clé `machine_uid`) suivies par le
// registre de clients connus (`known_clients_registry.h`, PR4). Distinct de MAX_CONTROL_SESSIONS :
// ce registre survit à la déconnexion (contrairement à `control_registry`),
// donc un parc qui tourne longtemps peut accumuler des machines vues puis
// définitivement parties.
//
// Exprimé en MULTIPLE de MAX_CONTROL_SESSIONS plutôt qu'en constante magique
// indépendante, pour deux raisons :
//  - le nombre de machines SIMULTANÉMENT connues (`nb_active_sessions` sommé
//    sur toutes les entrées) ne peut de toute façon jamais dépasser
//    MAX_CONTROL_SESSIONS (chaque session de contrôle occupe un slot de CE
//    registre-là, indépendant de NB_THREADS — voir son commentaire
//    ci-dessus) : la seule pression possible sur CETTE borne-ci vient du
//    CUMUL dans le temps (machines vues puis reparties), jamais du pic
//    instantané ;
//  - garder `MAX_KNOWN_CLIENTS` strictement AU-DESSUS de ce pic, d'un facteur
//    explicite, documente d'un coup d'œil « combien d'historique de machines
//    déconnectées ce registre peut encore garder au-delà du pic instantané »
//    — et la relation entre les deux bornes reste vraie si
//    MAX_CONTROL_SESSIONS change un jour, sans qu'il faille y repenser ici.
//
// Facteur 4 choisi arbitrairement comme marge confortable pour un parc réel ;
// la politique d'éviction (LRU parmi les entrées DÉCONNECTÉES, cf. le fichier
// .c) absorbe de toute façon le cas d'un parc qui dépasserait quand même la
// borne — jamais en évinçant une machine actuellement connectée. Coût mémoire
// négligeable (~200 octets/entrée, soit ~50 Ko à 256) : ce n'est pas une
// borne de sûreté contre un débordement, seulement un choix de rétention.
#define MAX_KNOWN_CLIENTS (4 * MAX_CONTROL_SESSIONS)
// Nombre maximal de sessions SIMULTANÉES suivies par machine connue (ex. un
// client de recherche et un pruner lancés en parallèle sur le même hôte,
// chacun avec son propre `client_uid`). Volontairement petit : dépasser ce
// nombre sur une même machine est un cas marginal, et le seul effet d'un
// dépassement est de dégrader (pas de perte) — la session surnuméraire n'est
// simplement pas comptée dans le cumul individuel tant qu'un slot ne se
// libère pas (cf. known_clients_registry.c).
#define KNOWN_CLIENT_MAX_SESSIONS 8

// Longueur maximale (avec terminateur) de l'adresse IP du pair d'une connexion
// TCP acceptée par le serveur (client_t.peer_ip, control_session_info_t.peer_ip),
// formatée par inet_ntop. Vaut INET6_ADDRSTRLEN (46) : le serveur n'écoute
// aujourd'hui qu'en IPv4 (create_tcp_server, AF_INET), mais dimensionner sur
// l'IPv6 évite une deuxième constante le jour où ça change.
#define PEER_IP_MAX_LEN 46

// Intervalle (secondes) entre deux sondages automatiques CTRL_GET_STATS d'une
// session de contrôle, quand aucune commande console (`clientsStats`, ou son
// équivalent HTTP `POST /api/v1/clients/stats`) n'a été postée manuellement.
// Sans ce sondage périodique, `control_session_step` (etii_server.c) ne fait
// que des CTRL_PING/CTRL_ACK entre deux commandes explicites : le tirage du
// meilleur plateau (CTRL_GET_BEST_BOARD, cf. docs/echanges_client_serveur.md "Canal de contrôle")
// est déclenché par la RÉCEPTION d'un CTRL_STATS dont `max_result` dépasse le
// record serveur connu — sans CTRL_STATS, un nouveau record côté client peut
// donc rester invisible côté serveur indéfiniment, jusqu'à ce qu'un opérateur
// pense à lancer `clientsStats` manuellement. 30 s garde le round-trip discret
// (un frame de plus dans le pire cas par cycle de keepalive) tout en bornant
// le délai de propagation d'un record à quelques dizaines de secondes.
#define CONTROL_AUTO_STATS_INTERVAL_SEC 30

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
 * au moins une pièce candidate. Voir `docs/conception/elagage_recherche.md`
 * §4.1 pour la mesure qui motive ce changement (39 % des relations de
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
 * uniforme — voir `docs/conception/elagage_recherche.md` (§4.1).
 */
extern volatile unsigned long long fc_pruned_at[FC_STAT_MAX_K + 1];

/**
 * @brief Compteur du nombre d'élagages dus à un CONFLIT DE SINGLETONS —
 *        sous-ensemble de `fc_pruned` (§4.4 de docs/conception/elagage_recherche.md).
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
 * @brief  Nombre de threads clients
 */
extern int NB_THREADS;

/**
 * @brief Nombre de process de recherche réellement créés lors du dernier
 *        `orchestrator_spawn_forks` (src/app/fork_orchestrator.c).
 *
 * Distinct de `NB_THREADS` (le nombre VISÉ, figé au démarrage) : celui-ci est
 * le bilan RÉEL après une éventuelle pénurie de ressources
 * (`count_created_forks`), remis à jour à chaque (re)fork. Lu par
 * `run_control_channel` (src/app/etii_control.c) à chaque reconnexion pour
 * annoncer un `hello.nb_forks` à jour au serveur — plus une valeur figée à la
 * création du thread. `0` tant qu'aucun fork n'a encore eu lieu.
 */
extern int g_active_forks;

/**
 * @brief 1 si le processus est un client pruner (mode `pruner`).
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
 * @brief 1 si l'exécution GPU du pruner a été demandée (option `--gpu`).
 *
 * Position-indépendante, retirée d'argv par `parse_cli_options`. Lue dans
 * `main()` par le mode `pruner` uniquement (les autres modes l'ignorent, comme
 * `--expand-level` hors serveur) : sur un build CUDA elle active
 * `gpu_pruner_mode` ; sur un build sans CUDA elle produit une erreur explicite
 * (plutôt qu'un mode silencieusement absent).
 */
extern int gpu_requested;

/**
 * @brief 1 si l'aide CLI a été demandée (option `--help` / `-h`).
 *
 * Position-indépendante comme `--stop-on-solution` : retirée d'argv par
 * `parse_cli_options`. Lue dans `main()` avant le dispatch des modes : l'aide
 * générale est affichée puis le programme sort avec EXIT_SUCCESS.
 */
extern int help_requested;

/**
 * @brief Niveau de curseur (`alloc`) minimal visé par l'expansion du stock au
 *        démarrage du serveur (option CLI `--expand-level <n>`).
 *
 * 0 (défaut) : pas d'expansion. Sinon, `runserver` développe le stock genèse
 * jusqu'à ce que chaque possibilité atteigne ce niveau, borné par
 * `EXPAND_MAX_LEVELS` passes et `expand_max_stock` possibilités. Lu côté
 * serveur uniquement (les autres modes l'ignorent). Position-indépendant,
 * retiré d'argv par `parse_cli_options` avant le parsing positionnel.
 */
extern int expand_min_level;

/**
 * @brief Plafond en NOMBRE de possibilités de l'expansion du stock au
 *        démarrage du serveur (option CLI `--expand-max-stock <n>`).
 *
 * Valeur par défaut `EXPAND_MAX_STOCK` (100000, ~54 Mo) — la mémoire qu'un
 * serveur "moyen" peut consacrer à cette pré-expansion. Un serveur disposant
 * de plus de capacité (RAM, cœurs) peut relever ce plafond pour produire une
 * réserve distribuable plus grande à `--expand-level` égal. `<n> <= 0` est
 * ignoré (garde la valeur par défaut ou celle déjà fixée) — contrairement à
 * `expand_min_level`, un plafond nul n'a pas de sens utile (l'expansion
 * s'arrêterait avant même la première pièce placée). Lu côté serveur
 * uniquement, position-indépendant, retiré d'argv par `parse_cli_options`
 * avant le parsing positionnel.
 */
extern int expand_max_stock;

/**
 * @brief Plafond en NOMBRE DE PASSES de l'expansion du stock au démarrage du
 *        serveur (option CLI `--expand-max-levels <n>`).
 *
 * Valeur par défaut `EXPAND_MAX_LEVELS` (4) — garde-fou en *profondeur*,
 * indépendant du garde-fou en *volume* `expand_max_stock` ci-dessus : même
 * avec un plafond de volume élevé, une consigne `expand_min_level` absurdement
 * grande ne peut pas faire tourner le serveur indéfiniment. Un serveur à plus
 * grosse capacité qui relève aussi `expand_max_stock` peut vouloir relever ce
 * plafond en parallèle pour atteindre un `expand_min_level` élevé sans être
 * arrêté prématurément par le nombre de passes. Même convention que
 * `expand_max_stock` : `<n> <= 0` est ignoré (garde la valeur par défaut ou
 * celle déjà fixée), un plafond nul empêcherait toute expansion. Lu côté
 * serveur uniquement, position-indépendant, retiré d'argv par
 * `parse_cli_options` avant le parsing positionnel.
 */
extern int expand_max_levels;

/**
 * @brief Nombre de possibilités déplacées de la file la plus pleine vers la
 *        plus vide à chaque tour de `check_server_step` (option CLI
 *        `--rebalance-budget <n>`, PR3).
 *
 * Valeur par défaut `REBALANCE_BUDGET_DEFAULT` (1000). Consommé par
 * `datamanager_rebalance_step` (`core/datamanager.h`), appelé une fois par
 * tour (10 s) — jamais un chemin chaud. `<n> <= 0` est ignoré (garde la
 * valeur par défaut ou celle déjà fixée), même convention que
 * `expand_max_stock`. Lu côté serveur uniquement, position-indépendant,
 * retiré d'argv par `parse_cli_options` avant le parsing positionnel.
 */
extern int rebalance_budget;

/**
 * @brief Nombre de files de stock demandé au démarrage (option CLI
 *        `--stock-files <n>`, PR4).
 *
 * 0 = non demandé (défaut `NB_FILE_POSSIBILITY_DEFAULT`, `core/datamanager.h`
 * — inchangé). Stocké ici plutôt qu'appliqué directement dans
 * `parse_cli_options` : ce fichier reste volontairement sans dépendance sur
 * `core/datamanager.h` (même raison que `HTTP_TOKEN_FILE`, dont le chargement
 * réel est aussi différé à `main()`) ; `main()` applique la valeur via
 * `datamanager_configure_stock_files`, avant tout fork/thread. `<n> <= 0`
 * fourni explicitement est ignoré (garde 0 = défaut), même convention que
 * `expand_max_stock`. Position-indépendant, retiré d'argv par
 * `parse_cli_options` avant le parsing positionnel.
 */
extern int stock_files_requested;

/**
 * @brief Plafond en Mo de la RAM consacrée aux DEUX pools de stock serveur
 *        (non vérifié + vérifié — option CLI `--stock-max-ram <mo>`).
 *
 * 0 (défaut) = illimité, comportement inchangé — même convention que `limit 0`
 * et `leaseDuration 0`. Une valeur strictement positive fixe la limite en Mo
 * telle que fournie par l'opérateur ; la conversion en NOMBRE de possibilités
 * (l'unité réellement comparée par `put_to_pool`) est faite une seule fois,
 * après le parsing, par `datamanager_configure_ram_limit`
 * (`core/datamanager.h`) — ce fichier reste volontairement sans dépendance
 * sur `core/datamanager.h`, même raison que `stock_files_requested`
 * ci-dessus. Le pool ANALYSÉ n'est délibérément PAS couvert par ce plafond :
 * il est déjà borné par le nombre de clients en vol et par les baux
 * d'expiration (`analysed_lease_seconds`), et son index de hachage impose une
 * recherche par correspondance exacte qu'un déport casserait. `<mo> <= 0`
 * fourni explicitement est ignoré (garde 0 = illimité), même convention que
 * `expand_max_stock`. Position-indépendant, retiré d'argv par
 * `parse_cli_options` avant le parsing positionnel.
 *
 * Contrairement à `expand_min_level` (lu UNIQUEMENT par `runserver`),
 * `datamanager_configure_ram_limit` est appelé sans condition de rôle dans
 * `main()`, avant tout fork — même endroit et même raison que
 * `stock_files_requested`/`datamanager_configure_stock_files` juste
 * au-dessus : `put_to_pool` (`core/datamanager.c`), qui applique le plafond,
 * est du code PARTAGÉ, utilisé aussi bien par le stock local d'un client/
 * pruner (`put_to_local`) que par le serveur. Même précédent que la commande
 * console `rebalance` (elle aussi mécaniquement active sur le stock local
 * d'un client, sans garde de rôle). En pratique, seul le stock SERVEUR
 * atteint un volume significatif : le stock local d'un client/pruner reste
 * déjà borné par `max_stock_by_thread`/`pruner_batch_size`, largement sous
 * tout plafond RAM raisonnable — d'où la description « serveur » de cette
 * option dans l'aide CLI (`--help`), qui reflète l'usage réel, pas une
 * restriction de code.
 */
extern int stock_max_ram_mb;

/**
 * @brief Répertoire de débordement sur disque du stock serveur (option CLI
 *        `--stock-spill-dir <chemin>`, PR2 — débordement, distinct de PR1
 *        ci-dessus qui ne fait que refuser au-delà du plafond).
 *
 * Défaut `STOCK_SPILL_DIR_DEFAULT` (`"./eternityii-spill"`, `core/
 * stock_spill.h`), même convention de chemin littéral que
 * `machine_uid_file_path` : jamais alloué, jamais libéré, un pointeur
 * `argv` le remplace directement si l'option est fournie (jamais copié).
 * Aucune E/S ici — la création/purge effective du répertoire est différée à
 * `stock_spill_configure` (`core/stock_spill.h`), appelée UNIQUEMENT depuis
 * `runserver` (`app/etii_server.c`) : contrairement à `stock_max_ram_mb`,
 * ce chemin n'a de sens que côté serveur (le stock local d'un client/pruner
 * n'a pas de thread de débordement) — jamais lu ni appliqué sur les autres
 * rôles. Position-indépendant, retiré d'argv par `parse_cli_options` avant
 * le parsing positionnel.
 */
extern const char *stock_spill_dir;

/**
 * @brief 1 si la console interactive (lecture de stdin) ne doit pas démarrer
 *        (option CLI `--headless`).
 *
 * Défaut 0 : `run_console()` est démarré normalement (serveur, client, mode
 * test). Pensé pour une exécution en service (systemd, `StandardInput=null`) :
 * sans ce flag, le thread console se termine déjà proprement sur EOF immédiat
 * quand stdin n'est pas un TTY, mais démarre et meurt inutilement à chaque
 * lancement. Le journal reste inchangé dans les deux cas — `logger.c` détecte
 * déjà `isatty(STDOUT_FILENO)` et n'émet jamais de codes ANSI hors TTY.
 * Position-indépendant, retiré d'argv par `parse_cli_options` avant le parsing
 * positionnel, comme `--stop-on-solution`.
 */
extern int headless_mode;

/**
 * @brief Nombre de possibilités qu'un client pruner demande/acquitte par lot.
 *
 * Configurable au démarrage (argument CLI du mode `pruner`) et à
 * l'exécution via la commande `prunerBatch <n>` (propagée aux process enfants).
 * Borne la mémoire de l'échange : le pruner ne détient jamais plus que ce lot,
 * la capacité mémoire n'a donc pas à être supposée illimitée. Défaut
 * `PRUNER_BATCH_SIZE`, plafonné à `PRUNER_BATCH_MAX`.
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

/**
 * @brief Moteur de la preuve de fermeture bornée du pruner CPU : ordre
 *        DYNAMIQUE (MRV) si levé, ordre FIXE sinon — §4.10 de
 *        docs/conception/elagage_recherche.md.
 *
 * Lu par `search_packet_backtracking_budgeted` (`src/core/etii_search.c`)
 * uniquement, donc sans aucun effet quand `pruner_dfs_budget <= 0` (la preuve
 * n'est alors jamais tentée) ni sur la recherche réelle, dont l'ordre reste
 * gouverné par `mrv_enabled` seul. Les deux drapeaux sont volontairement
 * INDÉPENDANTS : un même process n'a qu'un rôle (recherche OU pruner), mais
 * les mesurer ensemble interdirait l'A/B exigé par le protocole §7, et le
 * verdict n'est pas le même des deux côtés — MRV est mesuré favorable pour la
 * RÉFUTATION (le métier du pruner) sans l'être uniformément pour l'exploration
 * de sous-arbres encore vivants (§4.7).
 *
 * Défaut `PRUNER_DFS_MRV_DEFAULT` (0, cf. sa doc). La variable
 * d'environnement `ETII_PRUNER_DFS_MRV` (`0`/`1`) est lue une seule fois au
 * démarrage, AVANT tout `fork()` (invariant de résolution pré-fork), comme
 * `ETII_MRV` et `ETII_BENCH_NODES` : pas d'entrée `cli_topics[]`, pas de
 * commande console — c'est un levier de mesure et de déploiement par machine
 * (« les machines les plus performantes en pruner MRV »), pas un réglage à
 * changer en cours de route. Aucune conséquence sur le protocole : la preuve
 * bornée ne délègue rien et ne modifie pas la possibilité contrôlée, seul son
 * VERDICT compte, et il est identique par construction (condition nécessaire
 * exacte dans les deux ordres).
 */
extern int pruner_dfs_mrv;

/**
 * @brief Durée (secondes) du bail à expiration des possibilités attribuées à
 *        un client (PR7).
 *
 * Configurable à l'exécution via la commande console `leaseDuration <n>`.
 * Défaut `ANALYSED_LEASE_DEFAULT_SECONDS`. `<= 0` désactive le bail : les
 * possibilités attribuées ne sont alors jamais rendues automatiquement au
 * stock (comportement d'avant cette PR).
 */
extern int analysed_lease_seconds;

#ifdef WITH_CUDA
/**
 * @brief 1 si le processus est un client pruner GPU (option `--gpu` du mode `pruner`).
 *
 * Implique `pruner_mode == 1` (même plomberie réseau que le pruner CPU) mais le
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

extern unsigned long long non_null_possibilities;

extern volatile int request;

extern long inst_unknow;

extern int version;

extern pid_t parent_pid;

extern pid_t *childrens_pid;

extern char **forkId;

extern struct client_statistics *fork_statistics;

/**
 * @brief Dernier `time(NULL)` où le parent a reçu un signe d'activité de
 *        chaque fils (réception d'un datagramme `IPC_MSG_STATS`), parallèle
 *        à `fork_statistics` (même taille, même cycle de vie — alloué/
 *        réalloué/libéré aux côtés de celui-ci dans `init_childs`/
 *        `ensure_childs_capacity`/`free_childs`). `0` tant qu'aucune activité
 *        n'a encore été observée pour ce slot.
 *
 * Sert de base à l'escalade d'arrêt PAR FILS (`child_idle_ms`,
 * `src/app/fork_orchestrator.h`) : un fils qui rapporte encore de l'activité
 * (ex. vidage final de sa file d'acquittements en attente, ou renvoi de son
 * stock local restant, cf. `server_io_active`) ne doit pas être interrompu
 * par un délai fixe commun à tout le lot — seule SON inactivité doit compter.
 */
extern time_t *fork_last_activity;

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
 * bloqué/inactif ? » (cf. `fork_diagnostic_summary`).
 */
extern volatile int server_io_active;

extern int fork_checker_socket_id;

extern struct sockaddr_un *main_addr;

extern int *main_socket_id;

extern int SERVER_PORT;

/**
 * @brief Port TCP de l'API REST admin (option CLI `--http-port <n>`).
 *
 * 0 (défaut) : API désactivée, aucun socket supplémentaire n'est ouvert.
 * Sinon, `runserver` démarre un écouteur HTTP dédié sur ce port, lié à
 * `127.0.0.1` uniquement (pas d'exposition réseau par défaut — cf.
 * `src/net/http_server.h`). Lu côté serveur uniquement. Position-indépendant,
 * retiré d'argv par `parse_cli_options` avant le parsing positionnel.
 */
extern int HTTP_PORT;

/**
 * @brief Chemin du fichier contenant le jeton d'authentification Bearer de
 *        l'API HTTP admin (option CLI `--http-token-file <chemin>`).
 *
 * `NULL` (défaut) : aucun jeton — les commandes privilégiées (`restore`,
 * `backup`, `control_command_privileged`) restent inaccessibles via
 * `POST /api/v1/command` quel que soit `HTTP_PORT`. Pointeur direct dans
 * `argv` (même convention que `parts_files`) : jamais copié, valable pour
 * toute la durée du process. Lu une seule fois au démarrage (`main()`, avant
 * tout fork) via `http_token_load` (`src/net/http_server.h`), qui remplit
 * `HTTP_ADMIN_TOKEN` et fait échouer le démarrage (message explicite + exit)
 * si le fichier est illisible ou a des permissions plus larges que
 * propriétaire-seul (mode & 0077 != 0, comme une clé privée SSH). Position-
 * indépendant, retiré d'argv par `parse_cli_options`.
 */
extern const char *HTTP_TOKEN_FILE;

/// Taille de `HTTP_ADMIN_TOKEN`, terminateur NUL inclus.
#define HTTP_ADMIN_TOKEN_MAX 256

/**
 * @brief Jeton d'authentification Bearer chargé depuis `HTTP_TOKEN_FILE` au
 *        démarrage (`http_token_load`), chaîne vide si `--http-token-file`
 *        n'a pas été fourni (défaut : aucune commande privilégiée accessible
 *        via l'API HTTP, cf. `HTTP_TOKEN_FILE`). Jamais journalisé en clair.
 */
extern char HTTP_ADMIN_TOKEN[HTTP_ADMIN_TOKEN_MAX];

/**
 * @brief Libellé déclaré du client (option CLI `--name <label>`).
 *
 * `NULL` (défaut) : `init_client_identity` (src/app/app_runtime.h) retombe
 * sur le nom d'hôte (`gethostname`). Pointeur direct dans `argv` (même
 * convention que `parts_files`/`HTTP_TOKEN_FILE`) : jamais copié, valable
 * pour toute la durée du process. Position-indépendant, retiré d'argv par
 * `parse_cli_options`. Purement déclaratif : jamais vérifié par le serveur,
 * à la différence de `peer_ip`.
 */
extern const char *client_label;

/**
 * @brief Chemin du fichier d'identité machine persistante (option CLI
 *        `--machine-uid-file <chemin>`).
 *
 * Défaut `"./eternityii-machine_uid"` (même répertoire que les `.back`
 * existants). Lu/créé une seule fois au démarrage (`init_client_identity`,
 * avant tout fork) via `machine_uid_load_or_create` (net/client_identity.h).
 * Position-indépendant, retiré d'argv par `parse_cli_options`.
 */
extern const char *machine_uid_file_path;

/**
 * @brief Chemin du fichier de configuration client (option CLI
 *        `--config-file <chemin>`).
 *
 * Défaut `"./eternityii-client.conf"` (même convention que les `.back` et
 * `machine_uid_file_path`). Lu au démarrage du client/pruner (`handle_client`,
 * src/app/main.c) via `client_config_load` (src/app/client_config.h), qui
 * pré-remplit les valeurs par défaut des positions non fournies en ligne de
 * commande (priorité CLI > fichier > défauts) — jamais un échec de démarrage
 * si ce fichier est absent ou illisible. Position-indépendant, retiré d'argv
 * par `parse_cli_options`.
 */
extern const char *client_config_file_path;

/**
 * @brief Identité déclarée de CE process client, résolue une seule fois par
 *        `init_client_identity` (src/app/app_runtime.h) AVANT tout fork —
 *        chaque fork en hérite une copie identique par copy-on-write
 *        (`machine_uid`, `client_uid`, `mode`, `label` partagés ; seul
 *        `fork_seq` diffère, ajusté par chaque connexion au moment de
 *        l'émission de son hello, jamais ici). `fork_seq` vaut -1 dans ce
 *        gabarit (ni le parent ni aucun fork en particulier) : c'est au
 *        point d'envoi (INST_CLIENT_HELLO côté fork, INST_CONTROL_HELLO côté
 *        parent) de le fixer à sa valeur réelle sur une COPIE locale.
 */
extern client_identity_t g_client_identity_template;

/**
 * @brief Débit de recherche courant du serveur (essais/seconde), publié
 *        toutes les 10 s par `check_server_step` (src/app/etii_server.c).
 *
 * Lecture par l'API REST (`GET /api/v1/stats`) sans verrou : une lecture
 * concurrente à la publication peut voir une valeur en cours d'écriture d'au
 * plus quelques dizaines de millisecondes de retard, sans conséquence pour un
 * indicateur de télémétrie.
 */
extern volatile unsigned long long server_shots_per_second;

/**
 * @brief Durée (millisecondes) de la DERNIÈRE sauvegarde automatique
 *        effectivement exécutée — englobe tout ce que `check_server_step` déclenche à ce tour :
 *        `consistent_backup` (si stock ou pool analysé a bougé),
 *        `best_board_save`, `known_clients_registry_save` (chacun
 *        indépendamment sauté si son propre artefact n'a pas changé). 0 tant
 *        qu'aucune sauvegarde n'a encore eu lieu. Écrite par le seul thread
 *        `check_server` (pas de concurrence réelle), lue sans verrou par
 *        `GET /api/v1/status` — même tolérance que `server_shots_per_second`
 *        ci-dessus : un lecteur concurrent voit au pire une valeur d'un tour
 *        de retard, sans conséquence pour un indicateur de télémétrie.
 */
extern volatile unsigned long long server_last_backup_duration_ms;

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

// Timeout d'inactivité (secondes) des sockets TCP de travail, des deux côtés
// de la connexion : SO_RCVTIMEO/SO_SNDTIMEO côté client (create_tcp_client,
// src/net/tcpclient.c) et côté serveur (configure_client_socket,
// src/app/etii_server.c). Défaut DEFAULT_TCP_TIMEOUT (10 s) ; réglable via
// --tcp-timeout (src/app/static_variables.c:parse_cli_options), option
// globale sans restriction de mode (les deux côtés en dépendent). Une
// maintenance longue (sauvegarde, restore, tri) qui gèle temporairement le
// stock (cf. DATAMANAGER_TRYLOCK_MAX_SWEEPS ci-dessus) reste largement sous
// ce budget par construction ; cette option reste une soupape pour un
// réseau plus lent ou un stock encore plus volumineux.
extern int tcp_timeout;

extern int server;

extern int server_rmnonext_timing;

/**
 * @brief Nombre de nœuds cible du banc de mesure (variable d'environnement
 *        `ETII_BENCH_NODES`), 0 = désactivé.
 *
 * Lue une seule fois au démarrage (`bench_parse_nodes_env`, appelée dans
 * `main()` avant tout fork) depuis l'environnement plutôt qu'une option CLI :
 * hors du chemin de production, elle n'a donc pas besoin d'entrée dans
 * `cli_topics[]`. Un critère d'arrêt par nombre de nœuds est bien moins bruité
 * qu'un arrêt par durée — en mode `test` la recherche est déterministe, donc à
 * N fixé le travail exploré est strictement identique d'un run à l'autre.
 * Consommée uniquement par `check_client_threads` / `check_client_threads_step`
 * (src/app/etii_client.c), qui échantillonnent déjà `counters[]` : AUCUN coût
 * n'est ajouté à la boucle chaude de `autosearch()` (src/core/etii_search.c).
 * Voir `tests/bench/bench_search.sh`.
 */
extern unsigned long long bench_target_nodes;

/**
 * @brief Parse la variable d'environnement `ETII_BENCH_NODES` en nombre de
 *        nœuds cible. Fonction pure et testable : ne lit pas l'environnement
 *        elle-même, reçoit la valeur déjà récupérée par `getenv()`.
 *
 * @param env_value Valeur de la variable d'environnement, ou NULL si absente.
 * @return Le nombre de nœuds cible (0 si absente, vide, ou non numérique).
 */
unsigned long long bench_parse_nodes_env(const char *env_value);

/**
 * @brief Valeur par défaut de `mrv_enabled`.
 *
 * **0 (ordre FIXE) pour l'instant — décision de déploiement, pas de mesure.**
 * L'ordre dynamique (MRV, §4.7 de docs/conception/elagage_recherche.md) est
 * mesuré favorable sur le critère retenu — le coût de RÉFUTATION (prouver
 * qu'une possibilité est morte) sur un VRAI stock serveur, à temps CPU égal
 * (`tests/bench/bench_refutation.c`) : 79 racines fermées sur 120, contre 20
 * pour l'ordre fixe et 52 pour l'ordre fixe doté du seul balayage global
 * (`global_dead_check`), soit ~4× plus de stock résolu par seconde de CPU —
 * mais ce basculement change le moteur de recherche de toute une flotte
 * déployée, et l'opérateur a demandé du recul avant de l'imposer par défaut.
 * `ETII_MRV=1` reste le moyen de l'activer sans reconstruire, exactement le
 * même mécanisme que le repli l'aurait été dans l'autre sens.
 * NE PAS reprendre l'affirmation « le mur à max_result ≈ 74 était un artefact
 * de l'ordre fixe » : c'était un artefact du PROTOCOLE de mesure du banc de
 * débit (mono-processus, depuis la genèse, sans stock ni délégation) — contre
 * un vrai serveur, un client à ordre fixe atteint 186.
 */
#define MRV_DEFAULT_ENABLED 0

/**
 * @brief Sélectionne l'ordre de variable de la boucle de recherche : DYNAMIQUE
 *        (MRV, la case vide la plus contrainte à chaque nœud) ou FIXE
 *        (`directions[]`, le moteur historique) — §4.7 de
 *        docs/conception/elagage_recherche.md.
 *
 * Défaut : `MRV_DEFAULT_ENABLED` (FIXE, cf. sa doc — MRV est mesuré favorable
 * mais pas encore le défaut de déploiement). La variable d'environnement de
 * développement `ETII_MRV` (`0` = ordre fixe, `1` = ordre dynamique) est lue
 * une seule fois au démarrage, comme `ETII_BENCH_NODES`, et n'a donc pas
 * d'entrée dans `cli_topics[]` : elle sert aux mesures A/B du banc (protocole
 * §7 : chaque piste se mesure PAR-DESSUS la précédente) et à activer MRV sans
 * reconstruire, jamais comme réglage d'exploitation courant.
 *
 * Les deux moteurs sont interopérables : `search_packet_backtracking_mrv`
 * re-canonise tout paquet délégué (`bt_canonicalize_packet`), si bien qu'un
 * client MRV, un client à ordre fixe et un pruner peuvent se partager le même
 * serveur — aucun bump de `VERSION` (§5).
 */
extern int mrv_enabled;

/**
 * @brief Arme le balayage GLOBAL de case morte dans le moteur à ordre FIXE —
 *        expérience d'ABLATION, jamais un réglage d'exploitation (défaut 0).
 *
 * Les deux moteurs confondent deux axes indépendants : l'ordre fixe va toujours
 * avec une détection de case morte LOCALE (les 4 voisines, `bt_forward_check`),
 * l'ordre dynamique toujours avec une détection GLOBALE (le balayage de
 * `mrv_choose_cell` voit toute case morte du plateau, où qu'elle soit). Comparer
 * ces deux-là ne dit donc pas lequel des deux axes produit l'effet mesuré.
 * Ce drapeau remplit la case manquante : ordre fixe + détection globale, en
 * appelant exactement le même balayage que MRV et en JETANT le choix de case.
 *
 * Coût nul quand il vaut 0 (le miroir 64 bits des pièces utilisées n'est même
 * pas entretenu). Lu par `search_packet_backtracking_core` uniquement — le
 * moteur MRV fait déjà ce test par construction. Utilisé par
 * `tests/bench/bench_refutation.c` ; aucune entrée `cli_topics[]`, aucune
 * commande console.
 */
extern int global_dead_check;

/**
 * @brief Arme la détection de CONFLIT DE SINGLETONS dans `bt_forward_check` —
 *        expérience de mesure, jamais un réglage d'exploitation (défaut 0).
 *
 * §4.4 de docs/conception/elagage_recherche.md, réimplémentation post-PR 10 :
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
 * est inchangé). Lu par `bt_forward_check` uniquement, donc actif pour LES
 * DEUX moteurs (ordre fixe et MRV, qui partagent cette même fonction) dès
 * qu'il est levé.
 */
extern int singleton_conflict_check;

/**
 * @brief Parse `ETII_MRV` en drapeau d'activation. Fonction pure et testable.
 *
 * @param env_value Valeur de la variable d'environnement, ou NULL si absente.
 * @return 0 si `env_value` vaut exactement "0", 1 s'il vaut exactement "1",
 *         `MRV_DEFAULT_ENABLED` sinon (absente ou valeur non reconnue : jamais
 *         de bascule silencieuse hors du défaut du programme).
 */
int mrv_parse_env(const char *env_value);

/**
 * @brief Parse `ETII_PRUNER_DFS_MRV` en drapeau de moteur pour la preuve
 *        bornée du pruner. Fonction pure et testable.
 *
 * Volontairement distincte de `mrv_parse_env` malgré une logique identique :
 * les deux drapeaux ont des défauts et des verdicts de mesure indépendants
 * (cf. `pruner_dfs_mrv`), les fusionner ferait qu'un futur basculement de l'un
 * emporterait silencieusement l'autre.
 *
 * @param env_value Valeur de la variable d'environnement, ou NULL si absente.
 * @return 0 si `env_value` vaut exactement "0", 1 s'il vaut exactement "1",
 *         `PRUNER_DFS_MRV_DEFAULT` sinon (absente ou valeur non reconnue).
 */
int pruner_dfs_mrv_parse_env(const char *env_value);

/**
 * @brief Décide si le banc de mesure doit demander l'arrêt de la recherche.
 *
 * Fonction pure et testable, séparée du sondage réel (`check_client_threads`)
 * pour pouvoir être testée sans thread ni process de recherche. Un léger
 * dépassement de `target_nodes` est attendu et acceptable : l'appelant
 * échantillonne périodiquement plutôt que de tester à chaque nœud — c'est
 * `nodes_done`, la valeur réellement atteinte, que le banc doit reporter,
 * jamais `target_nodes`.
 *
 * @param target_nodes Nombre de nœuds visé (0 = banc désactivé, ne s'arrête jamais).
 * @param nodes_done   Nombre de nœuds effectivement visités jusqu'ici.
 * @return 1 si l'arrêt doit être demandé, 0 sinon.
 */
int bench_should_stop(unsigned long long target_nodes, unsigned long long nodes_done);

/**
 * @brief Extrait les options globales de `argv` et les retire du tableau.
 *
 * Reconnaît `--stop-on-solution`, `--expand-level <n>`, `--expand-max-stock <n>`,
 * `--expand-max-levels <n>`, `--http-port <n>`, `--http-token-file <chemin>`,
 * `--name <label>`, `--machine-uid-file <chemin>`, `--config-file <chemin>`,
 * `--stock-files <n>`, `--stock-max-ram <mo>`, `--stock-spill-dir <chemin>`,
 * `--rebalance-budget <n>`, `--tcp-timeout <n>`, `--gpu`, `--headless` et
 * `--help`/`-h` (positionne respectivement `stop_on_solution`,
 * `expand_min_level`, `expand_max_stock`, `expand_max_levels`, `HTTP_PORT`,
 * `HTTP_TOKEN_FILE`, `client_label`, `machine_uid_file_path`,
 * `client_config_file_path`, `stock_files_requested`, `stock_max_ram_mb`,
 * `stock_spill_dir`, `rebalance_budget`, `tcp_timeout`, `gpu_requested`,
 * `headless_mode` et `help_requested`). Compacte
 * `argv` en place pour supprimer les options reconnues, afin de ne pas perturber
 * le parsing positionnel des modes. Appelée AVANT tout fork.
 *
 * @param argc Nombre d'arguments.
 * @param argv Tableau d'arguments (modifié en place : options retirées).
 * @return     Le nouveau nombre d'arguments (sans les options reconnues).
 */
int parse_cli_options(int argc, const char *argv[]);
#endif /* static_variables_h */
