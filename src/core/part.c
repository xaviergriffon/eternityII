#include "core/part.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "core/core_static_variables.h"

#include "ui/logger.h"

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
	/* Alloue result->size + 4 éléments. Les 4 supplémentaires sont du padding
	   défensif : l'index max effectivement écrit/lu est
	   apart->size + ETERN_PARTS*(4-1) = result->size - 1 (voir la boucle
	   ci-dessous), donc ces 4 cases ne sont jamais touchées. */
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
	/* Borne d'abord, lecture ensuite : l'ancien ordre lisait elements[sizemap]
	 * (hors bornes) quand la sonde atteignait la fin de la table pleine. */
	while (l < map->sizemap && map->elements[l].key_int != 0)
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
		if (temp != NULL && temp->key_int == (unsigned int)key_int && strcmp(key, temp->key) == 0)
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
		/* maxFaceM dépend des données du CSV — pas de constante compile-time possible. */
		result = maxFaceM;
	}
	return result;
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

int map_packed_fits(unsigned long long total_parts, unsigned long long max_bucket)
{
	// Les offsets vont de 0 à total_parts - 1 : c'est bien total_parts (et non
	// total_parts - 1) qu'on borne, pour rester juste quand l'arène est vide.
	return (total_parts <= MAP_PACKED_FIELD_MAX && max_bucket <= MAP_PACKED_FIELD_MAX) ? 1 : 0;
}

/**
 * @brief Construit l'index compact `{offset:16 | size:16}` d'une map compactée.
 *
 * À appeler après le compactage de `flat` dans `arena` : chaque
 * `flat[i].parts` doit déjà pointer dans l'arène. Index purement redondant :
 * il ne change aucune donnée, il propose une seconde représentation plus
 * dense pour la boucle chaude.
 *
 * @return Index alloué, ou NULL si la capacité 16 bits est dépassée, si
 *         l'arène est vide, ou si l'allocation échoue — les lecteurs
 *         retombent alors sur `flat` (jamais de troncature).
 */
static uint32_t *build_packed_index(map_big_array *map, unsigned long long nbKeys,
                                    unsigned long long totalParts)
{
	// Invariant packed != NULL => arena != NULL : sans arène, pas d'index.
	if (map->arena == NULL)
	{
		return NULL;
	}

	unsigned long long maxBucket = 0;
	for (unsigned long long idx = 0; idx < nbKeys; idx++)
	{
		if ((unsigned long long)map->flat[idx].size > maxBucket)
		{
			maxBucket = (unsigned long long)map->flat[idx].size;
		}
	}

	if (!map_packed_fits(totalParts, maxBucket))
	{
		// Puzzle hors gabarit : on renonce à l'index plutôt que de tronquer.
		log_info("index compact non construit : %llu pièces / plus gros compartiment %llu"
		         " dépassent la capacité 16 bits (%u)\n",
		         totalParts, maxBucket, MAP_PACKED_FIELD_MAX);
		return NULL;
	}

	uint32_t *packed = malloc(sizeof(uint32_t) * nbKeys);
	if (packed == NULL)
	{
		return NULL;
	}
	for (unsigned long long idx = 0; idx < nbKeys; idx++)
	{
		const struct array_part *entry = &map->flat[idx];
		// Compartiment vide : offset 0 (jamais déréférencé puisque size == 0).
		uint32_t offset = entry->size > 0 ? (uint32_t)(entry->parts - map->arena) : 0u;
		packed[idx] = (offset << 16) | (uint32_t)entry->size;
	}
	return packed;
}

/**
 * @brief Construit `bucket_id_mask` : le masque des ids de chaque compartiment.
 *
 * À appeler après `build_packed_index` (l'offset d'un compartiment dans
 * `arena` est lu dans `packed`). Comme `packed`, l'index est purement
 * redondant : rend le comptage des pièces libres d'un compartiment
 * indépendant de sa taille (quelques `popcount` au lieu d'un parcours).
 *
 * Le nombre de mots est déduit du plus grand id réellement présent dans
 * l'arène, jamais de `ETERN_PARTS` : `part.c` reste agnostique de la taille
 * du puzzle.
 *
 * @param out_words Sortie : nombre de mots d'un masque (0 si non construit).
 * @return          Index alloué, ou NULL (les lecteurs comptent alors par
 *                  parcours — jamais de résultat tronqué).
 */
static uint64_t *build_bucket_id_mask(map_big_array *map, unsigned long long nbKeys,
                                      unsigned long long totalParts, int *out_words)
{
	*out_words = 0;
	// Sans index compact, pas d'offset de compartiment : rien à indexer.
	if (map->packed == NULL || map->arena == NULL || totalParts == 0)
	{
		return NULL;
	}

	int16_t maxId = 0;
	for (unsigned long long i = 0; i < totalParts; i++)
	{
		if (map->arena[i].id > maxId)
		{
			maxId = map->arena[i].id;
		}
	}
	if (maxId <= 0)
	{
		// Arène ne contenant que des entrées bouchon (id 0) : rien à masquer.
		return NULL;
	}

	int words = ((int)maxId + 63) / 64;
	uint64_t *masks = calloc((size_t)totalParts * (size_t)words, sizeof(uint64_t));
	if (masks == NULL)
	{
		return NULL;
	}

	for (unsigned long long idx = 0; idx < nbKeys; idx++)
	{
		uint32_t entry = map->packed[idx];
		unsigned int size = entry & MAP_PACKED_FIELD_MAX;
		if (size == 0)
		{
			continue;
		}
		unsigned int offset = entry >> 16;
		uint64_t *mask = &masks[(size_t)offset * (size_t)words];
		for (unsigned int s = 0; s < size; s++)
		{
			int16_t id = map->arena[offset + s].id;
			if (id > 0)
			{
				int bit = id - 1;
				mask[bit / 64] |= (uint64_t)1 << (bit % 64);
			}
		}
	}

	*out_words = words;
	return masks;
}

/**
 * @brief Fréquence globale de chaque couleur de bord (0..maxFace) dans `apart`.
 *
 * Compte les 4 faces de chaque entrée de `apart`, rotations comprises : si
 * `apart` vient de `rotate_all_parts` (4 rotations par pièce physique),
 * chaque couleur est comptée ×4 par rapport à un dénombrement sur les
 * seules pièces physiques — un facteur d'échelle uniforme qui ne change pas
 * leur ordre relatif de rareté. Alimente le score de rareté utilisé pour
 * trier chaque compartiment de `buildBigArray`.
 *
 * @return Tableau alloué de `maxFace + 1` entrées (à libérer par l'appelant),
 *         ou NULL si `maxFace < 0` ou si l'allocation échoue.
 */
long *compute_face_frequency(struct array_part *apart, int maxFace)
{
	if (maxFace < 0) {
		return NULL;
	}
	long *freq = calloc((size_t)maxFace + 1, sizeof(long));
	if (freq == NULL) {
		return NULL;
	}
	for (int i = 0; i < apart->size; i++) {
		struct part *p = &apart->parts[i];
		if (p->top >= 0 && p->top <= maxFace)      freq[p->top]++;
		if (p->right >= 0 && p->right <= maxFace)  freq[p->right]++;
		if (p->bottom >= 0 && p->bottom <= maxFace) freq[p->bottom]++;
		if (p->left >= 0 && p->left <= maxFace)    freq[p->left]++;
	}
	return freq;
}

/**
 * @brief Score de rareté des couleurs exposées d'une pièce dans un compartiment.
 *
 * Un compartiment (f1,f2,f3,f4) fixe la couleur attendue sur les côtés
 * contraints (f_i >= 0) : cette valeur est la même pour toutes les pièces du
 * compartiment, elle ne discrimine donc aucun ordre. Seuls les côtés
 * « wildcard » (f_i == -1) varient d'une pièce à l'autre : ce sont les
 * couleurs que cette pièce exposera vers une case encore vide si posée ici.
 *
 * @param freq    Fréquence globale de chaque couleur, cf. `compute_face_frequency`.
 * @param maxFace Borne supérieure de `freq` (même valeur que pour sa construction).
 */
long arena_exposed_score(const struct part *p, int f1, int f2, int f3, int f4,
                          const long *freq, int maxFace)
{
	long score = 0;
	if (f1 == -1 && p->top >= 0 && p->top <= maxFace)      score += freq[p->top];
	if (f2 == -1 && p->right >= 0 && p->right <= maxFace)  score += freq[p->right];
	if (f3 == -1 && p->bottom >= 0 && p->bottom <= maxFace) score += freq[p->bottom];
	if (f4 == -1 && p->left >= 0 && p->left <= maxFace)    score += freq[p->left];
	return score;
}

/**
 * @brief Trie en place les candidats d'un compartiment par rareté croissante
 *        de couleur exposée — la pièce exposant la couleur la plus rare
 *        d'abord (mesuré : +3,2 % de débit médian, taux d'élagage et
 *        profondeur atteinte inchangés). Comportement de production
 *        inconditionnel : pas d'interrupteur laissé en place.
 *
 * N'altère jamais le multi-ensemble de pièces du compartiment (même ids,
 * mêmes rotations, même taille) — uniquement l'ordre dans lequel elles
 * seront essayées : `arena_sort_preserves_multiset_and_orders_by_rarity`
 * (tests/core/test_part.c) verrouille cet invariant.
 *
 * Tri par insertion (les compartiments non vides sont petits en pratique,
 * quelques centaines de pièces au plus) avec les scores précalculés une
 * fois, pas recalculés à chaque comparaison.
 */
void sort_compartment_by_exposed_rarity(struct array_part *arraypart,
                                         int f1, int f2, int f3, int f4,
                                         const long *freq, int maxFace)
{
	if (arraypart->size < 2 || freq == NULL) {
		return;
	}
	int n = arraypart->size;
	long *scores = malloc(sizeof(long) * (size_t)n);
	if (scores == NULL) {
		return; // tri = optimisation, jamais bloquant si l'allocation échoue
	}
	for (int i = 0; i < n; i++) {
		scores[i] = arena_exposed_score(&arraypart->parts[i], f1, f2, f3, f4, freq, maxFace);
	}
	for (int i = 1; i < n; i++) {
		struct part moving = arraypart->parts[i];
		long moving_score = scores[i];
		int j = i - 1;
		while (j >= 0 && scores[j] > moving_score) {
			arraypart->parts[j + 1] = arraypart->parts[j];
			scores[j + 1] = scores[j];
			j--;
		}
		arraypart->parts[j + 1] = moving;
		scores[j + 1] = moving_score;
	}
	free(scores);
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
 * Chaque compartiment est trié par rareté de couleur exposée croissante (la
 * pièce la plus rare essayée en premier) avant compactage dans `arena` —
 * voir `sort_compartment_by_exposed_rarity`. Coût négligeable, payé une
 * seule fois à la construction de la map, jamais dans la boucle chaude.
 *
 * @return Tableau 4D alloué (à libérer avec `free_bigarray`).
 */
map_big_array *buildBigArray(struct array_part *apart, int maxFace)
{
	long *arena_sort_freq = compute_face_frequency(apart, maxFace);

	int sizeBigArray = (maxFace + 2);
	map_big_array *result = malloc(sizeof(map_big_array));
	result->sizearray = sizeBigArray;
	result->sizearrayM = sizeBigArray - 1;
	unsigned long long nbKeys = (unsigned long long)sizeBigArray * sizeBigArray * sizeBigArray * sizeBigArray;
	result->flat = malloc(sizeof(struct array_part) * nbKeys);

	int maxarray = 0;
	unsigned long long totalParts = 0;
	int f1;
	for (f1 = -1; f1 <= maxFace; f1++)
	{
		struct array_part *arraypart1 = search_face(apart, f1, PART_TOP);
		int p1 = f1;
		if (abs(p1) != p1)
		{
			p1 = maxFace + abs(p1);
		}

		int f2;
		for (f2 = -1; f2 <= maxFace; f2++)
		{
			int p2 = f2;
			if (abs(p2) != p2)
			{
				p2 = maxFace + abs(p2);
			}

			struct array_part *arraypart2 = search_face(arraypart1, f2, PART_RIGHT);
			int f3;
			for (f3 = -1; f3 <= maxFace; f3++)
			{
				int p3 = f3;
				if (abs(p3) != p3)
				{
					p3 = maxFace + abs(p3);
				}

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
					sort_compartment_by_exposed_rarity(arraypart, f1, f2, f3, f4,
					                                    arena_sort_freq, maxFace);
					if (f1 >= 0 || f2 >= 0 || f3 >= 0 || f4 >= 0)
					{
						if (arraypart->size > maxarray)
						{
							maxarray = arraypart->size;
						}
					}
					// On vole le tableau de pièces de search_face (compacté en arène plus bas)
					unsigned long long idx = (((unsigned long long)p1 * sizeBigArray + p2) * sizeBigArray + p3) * sizeBigArray + p4;
					result->flat[idx].size = arraypart->size;
					result->flat[idx].parts = arraypart->parts;
					totalParts += arraypart->size;
					free(arraypart);
				}
				free_array_part(arraypart3);
			}
			free_array_part(arraypart2);
		}
		free_array_part(arraypart1);
	}

	// Compactage : toutes les listes bout à bout dans un unique bloc contigu
	result->arena = totalParts > 0 ? malloc(sizeof(struct part) * totalParts) : NULL;
	unsigned long long position = 0;
	for (unsigned long long idx = 0; idx < nbKeys; idx++)
	{
		struct array_part *entry = &result->flat[idx];
		if (entry->size > 0)
		{
			memcpy(&result->arena[position], entry->parts, sizeof(struct part) * entry->size);
			free(entry->parts);
			entry->parts = &result->arena[position];
			position += entry->size;
		}
		else
		{
			if (entry->parts != NULL)
			{
				free(entry->parts);
			}
			entry->parts = NULL;
		}
	}
	result->packed = build_packed_index(result, nbKeys, totalParts);
	result->bucket_id_mask = build_bucket_id_mask(result, nbKeys, totalParts, &result->id_mask_words);
	free(arena_sort_freq);
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
	if (array_parts->arena != NULL)
	{
		free(array_parts->arena);
	}
	if (array_parts->packed != NULL)
	{
		free(array_parts->packed);
	}
	if (array_parts->bucket_id_mask != NULL)
	{
		free(array_parts->bucket_id_mask);
	}
	free(array_parts->flat);
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
	// La table à plat est déjà ordonnée par (f1, f2, f3, f4) : simple parcours linéaire
	int narray;
	for (narray = 0; narray < result->nbarrays; narray++)
	{
		struct array_part *apart = &map->flat[narray];
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
