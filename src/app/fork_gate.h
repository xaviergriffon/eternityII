#ifndef fork_gate_h
#define fork_gate_h

/*
 * Infrastructure de quiescence coopérative.
 *
 * Objectif : pouvoir forker de nouveaux process de recherche alors que les
 * threads du process PARENT (checker, réception IPC des forks, canal de
 * contrôle, console) tournent déjà — ce qui viole autrement la règle
 * « aucun thread du parent ne doit tourner pendant fork() »
 * (src/app/main.c) : un thread qui détient un verrou stdio/logger au moment
 * du fork le transmet verrouillé à l'enfant, qui n'a personne pour le
 * relâcher (blocage définitif au premier printf/malloc).
 *
 * Solution retenue : chaque thread candidat s'ENREGISTRE une fois puis
 * appelle `fork_gate_checkpoint` en tête de chaque tour de sa boucle. Tant
 * qu'aucune quiescence n'est demandée, le retour est immédiat (une lecture
 * atomique). Quand une quiescence est demandée, le thread se GARE sur une
 * condvar jusqu'à la levée du drapeau — un thread garé ne détient par
 * construction aucun verrou stdio/logger/malloc. La console, bloquée dans
 * read(), ne peut pas boucler jusqu'à un checkpoint : elle est instrumentée
 * différemment via `fork_gate_mark_blocked` autour de son read bloquant (cf.
 * détail sur cette fonction).
 */

#include <stddef.h>

/**
 * @brief Nombre maximal de threads candidats à la quiescence (checker,
 *        server_tcp, canal de contrôle, console) — borne large et statique,
 *        jamais dépassée par l'usage réel du process client.
 */
#define FORK_GATE_MAX_PARTICIPANTS 8

/** @brief Budget par défaut d'une demande de quiescence : si elle n'est pas
 *         atteinte sous ce délai, on refuse de forker. */
#define FORK_GATE_DEFAULT_TIMEOUT_MS 2000

/** @brief Issue d'une demande de quiescence. */
typedef enum {
    FORK_GATE_QUIESCED = 0,  /**< Tous les participants enregistrés sont garés ou bloqués. */
    FORK_GATE_TIMEOUT  = -1, /**< Budget écoulé : la demande est ANNULÉE (jamais de fork dans le doute). */
    FORK_GATE_FULL     = -2, /**< fork_gate_register : table des participants pleine. */
} fork_gate_result_t;

/**
 * @brief Réinitialise tout l'état du module (aucun participant enregistré,
 *        quiescence non demandée). Réservé aux tests unitaires : en
 *        production le module vit pour toute la durée du process.
 */
void fork_gate_reset(void);

/**
 * @brief Enregistre le thread appelant comme candidat à la quiescence.
 *
 * À appeler une seule fois, avant d'entrer dans la boucle principale du
 * thread. Le slot renvoyé est à repasser à `fork_gate_checkpoint` /
 * `fork_gate_mark_blocked` / `fork_gate_unregister`.
 *
 * @param name Nom court du participant (diagnostic uniquement, tronqué ;
 *             NULL accepté).
 * @return     Un slot >= 0, ou FORK_GATE_FULL si la table est pleine.
 */
int fork_gate_register(const char *name);

/**
 * @brief Désenregistre un participant (fin de thread). No-op si `slot < 0`
 *        (permet d'appeler sans condition après un `fork_gate_register` qui
 *        aurait échoué).
 */
void fork_gate_unregister(int slot);

/**
 * @brief Point de contrôle : à appeler en tête de chaque tour de boucle.
 *
 * Chemin rapide (aucune quiescence en cours) : une lecture atomique, retour
 * immédiat. Sinon, le thread annonce son état PARKED (il ne détient alors
 * plus aucun verrou) et attend sur une condvar jusqu'à
 * `fork_gate_release_quiesce`. No-op si `slot < 0`.
 */
void fork_gate_checkpoint(int slot);

/**
 * @brief Cas particulier des threads bloqués dans un appel connu pour ne
 *        détenir aucun verrou (ex. la console dans `read()` sur stdin) :
 *        marque le participant comme BLOCKED (quiescent sans se garer sur la
 *        condvar) tant que `blocked` est vrai.
 *
 * Contrat d'usage (cas particulier de la console) : le thread
 * appelle `fork_gate_mark_blocked(slot, 1)` juste avant l'appel bloquant,
 * `fork_gate_mark_blocked(slot, 0)` juste après son retour, PUIS
 * `fork_gate_checkpoint(slot)` avant tout traitement du résultat — pour se
 * garer pour de bon si une quiescence a été demandée pendant que le thread
 * était bloqué. No-op si `slot < 0`.
 */
void fork_gate_mark_blocked(int slot, int blocked);

/**
 * @brief Demande la quiescence de tous les participants enregistrés et
 *        attend qu'elle soit atteinte (PARKED ou BLOCKED), borné à
 *        `timeout_ms`.
 *
 * Un seul appelant à la fois (le thread orchestrateur) : cette
 * fonction n'est pas conçue pour des demandes concurrentes.
 *
 * @return FORK_GATE_QUIESCED si tous les participants sont quiescents avant
 *         le budget ; FORK_GATE_TIMEOUT sinon — dans ce cas la demande est
 *         annulée avant le retour (tout participant déjà garé est relâché) :
 *         on ne laisse jamais le système à moitié quiescent.
 */
fork_gate_result_t fork_gate_request_quiesce(long timeout_ms);

/**
 * @brief Lève une quiescence obtenue par `fork_gate_request_quiesce` :
 *        réveille tous les participants garés ou bloqués.
 */
void fork_gate_release_quiesce(void);

/**
 * @brief Lecture rapide (sans verrou) : une quiescence est-elle en cours ?
 *        Diagnostic uniquement — `fork_gate_checkpoint` fait déjà sa propre
 *        vérification atomique, inutile d'appeler ceci avant.
 */
int fork_gate_is_quiescing(void);

/**
 * @brief Primitives d'E/S autour du `fork()` : prend le verrou de sortie du
 *        logger (`logger_lock_output`) et vide `stdout`/`stderr`.
 *
 * Deux écarts délibérés par rapport à une version initiale
 * (`flockfile`+`fflush(NULL)`), trouvés en forkant réellement à chaud :
 *
 * 1. Pas de `fflush(NULL)` : parcourt tous les `FILE*` ouverts, y compris
 *    `stdin` — le thread console détient le verrou stdio de `stdin` pour
 *    toute la durée de son `fgetc()` bloquant, indépendamment de
 *    `fork_gate_mark_blocked`. Un opérateur simplement assis au prompt
 *    provoquait donc un interblocage systématique.
 *
 * 2. Pas de `flockfile(stdout)`/`flockfile(stderr)` : ce verrou stdio
 *    récursif suit un propriétaire, et ce suivi ne survit pas fiablement à
 *    `fork()` sous macOS multi-thread — le fils hérite un verrou marqué
 *    détenu par un thread dont l'identité OS a changé, et son premier
 *    `flockfile()` bloque indéfiniment. La quiescence coopérative protège
 *    déjà entièrement contre le risque que `flockfile` couvrait (un autre
 *    thread mi-écriture au moment du fork).
 *
 * À appeler par le thread forkeur uniquement après un
 * `fork_gate_request_quiesce` réussi, et à relâcher dans le parent et
 * l'enfant juste après le `fork()`.
 */
void fork_gate_acquire_io_locks(void);

/** @brief Relâche les verrous pris par `fork_gate_acquire_io_locks`, dans l'ordre inverse. */
void fork_gate_release_io_locks(void);

/* ============================ Journal de trace (diagnostic) =============== */

/**
 * @brief Événement enregistré par `fork_gate_trace_record` — un par appel
 *        d'une fonction de ce module qui modifie l'état d'un participant ou
 *        de la quiescence globale.
 *
 * Un blocage intermittent dans `fork_gate_release_quiesce` ne se
 * reproduisait pas sous `strace` (l'interception ptrace referme la fenêtre
 * de course). Ce module trace donc chaque transition sans aucun appel
 * système sur le chemin chaud (`clock_gettime` + écriture atomique dans un
 * tableau préalloué) — invisible pour ptrace — et se relit après coup via
 * `gdb -p <pid>` une fois le blocage survenu.
 */
typedef enum {
    FGT_REGISTER = 0,          /**< fork_gate_register : slot attribué. */
    FGT_UNREGISTER,             /**< fork_gate_unregister. */
    FGT_PARK_BEGIN,              /**< fork_gate_checkpoint : entrée dans pthread_cond_wait(&g_released). */
    FGT_PARK_END,                /**< fork_gate_checkpoint : sortie de pthread_cond_wait (réveillé). */
    FGT_BLOCKED_ON,               /**< fork_gate_mark_blocked(slot, 1). */
    FGT_BLOCKED_OFF,               /**< fork_gate_mark_blocked(slot, 0). */
    FGT_REQUEST_QUIESCE_BEGIN,      /**< fork_gate_request_quiesce : entrée. */
    FGT_REQUEST_QUIESCE_QUIESCED,    /**< fork_gate_request_quiesce : succès. */
    FGT_REQUEST_QUIESCE_TIMEOUT,      /**< fork_gate_request_quiesce : timeout, demande annulée. */
    FGT_RELEASE_QUIESCE_BEGIN,         /**< fork_gate_release_quiesce : juste avant pthread_cond_broadcast. */
    FGT_RELEASE_QUIESCE_END,            /**< fork_gate_release_quiesce : juste après (jamais atteint en cas de blocage). */
} fork_gate_trace_event_t;

/** @brief Capacité du ring de trace — dépassée, les plus anciennes entrées sont écrasées (diagnostic, pas une garantie de rétention). */
#define FORK_GATE_TRACE_CAPACITY 1024

/** @brief Une entrée du journal de trace. Champs bruts, décodage à la lecture (gdb ou tests). */
typedef struct {
    long long timestamp_ns; /**< CLOCK_MONOTONIC, comparable entre entrées du même process. */
    long tid;                /**< Identifiant de thread (TID noyau sous Linux, sinon pthread_self() casté). */
    int slot;                 /**< Slot concerné (-1 si sans objet, ex. RELEASE_QUIESCE). */
    fork_gate_trace_event_t event;
} fork_gate_trace_record_t;

/**
 * @brief Journal de trace globaux, en lecture directe via gdb sur un process
 *        vivant (y compris bloqué) : `print g_fork_gate_trace_buf`,
 *        `print g_fork_gate_trace_write_index`. L'entrée la plus récente est
 *        à l'indice `(g_fork_gate_trace_write_index - 1) % FORK_GATE_TRACE_CAPACITY`
 *        (l'index ne revient jamais à zéro, y compris après un tour du ring).
 *        Délibérément NON `static` pour rester trouvable par son nom simple
 *        sous gdb sans qualifier de fichier.
 */
extern fork_gate_trace_record_t g_fork_gate_trace_buf[FORK_GATE_TRACE_CAPACITY];
extern unsigned long g_fork_gate_trace_write_index;

/**
 * @brief Enregistre un événement dans le journal de trace. Async-signal-safe
 *        et sans appel système sur le chemin chaud (seul `clock_gettime`
 *        peut, selon la plateforme, être un vrai appel système — sous Linux
 *        avec VDSO, ce qui est le cas de la cible de production de ce
 *        projet, c'est un accès mémoire pur, pas un `syscall()`).
 */
void fork_gate_trace_record(fork_gate_trace_event_t event, int slot);

#endif /* fork_gate_h */
