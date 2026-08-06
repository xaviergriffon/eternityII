#include "net/client_identity.h"

#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>

int32_t client_identity_encode(const client_identity_t *id, uint8_t *buf, size_t bufsize)
{
	size_t label_len = strnlen(id->label, CLIENT_LABEL_MAX - 1);
	size_t needed = CLIENT_IDENTITY_WIRE_MIN_SIZE + label_len;
	if (bufsize < needed) {
		return -1;
	}

	size_t off = 0;
	memcpy(buf + off, id->machine_uid, MACHINE_UID_BYTES);
	off += MACHINE_UID_BYTES;
	memcpy(buf + off, id->client_uid, CLIENT_UID_BYTES);
	off += CLIENT_UID_BYTES;
	memcpy(buf + off, &id->fork_seq, sizeof(id->fork_seq));
	off += sizeof(id->fork_seq);
	memcpy(buf + off, &id->mode, sizeof(id->mode));
	off += sizeof(id->mode);
	uint8_t label_len_u8 = (uint8_t)label_len;
	memcpy(buf + off, &label_len_u8, sizeof(label_len_u8));
	off += sizeof(label_len_u8);
	if (label_len > 0) {
		memcpy(buf + off, id->label, label_len);
		off += label_len;
	}
	return (int32_t)off;
}

int client_identity_decode(const uint8_t *buf, int32_t len, client_identity_t *out)
{
	if (len < CLIENT_IDENTITY_WIRE_MIN_SIZE) {
		return -1;
	}

	int32_t off = 0;
	memcpy(out->machine_uid, buf + off, MACHINE_UID_BYTES);
	off += MACHINE_UID_BYTES;
	memcpy(out->client_uid, buf + off, CLIENT_UID_BYTES);
	off += CLIENT_UID_BYTES;
	memcpy(&out->fork_seq, buf + off, sizeof(out->fork_seq));
	off += (int32_t)sizeof(out->fork_seq);
	memcpy(&out->mode, buf + off, sizeof(out->mode));
	off += (int32_t)sizeof(out->mode);
	uint8_t label_len = 0;
	memcpy(&label_len, buf + off, sizeof(label_len));
	off += (int32_t)sizeof(label_len);

	if (label_len > CLIENT_LABEL_MAX - 1) {
		return -1;
	}
	if (off + (int32_t)label_len > len) {
		return -1;
	}
	if (label_len > 0) {
		memcpy(out->label, buf + off, label_len);
	}
	out->label[label_len] = '\0';
	return 0;
}

int client_identity_hex_encode(const uint8_t *bytes, size_t n, char *out, size_t out_size)
{
	if (out == NULL || out_size < 2 * n + 1) {
		return -1;
	}
	if (n > 0 && bytes == NULL) {
		return -1;
	}
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = digits[(bytes[i] >> 4) & 0xF];
		out[2 * i + 1] = digits[bytes[i] & 0xF];
	}
	out[2 * n] = '\0';
	return (int)(2 * n);
}

/**
 * @brief Valeur 0-15 d'un chiffre hexadécimal, ou -1 si `c` n'en est pas un.
 */
static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int client_identity_hex_decode(const char *hex, uint8_t *out, size_t n)
{
	if (hex == NULL || out == NULL) {
		return -1;
	}
	if (strnlen(hex, 2 * n + 1) != 2 * n) {
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		int hi = hex_nibble(hex[2 * i]);
		int lo = hex_nibble(hex[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

int client_identity_random_bytes(uint8_t *out, size_t n)
{
	if (out == NULL) {
		return -1;
	}
	if (getentropy(out, n) != 0) {
		return -1;
	}
	return 0;
}

/**
 * @brief Retire les espaces/retours à la ligne de fin d'une ligne lue par
 *        `fgets` (fichier généralement créé/édité à la main, terminé par
 *        `\n`). Même patron que `http_token_load` (src/net/http_server.c).
 */
static void trim_trailing_whitespace(char *s)
{
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'
	                    || s[len - 1] == ' ' || s[len - 1] == '\t')) {
		s[--len] = '\0';
	}
}

/**
 * @brief Tente de charger un nonce machine valide depuis `path`.
 *
 * @return 0 si `out` a été rempli depuis un contenu valide, -1 sinon (fichier
 *         absent, illisible, ou contenu qui n'est pas exactement
 *         `2*MACHINE_UID_BYTES` caractères hexadécimaux une fois les espaces
 *         de fin retirés).
 */
static int try_load_machine_uid(const char *path, uint8_t out[MACHINE_UID_BYTES])
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return -1;
	}
	char line[2 * MACHINE_UID_BYTES + 16];
	char *got = fgets(line, sizeof(line), f);
	fclose(f);
	if (got == NULL) {
		return -1;
	}
	trim_trailing_whitespace(line);
	return client_identity_hex_decode(line, out, MACHINE_UID_BYTES);
}

/**
 * @brief Écrit `uid` (encodé en hexadécimal) dans `path`, remplaçant tout
 *        contenu existant. Le fichier n'est PAS un secret (affiché en clair
 *        dans la console/l'API HTTP) : aucune contrainte de permission
 *        analogue à `--http-token-file`.
 *
 * @return 0 si l'écriture a réussi, -1 sinon.
 */
static int try_write_machine_uid(const char *path, const uint8_t uid[MACHINE_UID_BYTES])
{
	char hex[2 * MACHINE_UID_BYTES + 1];
	if (client_identity_hex_encode(uid, MACHINE_UID_BYTES, hex, sizeof(hex)) < 0) {
		return -1;
	}
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		return -1;
	}
	int written = fprintf(f, "%s\n", hex);
	int close_result = fclose(f);
	return (written > 0 && close_result == 0) ? 0 : -1;
}

machine_uid_status_t machine_uid_load_or_create(const char *path, uint8_t out[MACHINE_UID_BYTES])
{
	if (path != NULL && try_load_machine_uid(path, out) == 0) {
		return MACHINE_UID_LOADED;
	}

	// Absent, illisible, ou contenu invalide (version antérieure incompatible,
	// corruption, ...) : on régénère plutôt que de refuser de démarrer — une
	// identité machine est une donnée d'observation, pas de l'état de recherche.
	if (client_identity_random_bytes(out, MACHINE_UID_BYTES) != 0) {
		// getentropy() en échec (plateforme non supportée) : dernier recours,
		// remplir avec des zéros plutôt que de laisser `out` indéterminé.
		memset(out, 0, MACHINE_UID_BYTES);
	}

	if (path == NULL || try_write_machine_uid(path, out) != 0) {
		return MACHINE_UID_VOLATILE;
	}
	return MACHINE_UID_CREATED;
}
