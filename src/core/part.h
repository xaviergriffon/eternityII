#ifndef eternityII_part_h
#define eternityII_part_h

#include <stdint.h>

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

/** @brief Affiche les champs d'une pièce dans les logs (id, rotation, bords). */
void print_part(struct part *p);

/**
 * @brief Calcule l'indice d'une pièce dans le tableau des rotations.
 *
 * Le tableau `all_rotate_parts` indexe les pièces sous la forme
 * `id + ETERN_PARTS * rotation`. Cette fonction encapsule ce calcul.
 *
 * @param id               Identifiant de la pièce (1..ETERN_PARTS).
 * @param rotated_position Indice de rotation (0..3).
 * @return                 Indice dans `all_rotate_parts->parts[]`.
 */
uint16_t id_for_rotated_part(uint16_t id, uint8_t rotated_position);

/**
 * @brief Fait pivoter une pièce dans le sens des aiguilles d'une montre.
 *
 * La position de départ est TOP. Une rotation d'un quart de tour transforme :
 * top → right, right → bottom, bottom → left, left → top.
 *
 * @param p        Pièce source à pivoter.
 * @param nbRotate Nombre de quarts de tour (0 à 3 ; réduit modulo 4 si supérieur).
 * @return         Nouvelle pièce allouée représentant la pièce pivotée (à libérer par l'appelant).
 */
struct part *rotatePart(struct part *p, int nbRotate);

/**
 * @brief Retourne la valeur maximale de couleur de bord présente dans le tableau.
 *
 * Parcourt les quatre bords de chaque pièce pour trouver la valeur la plus élevée.
 * Sert à dimensionner les structures de lookup.
 *
 * @param apart Tableau de pièces à analyser.
 * @return      Valeur maximale de face trouvée.
 */
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

/**
 * @brief Filtre les pièces du tableau selon une valeur de face et sa position.
 *
 * Retourne un nouveau tableau contenant uniquement les pièces dont le bord
 * `position` (PART_TOP / RIGHT / BOTTOM / LEFT) vaut `face`. Si `face` vaut
 * FACE_UNKNOW, toute valeur non nulle est acceptée.
 *
 * @param apart    Tableau de pièces source.
 * @param face     Valeur de face recherchée (ou FACE_UNKNOW).
 * @param position Position du bord à tester (PART_TOP, PART_RIGHT, PART_BOTTOM, PART_LEFT, PART_NONE).
 * @return         Nouveau tableau alloué contenant les pièces correspondantes (à libérer par l'appelant).
 */
struct array_part * search_face(struct array_part *apart, int face, int position);

/**
 * @brief Construit la map de lookup par hachage textuel (ancienne implémentation).
 *
 * @param apart   Tableau de toutes les rotations.
 * @param maxFace Valeur maximale de couleur de bord.
 * @return        Map allouée (à libérer avec `free_map_part`).
 */
struct map_part *buildMapPart(struct array_part *apart, int maxFace);

/**
 * @brief Construit la table de lookup 4D indexée par (top, right, bottom, left).
 *
 * Pour chaque combinaison possible de valeurs de bord (−1 à maxFace), stocke la
 * liste des pièces compatibles. Les valeurs −1 représentent « toute couleur ».
 * Un accès direct en O(1) retourne toutes les pièces posables à un emplacement.
 *
 * @param apart   Tableau de toutes les rotations (sortie de `rotate_all_parts`).
 * @param maxFace Valeur maximale de couleur de bord (sortie de `search_max_face`).
 * @return        Tableau 4D alloué (à libérer avec `free_bigarray`).
 */
map_big_array *buildBigArray(struct array_part *apart, int maxFace);

/**
 * @brief Vérifie la cohérence des pièces d'un tableau (ids valides).
 *
 * Affiche dans les logs les pièces dont l'id est hors plage [0, 256].
 *
 * @param apart Tableau à vérifier (peut être NULL).
 */
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

/**
 * @brief Aplatit le tableau 4D en une structure linéaire `map_in_one`.
 *
 * Concatène toutes les pièces de tous les compartiments dans un seul tableau
 * contigu avec des index `position` et `quantity` pour accéder par compartiment.
 *
 * @param map Tableau 4D source.
 * @return    Structure `map_in_one` allouée (à libérer avec `free_map_in_one`).
 */
struct map_in_one *regroup_map(map_big_array *map);

/**
 * @brief Libère la mémoire d'une `map_part` (ancienne map par hachage).
 * @param map_parts Map à libérer.
 * @return          0.
 */
int free_map_part(struct map_part *map_parts);

/**
 * @brief Libère un `array_part` et son tableau de pièces interne.
 * @param array_parts Tableau à libérer (peut être NULL).
 * @return            0.
 */
int free_array_part(struct array_part *array_parts);

/**
 * @brief Libère la mémoire du tableau de lookup 4D `map_big_array`.
 * @param array_parts Structure 4D à libérer.
 * @return            0.
 */
int free_bigarray(map_big_array *array_parts);

/**
 * @brief Libère la mémoire d'un `map_in_one`.
 * @param map Structure à libérer.
 * @return    0.
 */
int free_map_in_one(struct map_in_one *map);

/**
 * @brief Crée une copie profonde d'un `array_part`.
 *
 * @param apart Tableau source (peut être NULL).
 * @return      Nouveau tableau alloué avec les mêmes pièces, ou NULL si `apart` est NULL.
 */
struct array_part *copy_array_part(struct array_part *apart);

/**
 * @brief Retourne une pièce unique correspondant à la clé, ou NULL s'il y en a plusieurs.
 *
 * Utile pour les emplacements n'admettant qu'une seule pièce possible (indices fixes).
 *
 * @param map_parts Tableau 4D de lookup.
 * @param key       Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 * @return          Pointeur vers la pièce si et seulement si exactement une correspond ; NULL sinon.
 */
struct part* get_one_part(map_big_array *map_parts, key_part key);

/**
 * @brief Construit la table de lookup 4D prête à l'emploi pour le moteur de recherche.
 *
 * Enchaîne `search_max_face` puis `buildBigArray` sur le tableau de rotations fourni.
 *
 * @param apart Tableau de toutes les rotations (sortie de `rotate_all_parts`).
 * @return      Tableau 4D alloué (à libérer avec `free_bigarray`).
 */
map_big_array *prepare_map_part(struct array_part *apart);

#endif
