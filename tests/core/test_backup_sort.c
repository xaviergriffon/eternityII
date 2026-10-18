/*
 * Tests unitaires de backup_sort.c — remise en ordre d'un `.back` par tri
 * externe (mode `eternityII sortback`).
 *
 * Ce que ces tests verrouillent :
 *
 *  1. La clé : plateau (bitmap puis valeurs) d'abord, en-tête ensuite ; 0 si
 *     et seulement si les octets sont identiques.
 *  2. La sortie porte le MÊME multi-ensemble que ce que `restore` tirerait de
 *     l'entrée (forme canonique), dans l'ordre de la clé — avec plusieurs
 *     paquets fusionnés comme en un seul, et octet pour octet le même fichier
 *     dans les deux cas.
 *  3. Les drapeaux de l'en-tête suivent ; un `.back` hérité est compacté.
 *  4. Refus sans dégât : sortie existante, entrée non autonome avec son
 *     `.spillcount`, entrée tronquée — ni sortie partielle ni paquet laissé.
 *  5. Sous zstd, un `.back` mélangé trié tient au moins deux fois moins de
 *     place : c'est la raison d'être du mode.
 *
 * Toutes les écritures sont relatives (bac à sable du runner).
 */
#include "greatest.h"
#include "core/backup_sort.h"
#include "core/backup_stream.h"
#include "core/packet_codec.h"
#include "core/possibility.h"
#include "packet_fixture.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SB_PREFIX (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES)

static char g_dir[64];

static void sb_begin(void)
{
    snprintf(g_dir, sizeof g_dir, "sortback_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        g_dir[0] = '\0';
    }
}

/* Nombre d'entrées du dossier de test, hors « . » et « .. ». */
static int sb_entries(void)
{
    DIR *d = opendir(g_dir);
    int n = 0;
    if (d == NULL) {
        return -1;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static void sb_end(void)
{
    DIR *d = opendir(g_dir);
    if (d != NULL) {
        char p[PATH_MAX];
        for (struct dirent *e; (e = readdir(d)) != NULL;) {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
                snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
                unlink(p);
            }
        }
        closedir(d);
    }
    rmdir(g_dir);
}

static void sb_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", g_dir, name);
}

/* Plateaux d'un arbre factice, par familles de 16 (même grand-parent, même
 * parent par 4) : cf. tests/core/test_stock_tier.c. `checked` alterne par
 * famille pour couvrir les deux pools. */
#define TREE_DEPTH (ETERN_PARTS < 48 ? ETERN_PARTS - 1 : 48)
static int16_t tree_piece(unsigned key, int d)
{
    uint32_t h = key * 2654435761u + (uint32_t)d * 40503u + 1u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (int16_t)(1 + h % (4u * ETERN_PARTS));
}
static void tree_packet(struct possibility_packet *pk, unsigned idx)
{
    fixture_blank(pk);
    for (int d = 0; d < TREE_DEPTH; d++) {
        unsigned key = (d < TREE_DEPTH - 2) ? idx / 16 : (d == TREE_DEPTH - 2) ? idx / 4 : idx;
        pk->grid[d / ETERN_SIZE][d % ETERN_SIZE] = tree_piece(key, d);
    }
    pk->alloc = (uint16_t)TREE_DEPTH;
    pk->checked = (uint8_t)((idx / 16) & 1);
}

/* Ordre mélangé de 0..n-1 (Fisher-Yates, graine fixe). */
static void shuffled(unsigned *order, unsigned n)
{
    uint32_t seed = 2463534242u;
    for (unsigned i = 0; i < n; i++) {
        order[i] = i;
    }
    for (unsigned i = n - 1; i > 0; i--) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        unsigned j = seed % (i + 1), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

/* Écrit un `.back` compact des plateaux `order[0..n)` (score MRV quelconque :
 * la sortie doit le ramener à « inconnu », comme `restore`). */
static int write_back(const char *path, const unsigned *order, unsigned n, uint8_t flags)
{
    FILE *raw = fopen(path, "wb");
    if (raw == NULL) {
        return -1;
    }
    FILE *f = backup_stream_open_write(raw, flags);
    if (f == NULL) {
        return -1;
    }
    for (unsigned i = 0; i < n; i++) {
        struct possibility_packet pk;
        tree_packet(&pk, order[i]);
        pk.min_candidats = (int16_t)(order[i] % 7);
        if (packet_codec_fwrite(f, &pk) != 0) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f);
}

/* Forme canonique attendue du plateau `idx` (ce que `restore` en ferait). */
static size_t canonical(unsigned idx, uint8_t *rec)
{
    struct possibility_packet pk;
    tree_packet(&pk, idx);
    pk.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
    size_t written = 0;
    packet_codec_encode(&pk, rec, PACKET_CODEC_MAX_BYTES, &written);
    return written;
}

typedef struct {
    uint8_t *bytes;
    size_t nbytes;
    unsigned count;
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
} file_records_t;

/* Lit tous les enregistrements d'un `.back` compact. */
static int read_back(const char *path, file_records_t *out)
{
    memset(out, 0, sizeof *out);
    int format = -1;
    FILE *f = backup_stream_fopen_read(path, &format, out->header);
    if (f == NULL || format != BACKUP_STREAM_PACKED) {
        return -1;
    }
    size_t cap = 1 << 16;
    out->bytes = malloc(cap);
    for (;;) {
        if (cap - out->nbytes < PACKET_CODEC_MAX_BYTES) {
            cap *= 2;
            out->bytes = realloc(out->bytes, cap);
        }
        uint8_t *rec = out->bytes + out->nbytes;
        size_t got = fread(rec, 1, SB_PREFIX, f);
        if (got == 0) {
            break;
        }
        size_t len = SB_PREFIX + PACKET_CODEC_VALUE_BYTES(packet_codec_peek_placed(rec, SB_PREFIX));
        if (got != SB_PREFIX || fread(rec + SB_PREFIX, 1, len - SB_PREFIX, f) != len - SB_PREFIX) {
            fclose(f);
            return -1;
        }
        out->nbytes += len;
        out->count++;
    }
    fclose(f);
    return 0;
}

static size_t rec_len(const uint8_t *rec)
{
    return SB_PREFIX + PACKET_CODEC_VALUE_BYTES(packet_codec_peek_placed(rec, SB_PREFIX));
}

/* Vrai si les enregistrements de `fr` sont dans l'ordre de la clé ET forment
 * le multi-ensemble canonique de `ids[0..n)`. */
static int sorted_and_same_multiset(const file_records_t *fr, const unsigned *ids, unsigned n)
{
    if (fr->count != n) {
        return 0;
    }
    /* Multi-ensemble : chaque enregistrement attendu, retrouvé et barré. */
    char *seen = calloc(n, 1);
    const uint8_t *p = fr->bytes, *prev = NULL;
    int ok = 1;
    for (unsigned i = 0; i < n && ok; i++) {
        size_t len = rec_len(p);
        if (prev != NULL && backup_sort_record_cmp(prev, rec_len(prev), p, len) > 0) {
            ok = 0;
        }
        int found = 0;
        for (unsigned k = 0; k < n && !found; k++) {
            uint8_t want[PACKET_CODEC_MAX_BYTES];
            if (!seen[k] && canonical(ids[k], want) == len && memcmp(want, p, len) == 0) {
                seen[k] = 1;
                found = 1;
            }
        }
        ok = ok && found;
        prev = p;
        p += len;
    }
    free(seen);
    return ok;
}

/* La clé range par plateau avant l'en-tête, et ne confond que des octets
 * identiques. Contre-épreuve : un `memcmp` brut rangerait `b` (checked=0,
 * case 0 grande) avant `a` (checked=1, case 0 petite). */
TEST record_cmp_orders_by_board_then_header(void)
{
    struct possibility_packet pa, pb;
    fixture_blank(&pa);
    fixture_blank(&pb);
    pa.grid[0][0] = 5; pa.alloc = 1; pa.checked = 1;
    pb.grid[0][0] = 9; pb.alloc = 1; pb.checked = 0;
    uint8_t a[PACKET_CODEC_MAX_BYTES], b[PACKET_CODEC_MAX_BYTES];
    size_t la = 0, lb = 0;
    ASSERT_EQ(0, packet_codec_encode(&pa, a, sizeof a, &la));
    ASSERT_EQ(0, packet_codec_encode(&pb, b, sizeof b, &lb));
    ASSERT(backup_sort_record_cmp(a, la, b, lb) < 0);
    ASSERT(backup_sort_record_cmp(b, lb, a, la) > 0);
    /* Même plateau : l'en-tête départage. */
    pb.grid[0][0] = 5;
    ASSERT_EQ(0, packet_codec_encode(&pb, b, sizeof b, &lb));
    ASSERT(backup_sort_record_cmp(b, lb, a, la) < 0);
    ASSERT_EQ(0, backup_sort_record_cmp(a, la, a, la));
    PASS();
}

/* Paquets multiples ou un seul : même multi-ensemble, même ordre, même
 * fichier à l'octet près ; doublons comptés et gardés ; drapeaux suivis ;
 * aucun paquet laissé dans le dossier de travail. */
TEST many_runs_and_one_run_give_the_same_sorted_file(void)
{
    enum { N = 600, DUP = 3 };
    sb_begin();
    ASSERT(g_dir[0] != '\0');
    static unsigned order[N + DUP];
    shuffled(order, N);
    for (unsigned d = 0; d < DUP; d++) {
        order[N + d] = order[d * 5]; /* trois doublons exacts */
    }
    char in[PATH_MAX], many[PATH_MAX], one[PATH_MAX];
    sb_path(in, sizeof in, "in.back");
    sb_path(many, sizeof many, "many.back");
    sb_path(one, sizeof one, "one.back");
    ASSERT_EQ(0, write_back(in, order, N + DUP, PACKET_CODEC_FILE_FLAG_COMPLETE));

    backup_sort_options_t small = { .mem_bytes = 8 * 1024, .tmp_dir = g_dir };
    backup_sort_stats_t st;
    ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, many, &small, &st));
    ASSERT_EQ_FMT((unsigned long long)(N + DUP), st.records, "%llu");
    ASSERT(st.runs > 2);
    unsigned long long dup_many = st.duplicates;
    /* Au moins les trois doublons plantés (le build 16, 64 valeurs par case,
     * en tire d'autres du générateur) ; le compte exact est vérifié sur le
     * fichier plus bas. */
    ASSERT(dup_many >= DUP);
    ASSERT_EQ(2, sb_entries()); /* in + many : paquets et `.sorting` effacés */

    ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, one, NULL, &st));
    ASSERT_EQ_FMT(1ULL, st.runs, "%llu");
    ASSERT_EQ_FMT(dup_many, st.duplicates, "%llu");

    file_records_t fm, fo;
    ASSERT_EQ(0, read_back(many, &fm));
    ASSERT_EQ(0, read_back(one, &fo));
    ASSERT(sorted_and_same_multiset(&fm, order, N + DUP));
    unsigned long long adjacent = 0;
    for (size_t off = 0, prev = 0, plen = 0; off < fm.nbytes;) {
        size_t len = rec_len(fm.bytes + off);
        if (plen == len && memcmp(fm.bytes + prev, fm.bytes + off, len) == 0) {
            adjacent++;
        }
        prev = off;
        plen = len;
        off += len;
    }
    ASSERT_EQ_FMT(adjacent, dup_many, "%llu");
    ASSERT_EQ_FMT(fm.nbytes, fo.nbytes, "%zu");
    ASSERT_MEM_EQ(fm.bytes, fo.bytes, fm.nbytes);
    ASSERT(packet_codec_file_header_flags(fm.header) & PACKET_CODEC_FILE_FLAG_COMPLETE);
    free(fm.bytes);
    free(fo.bytes);
    sb_end();
    PASS();
}

/* Un `.back` hérité (structs bruts, sans en-tête) sort compacté et trié,
 * normalisé comme `import` le normalise. */
TEST legacy_back_is_compacted_and_sorted(void)
{
    enum { N = 200 };
    sb_begin();
    static unsigned order[N];
    shuffled(order, N);
    char in[PATH_MAX], out[PATH_MAX];
    sb_path(in, sizeof in, "legacy.back");
    sb_path(out, sizeof out, "sorted.back");
    FILE *f = fopen(in, "wb");
    ASSERT(f != NULL);
    for (unsigned i = 0; i < N; i++) {
        struct possibility_packet pk;
        tree_packet(&pk, order[i]);
        pk.alloc = 7; /* au sens curseur d'avant la v13 : recompté */
        ASSERT_EQ(1, (int)fwrite(&pk, sizeof pk, 1, f));
    }
    fclose(f);
    ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, out, NULL, NULL));
    file_records_t fr;
    ASSERT_EQ(0, read_back(out, &fr));
    ASSERT(sorted_and_same_multiset(&fr, order, N));
    free(fr.bytes);
    sb_end();
    PASS();
}

/* Refus sans dégât : une sortie existante n'est pas touchée ; une entrée non
 * autonome est refusée si (et seulement si) son `.spillcount` existe ; une
 * entrée tronquée ne laisse ni sortie ni paquet. */
TEST refusals_leave_nothing_behind(void)
{
    enum { N = 300 };
    sb_begin();
    static unsigned order[N];
    shuffled(order, N);
    char in[PATH_MAX], out[PATH_MAX], side[PATH_MAX];
    sb_path(in, sizeof in, "in.back");
    sb_path(out, sizeof out, "out.back");
    ASSERT_EQ(0, write_back(in, order, N, 0));

    FILE *f = fopen(out, "w");
    ASSERT(f != NULL);
    fputs("garde", f);
    fclose(f);
    ASSERT_EQ(BACKUP_SORT_ERR_OUTPUT_EXISTS, backup_sort(in, out, NULL, NULL));
    struct stat sbuf;
    ASSERT_EQ(0, stat(out, &sbuf));
    ASSERT_EQ(5, (int)sbuf.st_size);
    unlink(out);

    snprintf(side, sizeof side, "%s.spillcount", in);
    f = fopen(side, "w");
    ASSERT(f != NULL);
    fputs("1 cliche\n", f);
    fclose(f);
    ASSERT_EQ(BACKUP_SORT_ERR_INCOMPLETE, backup_sort(in, out, NULL, NULL));
    ASSERT(access(out, F_OK) != 0);
    unlink(side);
    /* Sans `.spillcount`, un `.back` non autonome est tout le stock : trié. */
    ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, out, NULL, NULL));
    unlink(out);

    /* Tronqué au milieu d'un enregistrement (corps en clair pour pouvoir
     * couper à l'octet). */
    backup_stream_set_compress_for_tests(0);
    unlink(in);
    ASSERT_EQ(0, write_back(in, order, N, 0));
    backup_stream_set_compress_for_tests(-1);
    ASSERT_EQ(0, stat(in, &sbuf));
    ASSERT_EQ(0, truncate(in, sbuf.st_size - 3));
    backup_sort_options_t small = { .mem_bytes = 8 * 1024, .tmp_dir = g_dir };
    ASSERT_EQ(BACKUP_SORT_ERR_INPUT, backup_sort(in, out, &small, NULL));
    ASSERT_EQ(1, sb_entries()); /* l'entrée seule */
    sb_end();
    PASS();
}

#ifdef ETII_ZSTD
/* La raison d'être : un `.back` mélangé, trié, tient au moins deux fois moins
 * de place sous zstd (à rapprocher des ×5,9 à ×16 mesurés en production). Le
 * stock doit dépasser la fenêtre du flux zstd (quelques Mo) : en deçà, zstd
 * retrouve les voisins même mélangés (×1,65 seulement sur 8 192 plateaux). */
TEST zstd_sorted_back_is_at_least_twice_smaller(void)
{
    enum { N = 65536 };
    sb_begin();
    static unsigned order[N];
    shuffled(order, N);
    char in[PATH_MAX], out[PATH_MAX];
    sb_path(in, sizeof in, "mixed.back");
    sb_path(out, sizeof out, "sorted.back");
    ASSERT_EQ(0, write_back(in, order, N, 0));
    ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, out, NULL, NULL));
    struct stat si, so;
    ASSERT_EQ(0, stat(in, &si));
    ASSERT_EQ(0, stat(out, &so));
    if (getenv("ETII_TEST_VERBOSE") != NULL) {
        fprintf(stderr, "sortback : %lld -> %lld octets\n", (long long)si.st_size, (long long)so.st_size);
    }
    ASSERT(so.st_size * 2 < si.st_size);
    sb_end();
    PASS();
}
#endif

SUITE(backup_sort_suite)
{
    RUN_TEST(record_cmp_orders_by_board_then_header);
    RUN_TEST(many_runs_and_one_run_give_the_same_sorted_file);
    RUN_TEST(legacy_back_is_compacted_and_sorted);
    RUN_TEST(refusals_leave_nothing_behind);
#ifdef ETII_ZSTD
    RUN_TEST(zstd_sorted_back_is_at_least_twice_smaller);
#endif
}
