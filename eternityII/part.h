#ifndef eternityII_part_h
#define eternityII_part_h

#include "packed.h"
#ifdef WIN32
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
#else
#include <stdint.h>
#endif

#define PART_NONE -1
#define PART_TOP 0
#define PART_RIGHT 1
#define PART_BOTTOM 2
#define PART_LEFT 3
#define FACE_UNKNOW -1
#define MAX_KEY_LENGTH 12
#define MAX_FACE_MAP 24

PACK(
	 typedef struct
{
    int8_t k1;
    int8_t k2;
	int8_t k3;
	int8_t k4;
}) key_part;

struct part
{
    int16_t id;
    int8_t top;
    int8_t right;
    int8_t bottom;
    int8_t left;
    int8_t rotation;
};

struct list_part
{
	struct part *value;
	struct list_part *next;
};

struct array_part
{
    int size;
    struct part *parts;
} ;

struct map_in_one
{
	int nbparts;
	int nbarrays;
	int *quantity;
	int *position;
	struct part *parts;
};

typedef struct array_part ****big_array;

typedef struct
{
	int sizearray;
	big_array *bigarray;
} map_big_array;

struct map_part_element
{
	char *key;
	unsigned int key_int;
	struct array_part *apart;
};

struct map_part
{
	int size;
    int sizemap;
	struct map_part_element *elements;
};

void print_part(struct part *p);

/*
 * Indique un id correspondant à une piece dans un sens de rotation
 */
uint16_t id_for_rotated_part(uint16_t id, uint8_t rotated_position);

/*
 * Rotation dans le sens des aiguilles d'une montres
 * le départ est donc top
 */
struct part *rotatePart(struct part *p, int nbRotate);

int search_max_face(struct array_part *apart);

struct array_part * rotate_all_parts(struct array_part *apart);

struct array_part * search_face(struct array_part *apart, int face, int position);

struct map_part *buildMapPart(struct array_part *apart, int maxFace);
map_big_array *buildBigArray(struct array_part *apart,int maxFace);

//struct array_part *get_parts(struct map_part *map,char *key);
void check_array(struct array_part *apart);
struct array_part *get_parts_bigarray(map_big_array *map,int8_t p[4]);
struct array_part *get_parts_bigarray_with_key(map_big_array *map,key_part *key);

struct map_in_one *regroup_map(map_big_array *map);

int free_map_part(struct map_part *map_parts);
int free_array_part(struct array_part *array_parts);
int free_bigarray(map_big_array *array_parts);
int free_map_in_one(struct map_in_one *map);

struct array_part *copy_array_part(struct array_part *apart);

struct part* get_one_part(map_big_array *map_parts, key_part key);

map_big_array *prepare_map_part(struct array_part *apart);

#endif
