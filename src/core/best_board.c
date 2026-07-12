#include "core/best_board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

best_board_t g_search_best_board = { PTHREAD_MUTEX_INITIALIZER, 0, 0, { 0 } };
best_board_t g_client_aggregate_best_board = { PTHREAD_MUTEX_INITIALIZER, 0, 0, { 0 } };
best_board_t g_server_best_board = { PTHREAD_MUTEX_INITIALIZER, 0, 0, { 0 } };

void best_board_init(best_board_t *bb)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    bb->mutex = m;
    bb->valid = 0;
    bb->result = 0;
    memset(&bb->board, 0, sizeof(bb->board));
}

int best_board_try_record(best_board_t *bb, const struct possibility_packet *board, uint16_t alloc)
{
    if (bb == NULL || board == NULL) {
        return 0;
    }
    int recorded = 0;
    pthread_mutex_lock(&bb->mutex);
    if (!bb->valid || alloc > bb->result) {
        memcpy(&bb->board, board, sizeof(bb->board));
        bb->board.alloc = alloc;
        bb->result = alloc;
        bb->valid = 1;
        recorded = 1;
    }
    pthread_mutex_unlock(&bb->mutex);
    return recorded;
}

int best_board_get(best_board_t *bb, struct possibility_packet *out, uint16_t *out_alloc)
{
    if (bb == NULL) {
        return 0;
    }
    int valid;
    pthread_mutex_lock(&bb->mutex);
    valid = bb->valid;
    if (valid) {
        if (out != NULL) {
            memcpy(out, &bb->board, sizeof(*out));
        }
        if (out_alloc != NULL) {
            *out_alloc = bb->result;
        }
    }
    pthread_mutex_unlock(&bb->mutex);
    return valid;
}

uint16_t best_board_result(best_board_t *bb)
{
    if (bb == NULL) {
        return 0;
    }
    uint16_t r;
    pthread_mutex_lock(&bb->mutex);
    r = bb->result;
    pthread_mutex_unlock(&bb->mutex);
    return r;
}

int best_board_save(best_board_t *bb, const char *filename)
{
    if (bb == NULL || filename == NULL) {
        return -1;
    }
    uint8_t valid;
    struct possibility_packet board;
    pthread_mutex_lock(&bb->mutex);
    valid = (uint8_t)bb->valid;
    memcpy(&board, &bb->board, sizeof(board));
    pthread_mutex_unlock(&bb->mutex);

    size_t len = strlen(filename);
    char *tmp_filename = malloc(len + 5); // ".tmp" + '\0'
    if (tmp_filename == NULL) {
        return -1;
    }
    memcpy(tmp_filename, filename, len);
    memcpy(tmp_filename + len, ".tmp", 5);

    FILE *f = fopen(tmp_filename, "w");
    if (f == NULL) {
        free(tmp_filename);
        return -1;
    }

    int write_error = 0;
    if (fwrite(&valid, sizeof(valid), 1, f) != 1) {
        write_error = 1;
    }
    if (!write_error && valid && fwrite(&board, sizeof(board), 1, f) != 1) {
        write_error = 1;
    }
    if (fclose(f) != 0) {
        write_error = 1;
    }
    if (write_error) {
        unlink(tmp_filename);
        free(tmp_filename);
        return -1;
    }
    if (rename(tmp_filename, filename) != 0) {
        unlink(tmp_filename);
        free(tmp_filename);
        return -1;
    }
    free(tmp_filename);
    return 0;
}

int best_board_load(best_board_t *bb, const char *filename)
{
    if (bb == NULL || filename == NULL) {
        return -1;
    }
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        return -1;
    }
    uint8_t valid = 0;
    struct possibility_packet board;
    memset(&board, 0, sizeof(board));
    int ok = (fread(&valid, sizeof(valid), 1, f) == 1);
    if (ok && valid) {
        ok = (fread(&board, sizeof(board), 1, f) == 1);
    }
    fclose(f);
    if (!ok) {
        return -1;
    }

    pthread_mutex_lock(&bb->mutex);
    bb->valid = valid ? 1 : 0;
    if (valid) {
        memcpy(&bb->board, &board, sizeof(bb->board));
        bb->result = board.alloc;
    }
    pthread_mutex_unlock(&bb->mutex);
    return 0;
}
