#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/times.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h> /* close */
#include <netdb.h> /* gethostbyname */
#include <errno.h>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(s) close(s)
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
typedef struct in_addr IN_ADDR;
#endif
#include "etii_protocol.h"

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
			printf("problème recv_instruction : %i\n", errno);
		}
	} else if (rRecv == 0) {
		printf("recv_instruction recv 0\n");
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
		printf("problème send_instruction %i : %i\n", instruction, errno);
	}
	free(i_instruction);

	return result;
}

int is_connected(int socket_id) {
	long result = send_instruction(socket_id, INST_TEST_CONNECTED);
	if (result <= 0) {
		printf("socket deconnected s\n");
		opened_tcp--;
		shutdown(socket_id, 2);
		closesocket(socket_id);
		return 0;
	}
	result = recv_instruction(socket_id);
	if (result <= 0) {
		printf("socket deconnected r\n");
		opened_tcp--;
		shutdown(socket_id, 2);
		closesocket(socket_id);
		return 0;
	}
	// Le serveur nous retourne qu'il met fin
	if (result == INST_END) {
		printf("socket deconnected by server\n");
		opened_tcp--;
		shutdown(socket_id, 2);
		closesocket(socket_id);
		return 0;
	}
	if (result != INST_TEST_CONNECTED) {
		printf("bad test connected : %i\n", result);
	}
	return 1;
}

void close_socket(int socket_id) {
	send_instruction(socket_id, INST_END);
	shutdown(socket_id, 2);
	int err = closesocket(socket_id);
	opened_tcp--;
	if(0 != err)
	{
		printf("erreur close :%i\n",err);
	}
}
