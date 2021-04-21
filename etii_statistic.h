#ifndef etii_statistic_h
#define etii_statistic_h

#include <stdio.h>
#include "packed.h"

PACK(
struct client_statistics {
    unsigned long long shots_per_second;
    unsigned long long possibilities_in_stock;
    unsigned long long analyses_in_stock;
    uint16_t max_result;
});


#endif /* etii_statistic_h */
