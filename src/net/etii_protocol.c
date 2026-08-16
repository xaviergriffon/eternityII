#include <stdio.h>
#include <stdlib.h>

#include <sys/times.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#include "net/etii_protocol.h"
#include "ui/logger.h"

/**
 * @brief Reçoit une instruction (1 octet) depuis un socket TCP.
 *
 * Réessaie jusqu'à 10 fois en cas d'interruption `EINTR`. Retourne `INST_END`
 * sur timeout (`EAGAIN`/`EWOULDBLOCK`/`ETIMEDOUT`) ou fermeture propre (recv == 0).
 *
 * @param socket_id Descripteur du socket connecté.
 * @return          Byte d'instruction reçu, ou `INST_END` (-1) en cas d'erreur/déconnexion.
 */
int8_t recv_instruction(int socket_id)
{
	int8_t result = -1;
	int8_t *instruction = calloc(1,sizeof(int8_t));
	*instruction = INST_ERROR;
	//long rRecv = recv(socket_id, (int8_t *)instruction, sizeof(int8_t),0);
	long rRecv = -1;
	long maxTentative = 10;
	do {
		rRecv = recv(socket_id, (int8_t *)instruction, sizeof(int8_t),0);
		maxTentative--;
	} while (rRecv == -1 && errno == EINTR && maxTentative > 0);
	if (rRecv < 0) {
		// Deconnection timeout
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
#ifdef DEBUG_SOCKET
			log_debug("recv_instruction: timeout ou socket non bloquant (errno=%d)\n", errno);
#endif
			result = INST_END;
		} else if (errno == EINTR) {
			log_errno("recv_instruction recv EINTR => ");
		} else {
            log_errno("Error on recv_instruction => ");
		}
	} else if (rRecv == 0) {
#ifdef DEBUG_SOCKET
		log_debug("recv_instruction recv 0\n");
#endif // DEBUG_SOCKET
		result = INST_END;
	} else {
		result = *instruction;
	}
	
	free(instruction);
	
	return result;
}

/**
 * @brief Envoie une instruction (1 octet) sur un socket TCP.
 *
 * @param socket_id   Descripteur du socket connecté.
 * @param instruction Byte d'instruction à envoyer (cf. constantes `INST_*`).
 * @return            Nombre d'octets envoyés, ou valeur ≤ 0 en cas d'erreur.
 */
long send_instruction(int socket_id, int8_t instruction)
{
	int8_t *i_instruction = calloc(1,sizeof(int8_t));
	*i_instruction = instruction;
	long result = send(socket_id, (int8_t *)i_instruction, sizeof(int8_t), 0);
	if (result <= 0) {
        log_errno("Error on send_instruction %i (result %li) => ", instruction, result);
	}
	free(i_instruction);

	return result;
}

handshake_verdict_t handshake_verdict(int8_t result)
{
	if (result == INST_SUPPORTED_VERSION) {
		return HANDSHAKE_OK;
	}
	if (result == INST_UNSUPPORTED_VERSION) {
		return HANDSHAKE_VERSION_REJECTED;
	}
	// Tout le reste (INST_END/-1 de timeout, fermeture du pair, octet inattendu) :
	// échec transitoire — on ne tue pas le client, on réessaiera plus tard.
	return HANDSHAKE_RETRY;
}

long recv_all(int socket_id, void *buf, size_t len)
{
	size_t total = 0;
	char *p = (char *)buf;
	while (total < len) {
		long r = recv(socket_id, p + total, len - total, 0);
		if (r > 0) {
			total += (size_t)r;
			continue;
		}
		if (r == 0) {
			// Pair fermé : on rend le total partiel, l'appelant détecte < len.
			return (long)total;
		}
		if (errno == EINTR) {
			continue;
		}
		return -1;
	}
	return (long)total;
}

long send_all(int socket_id, const void *buf, size_t len)
{
	size_t total = 0;
	const char *p = (const char *)buf;
	while (total < len) {
		long s = send(socket_id, p + total, len - total, 0);
		if (s > 0) {
			total += (size_t)s;
			continue;
		}
		if (s < 0 && errno == EINTR) {
			continue;
		}
		return -1;
	}
	return (long)total;
}

/**
 * @brief Teste si un socket TCP est toujours connecté.
 *
 * Envoie `INST_TEST_CONNECTED` et attend la même réponse en retour.
 * Ferme et shutdownle socket si la connexion est rompue ou si le serveur
 * renvoie `INST_END`.
 *
 * @param socket_id Descripteur du socket à tester.
 * @return          1 si connecté, 0 sinon (le socket est fermé dans ce cas).
 */
int is_connected(int socket_id) {
	long result = send_instruction(socket_id, INST_TEST_CONNECTED);
	if (result <= 0) {
        log_info("socket deconnected s\n");
#ifdef DEBUG_SOCKET
		opened_tcp--;
#endif // DEBUG_SOCKET
		shutdown(socket_id, 2);
        close(socket_id);
		return 0;
	}
	result = recv_instruction(socket_id);
	if (result <= 0) {
#ifdef DEBUG_SOCKET
        log_debug("socket deconnected r\n");
		opened_tcp--;
#endif // DEBUG_SOCKET
        log_error("Error on test connection : %li\n", result);
		shutdown(socket_id, 2);
        close(socket_id);
		return 0;
	}
	// Le serveur nous retourne qu'il met fin
    if (result == INST_END) {
#ifdef DEBUG_SOCKET
        log_debug("socket deconnected by server\n");
		opened_tcp--;
#endif // DEBUG_SOCKET
		shutdown(socket_id, 2);
        close(socket_id);
		return 0;
	}
	if (result != INST_TEST_CONNECTED) {
        log_error("wrong instruction received for connection test : %li\n", result);
        // Manquait avant ce correctif, à la différence des trois autres
        // branches d'échec ci-dessus : sans shutdown()/close(), le socket
        // fuit côté client (le prochain appel en ouvre un nouveau sans
        // jamais refermer celui-ci) ET la session correspondante reste
        // ouverte côté serveur jusqu'à SON PROPRE timeout — fenêtre pendant
        // laquelle un travail en cours peut être remis en jeu ailleurs
        // (requeue_last_sent_possibility) alors que ce client y travaille
        // toujours.
        shutdown(socket_id, 2);
        close(socket_id);
        return 0;
	}
	return 1;
}

int32_t poll_server_hunger(int socket_id) {
	long result = send_instruction(socket_id, INST_NEED_WORK);
	if (result <= 0) {
		log_info("socket deconnected s\n");
#ifdef DEBUG_SOCKET
		opened_tcp--;
#endif // DEBUG_SOCKET
		shutdown(socket_id, 2);
		close(socket_id);
		return -1;
	}
	int32_t hunger = 0;
	if (recv_all(socket_id, &hunger, sizeof(hunger)) != (long)sizeof(hunger)
	    || hunger < 0) {
		log_error("Error on need work poll\n");
#ifdef DEBUG_SOCKET
		opened_tcp--;
#endif // DEBUG_SOCKET
		shutdown(socket_id, 2);
		close(socket_id);
		return -1;
	}
	return hunger;
}

/**
 * @brief Ferme proprement un socket TCP en envoyant `INST_END` au préalable.
 * @param socket_id Descripteur du socket à fermer.
 */
void close_socket(int socket_id) {
	send_instruction(socket_id, INST_END);
	shutdown(socket_id, 2);
	int err = close(socket_id);
#ifdef DEBUG_SOCKET
	opened_tcp--;
#endif // DEBUG_SOCKET

	if(0 != err)
	{
		log_error("error on close socket :%i\n",err);
	}
}
