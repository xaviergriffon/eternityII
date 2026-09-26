#include <stdlib.h>
#include <string.h>

#include "core/packet_codec.h"
#include "core/stock_tier.h"

#ifdef ETII_ZSTD
#include <zstd.h>
#endif

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

/// Codec appliqué aux blocs empilés désormais : zstd s'il est compilé
/// (`make ZSTD=1`), sinon brut. Seuls les tests en changent.
#ifdef ETII_ZSTD
static int g_codec = STOCK_TIER_CODEC_ZSTD;
#else
static int g_codec = STOCK_TIER_CODEC_RAW;
#endif

const char *stock_tier_compression(void)
{
	return (g_codec == STOCK_TIER_CODEC_ZSTD) ? "zstd -1" : "aucune";
}

// Réservée aux tests (non déclarée dans stock_tier.h) : force le codec des
// prochains blocs. Refuse zstd s'il n'est pas compilé.
int stock_tier_set_codec_for_tests(int codec)
{
#ifndef ETII_ZSTD
	if (codec == STOCK_TIER_CODEC_ZSTD) {
		return -1;
	}
#endif
	g_codec = codec;
	return 0;
}

#ifdef ETII_ZSTD
/*
 * Contextes zstd PAR THREAD, créés au premier usage et jamais libérés : un
 * contexte par appel coûterait une allocation de plusieurs centaines de Kio
 * par bloc. Seuls les threads qui déplacent des blocs en créent (débordement,
 * expansion, import) ; les threads de connexion n'y touchent pas. Pas de
 * tableau `__thread` statique : il serait réservé dans CHAQUE thread du
 * serveur, y compris les centaines qui n'en ont pas l'usage.
 */
static __thread ZSTD_CCtx *tl_cctx = NULL;
static __thread ZSTD_DCtx *tl_dctx = NULL;

/// Compresse `raw` dans `dst` (au moins `ZSTD_compressBound(raw_bytes)`
/// octets). Somme de contrôle de trame activée (4 octets par bloc) : un bloc
/// abîmé en mémoire est refusé à la relecture plutôt que rendu faux mais de
/// la bonne taille. @return La taille compressée, 0 sur échec.
static size_t tier_zstd_compress(uint8_t *dst, size_t cap, const uint8_t *raw, size_t raw_bytes)
{
	if (tl_cctx == NULL) {
		tl_cctx = ZSTD_createCCtx();
		if (tl_cctx == NULL
		    || ZSTD_isError(ZSTD_CCtx_setParameter(tl_cctx, ZSTD_c_compressionLevel, STOCK_TIER_ZSTD_LEVEL))
		    || ZSTD_isError(ZSTD_CCtx_setParameter(tl_cctx, ZSTD_c_checksumFlag, 1))) {
			ZSTD_freeCCtx(tl_cctx);
			tl_cctx = NULL;
			return 0;
		}
	}
	size_t r = ZSTD_compress2(tl_cctx, dst, cap, raw, raw_bytes);
	return ZSTD_isError(r) ? 0 : r;
}

/// @return 0 si `in` se décompresse en EXACTEMENT `raw_bytes` octets.
static int tier_zstd_decompress(uint8_t *out, size_t raw_bytes, const uint8_t *in, size_t in_bytes)
{
	if (tl_dctx == NULL) {
		tl_dctx = ZSTD_createDCtx();
		if (tl_dctx == NULL) {
			return -1;
		}
	}
	size_t r = ZSTD_decompressDCtx(tl_dctx, out, raw_bytes, in, in_bytes);
	return (!ZSTD_isError(r) && r == raw_bytes) ? 0 : -1;
}
#endif

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

size_t stock_tier_pack_bound(size_t raw_bytes)
{
#ifdef ETII_ZSTD
	if (g_codec == STOCK_TIER_CODEC_ZSTD) {
		size_t bound = ZSTD_compressBound(raw_bytes);
		return bound > raw_bytes ? bound : raw_bytes;
	}
#endif
	return raw_bytes;
}

size_t stock_tier_pack(const uint8_t *raw, size_t raw_bytes, uint8_t *dst, size_t cap, int *codec, int *records)
{
	int n = stock_tier_count_records(raw, raw_bytes);
	if (n < 0 || dst == NULL) {
		return 0;
	}
#ifdef ETII_ZSTD
	if (g_codec == STOCK_TIER_CODEC_ZSTD) {
		size_t z = tier_zstd_compress(dst, cap, raw, raw_bytes);
		// Gardé compressé seulement s'il y gagne : un bloc incompressible
		// reste brut, jamais plus gros que ses octets.
		if (z > 0 && z < raw_bytes) {
			*codec = STOCK_TIER_CODEC_ZSTD;
			*records = n;
			return z;
		}
	}
#endif
	if (cap < raw_bytes) {
		return 0;
	}
	memcpy(dst, raw, raw_bytes);
	*codec = STOCK_TIER_CODEC_RAW;
	*records = n;
	return raw_bytes;
}

int stock_tier_unpack(int codec, const uint8_t *stored, size_t stored_bytes, size_t raw_bytes, uint32_t records,
                      uint8_t *out, size_t cap)
{
	if (stored == NULL || out == NULL || raw_bytes == 0 || raw_bytes > STOCK_TIER_BLOCK_BYTES || cap < raw_bytes) {
		return -1;
	}
	if (codec == STOCK_TIER_CODEC_RAW) {
		if (stored_bytes != raw_bytes) {
			return -1;
		}
		memcpy(out, stored, raw_bytes);
	} else {
#ifdef ETII_ZSTD
		if (codec != STOCK_TIER_CODEC_ZSTD || tier_zstd_decompress(out, raw_bytes, stored, stored_bytes) != 0) {
			return -1;
		}
#else
		return -1;
#endif
	}
	int n = stock_tier_count_records(out, raw_bytes);
	return (n >= 0 && (uint32_t)n == records) ? n : -1;
}

int stock_tier_push(stock_tier_stack_t *stack, const uint8_t *raw, size_t raw_bytes)
{
	if (stock_tier_count_records(raw, raw_bytes) < 0) {
		return -1;
	}
	size_t room = stock_tier_pack_bound(raw_bytes);
	stock_tier_block_t *b = malloc(sizeof(*b) + room);
	if (b == NULL) {
		return -1;
	}
	int codec = STOCK_TIER_CODEC_RAW;
	int n = 0;
	size_t stored = stock_tier_pack(raw, raw_bytes, b->data, room, &codec, &n);
	if (stored == 0) {
		free(b);
		return -1;
	}
	b->codec = (uint8_t)codec;
	b->stored_bytes = (uint32_t)stored;
	if (room > b->stored_bytes) {
		// Rend la place réservée pour le pire cas du compresseur. Un échec de
		// réduction laisse le bloc tel quel, valide.
		stock_tier_block_t *shrunk = realloc(b, sizeof(*b) + b->stored_bytes);
		if (shrunk != NULL) {
			b = shrunk;
		}
	}
	b->seq = ++stack->last_seq;
	b->records = (uint32_t)n;
	b->raw_bytes = (uint32_t)raw_bytes;

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

int stock_tier_block_codec(const stock_tier_block_t *block)
{
	return block->codec;
}

size_t stock_tier_block_stored_bytes(const stock_tier_block_t *block)
{
	return block->stored_bytes;
}

uint32_t stock_tier_block_records(const stock_tier_block_t *block)
{
	return block->records;
}

size_t stock_tier_block_raw_bytes(const stock_tier_block_t *block)
{
	return block->raw_bytes;
}

const uint8_t *stock_tier_block_data(const stock_tier_block_t *block)
{
	return block->data;
}

int stock_tier_block_unpack(const stock_tier_block_t *block, uint8_t *out, size_t cap)
{
	if (block == NULL) {
		return -1;
	}
	return stock_tier_unpack(block->codec, block->data, block->stored_bytes, block->raw_bytes, block->records,
	                         out, cap);
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
