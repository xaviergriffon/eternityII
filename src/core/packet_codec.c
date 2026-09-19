/* Forme compacte d'un possibility_packet pour le stockage sur disque —
   voir packet_codec.h pour le raisonnement, les mesures et le format. */
#include "core/packet_codec.h"

#include <string.h>

/* La forme compacte ne peut pas être plus grande que la forme brute : c'est
   la propriété qui garantit qu'aucun profil de stock ne peut faire régresser
   la taille d'une sauvegarde. Vérifiée à la COMPILATION plutôt qu'en
   commentaire, pour que la modifier sans y penser casse le build. */
typedef char packet_codec_never_larger_than_raw[
    PACKET_CODEC_MAX_BYTES <= (int)sizeof(struct possibility_packet) ? 1 : -1];

/* Le plan des valeurs doit pouvoir coder la plus grande valeur possible :
   vérifié à la compilation, pour qu'une taille de puzzle ajoutée sans élargir
   PACKET_CODEC_VALUE_BITS casse le build au lieu de tronquer des plateaux. */
typedef char packet_codec_value_bits_suffice[
    PACKET_CODEC_VALUE_MAX < (1 << PACKET_CODEC_VALUE_BITS) ? 1 : -1];

static void put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFF);
	b[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *b)
{
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

/* Nombre de cases non vides — `alloc` dit la même chose, mais l'encodeur ne
   lui fait pas confiance : un paquet dont `alloc` mentirait produirait un
   enregistrement dont le plan des valeurs déborderait. `possibility_placed_count`
   fait déjà ce calcul, mais depuis `core/possibility.h` ; le refaire ici garde
   le codec autonome (il n'a besoin que de la grille). */
static size_t packet_codec_placed(const struct possibility_packet *packet)
{
	size_t placed = 0;
	for (int x = 0; x < ETERN_SIZE; x++) {
		for (int y = 0; y < ETERN_SIZE; y++) {
			if (packet->grid[x][y] != -2) {
				placed++;
			}
		}
	}
	return placed;
}

static size_t packet_codec_size_for(size_t placed)
{
	return (size_t)PACKET_CODEC_HEADER_BYTES + (size_t)PACKET_CODEC_BITMAP_BYTES
	       + PACKET_CODEC_VALUE_BYTES(placed);
}

size_t packet_codec_encoded_size(const struct possibility_packet *packet)
{
	return packet_codec_size_for(packet_codec_placed(packet));
}

/* Écrit `value` sur PACKET_CODEC_VALUE_BITS bits à la position `slot` du plan,
   poids faible d'abord. Le champ peut chevaucher deux octets (11 + 7 < 24), et
   jamais trois tant que PACKET_CODEC_VALUE_BITS <= 9 ; au-delà on écrit donc
   trois octets, le dernier pouvant ne rien recevoir. */
static inline void put_value(uint8_t *plane, size_t slot, uint16_t value)
{
	size_t bit = slot * (size_t)PACKET_CODEC_VALUE_BITS;
	size_t byte = bit >> 3;
	unsigned shift = (unsigned)(bit & 7);
	uint32_t v = (uint32_t)value << shift;
	plane[byte] |= (uint8_t)(v & 0xFF);
	plane[byte + 1] |= (uint8_t)((v >> 8) & 0xFF);
	if (shift + (unsigned)PACKET_CODEC_VALUE_BITS > 16) {
		plane[byte + 2] |= (uint8_t)((v >> 16) & 0xFF);
	}
}

static inline uint16_t get_value(const uint8_t *plane, size_t slot)
{
	size_t bit = slot * (size_t)PACKET_CODEC_VALUE_BITS;
	size_t byte = bit >> 3;
	unsigned shift = (unsigned)(bit & 7);
	uint32_t v = (uint32_t)plane[byte] | ((uint32_t)plane[byte + 1] << 8);
	if (shift + (unsigned)PACKET_CODEC_VALUE_BITS > 16) {
		v |= (uint32_t)plane[byte + 2] << 16;
	}
	return (uint16_t)((v >> shift) & ((1u << PACKET_CODEC_VALUE_BITS) - 1u));
}

int packet_codec_encode(const struct possibility_packet *packet, uint8_t *out, size_t outsize,
                        size_t *out_written)
{
	size_t placed = packet_codec_placed(packet);
	size_t needed = packet_codec_size_for(placed);
	if (outsize < needed) {
		return -1;
	}

	/* `put_value` peut toucher jusqu'à deux octets au-delà de celui qu'il vise :
	   le plan est donc écrit dans un tampon local dimensionné avec cette marge,
	   plutôt qu'en place dans `out` où la marge déborderait de l'enregistrement
	   sur le suivant. */
	uint8_t plane[PACKET_CODEC_VALUE_BYTES(ETERN_PARTS) + 2];
	memset(plane, 0, sizeof plane);
	memset(out, 0, needed);

	out[0] = packet->x;
	out[1] = packet->y;
	out[2] = (uint8_t)(packet->checked ? 1 : 0);
	out[3] = 0;
	put_u16(out + 4, (uint16_t)packet->min_candidats);

	uint8_t *bitmap = out + PACKET_CODEC_HEADER_BYTES;

	size_t k = 0;
	for (int x = 0; x < ETERN_SIZE; x++) {
		for (int y = 0; y < ETERN_SIZE; y++) {
			int16_t v = packet->grid[x][y];
			if (v == -2) {
				continue;
			}
			/* Seules les valeurs RÉELLEMENT non représentables sont refusées
			   (cf. packet_codec.h) : le format n'a pas à juger si un plateau
			   est jouable, seulement à le rendre tel qu'on le lui a confié. */
			if (v < 0 || v > PACKET_CODEC_VALUE_MAX) {
				return -1;
			}
			int cell = x * ETERN_SIZE + y;
			bitmap[cell >> 3] |= (uint8_t)(1u << (cell & 7));
			put_value(plane, k, (uint16_t)v);
			k++;
		}
	}

	memcpy(bitmap + PACKET_CODEC_BITMAP_BYTES, plane, PACKET_CODEC_VALUE_BYTES(placed));

	if (out_written != NULL) {
		*out_written = needed;
	}
	return 0;
}

int packet_codec_decode(const uint8_t *in, size_t insize, struct possibility_packet *out,
                        size_t *out_consumed)
{
	if (insize < (size_t)PACKET_CODEC_HEADER_BYTES + (size_t)PACKET_CODEC_BITMAP_BYTES) {
		return -1;
	}

	const uint8_t *bitmap = in + PACKET_CODEC_HEADER_BYTES;
	size_t placed = 0;
	for (int i = 0; i < PACKET_CODEC_BITMAP_BYTES; i++) {
		uint8_t b = bitmap[i];
		while (b != 0) {
			placed++;
			b = (uint8_t)(b & (b - 1));
		}
	}

	size_t total = packet_codec_size_for(placed);
	if (insize < total) {
		return -1;
	}

	/* Copie locale, pour la même raison qu'à l'encodage : `get_value` lit
	   jusqu'à deux octets au-delà de l'octet visé, qui appartiendraient à
	   l'enregistrement suivant. */
	uint8_t plane[PACKET_CODEC_VALUE_BYTES(ETERN_PARTS) + 2];
	memset(plane, 0, sizeof plane);
	memcpy(plane, bitmap + PACKET_CODEC_BITMAP_BYTES, PACKET_CODEC_VALUE_BYTES(placed));

	/* memset complet : un paquet décodé ne doit jamais porter d'octet
	   indéterminé, bourrage d'alignement compris — sinon le hash du pool
	   analysé (hash_possibility_key) verrait deux plateaux identiques comme
	   différents selon la pile qui a servi à les décoder. */
	memset(out, 0, sizeof *out);
	out->x = in[0];
	out->y = in[1];
	out->checked = (uint8_t)(in[2] == 1 ? 1 : 0);
	out->min_candidats = (int16_t)get_u16(in + 4);

	for (int x = 0; x < ETERN_SIZE; x++) {
		for (int y = 0; y < ETERN_SIZE; y++) {
			out->grid[x][y] = -2;
		}
	}

	size_t k = 0;
	for (int cell = 0; cell < ETERN_PARTS; cell++) {
		if (((bitmap[cell >> 3] >> (cell & 7)) & 1) == 0) {
			continue;
		}
		uint16_t v = get_value(plane, k);
		if (v > (uint16_t)PACKET_CODEC_VALUE_MAX) {
			return -1; /* enregistrement incohérent */
		}
		out->grid[cell / ETERN_SIZE][cell % ETERN_SIZE] = (int16_t)v;
		if (v >= 1) {
			set_face_used(out->b_faceused, (uint16_t)((v - 1) % ETERN_PARTS), 1);
		}
		k++;
	}

	out->alloc = (uint16_t)placed;

	if (out_consumed != NULL) {
		*out_consumed = total;
	}
	return 0;
}

void packet_codec_write_file_header(uint8_t *buf)
{
	memset(buf, 0, PACKET_CODEC_FILE_HEADER_BYTES);
	memcpy(buf, PACKET_CODEC_FILE_MAGIC, 8);
	put_u16(buf + 8, (uint16_t)PACKET_CODEC_FILE_VERSION);
	put_u16(buf + 10, (uint16_t)ETERN_SIZE);
	put_u16(buf + 12, (uint16_t)ETERN_PARTS);
	put_u16(buf + 14, (uint16_t)PACKET_CODEC_MAX_BYTES);
	put_u16(buf + 16, (uint16_t)PACKET_CODEC_HEADER_BYTES);
}

int packet_codec_read_file_header(const uint8_t *buf)
{
	if (memcmp(buf, PACKET_CODEC_FILE_MAGIC, 8) != 0) {
		return -1;
	}
	if (get_u16(buf + 8) != (uint16_t)PACKET_CODEC_FILE_VERSION
	    || get_u16(buf + 10) != (uint16_t)ETERN_SIZE
	    || get_u16(buf + 12) != (uint16_t)ETERN_PARTS
	    || get_u16(buf + 14) != (uint16_t)PACKET_CODEC_MAX_BYTES
	    || get_u16(buf + 16) != (uint16_t)PACKET_CODEC_HEADER_BYTES) {
		return -1;
	}
	return 0;
}

int packet_codec_fwrite(FILE *f, const struct possibility_packet *packet)
{
	uint8_t record[PACKET_CODEC_MAX_BYTES];
	size_t written = 0;
	if (packet_codec_encode(packet, record, sizeof record, &written) != 0) {
		return -1;
	}
	if (fwrite(record, 1, written, f) != written) {
		return -1;
	}
	return 0;
}

int packet_codec_fread(FILE *f, struct possibility_packet *packet)
{
	uint8_t record[PACKET_CODEC_MAX_BYTES];
	const size_t prefix = (size_t)PACKET_CODEC_HEADER_BYTES + (size_t)PACKET_CODEC_BITMAP_BYTES;

	size_t got = fread(record, 1, prefix, f);
	if (got == 0) {
		return feof(f) ? 0 : -1; /* fin de fichier propre */
	}
	if (got < prefix) {
		return -1; /* enregistrement tronqué */
	}

	/* Le bitmap dit combien de cases suivent : l'enregistrement est
	   auto-descriptif, aucun champ de longueur à maintenir en plus. */
	size_t placed = 0;
	for (int i = 0; i < PACKET_CODEC_BITMAP_BYTES; i++) {
		uint8_t b = record[PACKET_CODEC_HEADER_BYTES + i];
		while (b != 0) {
			placed++;
			b = (uint8_t)(b & (b - 1));
		}
	}
	size_t rest = PACKET_CODEC_VALUE_BYTES(placed);
	if (rest > 0 && fread(record + prefix, 1, rest, f) != rest) {
		return -1;
	}

	return (packet_codec_decode(record, prefix + rest, packet, NULL) == 0) ? 1 : -1;
}
