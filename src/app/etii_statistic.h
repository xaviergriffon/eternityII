#ifndef etii_statistic_h
#define etii_statistic_h

#include <stdint.h>

/**
 * @brief Taille maximale de fenêtre de forward-checking supportée par les statistiques.
 *
 * Dimensionne le tableau `fc_pruned_at` indépendamment de `FORWARD_CHECK_K`
 * (qui n'est pas encore défini lors de l'inclusion de ce header).
 */
#define FC_STAT_MAX_K 8

/**
 * @brief Structure pour des statistiques d'un thread calculant des possiblités.
 */
struct client_statistics {
    unsigned long long shots_per_second;
    unsigned long long possibilities_in_stock;
    unsigned long long analyses_in_stock;
    uint16_t max_result;
    /** Vrai si ce fork était en train d'échanger avec le serveur (connexion,
        envoi/réception, sonde de faim) au moment de ce rapport — cf.
        `server_io_active`, `src/core/core_static_variables.h`. */
    uint8_t server_io_active;
    /** Cumul des tentatives de placement soumises au forward-checking. */
    unsigned long long fc_attempts;
    /** Cumul des placements élagués par le forward-checking. */
    unsigned long long fc_pruned;
    /** Cumul des élagages par distance de la première case morte (indice 1..K). */
    unsigned long long fc_pruned_at[FC_STAT_MAX_K + 1];
    /** Cumul des possibilités validées par un client pruner (0 en mode recherche). */
    unsigned long long pruner_checked;
    /** Cumul des possibilités mortes éliminées par un client pruner. */
    unsigned long long pruner_removed;
    /** Cumul des cases étudiées par un client pruner (0 en mode recherche). */
    unsigned long long pruner_cells_studied;
    /** Débit des cases étudiées au prunage (moyenne glissante 5 s, comme shots_per_second). */
    unsigned long long pruner_cells_per_second;
} __attribute__((__packed__));


#endif /* etii_statistic_h */
