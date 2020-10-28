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
	*instruction = -1;
	recv(socket_id, (int8_t *)instruction, sizeof(int8_t),0);
	
	result = *instruction;
	free(instruction);
	
	return result;
}

void send_instruction(int socket_id, int8_t instruction)
{
	int8_t *i_instruction = calloc(1,sizeof(int8_t));
	*i_instruction = instruction;
	send(socket_id, (int8_t *)i_instruction, sizeof(int8_t),0);
	free(i_instruction);
}
