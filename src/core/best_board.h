/**
 * @file best_board.h
 * @brief Mémorisation du plateau (représentation complète, pas seulement le
 *        compte) correspondant au meilleur résultat observé.
 *
 * Jusqu'ici les statistiques (`max_result`, `client_statistics`,
 * `control_stats_t`, l'API HTTP) n'exposaient que le NOMBRE de pièces
 * placées au record — jamais l'agencement qui l'a produit, qui continue
 * d'être muté par le backtracking immédiatement après (cf. AGENTS.md,
 * section « Statistiques »). Ce module comble ce manque avec une primitive
 * unique, réutilisée à trois échelles indépendantes qui n'ont jamais
 * connaissance l'une de l'autre (chacune instancie son propre `best_board_t`,
 * cf. les globales `extern` ci-dessous) :
 *  - un fork de recherche (état local à CE process, alimenté par
 *    `etii_search.c`/`possibility.c` au fil du backtracking) ;
 *  - le processus PARENT client (agrégat de ses forks, alimenté par IPC —
 *    cf. `IPC_MSG_BEST_BOARD`, `src/net/ipc_protocol.h`) ;
 *  - le serveur (agrégat de tous les clients connectés, alimenté par le
 *    canal de contrôle — cf. `CTRL_GET_BEST_BOARD`/`CTRL_BEST_BOARD`,
 *    `src/net/control_protocol.h`).
 *
 * La règle est la même partout : on ne conserve QUE la première
 * représentation qui dépasse STRICTEMENT le nombre de pièces déjà
 * enregistré — un nouveau plateau à égalité n'écrase jamais le précédent
 * (demande explicite : mémoriser un enregistrement par record, pas la
 * dernière valeur vue).
 */
#ifndef eternityII_best_board_h
#define eternityII_best_board_h

#include <pthread.h>

#include "core/possibility.h"

/**
 * @brief État "meilleur plateau observé" (nombre de pièces + représentation),
 *        protégé par un mutex. Une instance par échelle de suivi (cf. les
 *        globales `extern` ci-dessous) — jamais partagée entre deux échelles.
 */
typedef struct {
    pthread_mutex_t mutex;
    /// 1 si `board`/`result` contiennent un enregistrement valide.
    int valid;
    /// Nombre de pièces placées du dernier enregistrement (== board.alloc).
    uint16_t result;
    /// Représentation complète du plateau au moment du record.
    struct possibility_packet board;
} best_board_t;

/**
 * @brief Initialise (ou réinitialise) un `best_board_t` à l'état "aucun
 *        enregistrement". Doit être appelé avant toute utilisation d'une
 *        instance allouée dynamiquement ou placée sur la pile ; les globales
 *        `extern` de ce module sont déjà initialisées statiquement.
 *
 * @param bb Instance à initialiser.
 */
void best_board_init(best_board_t *bb);

/**
 * @brief Enregistre `*board` si `alloc` dépasse STRICTEMENT le meilleur
 *        résultat déjà connu de `bb` (ou si `bb` n'a encore aucun
 *        enregistrement). Sans effet sinon (y compris à égalité).
 *
 * Le champ `alloc` du paquet stocké est toujours forcé à `alloc` (plutôt que
 * de faire confiance à `board->alloc`) : certains sites d'appel disposent
 * d'un plateau dont `alloc` ne reflète pas encore exactement la profondeur
 * atteinte (cf. `search_packet_backtracking`, cases pré-remplies).
 *
 * @param bb    Instance à mettre à jour (NULL toléré : no-op, renvoie 0).
 * @param board Plateau à copier si le record est battu.
 * @param alloc Nombre de pièces placées de ce plateau.
 * @return      1 si un nouveau record a été enregistré, 0 sinon.
 */
int best_board_try_record(best_board_t *bb, const struct possibility_packet *board, uint16_t alloc);

/**
 * @brief Recopie le dernier enregistrement de `bb` dans `*out` (si non NULL).
 *
 * @param bb        Instance à lire.
 * @param out       Destination du plateau (NULL toléré : on lit juste la validité/le résultat).
 * @param out_alloc Destination du nombre de pièces (NULL toléré).
 * @return          1 si `bb` a un enregistrement valide (et donc `*out`/`*out_alloc`
 *                  renseignés), 0 sinon.
 */
int best_board_get(best_board_t *bb, struct possibility_packet *out, uint16_t *out_alloc);

/**
 * @brief Nombre de pièces du dernier enregistrement de `bb`, 0 si aucun.
 * @param bb Instance à lire (NULL toléré : renvoie 0).
 */
uint16_t best_board_result(best_board_t *bb);

/**
 * @brief Sérialise `bb` dans `filename` (écriture atomique : fichier
 *        temporaire `.tmp` puis `rename`, comme `backup()`/`backup_analysed()`
 *        de `src/core/datamanager.c`). Format : `uint8_t valid` puis, si
 *        `valid`, `sizeof(struct possibility_packet)` octets bruts (même
 *        convention que les fichiers `.back` : round-trip sur le même build,
 *        jamais de comparaison par égalité sur ces octets bruts).
 *
 * @param bb       Instance à sauvegarder.
 * @param filename Chemin du fichier de destination.
 * @return         0 en cas de succès, -1 sinon.
 */
int best_board_save(best_board_t *bb, const char *filename);

/**
 * @brief Charge `bb` depuis `filename` (écrit par `best_board_save`).
 *
 * @param bb       Instance à remplir (remplace tout enregistrement courant).
 * @param filename Chemin du fichier source.
 * @return         0 en cas de succès (y compris un fichier marquant "aucun
 *                 enregistrement"), -1 si le fichier est absent/illisible/corrompu.
 */
int best_board_load(best_board_t *bb, const char *filename);

/**
 * @brief Meilleur plateau connu LOCALEMENT par ce process : alimenté par le
 *        backtracking d'un fork de recherche (`etii_search.c`) ou par la
 *        génération de la genèse côté serveur — jamais consulté à distance
 *        directement (chaque échelle qui en a besoin agrège via IPC/CTRL_*,
 *        cf. les deux globales suivantes).
 */
extern best_board_t g_search_best_board;

/**
 * @brief Agrégat, côté processus PARENT client, des meilleurs plateaux
 *        rapportés par ses forks de recherche (`IPC_MSG_BEST_BOARD`). C'est
 *        cette instance que le canal de contrôle sert en réponse à
 *        `CTRL_GET_BEST_BOARD` (cf. `etii_control.c`).
 */
extern best_board_t g_client_aggregate_best_board;

/**
 * @brief Agrégat, côté SERVEUR, des meilleurs plateaux connus : sa propre
 *        genèse (`first_possibility`) et ceux rapportés par les clients
 *        connectés (tiré via `CTRL_GET_BEST_BOARD` quand `control_session_step`
 *        détecte qu'un client rapporte un `max_result` supérieur, cf.
 *        `etii_server.c`). Exposé par `GET /api/v1/best-board` et persisté
 *        avec le reste du stock (`check_server_step`, `restore_interpreter`).
 */
extern best_board_t g_server_best_board;

#endif /* eternityII_best_board_h */
