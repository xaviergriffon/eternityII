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
#include "packed.h"
#include "static_variables.h"


#define DIR_UP 1
#define DIR_RIGHT 2
#define DIR_DOWN 3
#define DIR_LEFT 4
 
 PACK(
 struct possibility_packet
 {
 uint8_t x;
 uint8_t y;
 int16_t grid[ETERN_SIZE][ETERN_SIZE];
 uint16_t alloc;
 uint8_t faceused[ETERN_PARTS];
 });
 

typedef struct
{
	int size;
	struct possibility_packet *possibilities;
} array_possibility_packet;

struct possibility_packet *generate_possibility_packet(int x, int y, struct part *etern[ETERN_SIZE][ETERN_SIZE], int directory);
key_part what_search(struct array_part *all_rotate_parts, int x, int y, struct possibility_packet *possiblity);
void what_search_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity,key_part *key);

int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);
int possibility_all_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);
int search_possiblity(File *result,struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part);
int search_possiblity_light(File *result,key_part *key,struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part,int16_t idParts[ETERN_PARTS][4]);

int change_dir(int cur_dir, int x, int y, struct possibility_packet *possiblity);

struct possibility_packet *crypt_to_network(struct possibility_packet *packet);
struct possibility_packet *decrypt_from_network(struct possibility_packet *packet);

int print_possibility_packet(struct possibility_packet *packet);
int save_possibility(char *filename, struct possibility_packet *possibility);

// File *search_possiblity_opencl(etii_cl_instance *instance,struct possibility_packet *possiblity, int nbPossibility,map_big_array *mapParts, struct array_part *all_rotate_part);

// File *search_possiblity_light_opencl(etii_cl_instance *instance,struct possibility_packet *possiblity, int nbPossibility);

/*
 0 OK
 -1 packet NULL
 -2 x or y > ETERN_SIZE
 -3 directory > or < dir_possibilities
 -4 alloc <= 0
 -5 alloc <> faceused
 */
int check_possibility(struct possibility_packet *packet);

int test_directions(void);
int decode_direction(void);
#endif
