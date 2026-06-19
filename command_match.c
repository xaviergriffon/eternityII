#include "command_match.h"

#include <string.h>

int levenshtein(const char *a, const char *b) {
    int la = (int)strlen(a);
    int lb = (int)strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    if (lb > 62) lb = 62;   /* borne de sécurité : les commandes sont courtes */
    int prev[64], cur[64];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int m = del < ins ? del : ins;
            cur[j] = m < sub ? m : sub;
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

const char *closest_command(const char *instruction, const char *const *commands, int n) {
    const char *best = NULL;
    int best_dist = 1 << 30;
    for (int c = 0; c < n; c++) {
        int d = levenshtein(instruction, commands[c]);
        if (d < best_dist) {
            best_dist = d;
            best = commands[c];
        }
    }
    if (best != NULL) {
        int threshold = (int)strlen(best) / 3;
        if (threshold < 1) threshold = 1;
        if (threshold > 3) threshold = 3;
        if (best_dist <= threshold) return best;
    }
    return NULL;
}
