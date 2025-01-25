#ifndef etii_statistic_h
#define etii_statistic_h

#include <stdio.h>

/**
 * @brief Structure pour des statistiques d'un thread calculant des possiblités.
 */
struct client_statistics {
    unsigned long long shots_per_second;
    unsigned long long possibilities_in_stock;
    unsigned long long analyses_in_stock;
    uint16_t max_result;
} __attribute__((__packed__));


#endif /* etii_statistic_h */
