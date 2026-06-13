#ifndef eternityII_possibility_h
#define eternityII_possibility_h

#include <stdint.h>

#include "part.h"
#include "lifo.h"
#include "packed.h"
#include "static_variables.h"

#define DIR_UP 1
#define DIR_RIGHT 2
#define DIR_DOWN 3
#define DIR_LEFT 4
 
 struct possibility_packet
 {
 uint8_t x;
 uint8_t y;
 int16_t grid[ETERN_SIZE][ETERN_SIZE];
 uint16_t alloc;
 #ifdef FACES_USED_BITS
 uint16_t b_faceused[FACES_USED_SIZE] __attribute__ ((aligned (16)));
 #else
 uint8_t faceused[ETERN_PARTS];
 #endif // FACES_USED_BITS
 /// 1 si un client pruner a vérifié que toutes les cases vides ont encore au
 /// moins une pièce candidate. Remis à 0 sur tout paquet issu d'une expansion
 /// (le contrôle ne vaut que pour l'état exact du plateau).
 uint8_t checked;
 } __attribute__((__packed__));

typedef struct
{
	int size;
	struct possibility_packet *possibilities;
} array_possibility_packet;

#ifdef FACES_USED_BITS
/**
 * @brief Marque ou démarque une pièce comme utilisée dans le masque de bits.
 *
 * Stocke l'information sous forme de bitmask compact (1 bit par pièce, groupes de 16).
 * Inline : appelée pour chaque candidat de la boucle chaude de recherche.
 *
 * @param faceused Masque de bits des pièces utilisées (tableau de FACES_USED_SIZE uint16_t).
 * @param part     Identifiant de la pièce (base 0, i.e. id-1).
 * @param boolean  1 = utilisée, 0 = libre.
 */
static inline void set_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part, uint8_t boolean) {
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    int8_t n = part - (groupe << 4);
    number = (number & ~(1 << n)) | (boolean << n);
    faceused[groupe] = number;
}

/**
 * @brief Indique si une pièce est marquée comme utilisée dans le masque de bits.
 *
 * Inline : appelée pour chaque candidat de la boucle chaude de recherche.
 *
 * @param faceused Masque de bits des pièces utilisées.
 * @param part     Identifiant de la pièce (base 0).
 * @return         1 si utilisée, 0 si libre.
 */
static inline uint8_t is_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part) {
    uint16_t groupe = part >> 4;
    return (faceused[groupe] >> (part - (groupe << 4))) & 1;
}
#endif // FACES_USED_BITS
struct possibility_packet *generate_possibility_packet(int x, int y, struct part *etern[ETERN_SIZE][ETERN_SIZE], int directory);
key_part what_search(struct array_part *all_rotate_parts, int x, int y, struct possibility_packet *possiblity);
void what_search_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity,key_part *key);
void what_search_to_key2(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key, int8_t all_face);
void what_search_in_grid_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, int8_t x, int8_t y, key_part *key, int8_t all_face);

void checkIfResultFound(struct possibility_packet *poss, struct array_part *all_rotate_part);
int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);
int possibility_all_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);
int forward_check_next_k(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);
int search_possiblity(File *result,struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part);
int search_possiblity_light(File *result,key_part *key,struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part,int16_t idParts[ETERN_PARTS][4]);

int search_possiblity_light_with_big_table(big_table *result,key_part *key,struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part,int16_t idParts[ETERN_PARTS][4]);

int change_dir(int cur_dir, int x, int y, struct possibility_packet *possiblity);

struct possibility_packet *crypt_to_network(struct possibility_packet *packet);
struct possibility_packet *decrypt_from_network(struct possibility_packet *packet);

int print_possibility_packet(struct possibility_packet *packet);
int save_possibility(char *filename, struct possibility_packet *possibility);

void first_possibility(map_big_array *mapParts, struct array_part *all_rotate_part);
/*
 0 OK
 -1 packet NULL
 -2 x or y > ETERN_SIZE
 -3 directory > or < dir_possibilities
 -4 alloc > ETERN_PARTS (alloc = 0 est l'état genèse, valide)
 -5 alloc <> faceused
 */
int check_possibility(struct possibility_packet *packet, struct array_part *rotateParts);
int normalize_possibility_packet(struct possibility_packet *packet);

int test_directions(void);
int decode_direction(void);

int compare_possibility(struct possibility_packet *packet, struct possibility_packet *other_packet);
int is_origin_of(struct possibility_packet *packet, struct possibility_packet *other_packet);

array_possibility_packet *build_single_array_possibility_packet(struct possibility_packet *possibility);
void free_array_possibility_packet(array_possibility_packet *possibilities);
#endif
