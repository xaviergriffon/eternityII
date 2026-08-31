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

// Politique automatique de dosage recherche/contrôle (option `--auto-roles`).
// Chaque changement de dosage coûte un stopForks+re-fork chez le(s)
// client(s) visé(s) : un délai minimal entre deux changements (en tours de
// check_server_step, 10s chacun)
// évite l'oscillation qu'un ajustement à chaque tour produirait sur un signal
// bruité. ROLE_MIX_MIN_TICKS_DEFAULT × 10s ≈ 2 minutes, du même ordre que la
// fenêtre d'autobackup (should_autobackup, ~60s) mais délibérément plus longue
// (le coût d'un changement de dosage est plus élevé que celui d'une écriture
// disque sautée).
#define ROLE_MIX_MIN_TICKS_DEFAULT 12
// Plafond défensif sur le dosage ATTEINT par la seule politique automatique
// (pas la borne par-fork de resolve_pruner_forks, qui reste NB_THREADS) :
// la politique n'avance que par pas de ±1 depuis 0, ce plafond n'a donc
// d'effet que si un déséquilibre persiste des dizaines de tours d'affilée —
// garde-fou de dernier recours, pas un objectif.
#define ROLE_MIX_MAX_DOSAGE 8
// Plancher sous lequel un déséquilibre stock non-vérifié/vérifié est ignoré
// (bruit de démarrage/de faible activité), pour ne pas déclencher la
// politique sur un stock encore quasi vide.
#define ROLE_MIX_BACKLOG_FLOOR 8

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
 * @brief Dosage recherche/contrôle demandé pour le lot de forks à venir
 *        (`--pruner-forks <n>`, ou clé de configuration `pruner_forks`).
 *
 * `-1` (défaut) : non demandé, chaque fork garde le rôle impliqué par
 * `pruner_mode`. `0..NB_THREADS` : nombre de forks affectés au contrôle,
 * les autres cherchent. Une valeur hors bornes est clampée au moment de la
 * résolution (`resolve_pruner_forks`), jamais ici — cette globale ne porte
 * que ce qui a été demandé, pas ce qui est effectivement appliqué.
 */
extern int pruner_forks_requested;

/**
 * @brief 1 si la politique automatique de dosage recherche/contrôle a été
 *        demandée (`--auto-roles`).
 *
 * `0` (défaut) : désactivée, l'opérateur garde la main via `clientsRoles`.
 * `1` : `check_server_step` ajuste lui-même, à chaque tour de 10s, le
 * dosage diffusé au parc, sous hystérésis (voir `compute_desired_role_mix`).
 */
extern int auto_roles_requested;

/** @brief 1 si l'exécution GPU du pruner a été demandée (`--gpu`) : sur build CUDA active `gpu_pruner_mode`, sinon erreur explicite. */
extern int gpu_requested;

/** @brief 1 si l'aide CLI a été demandée (`--help`/`-h`) : affichée puis sortie immédiate. */
extern int help_requested;

/**
 * @brief Niveau de curseur (`alloc`) minimal visé par l'expansion du stock au
 *        démarrage du serveur (`--expand-level <n>`).
 *
 * 0 (défaut) : pas d'expansion. Sinon, `runserver` développe le stock genèse
 * jusqu'à ce niveau, borné par `EXPAND_MAX_LEVELS` passes et
 * `expand_max_stock` possibilités.
 */
extern int expand_min_level;

/**
 * @brief Plafond en nombre de possibilités de l'expansion du stock au
 *        démarrage du serveur (`--expand-max-stock <n>`).
 *
 * Défaut `EXPAND_MAX_STOCK` (100000, ~54 Mo). Contrairement à
 * `expand_min_level`, `<n> <= 0` est ignoré : un plafond nul arrêterait
 * l'expansion avant même la première pièce placée.
 */
extern int expand_max_stock;

/**
 * @brief Plafond en nombre de passes de l'expansion du stock au démarrage
 *        (`--expand-max-levels <n>`).
 *
 * Défaut `EXPAND_MAX_LEVELS` (4) — garde-fou en profondeur, indépendant du
 * garde-fou en volume `expand_max_stock` : même à volume élevé, un
 * `expand_min_level` absurdement grand ne peut pas tourner indéfiniment.
 */
extern int expand_max_levels;

/**
 * @brief Nombre de possibilités déplacées de la file la plus pleine vers la
 *        plus vide à chaque tour de `check_server_step`
 *        (`--rebalance-budget <n>`).
 *
 * Défaut `REBALANCE_BUDGET_DEFAULT` (1000). Consommé par
 * `datamanager_rebalance_step`, appelé une fois par tour (10s), jamais un
 * chemin chaud.
 */
extern int rebalance_budget;

/**
 * @brief Nombre de files de stock demandé au démarrage (`--stock-files <n>`).
 *
 * 0 = non demandé (défaut `NB_FILE_POSSIBILITY_DEFAULT`). Stocké ici plutôt
 * qu'appliqué directement dans `parse_cli_options` : ce fichier reste
 * volontairement sans dépendance sur `core/datamanager.h` ; `main()`
 * applique la valeur via `datamanager_configure_stock_files`, avant tout
 * fork/thread.
 */
extern int stock_files_requested;

/**
 * @brief Plafond en Mo de la RAM consacrée aux deux pools de stock serveur
 *        (non vérifié + vérifié — `--stock-max-ram <mo>`).
 *
 * 0 (défaut) = illimité. La conversion en nombre de possibilités (l'unité
 * comparée par `put_to_pool`) est faite une seule fois, après le parsing,
 * par `datamanager_configure_ram_limit`. Le pool analysé n'est
 * délibérément pas couvert : déjà borné par le nombre de clients en vol et
 * les baux d'expiration, et son index de hachage impose une correspondance
 * exacte qu'un déport casserait.
 *
 * `datamanager_configure_ram_limit` est appelé sans condition de rôle dans
 * `main()`, avant tout fork : `put_to_pool` est du code partagé, utilisé
 * aussi bien par le stock local d'un client/pruner que par le serveur. En
 * pratique seul le stock serveur atteint un volume significatif — d'où la
 * description « serveur » de cette option dans l'aide CLI.
 */
extern int stock_max_ram_mb;

/**
 * @brief Répertoire de débordement sur disque du stock serveur
 *        (`--stock-spill-dir <chemin>`).
 *
 * Défaut `STOCK_SPILL_DIR_DEFAULT` (`"./eternityii-spill"`). Aucune E/S ici
 * — la création/purge effective est différée à `stock_spill_configure`,
 * appelée uniquement depuis `runserver` : ce chemin n'a de sens que côté
 * serveur (le stock local d'un client/pruner n'a pas de thread de
 * débordement).
 */
extern const char *stock_spill_dir;

/**
 * @brief 1 si la console interactive (lecture de stdin) ne doit pas démarrer
 *        (`--headless`).
 *
 * Défaut 0. Pensé pour une exécution en service (systemd,
 * `StandardInput=null`) : sans ce flag, le thread console se termine déjà
 * proprement sur EOF immédiat hors TTY, mais démarre et meurt inutilement à
 * chaque lancement.
 */
extern int headless_mode;

/**
 * @brief Durée (secondes) du bail à expiration des possibilités attribuées à
 *        un client.
 *
 * Configurable à l'exécution via `leaseDuration <n>`. Défaut
 * `ANALYSED_LEASE_DEFAULT_SECONDS`. `<= 0` désactive le bail : les
 * possibilités attribuées ne sont jamais rendues automatiquement au stock.
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
 *        l'API HTTP admin (`--http-token-file <chemin>`).
 *
 * `NULL` (défaut) : aucun jeton, les commandes privilégiées restent
 * inaccessibles via `POST /api/v1/command`. Lu une seule fois au démarrage,
 * avant tout fork, via `http_token_load`, qui fait échouer le démarrage si
 * le fichier est illisible ou a des permissions plus larges que
 * propriétaire-seul (comme une clé privée SSH).
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
 * @brief Libellé déclaré du client (`--name <label>`).
 *
 * `NULL` (défaut) : `init_client_identity` retombe sur le nom d'hôte.
 * Purement déclaratif : jamais vérifié par le serveur, à la différence de
 * `peer_ip`.
 */
extern const char *client_label;

/**
 * @brief Chemin du fichier d'identité machine persistante
 *        (`--machine-uid-file <chemin>`).
 *
 * Défaut `"./eternityii-machine_uid"`. Lu/créé une seule fois au démarrage,
 * avant tout fork, via `machine_uid_load_or_create`.
 */
extern const char *machine_uid_file_path;

/**
 * @brief Chemin du fichier de configuration client (`--config-file <chemin>`).
 *
 * Défaut `"./eternityii-client.conf"`. Lu au démarrage via
 * `client_config_load`, qui pré-remplit les valeurs par défaut des options
 * non fournies en ligne de commande (priorité CLI > fichier > défauts) —
 * jamais un échec de démarrage si le fichier est absent ou illisible.
 */
extern const char *client_config_file_path;

/**
 * @brief Chemin du fichier de configuration serveur (`--config-file
 *        <chemin>`, partagée avec le client — un seul mode par process).
 *
 * Défaut `"./eternityii-server.conf"`. Même comportement que
 * `client_config_file_path` côté serveur.
 */
extern const char *server_config_file_path;

/**
 * @brief Identité déclarée de ce process client, résolue une seule fois par
 *        `init_client_identity` avant tout fork — chaque fork en hérite une
 *        copie identique par copy-on-write. `fork_seq` vaut -1 dans ce
 *        gabarit : c'est au point d'envoi (hello) de le fixer sur une copie
 *        locale.
 */
extern client_identity_t g_client_identity_template;

/**
 * @brief Débit de recherche courant du serveur (essais/seconde), publié
 *        toutes les 10s par `check_server_step`.
 *
 * Lecture par l'API REST sans verrou : une lecture concurrente peut voir
 * une valeur en cours d'écriture, sans conséquence pour un indicateur de
 * télémétrie.
 */
extern volatile unsigned long long server_shots_per_second;

/**
 * @brief Durée (ms) de la dernière sauvegarde automatique effectivement
 *        exécutée — englobe tout ce que `check_server_step` déclenche ce
 *        tour (chaque artefact indépendamment sauté si inchangé). 0 tant
 *        qu'aucune sauvegarde n'a encore eu lieu.
 */
extern volatile unsigned long long server_last_backup_duration_ms;

extern unsigned long long max_search_by_sec;

extern int communication_in_progress;

#ifdef DEBUG_SOCKET
extern int opened_tcp;
#endif // DEBUG_SOCKET

extern long nb_client;

// Timeout d'inactivité (secondes) des sockets TCP de travail, des deux côtés
// (SO_RCVTIMEO/SO_SNDTIMEO client et serveur). Défaut DEFAULT_TCP_TIMEOUT
// (10s), réglable via --tcp-timeout. Une maintenance longue qui gèle
// temporairement le stock reste largement sous ce budget par construction ;
// cette option reste une soupape pour un réseau plus lent ou un stock plus
// volumineux.
extern int tcp_timeout;

extern int server;

extern int server_rmnonext_timing;

/**
 * @brief Nombre de nœuds cible du banc de mesure (`ETII_BENCH_NODES`), 0 =
 *        désactivé.
 *
 * Lue depuis l'environnement plutôt qu'une option CLI : hors du chemin de
 * production. Un critère d'arrêt par nombre de nœuds est moins bruité qu'un
 * arrêt par durée — en mode `test` la recherche est déterministe. Consommée
 * uniquement par `check_client_threads_step`, qui échantillonne déjà
 * `counters[]` : aucun coût ajouté à la boucle chaude de `autosearch()`.
 */
extern unsigned long long bench_target_nodes;

/**
 * @brief Parse la variable d'environnement `ETII_BENCH_NODES` en nombre de
 *        nœuds cible. Fonction pure : reçoit la valeur déjà récupérée par
 *        `getenv()`.
 *
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
 * `--rebalance-budget <n>`, `--tcp-timeout <n>`, `--pruner-forks <n>`, `--auto-roles`,
 * `--gpu`, `--headless` et `--help`/`-h` (positionne respectivement `stop_on_solution`,
 * `expand_min_level`, `expand_max_stock`, `expand_max_levels`, `HTTP_PORT`,
 * `HTTP_TOKEN_FILE`, `client_label`, `machine_uid_file_path`,
 * `client_config_file_path` et `server_config_file_path` (les deux à la fois —
 * un seul mode s'exécute par process, cf. `server_config_file_path`),
 * `stock_files_requested`, `stock_max_ram_mb`,
 * `stock_spill_dir`, `rebalance_budget`, `tcp_timeout`, `pruner_forks_requested`,
 * `auto_roles_requested`, `gpu_requested`, `headless_mode` et `help_requested`). Compacte
 * `argv` en place pour supprimer les options reconnues, afin de ne pas perturber
 * le parsing positionnel des modes. Appelée AVANT tout fork.
 *
 * @param argc Nombre d'arguments.
 * @param argv Tableau d'arguments (modifié en place : options retirées).
 * @return     Le nouveau nombre d'arguments (sans les options reconnues).
 */
int parse_cli_options(int argc, const char *argv[]);
#endif /* app_static_variables_h */
