#ifndef fork_gate_h
#define fork_gate_h

/*
 * Infrastructure de quiescence coopérative (PR B de
 * docs/conception/cycle_vie_forks.md, arbitrage D2).
 *
 * Objectif final (PR C/D) : pouvoir forker de nouveaux process de recherche
 * alors que les threads du process PARENT (checker, réception IPC des forks,
 * canal de contrôle, console) tournent déjà — ce qui viole aujourd'hui la
 * règle « aucun thread du parent ne doit tourner pendant fork() »
 * (src/app/main.c:228-233) : un thread qui détient un verrou stdio/logger au
 * moment du fork le transmet verrouillé à l'enfant, qui n'a personne pour le
 * relâcher (blocage définitif au premier printf/malloc).
 *
 * Solution retenue (D2-c) : chaque thread candidat s'ENREGISTRE une fois puis
 * appelle `fork_gate_checkpoint` en tête de chaque tour de sa boucle. Tant
 * qu'aucune quiescence n'est demandée, le retour est immédiat (une lecture
 * atomique). Quand une quiescence est demandée, le thread se GARE sur une
 * condvar jusqu'à la levée du drapeau — un thread garé ne détient par
 * construction aucun verrou stdio/logger/malloc. La console, bloquée dans
 * read(), ne peut pas boucler jusqu'à un checkpoint : elle est instrumentée
 * différemment via `fork_gate_mark_blocked` autour de son read bloquant (cf.
 * détail sur cette fonction).
 *
 * PR B livre ces primitives et les câble aux points de contrôle (checker,
 * server_tcp, canal de contrôle, console) SANS jamais appeler
 * `fork_gate_request_quiesce` en production : le comportement observable est
 * inchangé, le fork reste avant le démarrage des threads (cf. main.c). C'est
 * PR C/D qui utiliseront réellement ces primitives pour forker à chaud.
 */

#include <stddef.h>

/**
 * @brief Nombre maximal de threads candidats à la quiescence (checker,
 *        server_tcp, canal de contrôle, console) — borne large et statique,
 *        jamais dépassée par l'usage réel du process client.
 */
#define FORK_GATE_MAX_PARTICIPANTS 8

/** @brief Budget par défaut d'une demande de quiescence (cf. D2 : "si la
 *         quiescence n'est pas atteinte sous ~2 s ... on refuse de forker"). */
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
 * Contrat d'usage (cf. D2, cas particulier de la console) : le thread
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
 * Un seul appelant à la fois (le futur thread orchestrateur, PR C) : cette
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
 * @brief Primitives d'E/S autour du `fork()` (D2) : prend le verrou de
 *        sortie du logger PUIS `flockfile(stdout)`/`flockfile(stderr)`, et
 *        vide tous les tampons stdio (`fflush(NULL)`).
 *
 * À appeler par le thread forkeur UNIQUEMENT après un
 * `fork_gate_request_quiesce` réussi, et à relâcher (`fork_gate_release_io_locks`)
 * dans le parent ET dans l'enfant juste après le `fork()` — les verrous
 * `flockfile` sont détenus par le thread forkeur, seul thread de l'enfant à
 * cet instant, donc `funlockfile` y est valide (cf. D2).
 *
 * PR B expose et teste ces primitives de façon autonome ; aucun site de PR B
 * ne les appelle encore autour d'un vrai `fork()` (PR C/D).
 */
void fork_gate_acquire_io_locks(void);

/** @brief Relâche les verrous pris par `fork_gate_acquire_io_locks`, dans l'ordre inverse. */
void fork_gate_release_io_locks(void);

#endif /* fork_gate_h */
