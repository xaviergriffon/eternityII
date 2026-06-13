#ifndef eternityII_part_h
#define eternityII_part_h

#include <stdint.h>

#include "packed.h"

#define PART_NONE -1
#define PART_TOP 0
#define PART_RIGHT 1
#define PART_BOTTOM 2
#define PART_LEFT 3
#define FACE_UNKNOW -1
#define MAX_KEY_LENGTH 12
#define MAX_FACE_MAP 24


typedef struct
{
    int8_t k1;
    int8_t k2;
	int8_t k3;
	int8_t k4;
} __attribute__((__packed__)) key_part;

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

/**
 * @brief Table de lookup à plat des pièces par contraintes de bord.
 *
 * Remplace l'ancien tableau 4D de pointeurs (4 déréférencements en cascade)
 * par un unique bloc contigu de `sizearray^4` listes, indexé par
 * `((k1*M + k2)*M + k3)*M + k4`. Les listes de candidats elles-mêmes sont
 * compactées bout à bout dans `arena` : un lookup = un calcul d'indice
 * + une lecture, et chaque liste est contiguë en mémoire.
 */
typedef struct
{
	int sizearray;
    int sizearrayM;
	/** `sizearray^4` listes contiguës ; `parts` pointe dans `arena`. */
	struct array_part *flat;
	/** Toutes les listes de candidats bout à bout. */
	struct part *arena;
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

/**
 * @brief Fait tourner toutes les pièces dans le tableau donné.
 *
 * Cette fonction prend un tableau de pièces et fait tourner chaque pièce.
 *
 * @param apart Pointeur vers le tableau de pièces à faire tourner.
 * @return Pointeur vers le tableau de pièces après rotation.
 */
struct array_part * rotate_all_parts(struct array_part *apart);

struct array_part * search_face(struct array_part *apart, int face, int position);

struct map_part *buildMapPart(struct array_part *apart, int maxFace);
map_big_array *buildBigArray(struct array_part *apart,int maxFace);

//struct array_part *get_parts(struct map_part *map,char *key);
void check_array(struct array_part *apart);

/**
 * @brief Retourne les pièces compatibles avec les quatre contraintes de bord données.
 *
 * Lookup direct dans la table à plat : un calcul d'indice et une lecture.
 *
 * @param map Table de lookup pré-calculée.
 * @param p   Tableau de 4 valeurs de face [top, right, bottom, left] (0 = bordure, sizearrayM = libre).
 * @return    Tableau de pièces compatibles (appartient à la map, ne pas libérer).
 */
static inline struct array_part *get_parts_bigarray(map_big_array *map, int8_t p[4])
{
	int m = map->sizearray;
	return &map->flat[(((int)p[0] * m + p[1]) * m + p[2]) * m + p[3]];
}

/**
 * @brief Retourne les pièces compatibles avec une `key_part` de recherche.
 *
 * Variante de `get_parts_bigarray` acceptant une `key_part` plutôt qu'un tableau brut.
 *
 * @param map Table de lookup pré-calculée.
 * @param key Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 * @return    Tableau de pièces compatibles (appartient à la map, ne pas libérer).
 */
static inline struct array_part *get_parts_bigarray_with_key(map_big_array *map, key_part *key)
{
	int m = map->sizearray;
	return &map->flat[(((int)key->k1 * m + key->k2) * m + key->k3) * m + key->k4];
}

struct map_in_one *regroup_map(map_big_array *map);

int free_map_part(struct map_part *map_parts);
int free_array_part(struct array_part *array_parts);
int free_bigarray(map_big_array *array_parts);
int free_map_in_one(struct map_in_one *map);

struct array_part *copy_array_part(struct array_part *apart);

struct part* get_one_part(map_big_array *map_parts, key_part key);

map_big_array *prepare_map_part(struct array_part *apart);

#endif
