/*
 * Tests unitaires de client_identity.c (identité déclarée d'un client, v12) :
 * codec (encode/decode), helpers hexadécimaux, et persistance du machine_uid.
 */
#include "greatest.h"
#include "net/client_identity.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static client_identity_t make_identity(int32_t fork_seq, uint8_t mode, const char *label)
{
	client_identity_t id;
	memset(&id, 0, sizeof(id));
	for (int i = 0; i < MACHINE_UID_BYTES; i++) {
		id.machine_uid[i] = (uint8_t)(i + 1);
	}
	for (int i = 0; i < CLIENT_UID_BYTES; i++) {
		id.client_uid[i] = (uint8_t)(0x80 + i);
	}
	id.fork_seq = fork_seq;
	id.mode = mode;
	strncpy(id.label, label, CLIENT_LABEL_MAX - 1);
	id.label[CLIENT_LABEL_MAX - 1] = '\0';
	return id;
}

/* ---------- client_identity_encode/decode -------------------------------- */

TEST identity_round_trip_short_label(void)
{
	client_identity_t id = make_identity(3, CLIENT_MODE_PRUNER, "jetson-1");
	uint8_t buf[CLIENT_IDENTITY_WIRE_MAX_SIZE];

	int32_t wlen = client_identity_encode(&id, buf, sizeof(buf));
	ASSERT_EQ_FMT((int32_t)(CLIENT_IDENTITY_WIRE_MIN_SIZE + strlen("jetson-1")), wlen, "%d");

	client_identity_t out;
	memset(&out, 0xAA, sizeof(out));
	ASSERT_EQ_FMT(0, client_identity_decode(buf, wlen, &out), "%d");
	ASSERT_MEM_EQ(id.machine_uid, out.machine_uid, MACHINE_UID_BYTES);
	ASSERT_MEM_EQ(id.client_uid, out.client_uid, CLIENT_UID_BYTES);
	ASSERT_EQ_FMT(id.fork_seq, out.fork_seq, "%d");
	ASSERT_EQ_FMT((int)id.mode, (int)out.mode, "%d");
	ASSERT_STR_EQ("jetson-1", out.label);
	PASS();
}

TEST identity_round_trip_empty_label(void)
{
	client_identity_t id = make_identity(-1, CLIENT_MODE_SEARCH, "");
	uint8_t buf[CLIENT_IDENTITY_WIRE_MAX_SIZE];

	int32_t wlen = client_identity_encode(&id, buf, sizeof(buf));
	ASSERT_EQ_FMT((int32_t)CLIENT_IDENTITY_WIRE_MIN_SIZE, wlen, "%d");

	client_identity_t out;
	ASSERT_EQ_FMT(0, client_identity_decode(buf, wlen, &out), "%d");
	ASSERT_EQ_FMT(-1, out.fork_seq, "%d");
	ASSERT_STR_EQ("", out.label);
	PASS();
}

TEST identity_round_trip_max_length_label(void)
{
	char label[CLIENT_LABEL_MAX];
	memset(label, 'x', CLIENT_LABEL_MAX - 1);
	label[CLIENT_LABEL_MAX - 1] = '\0';

	client_identity_t id = make_identity(0, CLIENT_MODE_GPU_PRUNER, label);
	uint8_t buf[CLIENT_IDENTITY_WIRE_MAX_SIZE];

	int32_t wlen = client_identity_encode(&id, buf, sizeof(buf));
	ASSERT_EQ_FMT((int32_t)CLIENT_IDENTITY_WIRE_MAX_SIZE, wlen, "%d");

	client_identity_t out;
	ASSERT_EQ_FMT(0, client_identity_decode(buf, wlen, &out), "%d");
	ASSERT_STR_EQ(label, out.label);
	PASS();
}

TEST identity_encode_rejects_buffer_too_small(void)
{
	client_identity_t id = make_identity(0, CLIENT_MODE_SEARCH, "abc");
	uint8_t buf[CLIENT_IDENTITY_WIRE_MIN_SIZE];
	ASSERT_EQ_FMT(-1, client_identity_encode(&id, buf, sizeof(buf)), "%d");
	PASS();
}

TEST identity_decode_rejects_short_fixed_part(void)
{
	uint8_t buf[CLIENT_IDENTITY_WIRE_MIN_SIZE] = { 0 };
	client_identity_t out;
	ASSERT_EQ_FMT(-1, client_identity_decode(buf, CLIENT_IDENTITY_WIRE_MIN_SIZE - 1, &out), "%d");
	PASS();
}

TEST identity_decode_rejects_label_shorter_than_declared(void)
{
	client_identity_t id = make_identity(1, CLIENT_MODE_SEARCH, "hello");
	uint8_t buf[CLIENT_IDENTITY_WIRE_MAX_SIZE];
	int32_t wlen = client_identity_encode(&id, buf, sizeof(buf));

	/* Le label_len déclaré annonce 5 octets, mais on ne fournit que len-1
	   au décodeur : la lecture doit être rejetée plutôt que de lire hors
	   des octets réellement disponibles. */
	client_identity_t out;
	ASSERT_EQ_FMT(-1, client_identity_decode(buf, wlen - 1, &out), "%d");
	PASS();
}

TEST identity_decode_rejects_label_len_over_max(void)
{
	uint8_t buf[CLIENT_IDENTITY_WIRE_MAX_SIZE + 1];
	client_identity_t id = make_identity(0, CLIENT_MODE_SEARCH, "");
	int32_t wlen = client_identity_encode(&id, buf, sizeof(buf));
	/* label_len occupe l'octet juste avant les octets de label (offset
	   CLIENT_IDENTITY_WIRE_MIN_SIZE - 1) ; on le force au-delà de la borne. */
	buf[CLIENT_IDENTITY_WIRE_MIN_SIZE - 1] = CLIENT_LABEL_MAX; /* > CLIENT_LABEL_MAX - 1 */

	client_identity_t out;
	ASSERT_EQ_FMT(-1, client_identity_decode(buf, wlen, &out), "%d");
	PASS();
}

/* ---------- hex helpers ---------------------------------------------------- */

TEST hex_encode_decode_round_trip(void)
{
	uint8_t bytes[MACHINE_UID_BYTES];
	for (int i = 0; i < MACHINE_UID_BYTES; i++) {
		bytes[i] = (uint8_t)(i * 17);
	}
	char hex[2 * MACHINE_UID_BYTES + 1];
	ASSERT_EQ_FMT((int)(2 * MACHINE_UID_BYTES), client_identity_hex_encode(bytes, MACHINE_UID_BYTES, hex, sizeof(hex)), "%d");

	uint8_t out[MACHINE_UID_BYTES];
	ASSERT_EQ_FMT(0, client_identity_hex_decode(hex, out, MACHINE_UID_BYTES), "%d");
	ASSERT_MEM_EQ(bytes, out, MACHINE_UID_BYTES);
	PASS();
}

TEST hex_encode_rejects_short_output(void)
{
	uint8_t bytes[4] = { 1, 2, 3, 4 };
	char hex[7]; /* il faudrait 9 (8 + NUL) */
	ASSERT_EQ_FMT(-1, client_identity_hex_encode(bytes, sizeof(bytes), hex, sizeof(hex)), "%d");
	PASS();
}

TEST hex_decode_rejects_wrong_length(void)
{
	uint8_t out[4];
	ASSERT_EQ_FMT(-1, client_identity_hex_decode("0102030", out, 4), "%d"); /* 7 au lieu de 8 */
	ASSERT_EQ_FMT(-1, client_identity_hex_decode("010203040", out, 4), "%d"); /* 9 au lieu de 8 */
	PASS();
}

TEST hex_decode_rejects_invalid_digit(void)
{
	uint8_t out[4];
	ASSERT_EQ_FMT(-1, client_identity_hex_decode("0102zz04", out, 4), "%d");
	PASS();
}

/* ---------- client_identity_random_bytes ----------------------------------- */

TEST random_bytes_fills_buffer(void)
{
	uint8_t a[MACHINE_UID_BYTES] = { 0 };
	uint8_t b[MACHINE_UID_BYTES] = { 0 };
	ASSERT_EQ_FMT(0, client_identity_random_bytes(a, sizeof(a)), "%d");
	ASSERT_EQ_FMT(0, client_identity_random_bytes(b, sizeof(b)), "%d");
	/* Deux tirages indépendants ne doivent (presque) jamais être identiques :
	   une régression qui laisserait le tampon à zéro le serait tout le temps. */
	ASSERT(memcmp(a, b, sizeof(a)) != 0);
	PASS();
}

/* ---------- machine_uid_load_or_create ------------------------------------- */

TEST machine_uid_missing_file_creates_and_persists(void)
{
	char tmpl[] = "/tmp/etii_muid_XXXXXX";
	char *dir = mkdtemp(tmpl);
	ASSERT(dir != NULL);
	char path[300];
	snprintf(path, sizeof(path), "%s/machine_uid", dir);

	uint8_t first[MACHINE_UID_BYTES];
	machine_uid_status_t st = machine_uid_load_or_create(path, first);
	ASSERT_EQ_FMT((int)MACHINE_UID_CREATED, (int)st, "%d");

	uint8_t second[MACHINE_UID_BYTES];
	machine_uid_status_t st2 = machine_uid_load_or_create(path, second);
	ASSERT_EQ_FMT((int)MACHINE_UID_LOADED, (int)st2, "%d");
	ASSERT_MEM_EQ(first, second, MACHINE_UID_BYTES);

	unlink(path);
	rmdir(dir);
	PASS();
}

TEST machine_uid_corrupt_file_regenerates(void)
{
	char tmpl[] = "/tmp/etii_muid_bad_XXXXXX";
	char *dir = mkdtemp(tmpl);
	ASSERT(dir != NULL);
	char path[300];
	snprintf(path, sizeof(path), "%s/machine_uid", dir);

	FILE *f = fopen(path, "w");
	ASSERT(f != NULL);
	fputs("not-a-valid-hex-uid\n", f);
	fclose(f);

	uint8_t out[MACHINE_UID_BYTES];
	machine_uid_status_t st = machine_uid_load_or_create(path, out);
	ASSERT_EQ_FMT((int)MACHINE_UID_CREATED, (int)st, "%d");

	/* Le fichier a bien été remplacé par un contenu rechargeable. */
	uint8_t reloaded[MACHINE_UID_BYTES];
	ASSERT_EQ_FMT((int)MACHINE_UID_LOADED, (int)machine_uid_load_or_create(path, reloaded), "%d");
	ASSERT_MEM_EQ(out, reloaded, MACHINE_UID_BYTES);

	unlink(path);
	rmdir(dir);
	PASS();
}

TEST machine_uid_null_path_is_volatile(void)
{
	uint8_t out[MACHINE_UID_BYTES] = { 0 };
	machine_uid_status_t st = machine_uid_load_or_create(NULL, out);
	ASSERT_EQ_FMT((int)MACHINE_UID_VOLATILE, (int)st, "%d");
	PASS();
}

#define SKIP_IF_ROOT()                                                        \
    do {                                                                      \
        if (geteuid() == 0)                                                   \
            SKIPm("root outrepasse les permissions : chmod 0444 sans effet"); \
    } while (0)

TEST machine_uid_unwritable_dir_is_volatile_but_fills_output(void)
{
	SKIP_IF_ROOT();

	char tmpl[] = "/tmp/etii_muid_ro_XXXXXX";
	char *dir = mkdtemp(tmpl);
	ASSERT(dir != NULL);
	if (chmod(dir, 0555) != 0) {
		rmdir(dir);
		SKIPm("chmod non supporté sur cet environnement");
	}
	char path[300];
	snprintf(path, sizeof(path), "%s/machine_uid", dir);

	uint8_t out[MACHINE_UID_BYTES];
	memset(out, 0, sizeof(out));
	machine_uid_status_t st = machine_uid_load_or_create(path, out);

	chmod(dir, 0755);
	rmdir(dir);

	ASSERT_EQ_FMT((int)MACHINE_UID_VOLATILE, (int)st, "%d");
	/* out doit malgré tout contenir un nonce utilisable pour cette exécution
	   (pas un tampon laissé à zéro/indéterminé). */
	uint8_t zero[MACHINE_UID_BYTES] = { 0 };
	ASSERT(memcmp(out, zero, sizeof(out)) != 0);
	PASS();
}

SUITE(client_identity_suite)
{
	RUN_TEST(identity_round_trip_short_label);
	RUN_TEST(identity_round_trip_empty_label);
	RUN_TEST(identity_round_trip_max_length_label);
	RUN_TEST(identity_encode_rejects_buffer_too_small);
	RUN_TEST(identity_decode_rejects_short_fixed_part);
	RUN_TEST(identity_decode_rejects_label_shorter_than_declared);
	RUN_TEST(identity_decode_rejects_label_len_over_max);
	RUN_TEST(hex_encode_decode_round_trip);
	RUN_TEST(hex_encode_rejects_short_output);
	RUN_TEST(hex_decode_rejects_wrong_length);
	RUN_TEST(hex_decode_rejects_invalid_digit);
	RUN_TEST(random_bytes_fills_buffer);
	RUN_TEST(machine_uid_missing_file_creates_and_persists);
	RUN_TEST(machine_uid_corrupt_file_regenerates);
	RUN_TEST(machine_uid_null_path_is_volatile);
	RUN_TEST(machine_uid_unwritable_dir_is_volatile_but_fills_output);
}
