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
 *  6. Le parallélisme ne change rien : même sortie à 1, 3 ou 8 fils ; un corps
 *     zstd fait de nombreuses trames — coupées au milieu d'un enregistrement,
 *     sans taille annoncée — est relu dans l'ordre ; un corps zstd tronqué est
 *     refusé.
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
#include <sys/resource.h>
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

/* Vrai si les enregistrements de `fr` sont EXACTEMENT les formes canoniques de
 * `ids[0..n)` triées par la clé. Deux enregistrements égaux pour la clé étant
 * identiques à l'octet, cet ordre est unique : on le construit (tri des
 * attendus, n log n) et on compare octet pour octet. Une première version
 * cherchait chaque enregistrement dans toute la liste en réencodant (n²) :
 * 46 M d'encodages à n = 9 600, et gcovr refusait en CI un compteur de ligne
 * de 6,6 milliards (« suspicious hits »). */
typedef struct {
    uint8_t rec[PACKET_CODEC_MAX_BYTES];
    size_t len;
} expected_rec_t;

static int expected_cmp(const void *a, const void *b)
{
    const expected_rec_t *x = a, *y = b;
    return backup_sort_record_cmp(x->rec, x->len, y->rec, y->len);
}

static int sorted_and_same_multiset(const file_records_t *fr, const unsigned *ids, unsigned n)
{
    if (fr->count != n) {
        return 0;
    }
    expected_rec_t *want = malloc((size_t)n * sizeof *want);
    if (want == NULL) {
        return 0;
    }
    for (unsigned k = 0; k < n; k++) {
        want[k].len = canonical(ids[k], want[k].rec);
    }
    qsort(want, n, sizeof *want, expected_cmp);
    const uint8_t *p = fr->bytes;
    int ok = 1;
    for (unsigned i = 0; i < n && ok; i++) {
        size_t len = rec_len(p);
        ok = (len == want[i].len && memcmp(p, want[i].rec, len) == 0);
        p += len;
    }
    free(want);
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

/* Le nombre de fils (tri des tampons, fusion par plages) ne change pas le
 * contenu de la sortie, paquets multiples compris. */
TEST thread_count_does_not_change_the_output(void)
{
    enum { N = 2000 };
    sb_begin();
    static unsigned order[N];
    shuffled(order, N);
    char in[PATH_MAX], out[PATH_MAX];
    sb_path(in, sizeof in, "in.back");
    ASSERT_EQ(0, write_back(in, order, N, PACKET_CODEC_FILE_FLAG_COMPLETE));
    file_records_t ref = { 0 };
    int threads[] = { 1, 3, 8 };
    for (int i = 0; i < 3; i++) {
        snprintf(out, sizeof out, "%s/out%d.back", g_dir, threads[i]);
        backup_sort_options_t o = { .mem_bytes = 8 * 1024, .tmp_dir = g_dir, .threads = threads[i] };
        backup_sort_stats_t st;
        ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, out, &o, &st));
        ASSERT(st.runs > 2);
        file_records_t fr;
        ASSERT_EQ(0, read_back(out, &fr));
        if (i == 0) {
            ASSERT(sorted_and_same_multiset(&fr, order, N));
            ref = fr;
        } else {
            ASSERT_EQ_FMT(ref.nbytes, fr.nbytes, "%zu");
            ASSERT_MEM_EQ(ref.bytes, fr.bytes, ref.nbytes);
            free(fr.bytes);
        }
    }
    free(ref.bytes);
    ASSERT_EQ(4, sb_entries()); /* l'entrée et les trois sorties, rien d'autre */
    sb_end();
    PASS();
}

/* Deux fichiers ouverts par paquet jusqu'à la fin de la fusion : sous une
 * limite souple basse (1 024 dans un terminal), le tri du niveau 27 mourait au
 * 510e paquet (« Too many open files »). Il relève lui-même sa limite souple
 * jusqu'à la dure. Ici : limite souple ramenée sous ce que demandent ~150
 * paquets ; le tri doit réussir. Contre-épreuve : sans le relèvement, il
 * échoue en `BACKUP_SORT_ERR_IO`. */
TEST raises_the_soft_open_files_limit(void)
{
    enum { N = 150 * 64 };
    struct rlimit rl;
    ASSERT_EQ(0, getrlimit(RLIMIT_NOFILE, &rl));
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < 1024) {
        SKIPm("limite dure de fichiers ouverts trop basse pour le test");
    }
    sb_begin();
    static unsigned order[N];
    shuffled(order, N);
    char in[PATH_MAX], out[PATH_MAX];
    sb_path(in, sizeof in, "in.back");
    sb_path(out, sizeof out, "out.back");
    ASSERT_EQ(0, write_back(in, order, N, 0));
    struct rlimit low = rl;
    low.rlim_cur = 200; /* ~150 paquets demandent ~300 fichiers */
    ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &low));
    backup_sort_options_t o = { .mem_bytes = 8 * 1024, .tmp_dir = g_dir, .threads = 2 };
    backup_sort_stats_t st;
    int rc = backup_sort(in, out, &o, &st);
    struct rlimit after;
    getrlimit(RLIMIT_NOFILE, &after);
    setrlimit(RLIMIT_NOFILE, &rl);
    ASSERT_EQ(BACKUP_SORT_OK, rc);
    ASSERT(st.runs >= 140);
    ASSERT(after.rlim_cur > 200);
    file_records_t fr;
    ASSERT_EQ(0, read_back(out, &fr));
    ASSERT(sorted_and_same_multiset(&fr, order, N));
    free(fr.bytes);
    sb_end();
    PASS();
}

#ifdef ETII_ZSTD
#include <zstd.h>

/* Corps zstd en trames indépendantes, comme un `.back` qui recopie les blocs
 * de l'étage : la lecture les décompresse en parallèle et doit les rendre
 * DANS L'ORDRE, y compris une trame coupée au milieu d'un enregistrement et
 * des trames sans taille annoncée (écrites en flux). */
TEST zstd_multi_frame_input_is_read_in_order(void)
{
    enum { N = 3000 };
    sb_begin();
    static unsigned order[N];
    shuffled(order, N);
    size_t cap = (size_t)N * PACKET_CODEC_MAX_BYTES, used = 0;
    uint8_t *body = malloc(cap);
    ASSERT(body != NULL);
    for (unsigned i = 0; i < N; i++) {
        used += canonical(order[i], body + used);
    }
    char in[PATH_MAX], out[PATH_MAX];
    sb_path(in, sizeof in, "frames.back");
    sb_path(out, sizeof out, "sorted.back");
    FILE *f = fopen(in, "wb");
    ASSERT(f != NULL);
    uint8_t header[PACKET_CODEC_FILE_HEADER_BYTES];
    packet_codec_write_file_header_codec(header, 0, 1);
    ASSERT_EQ(1, (int)fwrite(header, sizeof header, 1, f));
    uint8_t *frame = malloc(ZSTD_compressBound(cap));
    size_t off = 0;
    int k = 0, unknown = 0;
    while (off < used) {
        /* ~40 trames quel que soit le build (enregistrements de 22 octets en
         * build 16, ~76 en 256), de tailles irrégulières : presque jamais
         * alignées sur un enregistrement. */
        size_t base = used / 60 + 1;
        size_t n = base + (size_t)(k * 7919) % base;
        if (n > used - off) {
            n = used - off;
        }
        size_t z;
        if (k % 3 == 2) {
            /* En flux, sans taille annoncée. */
            ZSTD_CCtx *c = ZSTD_createCCtx();
            ZSTD_outBuffer ob = { frame, ZSTD_compressBound(n), 0 };
            ZSTD_inBuffer ib = { body + off, n, 0 };
            size_t r = ZSTD_compressStream2(c, &ob, &ib, ZSTD_e_continue);
            ASSERT(!ZSTD_isError(r));
            do {
                r = ZSTD_compressStream2(c, &ob, &ib, ZSTD_e_end);
                ASSERT(!ZSTD_isError(r));
            } while (r != 0);
            ZSTD_freeCCtx(c);
            z = ob.pos;
            ASSERT_EQ(ZSTD_CONTENTSIZE_UNKNOWN, ZSTD_getFrameContentSize(frame, z));
            unknown++;
        } else {
            z = ZSTD_compress(frame, ZSTD_compressBound(n), body + off, n, 1);
            ASSERT(!ZSTD_isError(z));
        }
        ASSERT_EQ(1, (int)fwrite(frame, z, 1, f));
        off += n;
        k++;
    }
    fclose(f);
    ASSERT(k > 20 && unknown > 5);
    backup_sort_options_t o = { .mem_bytes = 32 * 1024, .tmp_dir = g_dir, .threads = 4 };
    ASSERT_EQ(BACKUP_SORT_OK, backup_sort(in, out, &o, NULL));
    file_records_t fr;
    ASSERT_EQ(0, read_back(out, &fr));
    ASSERT(sorted_and_same_multiset(&fr, order, N));
    free(fr.bytes);

    /* Le même corps tronqué au milieu d'une trame : refusé, rien ne reste. */
    unlink(out);
    struct stat sbuf;
    ASSERT_EQ(0, stat(in, &sbuf));
    ASSERT_EQ(0, truncate(in, sbuf.st_size - 7));
    ASSERT_EQ(BACKUP_SORT_ERR_INPUT, backup_sort(in, out, &o, NULL));
    ASSERT_EQ(1, sb_entries());
    free(frame);
    free(body);
    sb_end();
    PASS();
}

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
    RUN_TEST(thread_count_does_not_change_the_output);
    RUN_TEST(raises_the_soft_open_files_limit);
#ifdef ETII_ZSTD
    RUN_TEST(zstd_multi_frame_input_is_read_in_order);
    RUN_TEST(zstd_sorted_back_is_at_least_twice_smaller);
#endif
}
