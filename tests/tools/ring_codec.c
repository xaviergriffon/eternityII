/* Implémentation du format compact « packed6 » — voir ring_codec.h pour le
   raisonnement (rotation implicite, 6 bits par pièce, accès aléatoire). */
#include "tools/ring_codec.h"

#include <string.h>

int ring_codec_build_table(struct array_part *all_rotate_parts, struct ring_codec_table *table)
{
    memset(table, 0, sizeof *table);
    for (int i = 0; i <= ETERN_PARTS; i++) {
        table->index_of[i] = -1;
    }
    int n = (all_rotate_parts->size - 1) / 4;
    for (int id = 1; id <= n && id <= ETERN_PARTS; id++) {
        /* rotation 0 = pièce d'origine, comme bd_build_classes */
        struct part *p = &all_rotate_parts->parts[id];
        int8_t v[4] = { p->top, p->right, p->bottom, p->left };
        int zeros = 0;
        for (int k = 0; k < 4; k++) {
            if (v[k] == 0) {
                zeros++;
            }
        }
        if (zeros != 1 && zeros != 2) {
            continue; /* pièce intérieure, ou forme inattendue */
        }
        if (table->count == RING_CODEC_MAX_BORDER_PIECES) {
            return -1; /* plus de 64 : un index de 6 bits ne suffit plus */
        }
        table->id[table->count] = (uint16_t)id;
        table->index_of[id] = (int16_t)table->count;
        table->count++;
    }
    return 0;
}

static void put_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *b)
{
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

void ring_codec_write_header(uint8_t *buf, const struct ring_codec_table *table)
{
    memset(buf, 0, RING_CODEC_HEADER_BYTES);
    memcpy(buf, RING_CODEC_MAGIC, 8);
    put_u16(buf + 8, RING_CODEC_VERSION);
    put_u16(buf + 10, (uint16_t)ETERN_SIZE);
    put_u16(buf + 12, (uint16_t)BORDER_RING_LEN);
    put_u16(buf + 14, (uint16_t)RING_CODEC_PACKED_BYTES);
    put_u16(buf + 16, (uint16_t)table->count);
    for (int i = 0; i < table->count; i++) {
        put_u16(buf + 18 + 2 * i, table->id[i]);
    }
}

int ring_codec_read_header(const uint8_t *buf, struct ring_codec_table *table)
{
    if (memcmp(buf, RING_CODEC_MAGIC, 8) != 0 || get_u16(buf + 8) != RING_CODEC_VERSION ||
        get_u16(buf + 10) != (uint16_t)ETERN_SIZE || get_u16(buf + 12) != (uint16_t)BORDER_RING_LEN ||
        get_u16(buf + 14) != (uint16_t)RING_CODEC_PACKED_BYTES) {
        return -1;
    }
    int count = (int)get_u16(buf + 16);
    if (count <= 0 || count > RING_CODEC_MAX_BORDER_PIECES) {
        return -1;
    }
    memset(table, 0, sizeof *table);
    for (int i = 0; i <= ETERN_PARTS; i++) {
        table->index_of[i] = -1;
    }
    table->count = count;
    for (int i = 0; i < count; i++) {
        uint16_t id = get_u16(buf + 18 + 2 * i);
        if (id == 0 || id > ETERN_PARTS) {
            return -1;
        }
        table->id[i] = id;
        table->index_of[id] = (int16_t)i;
    }
    /* 18 + 2*64 = 146 <= RING_CODEC_HEADER_BYTES : l'en-tête ne peut pas
       déborder, quelle que soit la taille de la table. */
    return 0;
}

/* Écrit `value` (6 bits) à la position `slot` du flux compacté. */
static void put6(uint8_t *out, int slot, unsigned value)
{
    size_t bit = (size_t)slot * 6;
    size_t byte = bit >> 3;
    unsigned shift = (unsigned)(bit & 7);
    /* Le champ peut chevaucher deux octets (jamais trois : 6 + 7 < 16). */
    out[byte] = (uint8_t)((out[byte] & ~(0x3F << shift)) | ((value & 0x3F) << shift));
    if (shift > 2) {
        out[byte + 1] = (uint8_t)((out[byte + 1] & ~(0x3F >> (8 - shift))) | ((value & 0x3F) >> (8 - shift)));
    }
}

static unsigned get6(const uint8_t *in, int slot)
{
    size_t bit = (size_t)slot * 6;
    size_t byte = bit >> 3;
    unsigned shift = (unsigned)(bit & 7);
    unsigned v = (unsigned)(in[byte] >> shift);
    if (shift > 2) {
        v |= (unsigned)in[byte + 1] << (8 - shift);
    }
    return v & 0x3F;
}

int ring_codec_pack(const struct possibility_packet *ring, const struct ring_codec_table *table,
                    uint8_t *out)
{
    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(order);
    memset(out, 0, RING_CODEC_PACKED_BYTES);

    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int16_t v = ring->grid[order[i][0]][order[i][1]];
        if (v < 0) {
            return -1; /* case du pourtour vide */
        }
        int base = ((v - 1) % ETERN_PARTS) + 1;
        if (base < 1 || base > ETERN_PARTS || table->index_of[base] < 0) {
            return -1; /* pièce absente de la table de bordure */
        }
        put6(out, i, (unsigned)table->index_of[base]);
    }
    return 0;
}

int ring_codec_unpack(const uint8_t *in, const struct ring_codec_table *table,
                      struct array_part *all_rotate_parts, struct possibility_packet *ring)
{
    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(order);

    memset(ring, 0, sizeof *ring);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            ring->grid[x][y] = -2;
        }
    }
    ring->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    for (int i = 0; i < BORDER_RING_LEN; i++) {
        unsigned idx = get6(in, i);
        if ((int)idx >= table->count) {
            return -1;
        }
        uint16_t id = table->id[idx];
        int x = order[i][0], y = order[i][1];

        /* Rotation retrouvée, pas stockée : la seule qui mette une face nulle
           face à chaque bord du plateau adjacent à cette case. Unique par
           construction (cf. ring_codec.h) ; aucune candidate = octets faux. */
        int found = -1;
        for (uint8_t rot = 0; rot < 4; rot++) {
            struct part *p = &all_rotate_parts->parts[id_for_rotated_part(id, rot)];
            int ok = 1;
            if (y == 0 && p->top != 0) ok = 0;
            if (x == ETERN_SIZE - 1 && p->right != 0) ok = 0;
            if (y == ETERN_SIZE - 1 && p->bottom != 0) ok = 0;
            if (x == 0 && p->left != 0) ok = 0;
            if (ok) {
                found = (int)rot;
                break;
            }
        }
        if (found < 0) {
            return -1;
        }
        ring->grid[x][y] = (int16_t)id_for_rotated_part(id, (uint8_t)found);
        set_face_used(ring->b_faceused, (uint16_t)(id - 1), 1);
    }
    ring->alloc = (uint16_t)BORDER_RING_LEN;
    return 0;
}
