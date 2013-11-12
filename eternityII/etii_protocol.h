#ifndef eternityII_etii_protocol_h
#define eternityII_etii_protocol_h

#include "possibility.h"

#ifdef WIN32
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#else
#define PACK( __Declaration__ ) __Declaration__ __attribute__((__packed__))
#endif

#define INST_ERROR -1
#define INST_ADD 1
#define INST_GET 2
#define INST_SOLUTION 3
#define INST_END 4
#define INST_CONSIDERED 5
#define INST_NULL 6



struct packet
{
    uint8_t instruction;
    struct possibility_packet possibility;
    
}__attribute__((packed));

int8_t recv_instruction(int socket_id);

void send_instruction(int socket_id, int8_t instruction);

#endif