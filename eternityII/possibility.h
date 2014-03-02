#ifndef eternityII_possibility_h
#define eternityII_possibility_h

#ifdef WIN32
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop) )
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#else
#include <stdint.h>
#define PACK( __Declaration__ ) __Declaration__ __attribute__((__packed__))
#endif

#include "part.h"
#include "lifo.h"
#include "etii_opencl_instance.h"

#define ETERN_SIZE 16
#define ETERN_PARTS 256
#define DIR_UP 1
#define DIR_RIGHT 2
#define DIR_DOWN 3
#define DIR_LEFT 4

struct possibility_packet
{
    uint8_t x;
    uint8_t y;
    key_part grid[ETERN_SIZE][ETERN_SIZE];
	uint16_t alloc;
	uint8_t faceused[ETERN_PARTS];
	uint8_t direcory;
    
} __attribute__((packed));

typedef struct
{
	int size;
	struct possibility_packet *possibilities;
} array_possibility_packet;

struct possibility_packet *generate_possibility_packet(int x, int y, struct part *etern[ETERN_SIZE][ETERN_SIZE], int directory);
key_part what_search(map_big_array *map_parts, int x, int y, struct possibility_packet possiblity);
int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts);
int search_possiblity(File *result,struct possibility_packet *possiblity, map_big_array *mapParts);

int change_dir(int cur_dir, int x, int y, struct possibility_packet *possiblity);

struct possibility_packet *crypt_to_network(struct possibility_packet *packet);
struct possibility_packet *decrypt_from_network(struct possibility_packet *packet);

int print_possibility_packet(struct possibility_packet *packet);
int save_possibility(char *filename, struct possibility_packet *possibility);

File *search_possiblity_opencl(etii_cl_instance *instance,struct possibility_packet *possiblity, int nbPossibility,map_big_array *mapParts);

/*
 0 OK
 -1 packet NULL
 -2 x or y > ETERN_SIZE
 -3 directory > or < dir_possibilities
 -4 alloc <= 0
 -5 alloc <> faceused
 */
int check_possibility(struct possibility_packet *packet);

int test_directions();
#endif