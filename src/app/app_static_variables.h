#ifndef app_static_variables_h
#define app_static_variables_h

#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/un.h>
#include <pthread.h>
#include "app/etii_statistic.h"
#include "net/client_identity.h"
#include "core/core_static_variables.h"

// Ce fichier contenait, jusqu'à son éclatement, TOUT l'état statique du
// programme — y compris des constantes/globales purement algorithmiques
// (géométrie du puzzle, compteurs de recherche) dont `src/core/` dépendait,
// en violation de la règle "core/ ne doit jamais dépendre de app/"
// (cf. AGENTS.md). Ce sous-ensemble a été extrait vers
// `src/core/core_static_variables.h` (inclus ci-dessus dans l'AUTRE sens
// autorisé : app/ PEUT dépendre de core/, jamais l'inverse). Ce fichier-ci ne
// garde que l'état réellement applicatif : options CLI, identité client, API
// HTTP admin, configuration serveur (expansion/rebalance/bail/RAM/débordement
// disque), bancs de mesure. `src/core/` ne doit PAS l'inclure — deux
// exceptions documentées et assumées (`core/datamanager.c`,
// `core/etii_search.c`) restent en pratique tributaires de symboles définis
// ici (version de protocole, port serveur, identité machine...) : voir la
// note en tête de `core/core_static_variables.h`.

#define VERSION 13

#define NB_CONNECTIONS_PER_THREAD 1
// Cadence de la sonde de faim du serveur (INST_NEED_WORK) émise par
// le thread d'alimentation pour chaque thread occupé disposant d'un socket.
// Elle remplace le keepalive INST_TEST_CONNECTED (un échange réussi prouve la
// session vivante) : l'intervalle effectif est min(tcp_timeout/2, cette valeur).
// Back-off du thread d'alimentation quand le serveur n'a AUCUNE possibilité à
// fournir (stock épuisé, ou serveur saturé qui ne répond pas au handshake) : au
// lieu de redemander toutes les THREAD_MICRO_SLEEP (core/core_static_variables.h,
// ≈ 100 req/s/thread, ce qui alimente la contention « all threads busy »), on
// attend de plus en plus longtemps (doublement) jusqu'à un plafond, puis on
// repart à zéro dès qu'un travail est obtenu. Bornes en microsecondes.
#define NO_WORK_SLEEP_START 50000    // 50 ms : première pause après un cycle à vide
#define NO_WORK_SLEEP_MAX  500000    // 0,5 s : plafond (sous la limite usleep POSIX de 1 s)
#define NEED_WORK_POLL_INTERVAL_S 2
// Faim du serveur par client actif : le serveur vise un stock d'au moins
// SERVER_HUNGER_PER_CLIENT × sessions connectées (marge pour que chaque GET
// trouve une possibilité), et publie le manque via INST_NEED_WORK.
#define SERVER_HUNGER_PER_CLIENT 2
// Plafond de la faim annoncée par le serveur : borne la matérialisation et
// l'envoi demandés aux clients occupés (chaque thread cède déjà au plus la
// moitié de son stock implicite, mais tous peuvent répondre en même temps).
#define SERVER_HUNGER_CAP 1000

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
// l'option CLI `--expand-max-levels <n>` — voir ci-dessous), quelle
// que soit la consigne de niveau — garde-fou en PROFONDEUR pour ne pas mettre
// le serveur au travail trop longtemps.
#define EXPAND_MAX_LEVELS 4
// EXPAND_MAX_STOCK : valeur par DÉFAUT du plafond de sécurité en NOMBRE de
// possibilités (variable globale `expand_max_stock`, configurable à chaud via
// l'option CLI `--expand-max-stock <n>` — voir ci-dessous). Le facteur
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

#define DEFAULT_TCP_TIMEOUT 10

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
 * Position-indépendante comme `--stop-on-solution`. Lue dans `main()` avant
 * le dispatch des modes : l'aide générale est affichée puis le programme
 * sort avec EXIT_SUCCESS.
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
 * `machine_uid_file_path`. Aucune E/S ici — la création/purge effective du
 * répertoire est différée à `stock_spill_configure` (`core/stock_spill.h`),
 * appelée UNIQUEMENT depuis `runserver` (`app/etii_server.c`) : contrairement
 * à `stock_max_ram_mb`, ce chemin n'a de sens que côté serveur (le stock
 * local d'un client/pruner n'a pas de thread de débordement) — jamais lu ni
 * appliqué sur les autres rôles. Position-indépendant, retiré d'argv par
 * `parse_cli_options` avant le parsing positionnel.
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

extern volatile int server_io_active;

extern int fork_checker_socket_id;

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

extern int communication_in_progress;

#ifdef DEBUG_SOCKET
extern int opened_tcp;
#endif // DEBUG_SOCKET

extern long nb_client;

// Timeout d'inactivité (secondes) des sockets TCP de travail, des deux côtés
// de la connexion : SO_RCVTIMEO/SO_SNDTIMEO côté client (create_tcp_client,
// src/net/tcpclient.c) et côté serveur (configure_client_socket,
// src/app/etii_server.c). Défaut DEFAULT_TCP_TIMEOUT (10 s) ; réglable via
// --tcp-timeout (src/app/app_static_variables.c:parse_cli_options), option
// globale sans restriction de mode (les deux côtés en dépendent). Une
// maintenance longue (sauvegarde, restore, tri) qui gèle temporairement le
// stock (cf. DATAMANAGER_TRYLOCK_MAX_SWEEPS, core/core_static_variables.h)
// reste largement sous ce budget par construction ; cette option reste une
// soupape pour un réseau plus lent ou un stock encore plus volumineux.
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
#endif /* app_static_variables_h */
