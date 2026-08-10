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

#include "app/client_config.h"

/** @brief États de l'orchestrateur. */
typedef enum {
    ORCH_WAITING_CONFIG = 0, /**< Aucun fichier de configuration au boot : attente manuelle, jamais de décompte. */
    ORCH_COUNTDOWN,           /**< Configuration chargée au boot : auto-démarrage à T+5 s. */
    ORCH_CONFIGURING,         /**< Une saisie `config <clé> <valeur>` a commencé : décompte annulé DÉFINITIVEMENT. */
    ORCH_RUNNING,             /**< Fils de recherche en cours d'exécution. */
    ORCH_STOPPING,            /**< Réservé pour un futur arrêt/escalade/récolte — déclaré pour la complétude de l'enum. */
    ORCH_APPLYING,            /**< Réservé pour une future application de configuration à chaud — idem. */
    ORCH_EXITING,             /**< Sortie demandée. Aucun driver ne poste encore EV_EXIT. */
} orch_state_t;

/** @brief Événements de l'orchestrateur. */
typedef enum {
    EV_CONFIG_BEGUN = 0, /**< `config <clé> <valeur>` a écrit une valeur valide dans la configuration en préparation. */
    EV_START,             /**< Commande `start`, OU décompte de COUNTDOWN écoulé (même chemin). */
    EV_STOP_FORKS,        /**< Réservé pour une future commande `stopForks`. */
    EV_RESTART,           /**< Réservé pour une future réapplication de configuration nécessitant un redémarrage. */
    EV_EXIT,              /**< Sortie demandée. Non posté par aucun driver actuellement. */
    EV_CHILD_DIED,        /**< Un ou plusieurs slots ont été nettoyés par `reap_dead_child_slots` (observabilité pure, jamais d'auto-respawn). */
} orch_event_t;

/** @brief Code d'erreur d'une transition refusée (`orch_actions_t.error`). */
typedef enum {
    ORCH_OK = 0,
    ORCH_ERR_ALREADY_RUNNING, /**< EV_START alors qu'un cycle de vie est déjà en cours (RUNNING/STOPPING/APPLYING/EXITING). */
    ORCH_ERR_UNSUPPORTED,     /**< EV_STOP_FORKS/EV_RESTART : sémantique pas encore implémentée. */
} orch_error_t;

/** @brief Actions décidées par `orchestrator_step` — jamais d'I/O ici, seulement des drapeaux pour l'appelant. */
typedef struct {
    int spawn_forks;   /**< 1 : l'appelant doit forker (appeler `orchestrator_spawn_forks`). */
    orch_error_t error; /**< ORCH_OK si la transition est acceptée. */
} orch_actions_t;

/** @brief Durée du décompte d'auto-démarrage. */
#define ORCH_COUNTDOWN_MS 5000
/** @brief Cadence du tick de la boucle orchestrateur. */
#define ORCH_TICK_MS 100

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
 * @brief Initialise l'état partagé de l'orchestrateur (`ORCH_COUNTDOWN` si
 *        @p config_loaded_at_boot, sinon `ORCH_WAITING_CONFIG`) — À APPELER
 *        AVANT le lancement de tout thread susceptible de poster un
 *        événement (console, canal de contrôle, HTTP…).
 *
 * Trouvé nécessaire via des tests manuels réels (invisible en local, reproduit
 * de façon fiable sous `make test-docker`, cf. AGENTS.md) : quand cette
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
 * `ORCH_RUNNING`. Ne retourne que lorsque plus aucun fork ne subsiste ET
 * (soit un cycle RUNNING a déjà eu lieu, soit `request == REQUEST_STOP`) —
 * reproduit exactement le contrat de `wait_child()` (sortie sur zéro enfant)
 * tout en couvrant le nouveau cas « jamais démarré, puis Ctrl-C ».
 *
 * @param config_loaded_at_boot Même valeur que celle déjà passée à
 *                               `fork_orchestrator_init_state` — utilisée ici
 *                               uniquement pour décider quel message afficher,
 *                               jamais pour re-toucher l'état.
 */
void fork_orchestrator_run(int config_loaded_at_boot);

#endif /* fork_orchestrator_h */
