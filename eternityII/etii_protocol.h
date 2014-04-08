#ifndef eternityII_etii_protocol_h
#define eternityII_etii_protocol_h

#include "possibility.h"
#include "packed.h"

#ifdef WIN32
#include <stdint.h>
#endif

#define INST_ERROR -1
#define INST_ADD 1
#define INST_GET 2
#define INST_SOLUTION 3
#define INST_END 4
#define INST_CONSIDERED 5
#define INST_NULL 6


PACK(
	 struct packet
{
    uint8_t instruction;
    struct possibility_packet possibility;
    
});

int8_t recv_instruction(int socket_id);

void send_instruction(int socket_id, int8_t instruction);

#endif