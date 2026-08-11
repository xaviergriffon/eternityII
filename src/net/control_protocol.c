#include "net/control_protocol.h"

#include <stdlib.h>
#include <string.h>

#include "net/etii_protocol.h"

int ctrl_send_frame(int socket_id, uint8_t cmd, const void *payload, int32_t len)
{
	if (len < 0 || len > CTRL_PAYLOAD_MAX) {
		return -1;
	}
	if (len > 0 && payload == NULL) {
		return -1;
	}

	if (send_all(socket_id, &cmd, sizeof(cmd)) != (long)sizeof(cmd)) {
		return -1;
	}
	if (send_all(socket_id, &len, sizeof(len)) != (long)sizeof(len)) {
		return -1;
	}
	if (len > 0) {
		if (send_all(socket_id, payload, (size_t)len) != (long)len) {
			return -1;
		}
	}
	return 0;
}

int ctrl_recv_frame(int socket_id, void **out_payload, int32_t *out_len)
{
	if (out_payload == NULL || out_len == NULL) {
		return -1;
	}
	*out_payload = NULL;
	*out_len = 0;

	uint8_t cmd = 0;
	if (recv_all(socket_id, &cmd, sizeof(cmd)) != (long)sizeof(cmd)) {
		return -1;
	}

	int32_t len = 0;
	if (recv_all(socket_id, &len, sizeof(len)) != (long)sizeof(len)) {
		return -1;
	}
	if (len < 0 || len > CTRL_PAYLOAD_MAX) {
		/* Longueur hors borne : trame corrompue ou pair malveillant. Ne
		 * JAMAIS tenter une allocation de taille absurde. */
		return -1;
	}

	if (len > 0) {
		void *payload = malloc((size_t)len);
		if (payload == NULL) {
			return -1;
		}
		if (recv_all(socket_id, payload, (size_t)len) != (long)len) {
			free(payload);
			return -1;
		}
		*out_payload = payload;
	}
	*out_len = len;

	return (int)cmd;
}

int32_t control_hello_encode(const control_hello_t *hello, uint8_t *buf, size_t bufsize)
{
	if (bufsize < 4 + 4) {
		return -1;
	}
	int32_t off = 0;
	memcpy(buf + off, &hello->pid, sizeof(hello->pid));
	off += (int32_t)sizeof(hello->pid);
	memcpy(buf + off, &hello->nb_forks, sizeof(hello->nb_forks));
	off += (int32_t)sizeof(hello->nb_forks);

	int32_t identity_len = client_identity_encode(&hello->identity, buf + off, bufsize - (size_t)off);
	if (identity_len < 0) {
		return -1;
	}
	off += identity_len;
	return off;
}

int control_hello_decode(const uint8_t *buf, int32_t len, control_hello_t *out)
{
	if (len < 4 + 4) {
		return -1;
	}
	int32_t off = 0;
	memcpy(&out->pid, buf + off, sizeof(out->pid));
	off += (int32_t)sizeof(out->pid);
	memcpy(&out->nb_forks, buf + off, sizeof(out->nb_forks));
	off += (int32_t)sizeof(out->nb_forks);

	if (client_identity_decode(buf + off, len - off, &out->identity) != 0) {
		return -1;
	}
	return 0;
}

int32_t control_stats_encode(const control_stats_t *stats, uint8_t *buf)
{
	int32_t off = 0;
	memcpy(buf + off, &stats->shots_per_second, sizeof(stats->shots_per_second));
	off += (int32_t)sizeof(stats->shots_per_second);
	memcpy(buf + off, &stats->possibility_stock, sizeof(stats->possibility_stock));
	off += (int32_t)sizeof(stats->possibility_stock);
	memcpy(buf + off, &stats->analysed_stock, sizeof(stats->analysed_stock));
	off += (int32_t)sizeof(stats->analysed_stock);
	memcpy(buf + off, &stats->max_result, sizeof(stats->max_result));
	off += (int32_t)sizeof(stats->max_result);
	memcpy(buf + off, &stats->pruner_checked, sizeof(stats->pruner_checked));
	off += (int32_t)sizeof(stats->pruner_checked);
	memcpy(buf + off, &stats->pruner_removed, sizeof(stats->pruner_removed));
	off += (int32_t)sizeof(stats->pruner_removed);
	memcpy(buf + off, &stats->pruner_cells_per_second, sizeof(stats->pruner_cells_per_second));
	off += (int32_t)sizeof(stats->pruner_cells_per_second);
	return off;
}

int control_stats_decode(const uint8_t *buf, int32_t len, control_stats_t *out)
{
	if (len < CONTROL_STATS_WIRE_SIZE) {
		return -1;
	}
	int32_t off = 0;
	memcpy(&out->shots_per_second, buf + off, sizeof(out->shots_per_second));
	off += (int32_t)sizeof(out->shots_per_second);
	memcpy(&out->possibility_stock, buf + off, sizeof(out->possibility_stock));
	off += (int32_t)sizeof(out->possibility_stock);
	memcpy(&out->analysed_stock, buf + off, sizeof(out->analysed_stock));
	off += (int32_t)sizeof(out->analysed_stock);
	memcpy(&out->max_result, buf + off, sizeof(out->max_result));
	off += (int32_t)sizeof(out->max_result);
	memcpy(&out->pruner_checked, buf + off, sizeof(out->pruner_checked));
	off += (int32_t)sizeof(out->pruner_checked);
	memcpy(&out->pruner_removed, buf + off, sizeof(out->pruner_removed));
	off += (int32_t)sizeof(out->pruner_removed);
	memcpy(&out->pruner_cells_per_second, buf + off, sizeof(out->pruner_cells_per_second));
	off += (int32_t)sizeof(out->pruner_cells_per_second);
	return 0;
}

/**
 * @brief Compare le premier mot de `command_name` (avant un espace éventuel)
 *        à une liste de commandes candidates. Cœur partagé de
 *        `control_command_allowed` et `control_command_privileged`.
 *
 * @param command_name Ligne (ou nom) de commande, `NULL` géré explicitement.
 * @param candidates    Tableau de noms de commandes candidates.
 * @param nb_candidates Nombre d'entrées dans `candidates`.
 * @return              1 si le premier mot correspond à l'une des candidates, 0 sinon.
 */
static int command_first_word_matches(const char *command_name, const char *const candidates[], size_t nb_candidates)
{
	if (command_name == NULL) {
		return 0;
	}

	/* Longueur du premier mot (avant un espace éventuel). */
	size_t word_len = 0;
	while (command_name[word_len] != '\0' && command_name[word_len] != ' ') {
		word_len++;
	}
	if (word_len == 0) {
		return 0;
	}

	for (size_t i = 0; i < nb_candidates; i++) {
		if (strlen(candidates[i]) == word_len
		    && strncmp(candidates[i], command_name, word_len) == 0) {
			return 1;
		}
	}
	return 0;
}

control_command_class_t control_command_classify(const char *command_name)
{
	static const char *const read_only[] = {
		"clientsWork",
	};
	static const char *const write_relayable[] = {
		"pause",
		"resume",
		"limit",
		"maxStockByThread",
		"prunerBatch",
		"clientsCommand",
		"clientsCmd",
		"start",
		"stopForks",
		"configApply",
		"config",
		"configSave",
	};
	static const char *const write_server_only[] = {
		"restore",
		"backup",
		"sortAsc",
		"sortDesc",
		"sortDescMulti",
		"split",
		"regroup",
	};

	if (command_first_word_matches(command_name, read_only, sizeof(read_only) / sizeof(read_only[0]))) {
		return CTRL_CMD_READ_ONLY;
	}
	if (command_first_word_matches(command_name, write_relayable, sizeof(write_relayable) / sizeof(write_relayable[0]))) {
		return CTRL_CMD_WRITE_RELAYABLE;
	}
	if (command_first_word_matches(command_name, write_server_only, sizeof(write_server_only) / sizeof(write_server_only[0]))) {
		return CTRL_CMD_WRITE_SERVER_ONLY;
	}
	return CTRL_CMD_UNKNOWN;
}

int control_command_allowed(const char *command_name)
{
	control_command_class_t cls = control_command_classify(command_name);
	return cls == CTRL_CMD_READ_ONLY || cls == CTRL_CMD_WRITE_RELAYABLE;
}

int control_command_privileged(const char *command_name)
{
	return control_command_classify(command_name) == CTRL_CMD_WRITE_SERVER_ONLY;
}

int control_command_read_only(const char *command_name)
{
	return control_command_classify(command_name) == CTRL_CMD_READ_ONLY;
}
