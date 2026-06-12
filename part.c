#include "part.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "static_variables.h"

#include "logger.h"
#include "readdata.h"

int error = 0;
int relocated = 0;
int maxrelocate = 0;

/**
 * @brief Affiche les informations d'une pièce dans les logs.
 * @param p Pointeur vers la pièce à afficher.
 */
void print_part(struct part *p)
{
	log_info("part [id:%i,rotation:%i / top:%i,left:%i,bottom:%i,right:%i]\n", p->id, p->rotation, p->top, p->left, p->bottom, p->right);
}

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
struct part *rotatePart(struct part *p, int nbRotate)
{
	struct part *result = malloc(sizeof(*result));
	result->id = p->id;
	result->top = p->top;
	result->right = p->right;
	result->bottom = p->bottom;
	result->left = p->left;

	int tour = nbRotate;
	if (tour >= 4)
	{
		tour = tour % 4;
	}
	result->rotation = tour;

	int t;
	for (t = 0; t < tour; t++)
	{
		int8_t left = result->left;
		result->left = result->bottom;
		result->bottom = result->right;
		result->right = result->top;
		result->top = left;
	}

	return result;
}

/**
 * @brief Génère les quatre rotations de chaque pièce du tableau source.
 *
 * La pièce d'id `i` (1..apart->size) en rotation `r` se trouve à l'indice
 * `i + ETERN_PARTS * r` dans le tableau résultant. L'indice 0 reste le bouchon
 * id = 0 (calloc), et `size` couvre l'indice maximal `apart->size + ETERN_PARTS * 3`.
 *
 * Les ids commençant à 1, itérer depuis l'indice 0 décalerait tout d'un cran :
 * le bouchon occuperait les indices `ETERN_PARTS * r` et la dernière pièce ne
 * serait jamais intégrée — donc absente de la map et inutilisable par la recherche.
 *
 * @param apart Tableau de pièces originales.
 * @return      Nouveau tableau de toutes les rotations (à libérer par l'appelant).
 */
struct array_part *rotate_all_parts(struct array_part *apart)
{
	struct array_part *result = malloc(sizeof *result);
	// + 1 pour le bouchon id = 0 à l'indice 0
	result->size = apart->size * 4 + 1;
	// TODO : pourquoi +4 ?
	// Sans doute pour la propriété size de la structure array_part mais ce n'est pas très propre
	result->parts = calloc((result->size + 4), sizeof(struct part));

	int i;
	for (i = 1; i <= apart->size; i++)
	{
		struct part *part = &apart->parts[i];
		int r;
		for (r = 0; r < 4; r++)
		{

			struct part *rotatepart = rotatePart(part, r);
			int position = i + ETERN_PARTS * r;
			memcpy(&result->parts[position], rotatepart, sizeof(struct part));
			// print_part(rotatepart);
			free(rotatepart);
		}
	}
	return result;
}

/**
 * @brief Retourne la valeur maximale de couleur de bord présente dans le tableau de pièces.
 *
 * Parcourt les quatre bords (top, right, bottom, left) de chaque pièce pour
 * trouver la valeur la plus élevée. Sert à dimensionner les structures de lookup.
 *
 * @param apart Tableau de pièces à analyser.
 * @return      Valeur maximale de face trouvée.
 */
int search_max_face(struct array_part *apart)
{
	int maxface = 0;
	int i;
	for (i = 0; i < apart->size; i++)
	{
		struct part *p = &apart->parts[i];
		if (p->top > maxface)
		{
			maxface = p->top;
		}
		if (p->right > maxface)
		{
			maxface = p->right;
		}
		if (p->bottom > maxface)
		{
			maxface = p->bottom;
		}
		if (p->left > maxface)
		{
			maxface = p->left;
		}
	}
	return maxface;
}

/**
 * @brief Recherche des pièces dans le tableau qui correspondent à la face et à la position spécifiées.
 *
 * Cette fonction parcourt le tableau de pièces donné et recherche les pièces
 * qui correspondent aux critères de face et de position spécifiés. Elle renvoie un nouveau tableau
 * contenant les pièces correspondantes.
 *
 * @param apart Pointeur vers le tableau de pièces à rechercher.
 * @param face La valeur de la face à rechercher. Si FACE_UNKNOW, toute valeur de face non nulle est considérée comme une correspondance.
 * @param position La position à rechercher (PART_TOP, PART_RIGHT, PART_BOTTOM, PART_LEFT, ou PART_NONE).
 * @return Un pointeur vers une nouvelle structure array_part contenant les pièces correspondantes.
 *
 * @note L'appelant est responsable de la libération de la mémoire allouée pour la structure array_part retournée.
 */
struct array_part *search_face(struct array_part *apart, int face, int position)
{

	struct list_part *list = malloc(sizeof *list);
	list->value = NULL;
	list->next = NULL;
	struct list_part *curList = NULL;
	curList = list;
	int array_size = 0;
	int i;
	for (i = 0; i < apart->size; i++)
	{
		int present = 0;
		struct part *p = &apart->parts[i];
		if ((position == PART_TOP || position == PART_NONE) && (p->top == face || (face == FACE_UNKNOW && p->top != 0)))
		{
			present = 1;
		}
		else if ((position == PART_RIGHT || position == PART_NONE) && (p->right == face || (face == FACE_UNKNOW && p->right != 0)))
		{
			present = 1;
		}
		else if ((position == PART_BOTTOM || position == PART_NONE) && (p->bottom == face || (face == FACE_UNKNOW && p->bottom != 0)))
		{
			present = 1;
		}
		else if ((position == PART_LEFT || position == PART_NONE) && (p->left == face || (face == FACE_UNKNOW && p->left != 0)))
		{
			present = 1;
		}

		if (present != 0)
		{
			if (curList->value == NULL)
			{
				curList->value = p;
			}
			else
			{
				struct list_part *newList = malloc(sizeof *newList);
				newList->value = p;
				newList->next = NULL;
				curList->next = newList;
				curList = newList;
			}
			array_size++;
		}
	}

	struct array_part *result = malloc(sizeof(struct array_part));
	result->parts = NULL;
	result->size = 0;
	if (array_size > 0)
	{
		size_t allocsize = array_size * sizeof(struct part);
		struct part *parts = malloc(allocsize);
		result->parts = parts;
	}
	else
	{
		result->parts = NULL;
	}

	curList = list;
	int c = 0;
	result->size = array_size;
	while (curList != NULL)
	{
		if (curList->value != NULL)
		{
			result->parts[c] = *curList->value;
			c++;
		}
		else
		{
			if (c > 1)
			{
				log_info("no value ligne:%i\n", c);
			}
		}
		struct list_part *last = curList;
		curList = curList->next;

		free(last);
	}
	return result;
}

/**
 * @brief Fonction de hachage d'un entier (Robert Jenkins' 32-bit Mix).
 *
 * Utilisée en interne pour distribuer les clés dans la table de hachage.
 * Le résultat est réduit modulo 1024.
 *
 * @param key Clé entière à hacher.
 * @return    Valeur de hachage dans l'intervalle [0, 1023].
 */
unsigned long hashmap_hash_int(unsigned long key)
{
	/* Robert Jenkins' 32 bit Mix Function */
	key += (key << 12);
	key ^= (key >> 22);
	key += (key << 4);
	key ^= (key >> 9);
	key += (key << 10);
	key ^= (key >> 2);
	key += (key << 7);
	key ^= (key >> 12);

	/* Knuth's Multiplicative Method */
	// key = (key >> 3) * 2654435761;

	return key % 1024;
}

/**
 * @brief Calcule un hash djb2-like sur une chaîne de caractères.
 *
 * Variante : hash = (hash << 5) + hash + c (équivalent à hash * 33 + c).
 * Utilisé pour indexer les pièces par clé textuelle dans `buildMapPart`.
 *
 * @param str Chaîne à hacher (terminée par '\0').
 * @return    Valeur de hachage non signée.
 */
unsigned int hash(char *str)
{
	unsigned int hash = *str; // 5381
	while (*str != 0)
	{
		int c = *str;
		// hash = hash * 33 + c;
		hash = ((hash << 5) + hash) + c;
		str++;
	}

	return hash;
}

/**
 * @brief Insère un tableau de pièces dans la table de hachage à la clé donnée.
 *
 * En cas de collision, sonde linéairement la table jusqu'à un emplacement libre.
 * Le tableau `apart` est copié en profondeur dans la table.
 *
 * @param map     Table de hachage cible.
 * @param key_int Valeur de hachage de la clé textuelle.
 * @param key     Clé textuelle (ownership transféré à la table).
 * @param apart   Tableau de pièces à stocker.
 * @return        Indice de l'emplacement utilisé dans la table.
 */
int put_part(struct map_part *map, unsigned int key_int, char *key, struct array_part *apart)
{
	int l = key_int % map->size;
	int first = l;
	int r = 0;
	while (map->elements[l].key_int != 0 && l < map->sizemap)
	{
		r++;
		l++;
	}
	if (first != l)
		relocated++;
	if (r > maxrelocate)
		maxrelocate = r;
	if (l >= map->sizemap)
	{
		log_error("map trop petite \n");
		exit(EXIT_FAILURE);
	}
	if (map->elements[l].key_int == 0)
	{
		map->elements[l].key_int = key_int;
		map->elements[l].key = key;
		map->elements[l].apart = copy_array_part(apart);
	}
	else
	{
		log_error("Probleme d'emplacement ligne:%i key_int:%i key:%s\n", l, key_int, key);
		error++;
	}

	return l;
}

/**
 * @brief Recherche un tableau de pièces dans la table de hachage par clé textuelle.
 *
 * @param map Table de hachage dans laquelle chercher.
 * @param key Clé textuelle au format "f1_f2_f3_f4" (faces top, right, bottom, left).
 * @return    Tableau de pièces correspondant, ou NULL si la clé est absente.
 */
struct array_part *get_parts(struct map_part *map, char *key)
{
	struct array_part *parts = NULL;

	int key_int = hash(key);
	int l = abs(key_int) % map->size;
	struct map_part_element *mpe = NULL;
	while (mpe == NULL && l < map->sizemap)
	{
		struct map_part_element *temp = &map->elements[l];
		if (temp != NULL && temp->key_int == key_int && strcmp(key, temp->key) == 0)
		{
			mpe = &map->elements[l];
		}
		l++;
	}
	if (mpe != NULL)
	{
		parts = mpe->apart;
	}
	return parts;
}

/**
 * @brief Convertit une valeur de face -1 (toute face) en l'indice maximal du tableau.
 *
 * Dans la `map_big_array`, la dimension supplémentaire à l'indice `maxFaceM`
 * représente « n'importe quelle face ». Cette fonction effectue la conversion
 * pour que le lookup dans le tableau 4D reste valide.
 *
 * @param p        Valeur de face (-1 = toute face, ≥ 0 = couleur spécifique).
 * @param maxFaceM Indice maximal du tableau (= map->sizearrayM).
 * @return         Indice utilisable directement dans `bigarray`.
 */
int8_t convert_p(int8_t p, int maxFaceM)
{
	int8_t result = p;
	if (result == -1)
	{
		// TODO : voir pour passer en dur
		result = maxFaceM;
	}
	return result;
}

/**
 * @brief Retourne les pièces compatibles avec les quatre contraintes de bord données.
 *
 * Effectue une lookup directe dans la structure 4D indexée par (top, right, bottom, left).
 *
 * @param map Tableau 4D des pièces pré-calculé.
 * @param p   Tableau de 4 valeurs de face [top, right, bottom, left] (0 = bordure, -1 = libre).
 * @return    Tableau de pièces compatibles (appartient à la map, ne pas libérer).
 */
struct array_part *get_parts_bigarray(map_big_array *map, int8_t p[4])
{
	struct array_part *parts = NULL;
	/*
		int8_t k1 = convert_p(p[0], map->sizearrayM);
		int8_t k2 = convert_p(p[1], map->sizearrayM);
		int8_t k3 = convert_p(p[2], map->sizearrayM);
		int8_t k4 = convert_p(p[3], map->sizearrayM);
	 */
	int8_t k1 = p[0];
	int8_t k2 = p[1];
	int8_t k3 = p[2];
	int8_t k4 = p[3];
	parts = map->bigarray[k1][k2][k3][k4];
	//	if(parts->size > 0 && parts->parts[0].id <0) {
	//		printf("get_parts_bigarray error : size:%i for %i:%i:%i:%i-%i:%i:%i:%i r[0].id = %i\n",parts->size,p[0],p[1],p[2],p[3],k1,k2,k3,k4,parts->parts[0].id );
	//	}
	return parts;
}
/**
 * @brief Retourne les pièces compatibles avec une `key_part` de recherche.
 *
 * Variante de `get_parts_bigarray` acceptant une `key_part` plutôt qu'un tableau brut.
 *
 * @param map Tableau 4D des pièces pré-calculé.
 * @param key Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 * @return    Tableau de pièces compatibles (appartient à la map, ne pas libérer).
 */
struct array_part *get_parts_bigarray_with_key(map_big_array *map, key_part *key)
{
	struct array_part *parts = NULL;
	/*
	int8_t k1 = convert_p(key->k1, map->sizearrayM);
	int8_t k2 = convert_p(key->k2, map->sizearrayM);
	int8_t k3 = convert_p(key->k3, map->sizearrayM);
	int8_t k4 = convert_p(key->k4, map->sizearrayM);
		 */
	int8_t k1 = (int)key->k1;
	int8_t k2 = (int)key->k2;
	int8_t k3 = (int)key->k3;
	int8_t k4 = (int)key->k4;
	parts = map->bigarray[k1][k2][k3][k4];
	//	if(parts->size > 0 && parts->parts[0].id <0) {
	//		printf("get_parts_bigarray error : size:%i for %i:%i:%i:%i-%i:%i:%i:%i r[0].id = %i\n",parts->size,p[0],p[1],p[2],p[3],k1,k2,k3,k4,parts->parts[0].id );
	//	}
	return parts;
}

/**
 * @brief Vérifie l'intégrité d'un tableau de pièces et logue les anomalies.
 *
 * Signale dans les logs toute pièce dont l'id est hors de la plage [0, 256].
 * Utilisé à des fins de débogage.
 *
 * @param apart Tableau de pièces à vérifier (peut être NULL).
 */
void check_array(struct array_part *apart)
{
	log_info("check_array :\n");
	if (apart != NULL)
	{
		log_info("size:%i\n", apart->size);
		for (int i = 0; i < apart->size; i++)
		{
			struct part p = apart->parts[i];
			if (p.id < 0 || p.id > 256)
			{
				log_info("p[%i] id false:%i\n", i, p.id);
				print_part(&p);
			}
		}
	}
	else
	{
		log_info("array_part NULL\n");
	}
	log_info("check_array end\n");
}

/**
 * @brief Construit la structure de lookup 4D indexée par (top, right, bottom, left).
 *
 * Pour chaque combinaison possible de valeurs de bord (de -1 à maxFace),
 * stocke la liste des pièces dont les bords correspondent. Les valeurs -1
 * représentent « toute couleur » et sont mappées à l'indice supplémentaire
 * `maxFace + 1`.
 *
 * Cette structure est la principale table de lookup du moteur de recherche :
 * un accès direct en O(1) retourne toutes les pièces posables à un emplacement.
 *
 * @param apart   Tableau de toutes les rotations (sortie de `rotate_all_parts`).
 * @param maxFace Valeur maximale de couleur de bord (sortie de `search_max_face`).
 * @return        Tableau 4D alloué (à libérer avec `free_bigarray`).
 */
map_big_array *buildBigArray(struct array_part *apart, int maxFace)
{
	int sizeBigArray = (maxFace + 2);
	map_big_array *result = malloc(sizeof(map_big_array));
	result->sizearray = sizeBigArray;
	result->sizearrayM = sizeBigArray - 1;
	result->bigarray = malloc(sizeof(big_array) * sizeBigArray);
	struct array_part *****big_array = result->bigarray;

	int maxarray = 0;
	int f1;
	for (f1 = -1; f1 <= maxFace; f1++)
	{
		struct array_part *arraypart1 = search_face(apart, f1, PART_TOP);
		int p1 = f1;
		if (abs(p1) != p1)
		{
			p1 = maxFace + abs(p1);
		}

		big_array[p1] = malloc(sizeof(struct array_part ***) * sizeBigArray);

		int f2;
		for (f2 = -1; f2 <= maxFace; f2++)
		{
			int p2 = f2;
			if (abs(p2) != p2)
			{
				p2 = maxFace + abs(p2);
			}
			big_array[p1][p2] = malloc(sizeof(struct array_part **) * sizeBigArray);

			struct array_part *arraypart2 = search_face(arraypart1, f2, PART_RIGHT);
			//            if(f1 >=0 && f2>=0 && arraypart2->size > maxarray)
			//			{
			//				maxarray = arraypart2->size;
			//			}
			int f3;
			for (f3 = -1; f3 <= maxFace; f3++)
			{
				int p3 = f3;
				if (abs(p3) != p3)
				{
					p3 = maxFace + abs(p3);
				}
				big_array[p1][p2][p3] = malloc(sizeof(struct array_part *) * sizeBigArray);

				struct array_part *arraypart3 = search_face(arraypart2, f3, PART_BOTTOM);

				int f4;
				for (f4 = -1; f4 <= maxFace; f4++)
				{
					int p4 = f4;
					if (abs(p4) != p4)
					{
						p4 = maxFace + abs(p4);
					}

					struct array_part *arraypart = search_face(arraypart3, f4, PART_LEFT);
					if (f1 >= 0 || f2 >= 0 || f3 >= 0 || f4 >= 0)
					{
						if (arraypart->size > maxarray)
						{
							maxarray = arraypart->size;
						}
					}
					big_array[p1][p2][p3][p4] = copy_array_part(arraypart);

					free_array_part(arraypart);
				}
				free_array_part(arraypart3);
			}
			free_array_part(arraypart2);
		}
		free_array_part(arraypart1);
	}
#ifdef DEBUG_CHECK_POSSIBILITY
	log_info("max array:%i\n", maxarray);
#endif // DEBUG_CHECK_POSSIBILITY
	return result;
}

/**
 * @brief Construit la table de hachage de lookup des pièces (implémentation legacy).
 *
 * Indexe les pièces par clé textuelle "top_right_bottom_left".
 * Conservée pour compatibilité ; préférer `buildBigArray` qui est plus rapide.
 *
 * @param apart   Tableau de toutes les rotations.
 * @param maxFace Valeur maximale de couleur de bord.
 * @return        Table de hachage allouée (à libérer avec `free_map_part`).
 */
struct map_part *buildMapPart(struct array_part *apart, int maxFace)
{
	error = 0;
	struct map_part *result = malloc(sizeof *result);
	result->size = pow((maxFace + 2), 4);
	// Considérant un tot de 50% de croisement du hash, on répercute sur la taille
	result->sizemap = result->size * 1.5;
	long size = (result->sizemap * sizeof(*result->elements));
	log_info("taille part : %i\n", apart->size);
	log_info("nb mappart : %i\n", result->sizemap);
	log_info("alloc : %li\n", size);
	result->elements = calloc(result->sizemap, sizeof(*result->elements));
	int f1;
	unsigned int key_int;

	for (f1 = -1; f1 <= maxFace; f1++)
	{
		char *c1 = malloc(MAX_KEY_LENGTH * sizeof(char)); // calloc('\0', MAX_KEY_LENGTH * sizeof(char));
		sprintf(c1, "%d", f1);

		struct array_part *arraypart1 = search_face(apart, f1, PART_TOP);

		int f2;
		for (f2 = -1; f2 <= maxFace; f2++)
		{
			char *c2 = malloc(MAX_KEY_LENGTH * sizeof(char)); // calloc('\0', MAX_KEY_LENGTH * sizeof(char));
			sprintf(c2, "%s_%d", c1, f2);

			struct array_part *arraypart2 = search_face(arraypart1, f2, PART_RIGHT);

			int f3;
			for (f3 = -1; f3 <= maxFace; f3++)
			{
				char *c3 = malloc(MAX_KEY_LENGTH * sizeof(char)); // calloc('\0', MAX_KEY_LENGTH * sizeof(char));
				sprintf(c3, "%s_%d", c2, f3);

				struct array_part *arraypart3 = search_face(arraypart2, f3, PART_BOTTOM);

				int f4;
				for (f4 = -1; f4 <= maxFace; f4++)
				{
					char *c4 = malloc(MAX_KEY_LENGTH * sizeof(char)); // calloc('\0', MAX_KEY_LENGTH * sizeof(char));
					sprintf(c4, "%s_%d", c3, f4);
					key_int = hash(c4);
					struct array_part *arraypart = search_face(arraypart3, f4, PART_LEFT);
					put_part(result, key_int, c4, arraypart);
					free_array_part(arraypart);
				}
				free_array_part(arraypart3);
			}
			free_array_part(arraypart2);
		}
		free_array_part(arraypart1);
	}
	log_info("nb erreur :%i\n", error);
	log_info("relocalisé : %i\n", relocated);
	log_info("max relocate : %i\n", maxrelocate);
	return result;
}

/**
 * @brief Libère la mémoire d'une table de hachage `map_part`.
 * @param map_parts Table à libérer.
 * @return 0.
 */
int free_map_part(struct map_part *map_parts)
{

	int i;
	for (i = 0; i < map_parts->sizemap; i++)
	{
		free(map_parts->elements[i].key);
		if (map_parts->elements[i].apart != NULL)
		{
			free_array_part(map_parts->elements[i].apart);
		}
	}
	free(map_parts->elements);
	free(map_parts);
	return 0;
}

/**
 * @brief Libère la mémoire d'un `array_part` et de son tableau de pièces interne.
 * @param array_parts Tableau à libérer (peut être NULL).
 * @return 0.
 */
int free_array_part(struct array_part *array_parts)
{
	if (array_parts != NULL)
	{
		free(array_parts->parts);
	}
	free(array_parts);

	return 0;
}

/**
 * @brief Libère la mémoire du tableau de lookup 4D `map_big_array`.
 *
 * Libère récursivement les quatre niveaux du tableau et tous les `array_part` qu'il contient.
 *
 * @param array_parts Structure 4D à libérer.
 * @return 0.
 */
int free_bigarray(map_big_array *array_parts)
{
	int sizeBigArray = array_parts->sizearray;
	int f1;
	for (f1 = 0; f1 < sizeBigArray; f1++)
	{
		int f2;
		for (f2 = 0; f2 < sizeBigArray; f2++)
		{
			int f3;
			for (f3 = 0; f3 < sizeBigArray; f3++)
			{
				int f4;
				for (f4 = 0; f4 < sizeBigArray; f4++)
				{
					free_array_part(array_parts->bigarray[f1][f2][f3][f4]);
				}
				free(array_parts->bigarray[f1][f2][f3]);
			}
			free(array_parts->bigarray[f1][f2]);
		}
		free(array_parts->bigarray[f1]);
	}
	free(array_parts->bigarray);
	free(array_parts);
	return 0;
}

/**
 * @brief Libère la mémoire d'un `map_in_one`.
 * @param map Structure à libérer.
 * @return 0.
 */
int free_map_in_one(struct map_in_one *map)
{
	if (map->position != NULL)
	{
		free(map->position);
	}
	if (map->quantity != NULL)
	{
		free(map->quantity);
	}
	if (map->parts != NULL)
	{
		free(map->parts);
	}
	free(map);
	return 0;
}

/**
 * @brief Crée une copie profonde d'un `array_part`.
 *
 * @param apart Tableau source à copier (peut être NULL).
 * @return      Nouveau tableau alloué avec les mêmes pièces, ou NULL si `apart` est NULL.
 */
struct array_part *copy_array_part(struct array_part *apart)
{
	struct array_part *result = NULL;

	if (apart != NULL)
	{
		result = malloc(sizeof(*result));
		result->size = apart->size;
		result->parts = NULL;

		if (apart->size > 0)
		{
			int sizeofarray = apart->size * sizeof(struct part);
			result->parts = malloc(sizeofarray);
			int i;
			for (i = 0; i < apart->size; i++)
			{
				struct part *part = &apart->parts[i];
				memcpy(&result->parts[i], part, sizeof(*part));
			}
		}
	}

	return result;
}

/**
 * @brief Retourne une pièce unique correspondant à la clé, ou NULL s'il y en a plusieurs.
 *
 * Utile pour les emplacements contraignants n'admettant qu'une seule pièce possible
 * (typiquement les indices fixes du puzzle officiel).
 *
 * @param map_parts Tableau 4D de lookup.
 * @param key       Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 * @return          Pointeur vers la pièce si et seulement si exactement une pièce correspond ; NULL sinon.
 */
struct part *get_one_part(map_big_array *map_parts, key_part key)
{
	struct part *result = NULL;
	if (key.k1 > -2)
	{
		int8_t p[4] = {key.k1, key.k2, key.k3, key.k4};
		struct array_part *search = get_parts_bigarray(map_parts, p);
		if (search != NULL && search->size == 1)
		{
			result = &search->parts[0];
		}
	}

	return result;
}

/**
 * @brief Construit le tableau de lookup 4D à partir des pièces avec rotations.
 *
 * Enchaîne `search_max_face` et `buildBigArray`. C'est le point d'entrée principal
 * pour préparer la structure de recherche de pièces.
 *
 * @param rotateParts Tableau de toutes les rotations (sortie de `rotate_all_parts`).
 * @return            Tableau 4D alloué (à libérer avec `free_bigarray`).
 */
map_big_array *prepare_map_part(struct array_part *rotateParts)
{
	int maxface = search_max_face(rotateParts);
	map_big_array *mapParts = buildBigArray(rotateParts, maxface);

	return mapParts;
}

/**
 * @brief Aplatit le tableau 4D en une structure linéaire `map_in_one`.
 *
 * Concatène toutes les pièces de tous les compartiments du tableau 4D dans un
 * seul tableau contigu, avec des tableaux d'index `position` et `quantity` pour
 * retrouver les pièces d'un compartiment donné. Permet un accès cache-friendly.
 *
 * @param map Tableau 4D source.
 * @return    Structure `map_in_one` allouée (à libérer avec `free_map_in_one`).
 */
struct map_in_one *regroup_map(map_big_array *map)
{
	struct map_in_one *result = malloc(sizeof(struct map_in_one));

	int sizeBigArray = map->sizearray;
	result->nbarrays = pow(sizeBigArray, 4);

	result->position = malloc(sizeof(int) * result->nbarrays);
	result->quantity = malloc(sizeof(int) * result->nbarrays);
	result->parts = malloc(sizeof(struct part) * result->nbarrays);
	int nbParts = 0;
	int dsize = sizeBigArray * sizeBigArray;
	int f1;
	for (f1 = 0; f1 < sizeBigArray; f1++)
	{
		int f2;
		for (f2 = 0; f2 < sizeBigArray; f2++)
		{
			int f3;
			for (f3 = 0; f3 < sizeBigArray; f3++)
			{
				int f4;
				for (f4 = 0; f4 < sizeBigArray; f4++)
				{

					int narray = f1 * sizeBigArray * dsize + f2 * dsize + f3 * sizeBigArray + f4;
					result->quantity[narray] = 0;
					result->position[narray] = 0;
					struct array_part *apart = map->bigarray[f1][f2][f3][f4];
					if (apart != NULL)
					{
						result->quantity[narray] = apart->size;
						result->position[narray] = nbParts;
						int p;
						for (p = 0; p < apart->size; p++)
						{
							memcpy(&result->parts[nbParts + p], &apart->parts[p], sizeof(apart->parts[p]));
						}
						if (apart->size > 1024)
						{
							log_info("apart->size > 1024\n");
						}
						nbParts += apart->size;
					}
					else
					{
						log_info("apart null\n");
					}
				}
			}
		}
	}
	if (nbParts > 0)
	{
		// result->parts = realloc(result->parts, sizeof(struct part) * nbParts);
	}
	else
	{
		free(result->parts);
		result->parts = NULL;
	}
	result->nbparts = nbParts;
	log_info("nbparts:%i\n", nbParts);

	return result;
}

/**
 * @brief Calcule l'indice d'une pièce dans le tableau des rotations.
 *
 * Le tableau `all_rotate_parts` (sortie de `rotate_all_parts`) indexe les pièces
 * sous la forme `id + ETERN_PARTS * rotation`. Cette fonction encapsule ce calcul.
 *
 * @param id               Identifiant de la pièce (1..ETERN_PARTS).
 * @param rotated_position Indice de rotation (0..3).
 * @return                 Indice dans `all_rotate_parts->parts[]`.
 */
uint16_t id_for_rotated_part(uint16_t id, uint8_t rotated_position)
{
	return id + ETERN_PARTS * rotated_position;
}
