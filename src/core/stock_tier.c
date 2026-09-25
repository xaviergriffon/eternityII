#include <stdlib.h>
#include <string.h>

#include "core/packet_codec.h"
#include "core/stock_tier.h"

/// Octets à lire avant de connaître la longueur d'un enregistrement.
#define STOCK_TIER_RECORD_PREFIX (PACKET_CODEC_HEADER_BYTES + PACKET_CODEC_BITMAP_BYTES)

struct stock_tier_block {
	struct stock_tier_block *below;
	struct stock_tier_block *above;
	unsigned long long seq;
	uint32_t records;
	uint32_t raw_bytes;
	uint32_t stored_bytes;
	uint8_t codec;
	uint8_t data[];
};

/// Ce que `block` coûte à l'allocateur, compté dans `stack->bytes`.
static unsigned long long block_cost(const stock_tier_block_t *block)
{
	return (unsigned long long)sizeof(*block) + block->stored_bytes + STOCK_TIER_MALLOC_OVERHEAD;
}

void stock_tier_stack_init(stock_tier_stack_t *stack)
{
	memset(stack, 0, sizeof(*stack));
}

void stock_tier_stack_clear(stock_tier_stack_t *stack)
{
	stock_tier_block_t *b = stack->bottom;
	while (b != NULL) {
		stock_tier_block_t *above = b->above;
		free(b);
		b = above;
	}
	stock_tier_stack_init(stack);
}

size_t stock_tier_record_len(const uint8_t *rec, size_t avail)
{
	if (avail < STOCK_TIER_RECORD_PREFIX) {
		return 0;
	}
	uint16_t placed = packet_codec_peek_placed(rec, avail);
	size_t len = STOCK_TIER_RECORD_PREFIX + PACKET_CODEC_VALUE_BYTES(placed);
	return len <= avail ? len : 0;
}

int stock_tier_count_records(const uint8_t *raw, size_t raw_bytes)
{
	if (raw == NULL || raw_bytes == 0 || raw_bytes > STOCK_TIER_BLOCK_BYTES) {
		return -1;
	}
	size_t off = 0;
	int n = 0;
	while (off < raw_bytes) {
		size_t len = stock_tier_record_len(raw + off, raw_bytes - off);
		if (len == 0) {
			return -1;
		}
		off += len;
		n++;
	}
	return n;
}

int stock_tier_push(stock_tier_stack_t *stack, const uint8_t *raw, size_t raw_bytes)
{
	int n = stock_tier_count_records(raw, raw_bytes);
	if (n < 0) {
		return -1;
	}
	stock_tier_block_t *b = malloc(sizeof(*b) + raw_bytes);
	if (b == NULL) {
		return -1;
	}
	b->seq = ++stack->last_seq;
	b->records = (uint32_t)n;
	b->raw_bytes = (uint32_t)raw_bytes;
	b->stored_bytes = (uint32_t)raw_bytes;
	b->codec = STOCK_TIER_CODEC_RAW;
	memcpy(b->data, raw, raw_bytes);

	b->below = stack->top;
	b->above = NULL;
	if (stack->top != NULL) {
		stack->top->above = b;
	} else {
		stack->bottom = b;
	}
	stack->top = b;
	stack->records += (unsigned long long)n;
	stack->bytes += block_cost(b);
	stack->blocks++;
	return n;
}

const stock_tier_block_t *stock_tier_top(const stock_tier_stack_t *stack)
{
	return stack->top;
}

const stock_tier_block_t *stock_tier_bottom(const stock_tier_stack_t *stack)
{
	return stack->bottom;
}

const stock_tier_block_t *stock_tier_block_above(const stock_tier_block_t *block)
{
	return block->above;
}

unsigned long long stock_tier_block_seq(const stock_tier_block_t *block)
{
	return block->seq;
}

uint32_t stock_tier_block_records(const stock_tier_block_t *block)
{
	return block->records;
}

size_t stock_tier_block_raw_bytes(const stock_tier_block_t *block)
{
	return block->raw_bytes;
}

int stock_tier_block_unpack(const stock_tier_block_t *block, uint8_t *out, size_t cap)
{
	if (block == NULL || out == NULL || cap < block->raw_bytes || block->codec != STOCK_TIER_CODEC_RAW
	    || block->stored_bytes != block->raw_bytes) {
		return -1;
	}
	memcpy(out, block->data, block->raw_bytes);
	int n = stock_tier_count_records(out, block->raw_bytes);
	return (n >= 0 && (uint32_t)n == block->records) ? n : -1;
}

/// Détache `b` de `stack` et le libère, compteurs compris.
static void stock_tier_unlink(stock_tier_stack_t *stack, stock_tier_block_t *b)
{
	if (b->below != NULL) {
		b->below->above = b->above;
	} else {
		stack->bottom = b->above;
	}
	if (b->above != NULL) {
		b->above->below = b->below;
	} else {
		stack->top = b->below;
	}
	stack->records -= b->records;
	stack->bytes -= block_cost(b);
	stack->blocks--;
	free(b);
}

void stock_tier_pop_top(stock_tier_stack_t *stack)
{
	if (stack->top != NULL) {
		stock_tier_unlink(stack, stack->top);
	}
}

void stock_tier_pop_bottom(stock_tier_stack_t *stack)
{
	if (stack->bottom != NULL) {
		stock_tier_unlink(stack, stack->bottom);
	}
}

void stock_tier_remove(stock_tier_stack_t *stack, const stock_tier_block_t *block)
{
	if (block != NULL) {
		stock_tier_unlink(stack, (stock_tier_block_t *)block);
	}
}

// Réservée aux tests (non déclarée dans stock_tier.h) : les octets stockés
// d'un bloc, pour y simuler une corruption que `stock_tier_block_unpack` doit
// détecter.
uint8_t *stock_tier_block_data_for_tests(stock_tier_block_t *block, size_t *stored_bytes)
{
	*stored_bytes = block->stored_bytes;
	return block->data;
}
