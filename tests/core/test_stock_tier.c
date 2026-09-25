/*
 * Tests unitaires de stock_tier.c — étage RAM du stock en blocs
 * (docs/conception/etage_ram_compresse.md).
 *
 * Ce que ces tests verrouillent :
 *
 *  1. Aller-retour exact : les octets rendus par `stock_tier_block_unpack`
 *     sont ceux qui ont été empilés, dans le même ordre, et le compte
 *     d'enregistrements est juste — y compris pour des tailles variables.
 *  2. Ordre de pile PAR BLOC : `top` est le plus récent, `bottom` le plus
 *     ancien, `stock_tier_block_above` parcourt du bas vers le haut.
 *  3. Peek puis commit : lire un bloc ne le retire pas ; seul `pop_*` le fait.
 *  4. Un contenu qui ne se pave pas en enregistrements entiers est REFUSÉ à
 *     l'entrée sans toucher la pile, et un bloc corrompu est signalé à la
 *     sortie sans être retiré.
 *  5. Les compteurs (possibilités, octets d'allocateur, blocs) suivent chaque
 *     opération et reviennent à zéro.
 *
 * Aucun état global : chaque test monte sa propre pile. Géométrie-agnostique
 * (builds 256 et 16) : les fixtures passent par tests/packet_fixture.h.
 */
#include "greatest.h"
#include "core/packet_codec.h"
#include "core/possibility.h"
#include "core/stock_tier.h"
#include "packet_fixture.h"

#include <stdlib.h>
#include <string.h>

uint8_t *stock_tier_block_data_for_tests(stock_tier_block_t *block, size_t *stored_bytes);

/* Encode `n` fixtures distinctes (tailles variables : le nombre de pièces
 * posées suit les bits de l'indice) à partir de l'indice `first`, bout à bout
 * dans `raw`. Rend le nombre d'octets écrits, ou 0 si `cap` ne suffit pas. */
static size_t encode_fixtures(uint8_t *raw, size_t cap, unsigned first, int n)
{
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        struct possibility_packet pk;
        fixture_packet_distinct(&pk, first + (unsigned)i);
        size_t written = 0;
        if (packet_codec_encode(&pk, raw + used, cap - used, &written) != 0) {
            return 0;
        }
        used += written;
    }
    return used;
}

/* Remplit `raw` d'enregistrements jusqu'à ce que le suivant ne tienne plus
 * dans `cap`. Rend les octets écrits, et le compte dans `*count`. */
static size_t fill_to(uint8_t *raw, size_t cap, unsigned first, int *count)
{
    size_t used = 0;
    int n = 0;
    for (;;) {
        struct possibility_packet pk;
        fixture_packet_distinct(&pk, first + (unsigned)n);
        size_t len = packet_codec_encoded_size(&pk);
        if (used + len > cap) {
            break;
        }
        size_t written = 0;
        if (packet_codec_encode(&pk, raw + used, cap - used, &written) != 0) {
            break;
        }
        used += written;
        n++;
    }
    *count = n;
    return used;
}

TEST record_len_matches_the_codec_and_refuses_a_short_buffer(void)
{
    struct possibility_packet pk;
    fixture_packet(&pk, 5);
    uint8_t rec[PACKET_CODEC_MAX_BYTES];
    size_t written = 0;
    ASSERT_EQ(0, packet_codec_encode(&pk, rec, sizeof(rec), &written));

    ASSERT_EQ_FMT(written, stock_tier_record_len(rec, written), "%zu");
    ASSERT_EQ_FMT(written, stock_tier_record_len(rec, sizeof(rec)), "%zu");
    /* Un octet de moins : l'enregistrement déborde, jamais une longueur
     * plausible qui ferait lire au-delà. */
    ASSERT_EQ_FMT((size_t)0, stock_tier_record_len(rec, written - 1), "%zu");
    ASSERT_EQ_FMT((size_t)0, stock_tier_record_len(rec, 3), "%zu");
    PASS();
}

TEST push_then_unpack_roundtrips_bytes_and_count(void)
{
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    size_t used = encode_fixtures(raw, sizeof(raw), 1, 40);
    ASSERT(used > 0);

    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);
    ASSERT_EQ(40, stock_tier_push(&stack, raw, used));

    const stock_tier_block_t *b = stock_tier_top(&stack);
    ASSERT(b != NULL);
    ASSERT_EQ_FMT(40u, stock_tier_block_records(b), "%u");
    ASSERT_EQ_FMT(used, stock_tier_block_raw_bytes(b), "%zu");

    uint8_t out[STOCK_TIER_BLOCK_BYTES];
    ASSERT_EQ(40, stock_tier_block_unpack(b, out, sizeof(out)));
    ASSERT_MEM_EQ(raw, out, used);

    /* Et chaque enregistrement relu décode vers la fixture d'origine. */
    size_t off = 0;
    for (int i = 0; i < 40; i++) {
        struct possibility_packet want, got;
        fixture_packet_distinct(&want, 1u + (unsigned)i);
        size_t len = stock_tier_record_len(out + off, used - off);
        ASSERT(len > 0);
        ASSERT_EQ(0, packet_codec_decode(out + off, len, &got, NULL));
        ASSERT_EQ(want.alloc, got.alloc);
        ASSERT_MEM_EQ(want.grid, got.grid, sizeof(want.grid));
        off += len;
    }
    ASSERT_EQ_FMT(used, off, "%zu");

    stock_tier_stack_clear(&stack);
    PASS();
}

TEST unpack_does_not_remove_and_pop_does(void)
{
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    size_t used = encode_fixtures(raw, sizeof(raw), 3, 10);
    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);
    ASSERT_EQ(10, stock_tier_push(&stack, raw, used));

    uint8_t out[STOCK_TIER_BLOCK_BYTES];
    ASSERT_EQ(10, stock_tier_block_unpack(stock_tier_top(&stack), out, sizeof(out)));
    ASSERT_EQ(10, stock_tier_block_unpack(stock_tier_top(&stack), out, sizeof(out)));
    ASSERT_EQ_FMT(10ULL, stack.records, "%llu");
    ASSERT_EQ_FMT(1ULL, stack.blocks, "%llu");

    stock_tier_pop_top(&stack);
    ASSERT(stock_tier_top(&stack) == NULL);
    ASSERT(stock_tier_bottom(&stack) == NULL);
    ASSERT_EQ_FMT(0ULL, stack.records, "%llu");
    /* Dépiler une pile vide ne fait rien. */
    stock_tier_pop_top(&stack);
    stock_tier_pop_bottom(&stack);
    ASSERT_EQ_FMT(0ULL, stack.blocks, "%llu");
    PASS();
}

/* Chaque bloc porte un nombre d'enregistrements différent, ce qui suffit à
 * l'identifier. */
TEST stack_order_is_per_block_lifo_with_a_bottom_up_walk(void)
{
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);
    for (int k = 1; k <= 4; k++) {
        size_t used = encode_fixtures(raw, sizeof(raw), (unsigned)(k * 100), k);
        ASSERT_EQ(k, stock_tier_push(&stack, raw, used));
    }

    ASSERT_EQ_FMT(4u, stock_tier_block_records(stock_tier_top(&stack)), "%u");
    ASSERT_EQ_FMT(1u, stock_tier_block_records(stock_tier_bottom(&stack)), "%u");

    unsigned expect = 1;
    for (const stock_tier_block_t *b = stock_tier_bottom(&stack); b != NULL; b = stock_tier_block_above(b)) {
        ASSERT_EQ_FMT(expect, stock_tier_block_records(b), "%u");
        expect++;
    }
    ASSERT_EQ_FMT(5u, expect, "%u");

    stock_tier_pop_top(&stack);    /* retire 4 */
    stock_tier_pop_bottom(&stack); /* retire 1 */
    ASSERT_EQ_FMT(3u, stock_tier_block_records(stock_tier_top(&stack)), "%u");
    ASSERT_EQ_FMT(2u, stock_tier_block_records(stock_tier_bottom(&stack)), "%u");
    ASSERT_EQ_FMT(5ULL, stack.records, "%llu");
    ASSERT_EQ_FMT(2ULL, stack.blocks, "%llu");

    stock_tier_pop_bottom(&stack);
    ASSERT(stock_tier_top(&stack) == stock_tier_bottom(&stack));
    ASSERT(stock_tier_block_above(stock_tier_bottom(&stack)) == NULL);

    stock_tier_stack_clear(&stack);
    PASS();
}

TEST push_refuses_raw_that_does_not_tile_without_touching_the_stack(void)
{
    static uint8_t raw[STOCK_TIER_BLOCK_BYTES + PACKET_CODEC_MAX_BYTES];
    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);
    size_t used = encode_fixtures(raw, sizeof(raw), 7, 3);
    ASSERT_EQ(3, stock_tier_push(&stack, raw, used));
    unsigned long long bytes = stack.bytes;

    /* Vide, NULL, tronqué d'un octet, ou un octet de trop. */
    ASSERT_EQ(-1, stock_tier_push(&stack, raw, 0));
    ASSERT_EQ(-1, stock_tier_push(&stack, NULL, used));
    ASSERT_EQ(-1, stock_tier_push(&stack, raw, used - 1));
    ASSERT_EQ(-1, stock_tier_push(&stack, raw, used + 1));

    /* Plus grand qu'un bloc, même fait d'enregistrements entiers. */
    int n = 0;
    size_t big = fill_to(raw, sizeof(raw), 1, &n);
    ASSERT(big > STOCK_TIER_BLOCK_BYTES);
    ASSERT_EQ(-1, stock_tier_push(&stack, raw, big));

    ASSERT_EQ_FMT(3ULL, stack.records, "%llu");
    ASSERT_EQ_FMT(1ULL, stack.blocks, "%llu");
    ASSERT_EQ_FMT(bytes, stack.bytes, "%llu");
    stock_tier_stack_clear(&stack);
    PASS();
}

TEST unpack_refuses_a_buffer_too_small(void)
{
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    size_t used = encode_fixtures(raw, sizeof(raw), 9, 5);
    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);
    ASSERT_EQ(5, stock_tier_push(&stack, raw, used));

    uint8_t out[STOCK_TIER_BLOCK_BYTES];
    ASSERT_EQ(-1, stock_tier_block_unpack(stock_tier_top(&stack), out, used - 1));
    ASSERT_EQ(5, stock_tier_block_unpack(stock_tier_top(&stack), out, used));
    stock_tier_stack_clear(&stack);
    PASS();
}

/* Un bitmap tout à 1 annonce un plateau plein, donc un enregistrement de
 * taille maximale : il ne tient plus dans ce qui reste du bloc, le pavage
 * casse — quelle que soit la géométrie compilée. */
TEST unpack_detects_a_corrupt_block_and_leaves_it_in_place(void)
{
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    size_t used = encode_fixtures(raw, sizeof(raw), 11, 6);
    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);
    ASSERT_EQ(6, stock_tier_push(&stack, raw, used));

    size_t last = 0, off = 0;
    for (int i = 0; i < 6; i++) {
        last = off;
        off += stock_tier_record_len(raw + off, used - off);
    }
    size_t stored = 0;
    uint8_t *data = stock_tier_block_data_for_tests((stock_tier_block_t *)stock_tier_top(&stack), &stored);
    ASSERT_EQ_FMT(used, stored, "%zu");
    memset(data + last + PACKET_CODEC_HEADER_BYTES, 0xFF, PACKET_CODEC_BITMAP_BYTES);

    uint8_t out[STOCK_TIER_BLOCK_BYTES];
    ASSERT_EQ(-1, stock_tier_block_unpack(stock_tier_top(&stack), out, sizeof(out)));
    ASSERT_EQ_FMT(6ULL, stack.records, "%llu");
    ASSERT_EQ_FMT(1ULL, stack.blocks, "%llu");
    stock_tier_stack_clear(&stack);
    PASS();
}

/* Le compteur d'octets est ce que le plafond RAM comptera : la charge utile,
 * plus un en-tête et un surcoût d'allocateur PAR BLOC — jamais par
 * possibilité, c'est tout l'intérêt de l'étage. */
TEST byte_counter_is_per_block_and_returns_to_zero(void)
{
    uint8_t raw[STOCK_TIER_BLOCK_BYTES];
    stock_tier_stack_t stack;
    stock_tier_stack_init(&stack);

    int n1 = 0, n2 = 0;
    size_t u1 = fill_to(raw, sizeof(raw), 1, &n1);
    ASSERT_EQ(n1, stock_tier_push(&stack, raw, u1));
    size_t u2 = encode_fixtures(raw, sizeof(raw), 5000, 3);
    n2 = 3;
    ASSERT_EQ(n2, stock_tier_push(&stack, raw, u2));

    ASSERT_EQ_FMT((unsigned long long)(n1 + n2), stack.records, "%llu");
    ASSERT_EQ_FMT(2ULL, stack.blocks, "%llu");
    ASSERT(stack.bytes > u1 + u2);
    /* Au plus 128 octets de frais par bloc, quel que soit leur nombre
     * d'enregistrements — un maillon en coûte déjà plus de 40 à lui seul. */
    ASSERT(stack.bytes <= u1 + u2 + 2 * 128);

    stock_tier_pop_bottom(&stack);
    ASSERT_EQ_FMT((unsigned long long)n2, stack.records, "%llu");
    ASSERT(stack.bytes > u2 && stack.bytes <= u2 + 128);

    stock_tier_stack_clear(&stack);
    ASSERT_EQ_FMT(0ULL, stack.records, "%llu");
    ASSERT_EQ_FMT(0ULL, stack.bytes, "%llu");
    ASSERT_EQ_FMT(0ULL, stack.blocks, "%llu");
    ASSERT(stock_tier_top(&stack) == NULL);
    PASS();
}

SUITE(stock_tier_suite)
{
    RUN_TEST(record_len_matches_the_codec_and_refuses_a_short_buffer);
    RUN_TEST(push_then_unpack_roundtrips_bytes_and_count);
    RUN_TEST(unpack_does_not_remove_and_pop_does);
    RUN_TEST(stack_order_is_per_block_lifo_with_a_bottom_up_walk);
    RUN_TEST(push_refuses_raw_that_does_not_tile_without_touching_the_stack);
    RUN_TEST(unpack_refuses_a_buffer_too_small);
    RUN_TEST(unpack_detects_a_corrupt_block_and_leaves_it_in_place);
    RUN_TEST(byte_counter_is_per_block_and_returns_to_zero);
}
