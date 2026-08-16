#ifndef fork_orchestrator_h
#define fork_orchestrator_h

/*
 * Orchestrateur de démarrage différé du client.
 *
 * `handle_client` (src/app/main.c) ne forke plus ses NB_THREADS fils de
 * recherche IMMÉDIATEMENT au démarrage : le fork est un événement DIFFÉRÉ.
 * Au boot, soit un fichier de configuration existe (chargé par
 * `client_config_load`) et un décompte de 5 s démarre l'auto-fork, soit
 * aucun fichier n'existe et le process attend une commande console
 * (`start`, ou `config <clé> <valeur>` qui annule le décompte).
 *
 * Cœur PUR et testable (`orchestrator_step`) : aucune I/O, aucun fork, une
 * simple table de transition (état, événement) -> (nouvel état, actions).
 * Autour, un driver IMPUR (`fork_orchestrator_run`) tourne sur le thread
 * PARENT d'origine (celui qui bloquait autrefois dans `wait_child()`) et est
 * l'UNIQUE thread qui appelle jamais `fork()` — via `orchestrator_spawn_forks`,
 * protégé par l'infrastructure de quiescence coopérative de
 * `src/app/fork_gate.h` puisque les threads du parent (checker, réception
 * stats, canal de contrôle, console) tournent désormais AVANT le fork.
 *
 * `fork_orchestrator_post_event` est appelable depuis n'importe quel thread
 * (aujourd'hui : la console uniquement) : elle applique la transition pure
 * SYNCHRONEMENT sous un mutex partagé et réveille le thread orchestrateur si
 * un fork est décidé — une alternative plus simple qu'une file bornée
 * d'événements opaques, puisque chaque événement est un changement d'état
 * idempotent, pas un travail à empiler.
 */

#include <stddef.h>
#include <sys/types.h>

#include "app/client_config.h"
#include "app/etii_statistic.h"

/* Déclaration avancée plutôt qu'un `#include "app/etii_client.h"` complet :
   ce dernier tire (via core/possibility.h -> core/lifo.h) une déclaration
   `int scroll(File *, void *)` qui entre en collision avec la macro
   `scroll(win)` de <ncurses.h> quand ce header est inclus, transitivement,
   par src/ui/logger_ncurses.c (build NCURSES=1) — trouvé via un échec de
   compilation réel (`too many arguments provided to function-like macro
   invocation`). `struct search_parts` est un type opaque ici : seul un
   pointeur y transite (`fork_orchestrator_run`), jamais un accès à ses
   champs, donc une déclaration avancée suffit. La définition complète
   (`typedef struct search_parts { ... } search_parts_t`, src/app/etii_client.h)
   reste l'unique source de vérité du contenu du type. */
struct search_parts;

/** @brief États de l'orchestrateur. */
typedef enum {
    ORCH_WAITING_CONFIG = 0, /**< Aucun fichier de configuration au boot : attente manuelle, jamais de décompte. */
    ORCH_COUNTDOWN,           /**< Configuration chargée au boot : auto-démarrage à T+5 s. */
    ORCH_CONFIGURING,         /**< Une saisie `config <clé> <valeur>` a commencé : décompte annulé DÉFINITIVEMENT. */
    ORCH_RUNNING,             /**< Fils de recherche en cours d'exécution. */
    ORCH_STOPPING,            /**< Arrêt/escalade/récolte des fils en cours (`stopForks`, ou `configApply` NEEDS_RESTART avant re-fork). */
    ORCH_APPLYING,            /**< Application de la configuration en préparation aux tableaux/à la map, fils déjà tous arrêtés — suivie d'un re-fork (`configApply` uniquement). */
    ORCH_EXITING,             /**< Sortie demandée. Aucun driver ne poste encore EV_EXIT. */
} orch_state_t;

/** @brief Événements de l'orchestrateur. */
typedef enum {
    EV_CONFIG_BEGUN = 0, /**< `config <clé> <valeur>` a écrit une valeur valide dans la configuration en préparation. */
    EV_START,             /**< Commande `start`, OU décompte de COUNTDOWN écoulé, OU fin de la phase APPLYING d'un `configApply` (même chemin de spawn dans les trois cas). */
    EV_STOP_FORKS,        /**< Commande `stopForks` : arrête les fils sans redémarrer. */
    EV_RESTART,           /**< `configApply` avec un changement nécessitant un redémarrage (cf. `client_config_diff`) : arrête les fils puis réapplique/re-forke. */
    EV_EXIT,              /**< Sortie demandée. Non posté par aucun driver actuellement. */
    EV_CHILD_DIED,        /**< Un ou plusieurs slots ont été nettoyés par `reap_dead_child_slots` (observabilité pure, jamais d'auto-respawn). */
} orch_event_t;

/** @brief Code d'erreur d'une transition refusée (`orch_actions_t.error`). */
typedef enum {
    ORCH_OK = 0,
    ORCH_ERR_ALREADY_RUNNING, /**< EV_START alors qu'un cycle de vie est déjà en cours (RUNNING/STOPPING/EXITING). */
    ORCH_ERR_UNSUPPORTED,     /**< Réservé — plus émis actuellement, conservé pour compatibilité binaire de l'enum. */
    ORCH_ERR_NOT_RUNNING,     /**< EV_STOP_FORKS/EV_RESTART alors qu'aucun fils n'est en cours d'exécution (hors ORCH_RUNNING). */
} orch_error_t;

/** @brief Actions décidées par `orchestrator_step` — jamais d'I/O ici, seulement des drapeaux pour l'appelant. */
typedef struct {
    int spawn_forks;   /**< 1 : l'appelant doit forker (appeler `orchestrator_spawn_forks`). */
    int stop_forks;    /**< 1 : l'appelant doit exécuter la séquence d'arrêt/escalade/récolte (RUNNING -> STOPPING). */
    orch_error_t error; /**< ORCH_OK si la transition est acceptée. */
} orch_actions_t;

/** @brief Durée du décompte d'auto-démarrage. */
#define ORCH_COUNTDOWN_MS 5000
/** @brief Cadence du tick de la boucle orchestrateur. */
#define ORCH_TICK_MS 100

/** @brief Action décidée par `stop_escalation_next` pour l'arrêt d'un lot de fils. */
typedef enum {
    STOP_ESCALATION_NONE = 0,   /**< Continuer d'attendre, aucun nouveau signal à envoyer. */
    STOP_ESCALATION_SIGTERM,    /**< Escalader vers SIGTERM (délai initial dépassé). */
    STOP_ESCALATION_SIGKILL,    /**< Escalader vers SIGKILL (délai SIGTERM dépassé). */
} stop_escalation_action_t;

/** @brief Délai (ms) après le SIGINT initial avant escalade SIGTERM. */
#define STOP_ESCALATION_SIGTERM_MS 5000
/** @brief Délai (ms) après le SIGINT initial avant escalade SIGKILL. */
#define STOP_ESCALATION_SIGKILL_MS 10000

/**
 * @brief Délai (ms) après un démarrage/redémarrage réussi (ORCH_RUNNING) sans
 *        qu'AUCUN fork ne rapporte le moindre indicateur (stock, analysé,
 *        coups/s) avant que l'orchestrateur ne signale la situation comme
 *        suspecte (`log_error`, une seule fois par démarrage).
 *
 * Ne diagnostique jamais LA cause (connexion serveur en échec, stock serveur
 * vide, fork bloqué avant sa première recherche…) — seulement le SYMPTÔME
 * rapporté à plusieurs reprises par l'exploitant : après un `start`/`config
 * Apply`, tous les indicateurs restent obstinément à 0 sans qu'aucune trace
 * ne permette de comprendre pourquoi. Combiné aux logs de connexion déjà
 * inconditionnels (`check_and_connect_to_server`, `core/datamanager.c`) et au
 * drainage des morts d'enfants (`app_runtime.h`), ce filet de sécurité couvre
 * le cas restant : des forks vivants, correctement connectés ou non, mais qui
 * ne produisent tout simplement rien.
 */
#define STUCK_FORKS_WARN_MS 30000

/**
 * @brief Prédicat PUR : le délai `STUCK_FORKS_WARN_MS` est-il écoulé depuis
 *        `running_since_ms` ? Même convention que `orchestrator_countdown_elapsed`
 *        (horloge injectée, jamais lue directement — testable sans `sleep`).
 */
int stuck_forks_threshold_elapsed(long running_since_ms, long now_ms);

/**
 * @brief Prédicat PUR : ce fork rapporte-t-il zéro sur les trois indicateurs
 *        qui comptent pour l'exploitant (stock en cours, stock analysé,
 *        coups/s) ? `stat == NULL` renvoie 1 (rien à montrer = suspect).
 *
 * Version PAR FORK de `fork_stats_all_zero` : indispensable pour repérer un
 * sous-ensemble de forks bloqués pendant que les autres travaillent
 * normalement — l'agrégat "tous à zéro" ne s'alarme jamais dans ce cas,
 * exactement le cas trouvé en conditions réelles (voir
 * `g_stuck_fork_warned`, `src/app/fork_orchestrator.c`).
 */
int fork_stat_is_zero(const struct client_statistics *stat);

/**
 * @brief Prédicat PUR : tous les forks de `stats` (tableau de taille `nb`)
 *        rapportent-ils zéro sur les trois indicateurs qui comptent pour
 *        l'exploitant (stock en cours, stock analysé, coups/s) ?
 *
 * `nb <= 0` renvoie 1 (« rien à montrer » compte comme suspect, ne bloque
 * jamais la détection sur un NB_THREADS mal lu). Conservé pour compatibilité
 * (et testé indépendamment) mais SUPPLANTÉ en production par le filet par
 * fork (`fork_stat_is_zero` + `g_stuck_fork_warned`) : un agrégat "tous à
 * zéro" ne détecte jamais un sous-ensemble de forks bloqués pendant que les
 * autres travaillent — voir le correctif documenté dans
 * docs/echanges_client_serveur.md.
 */
int fork_stats_all_zero(const struct client_statistics *stats, int nb);

/**
 * @brief Prédicat PUR décidant l'escalade de signal d'arrêt de la séquence
 *        de redémarrage à chaud :
 *        SIGINT initial (envoyé par l'appelant, hors de cette fonction), puis
 *        SIGTERM à +5 s si des fils sont toujours vivants, puis SIGKILL à
 *        +10 s. Ne dépend que d'une horloge injectée (jamais `time()`
 *        lui-même) : testable sans "sleep" réel.
 *
 * @param elapsed_ms Millisecondes écoulées depuis l'envoi du SIGINT initial.
 * @return           `STOP_ESCALATION_NONE` avant 5 s, `STOP_ESCALATION_SIGTERM`
 *                    entre 5 s et 10 s, `STOP_ESCALATION_SIGKILL` à partir de 10 s.
 */
stop_escalation_action_t stop_escalation_next(long elapsed_ms);

/**
 * @brief Interprète PUREMENT le résultat d'un `waitpid(target_pid, &status,
 *        WNOHANG)` ciblé, tel qu'utilisé par la séquence d'arrêt
 *        (`orchestrator_do_stop_forks`, `src/app/fork_orchestrator.c`).
 *
 * `sigchld_handler` (`src/app/app_runtime.c`) installe un gestionnaire
 * PROCESS-WIDE sur SIGCHLD, mais le MASQUAGE de ce signal
 * (`pthread_sigmask`, D2/risque #2) n'est posé que sur le thread
 * orchestrateur appelant — les autres threads du parent (checker,
 * `server_tcp`, canal de contrôle, console) ne le bloquent PAS. Un enfant qui
 * meurt pendant la séquence d'arrêt peut donc être moissonné par
 * `sigchld_handler` sur N'IMPORTE LEQUEL de ces autres threads (via son
 * propre `waitpid(-1, …, WNOHANG)`) AVANT que le `waitpid(pid, …)` explicite
 * ci-dessous n'ait sa chance — auquel cas ce dernier renvoie `-1`/`ECHILD`
 * (« plus mon enfant », déjà réclamé), pas `0` (« encore vivant, rien à
 * signaler pour l'instant »). Confondre ces deux cas fait tourner la
 * séquence d'arrêt INDÉFINIMENT en croyant l'enfant toujours vivant alors
 * qu'il est mort depuis longtemps — bogue réel trouvé en testant
 * manuellement `configApply` (état `STOPPING` qui ne se résorbait jamais,
 * même après l'escalade SIGKILL).
 *
 * @param waitpid_result  Valeur de retour de `waitpid`.
 * @param target_pid      Le pid explicitement attendu (2ᵉ argument passé à `waitpid`).
 * @param wait_errno       `errno` immédiatement après l'appel à `waitpid`
 *                         (capturé par l'appelant — cette fonction ne touche
 *                         jamais `errno` elle-même, pour rester pure/testable).
 * @return 1 si le pid est mort — soit réclamé par CET appel
 *         (`waitpid_result == target_pid`), soit déjà réclamé ENTRE-TEMPS par
 *         un autre thread (`waitpid_result == -1 && wait_errno == ECHILD`,
 *         ce qui compte comme mort aussi) ; 0 s'il est encore vivant
 *         (`WNOHANG` n'a rien trouvé, `waitpid_result == 0`) ou en cas
 *         d'erreur transitoire (`EINTR`, …) — l'appelant retentera au tour
 *         suivant.
 */
int waitpid_target_is_reaped(pid_t waitpid_result, pid_t target_pid, int wait_errno);

/**
 * @brief Transition PURE de la machine à états. Aucune I/O, aucun fork.
 *
 * `now_ms` n'est pas consommé par la table de transition actuelle (le
 * décompte de 5 s est une deadline suivie par le DRIVER impur, pas par cet
 * état — cf. `orchestrator_countdown_elapsed`) ; conservé pour un usage
 * futur (horodatage d'une transition STOPPING/APPLYING).
 *
 * @param s      État courant.
 * @param ev     Événement reçu.
 * @param now_ms Horloge courante en millisecondes (réservé, cf. ci-dessus).
 * @param out    Sortie (jamais NULL déréférencé : NULL accepté, ignoré).
 * @return       Le nouvel état.
 */
orch_state_t orchestrator_step(orch_state_t s, orch_event_t ev, long now_ms, orch_actions_t *out);

/** @brief Prédicat pur : le décompte défini par `deadline_ms` est-il écoulé à `now_ms` ? */
int orchestrator_countdown_elapsed(long countdown_deadline_ms, long now_ms);

/**
 * @brief Réinitialise tout l'état partagé de l'orchestrateur (état, config en
 *        préparation, drapeau de fork en attente). Réservé aux tests
 *        unitaires — en production le module vit pour toute la durée du
 *        process (initialisé par `fork_orchestrator_run`).
 */
void fork_orchestrator_reset(void);

/**
 * @brief Applique un événement de façon thread-safe, appelable depuis
 *        N'IMPORTE QUEL thread (aujourd'hui : la console).
 *
 * Prend le verrou partagé, applique `orchestrator_step` immédiatement,
 * met à jour l'état partagé, réveille le thread orchestrateur si un fork est
 * décidé, puis rend la main avec le résultat exact — c'est ce qui donne à
 * `start`/`config <clé> <valeur>` un retour d'erreur immédiat et correct
 * (« déjà en cours d'exécution ») sans latence de sondage.
 *
 * @param ev  Événement à appliquer.
 * @param out Sortie (NULL accepté si l'appelant ne veut pas le résultat).
 */
void fork_orchestrator_post_event(orch_event_t ev, orch_actions_t *out);

/**
 * @brief Lecture thread-safe de l'état courant et du temps restant avant
 *        auto-démarrage (utilisée par la commande console `config`).
 *
 * @param out_state                  Sortie (NULL accepté) : état courant.
 * @param out_countdown_remaining_ms Sortie (NULL accepté) : millisecondes
 *                                   restantes si l'état est `ORCH_COUNTDOWN`,
 *                                   -1 sinon.
 */
void fork_orchestrator_snapshot(orch_state_t *out_state, long *out_countdown_remaining_ms);

/**
 * @brief Écrit une ligne `clé = valeur` dans la configuration "en
 *        préparation" (réutilise `client_config_parse_line`, jamais de
 *        logique de validation dupliquée) et poste `EV_CONFIG_BEGUN`
 *        SEULEMENT si la ligne a été acceptée (`CLIENT_CONFIG_LINE_SET`) —
 *        une ligne invalide ne doit pas annuler le décompte.
 *
 * @param line Ligne au format `clé = valeur` (même format que le fichier de
 *             configuration, cf. `client_config_parse_line`).
 * @return     Le statut de parsing, pour que l'appelant (l'interpréteur
 *             console) puisse rapporter une erreur précise.
 */
client_config_line_status_t fork_orchestrator_stage_config_line(const char *line);

/**
 * @brief Formate la configuration "en préparation" courante dans `out`, sous
 *        verrou (jamais de pointeur brut exposé vers l'état partagé — cf.
 *        `client_config_format`). Utilisée par la commande console `config`.
 *
 * @return Comme `client_config_format` : octets écrits, ou -1 si tronqué.
 */
int fork_orchestrator_format_staged_config(char *out, size_t out_size);

/**
 * @brief Superpose la configuration "en préparation" sur `out` (déjà rempli,
 *        typiquement par `client_config_capture_effective`) : chaque clé
 *        présente côté staged écrase la valeur effective correspondante.
 *
 * Sans ceci, une valeur préparée par `config <clé> <valeur>` puis écrite par
 * `configSave` était silencieusement perdue — `configSave` ne capturait que
 * l'EFFECTIVE, jamais la configuration en préparation, donc un
 * `config nb_forks 8` suivi de `configSave` puis d'un redémarrage du
 * process ne changeait jamais rien. Appelée par `configSave` avant
 * l'écriture, pour que ce qui a été préparé finisse bien par prendre effet
 * au prochain démarrage du process (redémarrage, cf. `--config-file`). Pour
 * une prise d'effet immédiate SANS redémarrer, voir
 * `fork_orchestrator_apply_staged_config`.
 *
 * @param out Configuration à mettre à jour en place (chaînes déjà présentes
 *            libérées avant remplacement si la clé est staged).
 */
void fork_orchestrator_merge_staged_config(client_config_t *out);

/**
 * @brief Applique IMMÉDIATEMENT la configuration "en préparation" aux
 *        globales en vigueur (`client_config_apply_direct`), sans attendre
 *        un redémarrage du process.
 *
 * Appelée par le driver (`fork_orchestrator_run`) juste avant tout fork
 * effectif — `start` manuel ou décompte auto-déclenché, le même point de
 * code pour les deux — pour que `config <clé> <valeur>` suivi de `start`
 * (ou d'un décompte qui va à son terme) prenne effet sur les process de
 * recherche qui vont être lancés, sans nécessiter de redémarrer l'exécutable.
 * Ceci est distinct de `fork_orchestrator_merge_staged_config` (qui vise le
 * fichier écrit par `configSave`, consommé au PROCHAIN démarrage) : les deux
 * chemins existent en parallèle, aucun n'est un raccourci vers l'autre.
 */
void fork_orchestrator_apply_staged_config(void);

/**
 * @brief Compare la configuration EFFECTIVE @p effective à la configuration
 *        EN PRÉPARATION courante (`client_config_diff`, sous verrou) — utilisée
 *        par `configApply` pour décider entre diffusion IPC seule et
 *        redémarrage complet.
 *
 * @param effective Configuration effective déjà capturée par l'appelant
 *                   (`client_config_capture_effective`).
 * @return           Le verdict de `client_config_diff`.
 */
client_config_diff_t fork_orchestrator_diff_staged_config(const client_config_t *effective);

/**
 * @brief Applique la configuration en préparation aux globales du PARENT
 *        (`client_config_apply_direct`) PUIS diffuse aux process fils déjà en
 *        cours d'exécution, par IPC, les seules clés à chaud effectivement
 *        stagées (`maxStockByThread`/`limit`/`prunerBatch`) — même mécanisme
 *        que les commandes console homonymes (`send_command_to_childs`).
 *
 * Réservée à la branche `HOT_ONLY` de `configApply`
 * (`fork_orchestrator_diff_staged_config` ayant déjà garanti qu'aucune clé
 * nécessitant un redémarrage n'est stagée). Sans effet sur `nb_forks`/
 * `server_host`/`parts_file`, qui ne peuvent être stagées ici puisque
 * `HOT_ONLY` l'exclut par construction — mais recopiées quand même vers les
 * globales si présentes, par simple réutilisation de
 * `fork_orchestrator_apply_staged_config` (no-op dans ce cas précis).
 */
void fork_orchestrator_apply_hot_staged_config(void);

/**
 * @brief Fork réel des `NB_THREADS` process de recherche — corps de la
 *        boucle historique de `handle_client` (src/app/main.c), déplacé ici
 *        À L'IDENTIQUE, avec la quiescence coopérative prise/relâchée AUTOUR
 *        DE CHAQUE `fork()` PRIS INDIVIDUELLEMENT (pas une seule fois pour toute
 *        la boucle) puisque les threads du parent tournent déjà :
 *        `fork_gate_request_quiesce` + `fork_gate_acquire_io_locks`
 *        immédiatement avant chaque `fork()`, `fork_gate_release_io_locks`/
 *        `_release_quiesce` immédiatement après (dans les DEUX branches,
 *        avant tout `log_info`/`log_error` de bilan) — une section critique
 *        élargie à toute la boucle auto-interbloquerait le thread forkeur
 *        dès qu'un message diagnostique (erreur de fork, ligne
 *        `DEBUG_THREAD`) reprend le verrou de sortie du logger, non
 *        récursif, qu'il détient déjà.
 *
 * Un échec de quiescence pour UN slot donné (timeout ~2 s, jamais de fork
 * dans le doute) est traité comme un échec de fork ordinaire : compté dans
 * le même compteur d'échecs (abandon après 10), le slot suivant est
 * retenté.
 *
 * Sur succès (`created > 0`) : met à jour `g_active_forks` et appelle
 * `control_channel_request_reconnect()`.
 *
 * @return Le nombre de process créés (>= 0, comme `count_created_forks` —
 *         0 si aucun, y compris si la quiescence a échoué pour tous les
 *         slots tentés).
 */
int orchestrator_spawn_forks(void);

/**
 * @brief Travail d'APPLYING (ORCH_APPLYING) : appelée par le driver
 *        uniquement une fois `orchestrator_do_stop_forks` revenue, donc zéro
 *        fils vivant — applique la configuration en préparation
 *        (`fork_orchestrator_apply_staged_config`), puis, SEULEMENT pour les
 *        clés qui ont réellement changé, reconstruit `childrens_pid`/
 *        `forkId`/`fork_statistics` (`nb_forks`) et/ou la map de recherche
 *        partagée COW (`parts_file`).
 *
 * Protégée par la quiescence coopérative (`fork_gate_request_quiesce`,
 * budget `FORK_GATE_DEFAULT_TIMEOUT_MS`) : sans elle, un lecteur concurrent
 * de ces tableaux (checker, `server_tcp`, canal de contrôle, console) peut
 * déréférencer un pointeur libéré pendant la fenêtre de reconstruction —
 * observé en pratique comme un crash réel du thread console sous
 * `NCURSES=1` et, plus discrètement en mode ANSI, comme une configuration
 * qui « ne semble pas prise en compte ». Jamais dans le doute : sur timeout,
 * la fonction ne modifie RIEN (configuration, tableaux, map inchangés) et
 * renvoie 0 — l'appelant retombe en `ORCH_WAITING_CONFIG`, fils déjà arrêtés,
 * en attente d'un nouveau `configApply`/`start`.
 *
 * Exposée (non `static`) pour être testable directement : voir
 * `apply_restart_config_quiesces_concurrent_array_readers`
 * (tests/app/test_fork_orchestrator.c), qui prouve par construction (un
 * thread compagnon en boucle serrée sur `fork_gate_checkpoint` ne peut par
 * définition jamais s'exécuter pendant que la quiescence est active) que
 * cette fenêtre est bien protégée.
 *
 * @param shared_parts Pièces de recherche partagées, ou NULL (aucune
 *                      reconstruction de map n'est alors tentée, même si
 *                      `parts_file` a changé — mode dégradé défensif, ne
 *                      devrait pas se produire en production côté client).
 * @return 1 si la reconstruction a eu lieu (quiescence atteinte), 0 si elle
 *         a été refusée (timeout).
 */
int orchestrator_apply_restart_config(struct search_parts *shared_parts);

/**
 * @brief Initialise l'état partagé de l'orchestrateur (`ORCH_COUNTDOWN` si
 *        @p config_loaded_at_boot, sinon `ORCH_WAITING_CONFIG`) — À APPELER
 *        AVANT le lancement de tout thread susceptible de poster un
 *        événement (console, canal de contrôle, HTTP…).
 *
 * Trouvé nécessaire via des tests manuels réels (invisible en local, reproduit
 * de façon fiable sous `make test-docker`) : quand cette
 * initialisation faisait partie de `fork_orchestrator_run` elle-même — appelée
 * APRÈS le lancement du thread console dans `handle_client` — un opérateur (ou
 * un banc de test piloté par FIFO) tapant `start` assez vite gagnait la course
 * : la console postait `EV_START` (état `RUNNING`, fork en attente) avant que
 * `fork_orchestrator_run` n'ait fini d'écraser SANS CONDITION l'état partagé
 * avec ses valeurs de démarrage — annulant silencieusement le `start`
 * (`g_orch_pending_spawn` remis à 0, état ramené à `WAITING_CONFIG`), sans
 * aucune erreur observable. Sur macOS la fenêtre de course est assez étroite
 * pour ne quasiment jamais se déclencher ; le conteneur Linux/gcc de
 * `make test-docker`, plus chargé et ordonnancé différemment, la reproduit à
 * chaque exécution de `run_solution_16.sh`. La correction structurelle est de
 * ne plus jamais réinitialiser cet état après le lancement d'un thread
 * concurrent : `fork_orchestrator_init_state` est donc désormais appelée par
 * `handle_client` (`src/app/main.c`) AVANT `run_server_thread`/`run_checker`/
 * `run_console`/`start_control_channel`, pendant que le process est encore
 * mono-thread — plus aucune course possible par construction.
 *
 * @param config_loaded_at_boot Statut de `client_config_load` au démarrage
 *                               de `handle_client` (1 = un fichier a été
 *                               chargé, 0 = absent/illisible).
 */
void fork_orchestrator_init_state(int config_loaded_at_boot);

/**
 * @brief Boucle principale de l'orchestrateur — remplace `wait_child()` dans
 *        `handle_client`. Tourne sur le thread PARENT d'origine.
 *
 * PRÉREQUIS : `fork_orchestrator_init_state(config_loaded_at_boot)` doit déjà
 * avoir été appelée (voir sa doc) — cette fonction ne touche plus l'état
 * partagé elle-même, seulement le journal (affichage de la configuration
 * effective si @p config_loaded_at_boot) puis la boucle : réveil sur
 * événement posté ou sur timeout (tick `ORCH_TICK_MS`), décompte (log une
 * fois par seconde, auto-`EV_START` à échéance), spawn effectif sur décision,
 * nettoyage des slots morts (`reap_dead_child_slots`) et `EV_CHILD_DIED` en
 * `ORCH_RUNNING` ; en `ORCH_STOPPING`, exécute la séquence d'arrêt/escalade/
 * récolte (SIGCHLD masqué, SIGINT puis escalade `stop_escalation_next`), puis
 * soit revient en `ORCH_WAITING_CONFIG` (arrêt simple, `stopForks`), soit
 * passe en `ORCH_APPLYING` (redémarrage à chaud, `configApply` NEEDS_RESTART :
 * reconstruction des tableaux de fils si `nb_forks` a changé, de la map de
 * recherche partagée si `parts_file` a changé) puis re-fork via le MÊME
 * chemin `EV_START` qu'un `start` manuel. Ne retourne que lorsque plus aucun
 * fork ne subsiste ET
 * (soit un cycle RUNNING a déjà eu lieu, soit `request == REQUEST_STOP`) —
 * reproduit exactement le contrat de `wait_child()` (sortie sur zéro enfant)
 * tout en couvrant le nouveau cas « jamais démarré, puis Ctrl-C ».
 *
 * @param config_loaded_at_boot Même valeur que celle déjà passée à
 *                               `fork_orchestrator_init_state` — utilisée ici
 *                               uniquement pour décider quel message afficher,
 *                               jamais pour re-toucher l'état.
 * @param shared_parts           Pièces de recherche partagées COW, construites
 *                               et publiées par `handle_client` AVANT l'appel
 *                               (`build_search_parts` + `set_inherited_search_parts`).
 *                               La PROPRIÉTÉ de l'allocation reste à
 *                               `handle_client` (qui la libère après le retour
 *                               de cette fonction), mais la RECONSTRUCTION sur
 *                               changement de `parts_file` est déléguée à
 *                               l'orchestrateur (`ORCH_APPLYING`), seul à
 *                               savoir quand plus aucun fils n'est vivant.
 */
void fork_orchestrator_run(int config_loaded_at_boot, struct search_parts *shared_parts);

#endif /* fork_orchestrator_h */
