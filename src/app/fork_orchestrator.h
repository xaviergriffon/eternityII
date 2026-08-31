#ifndef fork_orchestrator_h
#define fork_orchestrator_h

/*
 * Démarrage différé du client : fork des fils de recherche sur événement
 * (config chargée -> décompte 5s, ou commande `start`), pas au boot.
 *
 * `orchestrator_step` est pur (aucune I/O, aucun fork) ; le driver impur
 * `fork_orchestrator_run` tourne sur le thread parent d'origine et est
 * l'unique thread à appeler `fork()`, sous la quiescence de `fork_gate.h`.
 * `fork_orchestrator_post_event` applique la transition synchronement sous
 * mutex depuis n'importe quel thread (console).
 */

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "app/client_config.h"
#include "app/etii_statistic.h"

/* Déclaration avancée : un `#include "app/etii_client.h"` complet tire
   `scroll(File*, void*)` qui collisionne avec la macro ncurses `scroll(win)`
   (build NCURSES=1, via logger_ncurses.c). Seul un pointeur transite ici. */
struct search_parts;

/** @brief États de l'orchestrateur. */
typedef enum {
    ORCH_WAITING_CONFIG = 0, /**< Aucune config au boot : attente manuelle, jamais de décompte. */
    ORCH_COUNTDOWN,           /**< Config chargée au boot : auto-démarrage à T+5 s. */
    ORCH_CONFIGURING,         /**< `config <clé> <valeur>` saisie : décompte annulé définitivement. */
    ORCH_RUNNING,             /**< Fils de recherche en cours d'exécution. */
    ORCH_STOPPING,            /**< Arrêt/escalade/récolte en cours (`stopForks`, ou `configApply` avant re-fork). */
    ORCH_APPLYING,            /**< Config en préparation appliquée aux tableaux/map, fils déjà arrêtés — suivi d'un re-fork. */
    ORCH_EXITING,             /**< Sortie demandée. Aucun driver ne poste encore EV_EXIT. */
} orch_state_t;

/** @brief Événements de l'orchestrateur. */
typedef enum {
    EV_CONFIG_BEGUN = 0, /**< `config <clé> <valeur>` a écrit une valeur valide en préparation. */
    EV_START,             /**< `start`, décompte écoulé, ou fin de phase APPLYING — même chemin de spawn. */
    EV_STOP_FORKS,        /**< `stopForks` : arrête les fils sans redémarrer. */
    EV_RESTART,           /**< `configApply` nécessitant un redémarrage : arrête puis réapplique/re-forke. */
    EV_EXIT,              /**< Sortie demandée. Non posté par aucun driver actuellement. */
    EV_CHILD_DIED,        /**< Slots nettoyés par `reap_dead_child_slots` (observabilité, jamais d'auto-respawn). */
} orch_event_t;

/** @brief Code d'erreur d'une transition refusée (`orch_actions_t.error`). */
typedef enum {
    ORCH_OK = 0,
    ORCH_ERR_ALREADY_RUNNING, /**< EV_START alors qu'un cycle de vie est déjà en cours. */
    ORCH_ERR_UNSUPPORTED,     /**< Réservé — plus émis, conservé pour compatibilité binaire de l'enum. */
    ORCH_ERR_NOT_RUNNING,     /**< EV_STOP_FORKS/EV_RESTART hors ORCH_RUNNING. */
} orch_error_t;

/** @brief Actions décidées par `orchestrator_step` — jamais d'I/O, seulement des drapeaux. */
typedef struct {
    int spawn_forks;   /**< 1 : l'appelant doit forker (`orchestrator_spawn_forks`). */
    int stop_forks;    /**< 1 : l'appelant doit exécuter l'arrêt/escalade/récolte (RUNNING -> STOPPING). */
    orch_error_t error; /**< ORCH_OK si la transition est acceptée. */
} orch_actions_t;

/** @brief Rôle assigné à un fork de travail (dosage recherche/contrôle par fork). */
typedef enum {
    FORK_ROLE_SEARCH = 0, /**< Cherche (`autosearch`) : demande des possibilités vérifiées, en produit. */
    FORK_ROLE_PRUNE = 1,  /**< Contrôle (`autoprune`) : demande des possibilités non vérifiées, élimine les mortes. */
} fork_role_t;

/**
 * @brief Résout PUREMENT le nombre de forks affectés au contrôle du stock,
 *        borné à `[0, nb_forks]`, à partir de ce qui a été demandé.
 *
 * `pruner_forks_requested < 0` (non demandé) retombe sur le comportement
 * historique : `nb_forks` en mode pruner, `0` en mode client. Une valeur hors
 * `[0, nb_forks]` est clampée plutôt que rejetée.
 */
int resolve_pruner_forks(int pruner_forks_requested, int pruner_mode, int nb_forks);

/**
 * @brief Décide PUREMENT le rôle du fork `fork_seq` parmi `nb_forks`, pour un
 *        dosage `pruner_forks` de forks de contrôle.
 *
 * Les `pruner_forks` forks de plus haut rang sont `FORK_ROLE_PRUNE`, les
 * autres `FORK_ROLE_SEARCH` — convention stable tant que `nb_forks`/
 * `pruner_forks` ne changent pas. Un `fork_seq` hors bornes renvoie
 * `FORK_ROLE_SEARCH` par défaut sûr.
 */
fork_role_t fork_role_for(int fork_seq, int nb_forks, int pruner_forks);

/**
 * @brief Enveloppe impure de `resolve_pruner_forks` + `fork_role_for` : rôle
 *        effectif du fork `fork_seq` du lot en cours, depuis les globales
 *        courantes.
 *
 * Utilisée pour fixer `pruner_mode` par fork avant tout log, et par les
 * points d'affichage diagnostique qui doivent le rôle du fork concerné —
 * jamais la globale `pruner_mode` du parent, plus représentative dès qu'un
 * lot est mixte.
 */
fork_role_t current_fork_role(int fork_seq);

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
 * @brief Délai (ms) après un `ORCH_RUNNING` réussi sans qu'aucun fork ne
 *        rapporte de signal d'activité (stock/analysé/coups-s) avant qu'un
 *        `log_error` (une fois par démarrage) signale la situation.
 *
 * Ne diagnostique pas la cause, seulement le symptôme : après un
 * `start`/`configApply`, tous les indicateurs restent à 0 sans trace.
 */
#define STUCK_FORKS_WARN_MS 30000

/** @brief Prédicat PUR : `STUCK_FORKS_WARN_MS` est-il écoulé depuis `running_since_ms` ? Horloge injectée, testable sans sleep. */
int stuck_forks_threshold_elapsed(long running_since_ms, long now_ms);

/**
 * @brief Prédicat PUR : ce fork rapporte-t-il zéro sur les trois indicateurs
 *        qui comptent (stock en cours, stock analysé, coups/s) ? `stat ==
 *        NULL` renvoie 1 (rien à montrer = suspect).
 *
 * Version par fork de `fork_stats_all_zero` : repère un sous-ensemble de
 * forks bloqués pendant que les autres travaillent, cas où l'agrégat "tous à
 * zéro" ne s'alarme jamais.
 */
int fork_stat_is_zero(const struct client_statistics *stat);

/**
 * @brief Prédicat PUR : tous les forks de `stats` rapportent-ils zéro sur les
 *        trois indicateurs qui comptent ? `nb <= 0` renvoie 1.
 *
 * Conservé pour compatibilité mais supplanté en production par le filet par
 * fork (`fork_stat_is_zero`), seul à détecter un sous-ensemble bloqué.
 */
int fork_stats_all_zero(const struct client_statistics *stats, int nb);

/**
 * @brief Prédicat PUR décidant l'escalade de signal d'arrêt : SIGINT initial
 *        (hors de cette fonction), SIGTERM à +5s si des fils sont encore
 *        vivants, SIGKILL à +10s. Horloge injectée, testable sans sleep.
 */
stop_escalation_action_t stop_escalation_next(long elapsed_ms);

/**
 * @brief Prédicat PUR : millisecondes d'inactivité d'un fils donné, base de
 *        l'escalade d'arrêt PAR FILS plutôt qu'un délai unique pour tout le
 *        lot.
 *
 * Sans ceci un fils encore en train de vider son stock local se voyait
 * interrompu au même instant qu'un fils réellement bloqué. Si
 * `last_activity == 0` (jamais observé), l'inactivité est comptée depuis
 * `escalation_start` — reste soumis à l'escalade normale plutôt que
 * d'en être indéfiniment protégé.
 */
long child_idle_ms(time_t last_activity, time_t escalation_start, time_t now);

/**
 * @brief Formate PUREMENT un résumé diagnostique court du dernier état connu
 *        d'un fils, pour les lignes d'escalade d'arrêt par fils.
 *
 * Sans lui, une ligne d'escalade ne dit que « ce fils est encore vivant » —
 * pas de distinction entre un fils bloqué et un fils occupé sur un état
 * invisible autrement (gros lot d'acquittements en cours de vidage, etc.).
 *
 * @param reported    Vrai si `stat` a réellement été rapporté au moins une
 *                    fois — sinon `stat` peut n'être que des zéros
 *                    d'initialisation, à ne jamais présenter comme réel.
 * @param pruner_mode Bascule entre compteurs de recherche et de pruner.
 */
void fork_diagnostic_summary(const struct client_statistics *stat, int reported,
                              int pruner_mode, char *out, size_t out_size);

/**
 * @brief Interprète PUREMENT un `waitpid(target_pid, &status, WNOHANG)`
 *        ciblé, tel qu'utilisé par la séquence d'arrêt.
 *
 * Le masquage SIGCHLD n'est posé que sur le thread orchestrateur appelant ;
 * un enfant mort pendant l'arrêt peut être moissonné par le handler global
 * sur un autre thread avant que ce `waitpid` explicite n'ait sa chance —
 * auquel cas il renvoie `-1`/`ECHILD` (déjà réclamé), pas `0` (encore
 * vivant). Confondre les deux fait tourner l'arrêt indéfiniment en croyant
 * l'enfant vivant — bogue réel trouvé en test manuel de `configApply`.
 *
 * @return 1 si le pid est mort (réclamé par cet appel, ou déjà réclamé
 *         entre-temps via ECHILD) ; 0 s'il est encore vivant ou en cas
 *         d'erreur transitoire (EINTR…).
 */
int waitpid_target_is_reaped(pid_t waitpid_result, pid_t target_pid, int wait_errno);

/**
 * @brief Transition PURE de la machine à états. Aucune I/O, aucun fork.
 *
 * `now_ms` n'est pas consommé par la table actuelle (le décompte de 5s est
 * suivi par le driver impur, pas par cet état) ; conservé pour un usage
 * futur.
 */
orch_state_t orchestrator_step(orch_state_t s, orch_event_t ev, long now_ms, orch_actions_t *out);

/** @brief Prédicat pur : le décompte défini par `deadline_ms` est-il écoulé à `now_ms` ? */
int orchestrator_countdown_elapsed(long countdown_deadline_ms, long now_ms);

/**
 * @brief Réinitialise tout l'état partagé de l'orchestrateur. Réservé aux
 *        tests unitaires — en production le module vit pour toute la durée
 *        du process.
 */
void fork_orchestrator_reset(void);

/**
 * @brief Applique un événement de façon thread-safe, appelable depuis
 *        n'importe quel thread (aujourd'hui : la console).
 *
 * Prend le verrou partagé, applique `orchestrator_step` immédiatement, réveille
 * l'orchestrateur si un fork est décidé, rend la main avec le résultat exact —
 * donne à `start`/`config` un retour d'erreur immédiat sans latence de sondage.
 */
void fork_orchestrator_post_event(orch_event_t ev, orch_actions_t *out);

/**
 * @brief Lecture thread-safe de l'état courant et du temps restant avant
 *        auto-démarrage (commande console `config`).
 *
 * @param out_countdown_remaining_ms -1 sauf en `ORCH_COUNTDOWN`.
 */
void fork_orchestrator_snapshot(orch_state_t *out_state, long *out_countdown_remaining_ms);

/**
 * @brief Écrit une ligne `clé = valeur` dans la configuration en préparation
 *        et poste `EV_CONFIG_BEGUN` seulement si la ligne a été acceptée —
 *        une ligne invalide ne doit pas annuler le décompte.
 */
client_config_line_status_t fork_orchestrator_stage_config_line(const char *line);

/**
 * @brief Formate la configuration en préparation dans `out`, sous verrou.
 *        Utilisée par la commande console `config`.
 */
int fork_orchestrator_format_staged_config(char *out, size_t out_size);

/**
 * @brief Superpose la configuration en préparation sur `out` (déjà rempli) :
 *        chaque clé staged écrase la valeur effective correspondante.
 *
 * Sans ceci, une valeur préparée par `config <clé> <valeur>` puis écrite par
 * `configSave` était silencieusement perdue. Pour une prise d'effet
 * immédiate sans redémarrer, voir `fork_orchestrator_apply_staged_config`.
 */
void fork_orchestrator_merge_staged_config(client_config_t *out);

/**
 * @brief Applique immédiatement la configuration en préparation aux
 *        globales en vigueur, sans attendre un redémarrage du process.
 *
 * Appelée juste avant tout fork effectif (`start` ou décompte) pour que
 * `config` suivi de `start` prenne effet sans redémarrer l'exécutable.
 * Distinct de `fork_orchestrator_merge_staged_config` (fichier consommé au
 * prochain démarrage) : les deux chemins coexistent.
 */
void fork_orchestrator_apply_staged_config(void);

/**
 * @brief Compare la configuration effective à la configuration en
 *        préparation — utilisée par `configApply` pour décider entre
 *        diffusion IPC seule et redémarrage complet.
 */
client_config_diff_t fork_orchestrator_diff_staged_config(const client_config_t *effective);

/**
 * @brief La configuration en préparation, une fois appliquée, violerait-elle
 *        `gpu_pruner_forks_conflict` ?
 *
 * Le garde-fou `--gpu` + `--pruner-forks != nb_forks` de `handle_client`
 * n'est évalué qu'une fois, avant le premier fork — jamais réévalué sur le
 * chemin de reconfiguration à chaud. Sans ce test, un client `pruner --gpu`
 * peut re-forker silencieusement dans un état que le garde-fou de démarrage
 * rend impossible.
 *
 * @return 1 si l'application créerait le conflit, 0 sinon (toujours 0 sur un
 *         build sans CUDA).
 */
int fork_orchestrator_staged_gpu_pruner_conflict(void);

/**
 * @brief Cœur testable de `fork_orchestrator_staged_gpu_pruner_conflict`,
 *        avec `gpu_pruner_mode` injecté plutôt que lu sur la globale (qui
 *        n'existe pas hors build `WITH_CUDA`).
 */
int fork_orchestrator_staged_gpu_pruner_conflict_for(int gpu_pruner_mode_value);

/**
 * @brief Applique la configuration en préparation aux globales du parent
 *        puis diffuse aux fils déjà en cours d'exécution, par IPC, les clés
 *        à chaud effectivement stagées (`maxStockByThread`/`limit`/
 *        `prunerBatch`).
 *
 * Réservée à la branche HOT_ONLY de `configApply` (aucune clé nécessitant un
 * redémarrage n'est stagée). Sans effet sur `nb_forks`/`server_host`/
 * `parts_file`, exclus par construction du chemin HOT_ONLY.
 */
void fork_orchestrator_apply_hot_staged_config(void);

/**
 * @brief Fork réel des `NB_THREADS` process de recherche.
 *
 * Quiescence coopérative prise/relâchée autour de chaque `fork()`
 * individuellement (pas une seule fois pour toute la boucle), puisque les
 * threads du parent tournent déjà — une section critique élargie à toute la
 * boucle auto-interbloquerait le thread forkeur dès qu'un log de bilan
 * reprend le verrou de sortie non récursif qu'il détient déjà.
 *
 * Un échec de quiescence pour un slot (timeout ~2s, jamais de fork dans le
 * doute) est traité comme un échec de fork ordinaire.
 *
 * @return Le nombre de process créés (>= 0).
 */
int orchestrator_spawn_forks(void);

/**
 * @brief Travail d'APPLYING : appelée une fois `orchestrator_do_stop_forks`
 *        revenue (zéro fils vivant) — applique la configuration en
 *        préparation puis, seulement pour les clés qui ont changé,
 *        reconstruit les tableaux de fils (`nb_forks`) et/ou la map de
 *        recherche partagée COW (`parts_file`).
 *
 * Protégée par la quiescence coopérative : sans elle, un lecteur concurrent
 * (checker, `server_tcp`, console…) peut déréférencer un pointeur libéré
 * pendant la reconstruction — observé en pratique comme un crash console
 * sous NCURSES=1. Sur timeout, ne modifie rien et renvoie 0 — l'appelant
 * retombe en `ORCH_WAITING_CONFIG`.
 *
 * @param shared_parts NULL : aucune reconstruction de map n'est tentée, même
 *                     si `parts_file` a changé (mode dégradé défensif).
 * @return 1 si la reconstruction a eu lieu, 0 si refusée (timeout).
 */
int orchestrator_apply_restart_config(struct search_parts *shared_parts);

/**
 * @brief Initialise l'état partagé de l'orchestrateur (`ORCH_COUNTDOWN` si
 *        @p config_loaded_at_boot, sinon `ORCH_WAITING_CONFIG`) — à appeler
 *        avant le lancement de tout thread susceptible de poster un
 *        événement (console, canal de contrôle, HTTP…).
 *
 * Doit rester appelée pendant que le process est encore mono-thread : quand
 * cette init faisait partie de `fork_orchestrator_run` (après le lancement
 * du thread console), un `start` tapé assez vite gagnait la course contre
 * l'écrasement sans condition de l'état partagé, annulant silencieusement le
 * `start` — invisible sur macOS, reproduit à chaque run sous
 * `make test-docker` (plus chargé, ordonnancé différemment).
 */
void fork_orchestrator_init_state(int config_loaded_at_boot);

/**
 * @brief Boucle principale de l'orchestrateur — remplace `wait_child()` dans
 *        `handle_client`. Tourne sur le thread parent d'origine.
 *
 * Prérequis : `fork_orchestrator_init_state` déjà appelée. Réveil sur
 * événement posté ou timeout (tick `ORCH_TICK_MS`) ; décompte en
 * `ORCH_COUNTDOWN` ; spawn effectif sur décision ; nettoyage des slots morts
 * en `ORCH_RUNNING`. En `ORCH_STOPPING` : arrêt/escalade/récolte, puis retour
 * en `ORCH_WAITING_CONFIG` (arrêt simple) ou passage en `ORCH_APPLYING`
 * (redémarrage à chaud) suivi d'un re-fork via le même chemin `EV_START`
 * qu'un `start` manuel. Ne retourne que quand plus aucun fork ne subsiste ET
 * (un cycle RUNNING a déjà eu lieu, ou `request == REQUEST_STOP`).
 *
 * @param shared_parts Pièces de recherche partagées COW, construites et
 *                     publiées par `handle_client` avant l'appel ; propriété
 *                     de l'allocation reste au caller, mais sa reconstruction
 *                     sur changement de `parts_file` est déléguée ici.
 */
void fork_orchestrator_run(int config_loaded_at_boot, struct search_parts *shared_parts);

/**
 * @brief Journalise dans `events.log` (jamais sur la console) un instantané
 *        de la configuration effective, juste après un démarrage réussi des
 *        fils de recherche.
 *
 * Exposée (non `static`) pour être testable sans fork réel.
 */
void log_startup_diagnostics(int nb_created);

#endif /* fork_orchestrator_h */
