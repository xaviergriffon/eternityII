#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

#include "etii_protocol.h"
#include "logger.h"

int8_t recv_instruction(int socket_id)
{
	int8_t result = -1;
	
	int8_t *instruction = calloc(1,sizeof(int8_t));
	*instruction = INST_ERROR;
	long rRecv = recv(socket_id, (int8_t *)instruction, sizeof(int8_t),0);
	if (rRecv < 0) {
		// Deconnection timeout
		if (errno == EDEADLK || errno == EDEADLK || errno == EWOULDBLOCK) {
			// TODO: faire un end timeout
			result = INST_END;
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
        return 0;
	}
	return 1;
}

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
