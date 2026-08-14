#ifndef eternityII_part_h
#define eternityII_part_h

#include <stddef.h>
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
 * @brief Valeur maximale représentable sur les 16 bits d'un champ de `packed`.
 *
 * Borne à la fois l'offset d'un compartiment dans l'arène et sa taille.
 */
#define MAP_PACKED_FIELD_MAX 65535u

/**
 * @brief Table de lookup à plat des pièces par contraintes de bord.
 *
 * Remplace l'ancien tableau 4D de pointeurs (4 déréférencements en cascade)
 * par un unique bloc contigu de `sizearray^4` listes, indexé par
 * `((k1*M + k2)*M + k3)*M + k4`. Les listes de candidats elles-mêmes sont
 * compactées bout à bout dans `arena` : un lookup = un calcul d'indice
 * + une lecture, et chaque liste est contiguë en mémoire.
 *
 * `packed` est un INDEX COMPACT redondant sur `flat`, destiné au seul chemin
 * chaud du forward-checking : mêmes `sizearray^4` compartiments, mais réduits
 * à un `uint32_t` `{offset:16 | size:16}` (offset dans `arena`) au lieu d'un
 * `struct array_part` de 16 octets. Sur le puzzle 256 cela ramène la table
 * balayée par la boucle chaude de 5,06 Mo à 1,33 Mo (une ligne de cache
 * couvre 16 compartiments au lieu de 4) — or 98 % de ces compartiments sont
 * vides et ne sont lus que pour tester un compteur.
 */
typedef struct
{
	int sizearray;
    int sizearrayM;
	/** `sizearray^4` listes contiguës ; `parts` pointe dans `arena`. */
	struct array_part *flat;
	/** Toutes les listes de candidats bout à bout. */
	struct part *arena;
	/**
	 * Index compact `{offset:16 | size:16}` de `sizearray^4` entrées, offset
	 * relatif à `arena`. NULL si l'index n'a pas pu être construit (map bâtie
	 * à la main, ou capacité 16 bits dépassée — cf. `map_packed_fits`) ; les
	 * lecteurs retombent alors sur `flat`, cf. `map_bucket_packed`.
	 * Invariant : `packed != NULL` implique `arena != NULL`.
	 */
	uint32_t *packed;
	/**
	 * Masque des IDENTIFIANTS de pièces présents dans chaque compartiment, un
	 * bit par pièce (bit `id - 1`, mot `(id - 1) / 64`), indexé par l'OFFSET
	 * du compartiment dans `arena` : le masque du compartiment commençant à
	 * `offset` occupe `id_mask_words` mots à partir de
	 * `bucket_id_mask[offset * id_mask_words]`. Seuls les offsets de début de
	 * compartiment sont renseignés (les autres restent à zéro) — un tableau
	 * volontairement creux, 0,46 Mo sur le puzzle 256, qui évite une seconde
	 * table indexée par clé (10,6 Mo) ou une indirection supplémentaire.
	 *
	 * Sert au choix de case de l'ordre dynamique MRV (§4.7 de
	 * `docs/conception/elagage_recherche.md`) : compter les pièces ENCORE
	 * LIBRES d'un compartiment devient quelques `popcount` au lieu d'un
	 * parcours de toutes ses entrées. Purement redondant, comme `packed`.
	 *
	 * NULL si non construit (map bâtie à la main, `packed` absent, ou échec
	 * d'allocation) : les lecteurs retombent alors sur le comptage par
	 * parcours, cf. `map_bucket_id_mask`.
	 */
	uint64_t *bucket_id_mask;
	/** Nombre de mots de 64 bits d'un masque de `bucket_id_mask` (0 si absent). */
	int id_mask_words;
} map_big_array;

/**
 * @brief Vue d'un compartiment de candidats, indépendante de sa représentation.
 *
 * Retournée par `map_bucket_packed`, qui sait la produire aussi bien depuis
 * l'index compact que depuis `flat`.
 */
typedef struct
{
	/** Première pièce candidate (ne pas libérer) ; non déréférençable si `size == 0`. */
	const struct part *parts;
	/** Nombre de pièces candidates. */
	int size;
} map_bucket;

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
 * @brief Retourne le compartiment de candidats d'une clé via l'index compact.
 *
 * Variante « boucle chaude » de `get_parts_bigarray_with_key` : lit l'index
 * compact `packed` (4 octets par compartiment) plutôt que `flat` (16 octets),
 * ce qui divise par ~3,8 le volume balayé par le forward-checking.
 *
 * Le résultat est STRICTEMENT identique à celui de `get_parts_bigarray_with_key`
 * pour toute clé (même taille, même liste de pièces) — c'est un changement de
 * représentation, jamais de sémantique.
 *
 * Repli : si la map n'a pas d'index compact (`packed == NULL` — map bâtie à la
 * main dans un test, ou capacité 16 bits dépassée à la construction), la
 * lecture se fait dans `flat`. Le test est une simple lecture d'un pointeur
 * chaud dont l'issue est constante pour toute la durée du processus : il est
 * parfaitement prédit et ne coûte rien face à un défaut de cache évité.
 *
 * @param map Table de lookup pré-calculée.
 * @param key Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 * @return    Vue du compartiment (appartient à la map, ne pas libérer).
 */
static inline map_bucket map_bucket_packed(const map_big_array *map, const key_part *key)
{
	int m = map->sizearray;
	size_t idx = (size_t)((((int)key->k1 * m + key->k2) * m + key->k3) * m + key->k4);
	map_bucket bucket;
	if (map->packed != NULL)
	{
		uint32_t entry = map->packed[idx];
		// arena != NULL est garanti dès que packed != NULL, et offset vaut 0
		// pour un compartiment vide : l'arithmétique de pointeur reste valide.
		bucket.parts = map->arena + (entry >> 16);
		bucket.size = (int)(entry & MAP_PACKED_FIELD_MAX);
	}
	else
	{
		const struct array_part *entry = &map->flat[idx];
		bucket.parts = entry->parts;
		bucket.size = entry->size;
	}
	return bucket;
}

/**
 * @brief Masque des ids de pièces d'un compartiment, ou NULL si indisponible.
 *
 * Complément de `map_bucket_packed` : même clé, même compartiment, mais la
 * réponse est le masque de bits des IDENTIFIANTS présents (bit `id - 1`)
 * plutôt que la liste des entrées. Permet de compter les pièces encore libres
 * par `popcount` contre le masque des pièces utilisées du plateau, sans
 * parcourir le compartiment — cf. `map_mask_free_count`.
 *
 * Renvoie NULL quand l'index n'existe pas (map bâtie à la main, `packed`
 * absent, allocation échouée) OU quand le compartiment est vide : dans les
 * deux cas l'appelant doit retomber sur un comptage par parcours, jamais
 * supposer « zéro pièce libre ».
 *
 * @param map Table de lookup pré-calculée.
 * @param key Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 * @return    Masque de `map->id_mask_words` mots (appartient à la map, ne pas
 *            libérer), ou NULL.
 */
static inline const uint64_t *map_bucket_id_mask(const map_big_array *map, const key_part *key)
{
	// packed est testé aussi : un test peut neutraliser l'index compact d'une
	// map réelle après construction pour forcer le repli sur `flat`.
	if (map->bucket_id_mask == NULL || map->packed == NULL)
	{
		return NULL;
	}
	int m = map->sizearray;
	size_t idx = (size_t)((((int)key->k1 * m + key->k2) * m + key->k3) * m + key->k4);
	// bucket_id_mask != NULL implique packed != NULL (cf. build_bucket_id_mask).
	uint32_t entry = map->packed[idx];
	if ((entry & MAP_PACKED_FIELD_MAX) == 0)
	{
		return NULL;
	}
	return &map->bucket_id_mask[(size_t)(entry >> 16) * (size_t)map->id_mask_words];
}

/**
 * @brief Nombre de pièces d'un compartiment encore libres, par `popcount`.
 *
 * @param mask  Masque des ids du compartiment (`map_bucket_id_mask`).
 * @param words Nombre de mots du masque (`map->id_mask_words`).
 * @param used  Masque des pièces déjà posées, même convention de bits
 *              (bit `id - 1`), au moins `words` mots.
 * @return      Nombre d'ids présents dans le compartiment et non utilisés.
 */
static inline int map_mask_free_count(const uint64_t *mask, int words, const uint64_t *used)
{
	int count = 0;
	for (int w = 0; w < words; w++)
	{
		count += __builtin_popcountll(mask[w] & ~used[w]);
	}
	return count;
}

/**
 * @brief Indique si une arène tient dans les champs 16 bits de l'index compact.
 *
 * Les offsets utilisés vont de 0 à `total_parts - 1` et les tailles de
 * compartiment jusqu'à `max_bucket` : les deux doivent tenir sur 16 bits.
 * Un puzzle plus gros que celui d'Eternity II pourrait dépasser cette borne —
 * dans ce cas l'index n'est PAS construit (jamais tronqué silencieusement) et
 * les lecteurs retombent sur `flat`.
 *
 * @param total_parts Nombre total de pièces dans l'arène (somme des tailles).
 * @param max_bucket  Taille du plus gros compartiment.
 * @return            1 si l'index compact est représentable, 0 sinon.
 */
int map_packed_fits(unsigned long long total_parts, unsigned long long max_bucket);

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
