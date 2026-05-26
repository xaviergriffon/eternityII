#include "possibility.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "static_variables.h"
#include "datamanager.h"
#include "readdata.h"

#ifdef FACES_USED_BITS
/**
 * @brief Marque ou démarque une pièce comme utilisée dans le masque de bits.
 *
 * Stocke l'information sous forme de bitmask compact (1 bit par pièce, groupes de 16).
 *
 * @param faceused Masque de bits des pièces utilisées (tableau de FACES_USED_SIZE uint16_t).
 * @param part     Identifiant de la pièce (base 0, i.e. id-1).
 * @param boolean  1 = utilisée, 0 = libre.
 */
void set_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part, uint8_t boolean) {
    //uint16_t groupe = part / 16;
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    //int8_t n = part % 16;
    int8_t n = part - (groupe << 4);
    number = (number & ~(1 << n)) | (boolean << n);
    faceused[groupe] = number;
}

/**
 * @brief Indique si une pièce est marquée comme utilisée dans le masque de bits.
 *
 * @param faceused Masque de bits des pièces utilisées.
 * @param part     Identifiant de la pièce (base 0).
 * @return         1 si utilisée, 0 si libre.
 */
uint8_t is_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part) {
    //uint16_t groupe = part / 16;
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    //int8_t n = part % 16;
    int8_t n = part - (groupe << 4);
    return (number >> n) & 1;
}
#endif // FACES_USED_BITS

/**
 * @brief Affiche dans les logs les coordonnées x et y de chaque position du parcours.
 *
 * Outil de débogage permettant de visualiser l'ordre dans lequel les cases
 * de la grille sont parcourues (tableau `directions`).
 *
 * @return 0.
 */
int decode_direction(void)
{
	log_info("/nx : ");
	int i;
	for(i=0;i < ETERN_PARTS;i++) {
		int x = directions[i] % ETERN_SIZE;
        log_info("%i,",x);
	}
	
    log_info("/ny : ");
	for(i=0;i < ETERN_PARTS;i++) {
		int x = directions[i] % ETERN_SIZE;
		int y = (directions[i] - x) / ETERN_SIZE;
        log_info("%i,",y);
	}
	

	return 0;
}

int test_directions(void)
{
	int grille[ETERN_PARTS];
	int i;
	for(i=0; i < ETERN_PARTS; i++)
	{
		grille[i] = 0;
	}
	
	for(i=0; i < ETERN_PARTS;i++)
	{
		int x = directions[i];
		grille[x] = 1;
	}
	
	for(i=0; i < ETERN_PARTS;i++)
	{
		if(grille[i] == 0)
		{
            log_info("grille : %i not use\n", i);
			return -1;
		}
	}
	
	return 0;
}

/**
 * @brief Crée un `possibility_packet` représentant l'état actuel de la grille.
 *
 * Encode chaque pièce placée dans `etern` sous la forme d'un indice de rotation
 * (`id_for_rotated_part`) et construit le masque des pièces utilisées.
 * Les cases vides sont encodées à -2.
 *
 * @param x         Coordonnée x de la prochaine case à remplir.
 * @param y         Coordonnée y de la prochaine case à remplir.
 * @param etern     Grille 2D de pointeurs vers les pièces placées (NULL = vide).
 * @param directory Direction de parcours courante (constante DIR_*).
 * @return          Paquet alloué représentant cet état (à libérer par l'appelant).
 */
struct possibility_packet *generate_possibility_packet(int x, int y, struct part *etern[ETERN_SIZE][ETERN_SIZE], int directory)
{
	struct possibility_packet *result = malloc(sizeof(*result));
	result->x = x;
	result->y = y;
	result->alloc = 0;
#ifdef FACES_USED_BITS
	memset(result->b_faceused, 0, sizeof(result->b_faceused));
#else
    memset(result->faceused, 0, sizeof(result->faceused));
#endif // FACES_USED_BITS
	int l;
	for (l = 0; l < ETERN_SIZE; l++)
	{
		int h;
		for(h = 0; h < ETERN_SIZE; h++)
		{
			struct part *part = etern[l][h];
			if(part != NULL)
			{
				result->grid[l][h] = id_for_rotated_part(part->id, part->rotation);
#ifdef FACES_USED_BITS
                set_face_used(result->b_faceused, part->id-1, 1);
#else
                result->faceused[part->id-1] = 1;
#endif
			} else
			{
				result->grid[l][h] = -2;
			}
		}
	}
	return result;
}

/**
 * @brief Chiffre un paquet pour l'envoi réseau. (Non implémenté)
 * @param packet Paquet source.
 * @return       NULL (non implémenté).
 */
struct possibility_packet *crypt_to_network(struct possibility_packet *packet)
{
    
	return NULL;
}

/**
 * @brief Déchiffre un paquet reçu du réseau. (Non implémenté)
 * @param packet Paquet source.
 * @return       NULL (non implémenté).
 */
struct possibility_packet *decrypt_from_network(struct possibility_packet *packet)
{
	return NULL;
}

/**
 * @brief Calcule la clé de recherche pour une case (x, y) quelconque de la grille.
 *
 * Variante de `what_search_to_key2` qui prend des coordonnées explicites au lieu
 * d'utiliser la position courante du paquet. Utilisée par `possibility_all_has_a_next`
 * pour tester chaque case libre de la grille.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant (état de la grille).
 * @param x                Coordonnée x de la case à tester.
 * @param y                Coordonnée y de la case à tester.
 * @param key              Clé résultante (sortie).
 * @param all_face         Valeur représentant « toute face » dans la map (= map->sizearrayM).
 */
void what_search_in_grid_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, int8_t x, int8_t y,key_part *key, int8_t all_face) {
	key->k1 =-2;
	key->k2 =-2;
	key->k3 =-2;
	key->k4 =-2;
		
	// TOP
	if(y -1 < 0)
	{
		key->k1 = 0;
	} else
	{
        // Todo : tester -2 ou -1 (optim)
		if(possiblity->grid[x][y-1] < 0)
		{
			key->k1 = all_face;
		} else
		{
			key->k1 = all_rotate_parts->parts[possiblity->grid[x][y-1]].bottom;
		}
	}
	
	// RIGHT
	if(x + 1 >= ETERN_SIZE)
	{
		key->k2 = 0;
	} else
	{
		if(possiblity->grid[x+1][y] < 0)
		{
			key->k2 = all_face;
		} else
		{
			key->k2 = all_rotate_parts->parts[possiblity->grid[x+1][y]].left;
		}
	}
	
	// BOTTOM
	if(y + 1 >= ETERN_SIZE)
	{
		key->k3 = 0;
	} else
	{
		if(possiblity->grid[x][y+1] < 0)
		{
			key->k3 = all_face;
		} else
		{
			key->k3 = all_rotate_parts->parts[possiblity->grid[x][y+1]].top;
		}
	}
	
	// LEFT
	if(x -1 < 0)
	{
		key->k4 = 0;
	} else
	{
		if(possiblity->grid[x-1][y] < 0)
		{
			key->k4 = all_face;
		} else
		{
			key->k4 = all_rotate_parts->parts[possiblity->grid[x-1][y]].right;
		}
	}
}


/**
 * @brief Calcule la clé de recherche pour la case courante du paquet.
 *
 * Détermine les contraintes de bord imposées par les pièces voisines déjà placées.
 * - k1 = bord TOP requis (bottom du voisin du dessus, 0 si bord de grille, all_face si voisin absent)
 * - k2 = bord RIGHT requis
 * - k3 = bord BOTTOM requis
 * - k4 = bord LEFT requis
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant indiquant la position (x, y) à remplir.
 * @param key              Clé résultante (sortie).
 * @param all_face         Valeur représentant « toute face » (= map->sizearrayM).
 */
void what_search_to_key2(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key, int8_t all_face) {
    // TODO : ne pas utilisé -1 mais MAX_FACE-1 pour éviter de le faire dans convert_p
    // -2 : non défini
    // -1 toute face
    // 0 bordure
    
    // Toujours valorisé dans les if
    /*
    key->k1 =-2;
    key->k2 =-2;
    key->k3 =-2;
    key->k4 =-2;
     */
    
    int x = possiblity->x;
    int xm = x - 1;
    int xp = x + 1;
    int y = possiblity->y;
    int ym = y - 1;
    int yp = y + 1;

    // tODO : diminuer les calculs -1 +1 en conservant le résultat
    
    // TOP
    if(ym < 0)
    {
        key->k1 = 0;
    } else
    {
        int16_t partId = possiblity->grid[x][ym];
        if(partId < 0)
        {
            key->k1 = all_face;
        } else
        {
            key->k1 = all_rotate_parts->parts[partId].bottom;
        }
    }
    
    // RIGHT
    if(xp >= ETERN_SIZE)
    {
        key->k2 = 0;
    } else
    {
        int16_t partId = possiblity->grid[xp][y];
        if(partId < 0)
        {
            key->k2 = all_face;
        } else
        {
            key->k2 = all_rotate_parts->parts[partId].left;
        }
    }
    
    // BOTTOM
    if(yp >= ETERN_SIZE)
    {
        key->k3 = 0;
    } else
    {
        int16_t partId = possiblity->grid[x][yp];
        if(partId < 0)
        {
            key->k3 = all_face;
        } else
        {
            key->k3 = all_rotate_parts->parts[partId].top;
        }
    }
    
    // LEFT
    if(xm < 0)
    {
        key->k4 = 0;
    } else
    {
        int16_t partId = possiblity->grid[xm][y];
        if(partId < 0)
        {
            key->k4 = all_face;
        } else
        {
            key->k4 = all_rotate_parts->parts[partId].right;
        }
    }
}
/**
 * @brief Calcule la clé de recherche pour la case courante (version sans all_face).
 *
 * Identique à `what_search_to_key2` mais utilise -1 (au lieu de all_face) pour
 * représenter les voisins absents. Destinée à la `map_part` avec hachage textuel.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant.
 * @param key              Clé résultante (sortie).
 */
void what_search_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key) {
    // TODO : ne pas utilisé -1 mais MAX_FACE-1 pour éviter de le faire dans convert_p
    // -2 : non défini
    // -1 toute face
    // 0 bordure
    
    // Toujours valorisé dans les if
    /*
	key->k1 =-2;
	key->k2 =-2;
	key->k3 =-2;
	key->k4 =-2;
     */
	
	int x = possiblity->x;
    int xm = x - 1;
    int xp = x + 1;
	int y = possiblity->y;
    int ym = y - 1;
    int yp = y + 1;

    // tODO : diminuer les calculs -1 +1 en conservant le résultat
    
	// TOP
	if(ym < 0)
	{
		key->k1 = 0;
	} else
	{
        // Todo : tester -2 ou -1 (optim)
		if(possiblity->grid[x][ym] < 0)
		{
			key->k1 = -1;
		} else
		{
			key->k1 = all_rotate_parts->parts[possiblity->grid[x][ym]].bottom;
		}
	}
	
	// RIGHT
	if(xp >= ETERN_SIZE)
	{
		key->k2 = 0;
	} else
	{
		if(possiblity->grid[xp][y] < 0)
		{
			key->k2 = -1;
		} else
		{
			key->k2 = all_rotate_parts->parts[possiblity->grid[xp][y]].left;
		}
	}
	
	// BOTTOM
	if(yp >= ETERN_SIZE)
	{
		key->k3 = 0;
	} else
	{
		if(possiblity->grid[x][yp] < 0)
		{
			key->k3 = -1;
		} else
		{
			key->k3 = all_rotate_parts->parts[possiblity->grid[x][yp]].top;
		}
	}
	
	// LEFT
	if(xm < 0)
	{
		key->k4 = 0;
	} else
	{
		if(possiblity->grid[xm][y] < 0)
		{
			key->k4 = -1;
		} else
		{
			key->k4 = all_rotate_parts->parts[possiblity->grid[xm][y]].right;
		}
	}
}

/**
 * @brief Calcule et retourne la clé de recherche pour une case donnée.
 *
 * Version retournant une `key_part` par valeur au lieu d'écrire dans un pointeur.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param x                Coordonnée x de la case.
 * @param y                Coordonnée y de la case.
 * @param possiblity       État courant de la grille.
 * @return                 Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 */
key_part what_search(struct array_part *all_rotate_parts, int x, int y, struct possibility_packet *possiblity)
{
	//char *result = malloc(MAX_KEY_LENGTH * sizeof(char));
	key_part result;
    // Toujours valorisé dans les if
    /*
	result.k1 =-2;
	result.k2 =-2;
	result.k3 =-2;
	result.k4 =-2;
     */
    
    int xm = x - 1;
    int xp = x + 1;
    int ym = y - 1;
    int yp = y + 1;
    
	// TOP
	if(ym < 0)
	{
		result.k1 = 0;
	} else
	{
		if(possiblity->grid[x][ym] < 0)
		{
			result.k1 = -1;
		} else
		{
			result.k1 = all_rotate_parts->parts[possiblity->grid[x][ym]].bottom;
		}
	}
	
	// RIGHT
	if(xp >= ETERN_SIZE)
	{
		result.k2 = 0;
	} else
	{
		if(possiblity->grid[xp][y] < 0)
		{
			result.k2 = -1;
		} else
		{
			result.k2 = all_rotate_parts->parts[possiblity->grid[xp][y]].left;
		}
	}
	
	// BOTTOM
	if(yp >= ETERN_SIZE)
	{
		result.k3 = 0;
	} else
	{
		if(possiblity->grid[x][yp] < 0)
		{
			result.k3 = -1;
		} else
		{
			result.k3 = all_rotate_parts->parts[possiblity->grid[x][yp]].top;
		}
	}
	
	// LEFT
	if(xm < 0)
	{
		result.k4 = 0;
	} else
	{
		if(possiblity->grid[xm][y] < 0)
		{
			result.k4 = -1;
		} else
		{
			result.k4 = all_rotate_parts->parts[possiblity->grid[xm][y]].right;
		}
	}
	
//	if(result.k1 == -1 && result.k2 == -1 && result.k3 == -1 && result.k4 == -1) {
//		printf("nothing to search x:%i y:%i \n",x,y);
//	}
	
	return result;
}

/**
 * @brief Sérialise un `possibility_packet` dans un fichier binaire.
 *
 * @param filename    Chemin du fichier de destination.
 * @param possibility Paquet à sauvegarder.
 * @return            0 en cas de succès.
 */
int save_possibility(char *filename, struct possibility_packet *possibility)
{
	FILE *f = fopen(filename, "w");
	if(!f)
	{
		log_error("save_possibility file :%s",filename);
		perror("fopen()");
		exit(EXIT_FAILURE);
	}
	
	fwrite(possibility, sizeof(struct possibility_packet), 1, f);
	
	fclose(f);
	return 0;
}

/**
 * @brief Vérifie si toutes les 256 pièces sont placées et traite la solution trouvée.
 *
 * Si `alloc >= ETERN_PARTS`, affiche la grille, sauvegarde le fichier solution
 * (`./solution_<pid>`) et termine le processus avec EXIT_SUCCESS.
 *
 * @param poss            Paquet à vérifier.
 * @param all_rotate_part Tableau de toutes les rotations (pour affichage).
 */
void checkIfResultFound(struct possibility_packet *poss, struct array_part *all_rotate_part) {
    if(poss->alloc >= ETERN_PARTS)
    {
        log_info("fin de la boucle à %i \n", poss->alloc);
        log_event("SOLUTION FOUND! (%i pieces) - saved to ./solution_%i", poss->alloc, getpid());
        for(int x = 0; x < ETERN_SIZE; x++)
        {
            for(int y=0;y < ETERN_SIZE; y++)
            {
                struct part *part = &all_rotate_part->parts[poss->grid[x][y]];
                log_info("%i;%i; ",x,y);
                print_part(part);
            }
        }
        char *fileName = calloc(100, sizeof(char));
        sprintf(fileName, "./solution_%i", getpid());
        save_possibility(fileName, poss);
        free(fileName);
        exit(EXIT_SUCCESS);
    }
}

/**
 * @brief Indique si la case courante du paquet a au moins une pièce posable.
 *
 * Vérifie uniquement la case indiquée par `possibility->x` et `possibility->y`.
 *
 * @param possibility    Paquet à tester.
 * @param mapParts       Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return               1 si au moins une pièce disponible peut être placée, 0 sinon.
 */
int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    int result = 0;
    
    // initialisation
	uint8_t x = possibility->x;
	uint8_t y = possibility->y;
    key_part wsearch = what_search(all_rotate_part, x, y, possibility);
	
	int8_t p[4] = {wsearch.k1,wsearch.k2,wsearch.k3,wsearch.k4};
    struct array_part *search = get_parts_bigarray(mapParts, p);
	int s;
	if(search->size > 0)
	{
		for(s=0; s< search->size && result == 0; s++)
		{
#ifdef FACES_USED_BITS
			if(search->parts[s].id != 0 && is_face_used(possibility->b_faceused, search->parts[s].id -1) == 0)
#else
            if(search->parts[s].id != 0 && possibility->faceused[search->parts[s].id -1] == 0)
#endif // FACES_USED_BITS
			{
                result = 1;
            }
        }
    }
    return result;
}

/**
 * @brief Vérifie que toutes les cases encore libres de la grille ont au moins une pièce posable.
 *
 * Parcourt toutes les cases non encore remplies à partir de `possibility->alloc`.
 * Si une case n'admet aucune pièce, retourne 0 (le paquet est sans issue).
 * Optimisation : si une case n'admet qu'une seule pièce, la place immédiatement.
 *
 * @param possibility    Paquet à analyser (peut être modifié si des pièces uniques sont placées).
 * @param mapParts       Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return               1 si toutes les cases libres ont au moins une suite, 0 sinon.
 */
int possibility_all_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    int result = 1;
    
	key_part wsearch;
	int c;
    int alloc = possibility->alloc;
    // On parcours
	for(c=possibility->alloc;c < ETERN_PARTS && result == 1;c++) {
		result = 0;
		int8_t x = dirx[c];
		int8_t y = diry[c];
		if(possibility->grid[x][y] == -2) {
			what_search_in_grid_to_key(all_rotate_part, possibility, x, y,&wsearch, mapParts->sizearrayM);
			if(wsearch.k1 < mapParts->sizearrayM || wsearch.k2 < mapParts->sizearrayM || wsearch.k3 < mapParts->sizearrayM || wsearch.k4 < mapParts->sizearrayM) {
				
				struct array_part *search = get_parts_bigarray_with_key(mapParts, &wsearch);
				int s;
				if(search->size > 0)
				{
					for(s=0; s< search->size && result == 0; s++)
					{
#ifdef FACES_USED_BITS
						if(search->parts[s].id != 0 && is_face_used(possibility->b_faceused, search->parts[s].id -1) == 0)
#else
                        if(search->parts[s].id != 0 && possibility->faceused[search->parts[s].id -1] == 0)
#endif // FACES_USED_BITS
						{
							if( search->size == 1) {
#ifdef FACES_USED_BITS
                                set_face_used(possibility->b_faceused, search->parts[s].id - 1, 1);
#else
                                possibility->faceused[search->parts[s].id -1] = 1;
#endif // FACES_USED_BITS
								possibility->grid[x][y] = id_for_rotated_part(search->parts[s].id, search->parts[s].rotation);
                                alloc++;
							}
							result = 1;
						}
					}
					
                } else {
                    // On a rien trouvé, il n'y a donc pas de suite
                    break;
                }
			}else {
				result = 1;
				break;
			}
		} else {
			result = 1;
		}
	}
#ifdef DEBUG_RM_NO_NEXT
    if (alloc > possibility->alloc) {
        log_debug("all has next (%i) allocated %i -> %i\n", result, possibility->alloc, alloc);
    }
#endif // DEBUG_RM_NO_NEXT
    if (alloc == ETERN_PARTS) {
        possibility->alloc = alloc;
        checkIfResultFound(possibility, all_rotate_part);
    }
	
	return result;
}

/**
 * @brief Ajoute un `possibility_packet` en fin de file.
 *
 * Identique à `put` mais typé pour `possibility_packet`. En mode DEBUG_CHECK_POSSIBILITY,
 * valide le paquet avant insertion.
 *
 * @param suite File cible.
 * @param value Paquet à ajouter (copié dans la file).
 */
void put_possibility (File * suite, struct possibility_packet *value){
#ifdef DEBUG_CHECK_POSSIBILITY
    int analyse = check_possibility(value, NULL);
    if (analyse < 0)
    {
        log_error("possibility error : %i\n",analyse);
        log_error(" ---");
        print_possibility_packet(value);
    }
#endif // DEBUG_CHECK_POSSIBILITY
	Element *new_element = NULL;
    // On vérifie on peut encore positionner dans le cache
	if(suite->lastPostionCache < suite->cacheSize)
	{
		
		new_element = &suite->cacheElement[suite->lastPostionCache];
		suite->lastPostionCache++;
	} else
	{
        // création d'un nouvel élément
		new_element = malloc(sizeof(Element));
        // ?????
		if (suite->sizeofvalue <= 0 || (new_element->value = malloc(suite->sizeofvalue))
			== NULL)
		{
			free (new_element);
			return;
		}
	}
	
	new_element->previous = NULL;
	new_element->next = NULL;
	
	// par précaution du cache on vérifie que qu'il ne s'agit pas de la meme valeur
    memcpy (new_element->value, value, sizeof(struct possibility_packet));
	
    // On place l'élément dans la suite
	if(suite->start == NULL){
		suite->start = new_element;
	}else {
		suite->end->next = new_element;
		new_element->previous = suite->end;
	}
    
    suite->end = new_element;
	suite->size++;
	return;
}

/**
 * @brief Développe un paquet en ajoutant une pièce à la case courante.
 *
 * Pour chaque pièce compatible avec la clé (et non encore utilisée), crée une
 * copie du paquet avec la pièce placée et l'ajoute dans `result`. Utilise une
 * `File` comme structure de résultat.
 *
 * @param result       File de destination des nouveaux paquets.
 * @param key          Clé de recherche pour la case courante.
 * @param possiblity   Paquet source à développer.
 * @param mapParts     Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param idParts      Table de pré-calcul des indices de rotation [id][rotation].
 * @return             Nombre de pièces allouées dans le meilleur paquet produit,
 *                     ou 0 si aucune pièce posable.
 */
int search_possiblity_light(File *result, key_part *key, struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part, int16_t idParts[ETERN_PARTS][4])
{
	int max_result=0;
    uint8_t x;
	uint8_t y;
	
	// initialisation
	x = possiblity->x;
	y = possiblity->y;

	uint16_t incAlloc = possiblity->alloc + 1;
	uint8_t nX = dirx[incAlloc];
	uint8_t nY = diry[incAlloc];
	
	int s;
	int lastId =-1;
	
	struct possibility_packet *currPossibility = possiblity;
	
    // On vérifie si la possibilité à cette position n'est toujours pas connu.
	if(currPossibility->grid[x][y] == -2) {
    
        // TODO : vérifier si suffisament d'espace mémoire pour intégrer un le nombre de possibilité retournée
        
        
		// TODO : voir pour réviser la recherche avec seulement des id de all_rotate_part
        // liste des pieces répondant à la recherche (key)
        struct array_part *search = get_parts_bigarray_with_key(mapParts, key);
        for(s=0; s< search->size; s++)
        {
            int position = search->parts[s].id -1;
            // Si la piece n'est pas déjà utilisée dans la suite de possibilité, on a donc une possiblité supplémentaire
#ifdef FACES_USED_BITS
            if(!is_face_used(currPossibility->b_faceused, position))
#else
            if(!currPossibility->faceused[position])
#endif // FACES_USED_BITS
            {
                
                // On ajoute la définition d'une possibilité dans la suite.
                // effectue une copie dans le end->value
                // TODO : utiliser un système moins couteux en copie de mémoire
                put_possibility(result, currPossibility);
                // On se place à la fin de la suite qui correspond à la nouvelle définition
                currPossibility = result->end->value;
                // Dans le cas où on a déjà généré une possiblité, on libère la piece qui avait été utilisée avant de généré un nouveau jeu
                if(lastId>0) {
#ifdef FACES_USED_BITS
                    set_face_used(currPossibility->b_faceused, lastId - 1, 0);
#else
                    currPossibility->faceused[lastId -1] = 0;
#endif // FACES_USED_BITS
                }
                // On place la piece
                currPossibility->grid[x][y] = idParts[search->parts[s].id][search->parts[s].rotation];
                // statistique du nombre de piece placée
                currPossibility->alloc = incAlloc;
                
                currPossibility->x = nX;
                currPossibility->y = nY;
                // On indique que la piece est utilisée
#ifdef FACES_USED_BITS
                set_face_used(currPossibility->b_faceused, position, 1);
#else
                currPossibility->faceused[position] = 1;
#endif // FACES_USED_BITS
                // identifiant de la dernière piece utilisée
                
                lastId = search->parts[s].id;
                // On vérifie que les emplacements libres ont tous une piece possible
                // Si qu'une possiblité, alors place la piece
                /*
                 * TODO : faire plus tard (après put ou après la boucle) car est recopié sur les autres qui n'ont pas la meme piece a position.
                if(possibility_all_has_a_next(currPossibility, mapParts, all_rotate_part) == 0 && incAlloc < ETERN_PARTS) {
                    // Consomme la suite ou fournie la possiblité actuel si pas d'élément dans la suite
                    scroll_cache(result);
                }
                 */
#ifdef DEBUG_CHECK_POSSIBILITY
                int analyse = check_possibility(currPossibility, all_rotate_part);
                if (analyse < 0)
                {
                    log_error("possibility error : %i\n",analyse);
                    log_error(" ---");
                    print_possibility_packet(currPossibility);
                }
#endif // DEBUG_CHECK_POSSIBILITY
                // si toutes les pieces sont placées alors on n'entrera pas dasn le if !faceused et sortira donc
            }
        }
	} else {
        // ?? à quoi correspond % 256
		//lastId = currPossibility->grid[x][y] % 256;
        lastId = 1;// pour indiquer qu'on a trouvé qqc
        
        // On remet la possibilté dans la suite car elle ne doit pas être résolu sinon on aurait arreter
		put_possibility(result, currPossibility);
		currPossibility->alloc = incAlloc;
		
		currPossibility->x = nX;
		currPossibility->y = nY;
        // On vérifie que les emplacements libres ont tous une piece possible
        // Si qu'une possiblité, alors place la piece
        /*
		if(possibility_all_has_a_next(currPossibility, mapParts, all_rotate_part) == 0 && incAlloc < ETERN_PARTS) {
            // On consomme pour éviter de recalculer
            scroll_cache(result);
		}
         */
#ifdef DEBUG_CHECK_POSSIBILITY
        int analyse = check_possibility(currPossibility, all_rotate_part);
        if (analyse < 0)
        {
            log_error("possibility error : %i\n",analyse);
            log_error(" ---");
            print_possibility_packet(currPossibility);
        }
#endif // DEBUG_CHECK_POSSIBILITY
	}

    // On a au moins placé une piece
	if (lastId>-1) {
		max_result = incAlloc;
        checkIfResultFound(currPossibility, all_rotate_part);
	}
	return max_result;
}

/**
 * @brief Développe un paquet en ajoutant une pièce à la case courante (version big_table).
 *
 * Variante de `search_possiblity_light` utilisant un `big_table` comme structure
 * de résultat. Plus performante car évite les allocations individuelles de la `File`.
 * C'est cette version qui est utilisée dans la boucle principale de `autosearch`.
 *
 * @param result       Tableau dynamique de destination des nouveaux paquets.
 * @param key          Clé de recherche pour la case courante.
 * @param possiblity   Paquet source à développer.
 * @param mapParts     Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param idParts      Table de pré-calcul des indices de rotation [id][rotation].
 * @return             Nombre de pièces allouées (`alloc`) dans le paquet produit, ou 0.
 */
int search_possiblity_light_with_big_table(big_table *result, key_part *key, struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part, int16_t idParts[ETERN_PARTS][4])
{
    uint8_t x;
    uint8_t y;
    
    // initialisation
    x = possiblity->x;
    y = possiblity->y;

    uint16_t incAlloc = possiblity->alloc + 1;
    uint8_t nX = dirx[incAlloc];
    uint8_t nY = diry[incAlloc];
    int lastId =-1;
    
    struct possibility_packet *currPossibility = possiblity;
    
    // On vérifie si la possibilité à cette position n'est toujours pas connu.
    if(currPossibility->grid[x][y] == -2) {
    
        // TODO : vérifier si suffisament d'espace mémoire pour intégrer un le nombre de possibilité retournée
        
        
        // TODO : voir pour réviser la recherche avec seulement des id de all_rotate_part
        // liste des pieces répondant à la recherche (key)
        struct array_part *search = get_parts_bigarray_with_key(mapParts, key);
        for(int s=0; s< search->size; s++)
        {
            // Position de la pieces dans faceused
            int position = search->parts[s].id -1;
            // Si la piece n'est pas déjà utilisée dans la suite de possibilité, on a donc une possiblité supplémentaire
#ifdef FACES_USED_BITS
            if(!is_face_used(currPossibility->b_faceused, position))
#else
            if(!currPossibility->faceused[position])
#endif // FACES_USED_BITS
            {
                
                // On ajoute la définition d'une possibilité dans la suite.
                // effectue une copie dans le end->value
                // TODO : utiliser un système moins couteux en copie de mémoire
                
                // On se place à la fin de la suite qui correspond à la nouvelle définition
                currPossibility = put_big_table(result, currPossibility);;
                // Dans le cas où on a déjà généré une possiblité, on libère la piece qui avait été utilisée avant de généré un nouveau jeu
                if(lastId>0) {
#ifdef FACES_USED_BITS
                    set_face_used(currPossibility->b_faceused, lastId - 1, 0);
#else
                    currPossibility->faceused[lastId -1] = 0;
#endif // FACES_USED_BITS
                }
                // On place la piece
                currPossibility->grid[x][y] = idParts[search->parts[s].id][search->parts[s].rotation];
                // statistique du nombre de piece placée
                currPossibility->alloc = incAlloc;
                
                currPossibility->x = nX;
                currPossibility->y = nY;
                // On indique que la piece est utilisée
#ifdef FACES_USED_BITS
                set_face_used(currPossibility->b_faceused, position, 1);
#else
                currPossibility->faceused[position] = 1;
#endif
                // identifiant de la dernière piece utilisée
                
                lastId = search->parts[s].id;
                // On vérifie que les emplacements libres ont tous une piece possible
                // Si qu'une possiblité, alors place la piece
                /*
                 * TODO : faire plus tard (après put ou après la boucle) car est recopié sur les autres qui n'ont pas la meme piece a position.
                if(possibility_all_has_a_next(currPossibility, mapParts, all_rotate_part) == 0 && incAlloc < ETERN_PARTS) {
                    // Consomme la suite ou fournie la possiblité actuel si pas d'élément dans la suite
                    scroll_cache(result);
                }
                 */
#ifdef DEBUG_CHECK_POSSIBILITY
                int analyse = check_possibility(currPossibility, all_rotate_part);
                if (analyse < 0)
                {
                    log_error("possibility error : %i\n",analyse);
                    log_error(" ---");
                    print_possibility_packet(currPossibility);
                }
#endif // DEBUG_CHECK_POSSIBILITY
                // si toutes les pieces sont placées alors on n'entrera pas dasn le if !faceused et sortira donc
            }
        }
    } else {
        // ?? à quoi correspond % 256 : l'identifiant de la piece (reste division par 256)
        //lastId = currPossibility->grid[x][y] % 256;
        lastId = 1;// pour indiquer qu'on a trouvé qqc
        
        // On remet la possibilté dans la suite car elle ne doit pas être résolu sinon on aurait arreter
        put_big_table(result, currPossibility);
        currPossibility->alloc = incAlloc;
        
        currPossibility->x = nX;
        currPossibility->y = nY;
        // On vérifie que les emplacements libres ont tous une piece possible
        // Si qu'une possiblité, alors place la piece
        /*
        if(possibility_all_has_a_next(currPossibility, mapParts, all_rotate_part) == 0 && incAlloc < ETERN_PARTS) {
            // On consomme pour éviter de recalculer
            scroll_cache(result);
        }
         */
#ifdef DEBUG_CHECK_POSSIBILITY
        int analyse = check_possibility(currPossibility, all_rotate_part);
        if (analyse < 0)
        {
            log_error("possibility error : %i\n",analyse);
            log_error(" ---");
            print_possibility_packet(currPossibility);
        }
#endif // DEBUG_CHECK_POSSIBILITY
    }

    // On a au moins placé une piece
    if (lastId>-1) {
        checkIfResultFound(currPossibility, all_rotate_part);
        return incAlloc;
    }
    return 0;
}

/**
 * @brief Valide l'intégrité d'un `possibility_packet`.
 *
 * @param packet      Paquet à valider.
 * @param rotateParts Tableau de toutes les rotations (chargé depuis `parts_files` si NULL).
 * @return  0 si valide, sinon un code négatif :
 *          -1 packet NULL, -2 coordonnées hors grille, -4 alloc ≤ 0,
 *          -5 nombre de faces utilisées < alloc, -6 pièce absente à (7,8),
 *          -7 valeur de grille invalide, -9 incohérence de bord.
 */
int check_possibility(struct possibility_packet *packet, struct array_part *rotateParts)
{
    if (rotateParts == NULL) {
        struct array_part *apart= read_parts(parts_files);
        
        rotateParts = rotate_all_parts(apart);
    }
    
	if(packet == NULL) return -1;
	
	if(packet->x < 0 || packet->x >= ETERN_SIZE || packet->y < 0 || packet->y >= ETERN_SIZE) return -2;
	
	//if(packet->direcory < DIR_UP || packet->direcory > DIR_LEFT) return -3;
	
	if(packet->alloc <= 0) return -4;
	
	int i;
	int faceused= 0;
	for(i = 0; i < ETERN_PARTS;i++)
	{
#ifdef FACES_USED_BITS
		if(is_face_used(packet->b_faceused, i) == 1)
#else
        if(packet->faceused[i] == 1)
#endif // FACES_USED_BITS
		{
			faceused++;
		}
	}
    // peu être différent à cause de possibility_all_has_a_next qui alloue où il n'y a qu'une possibilité
    // mais ne change pas alloc pour poursuivre la recherche
    if(faceused < packet->alloc) {
        return -5;
    }
	
    if (packet->grid[7][8] != id_for_rotated_part(139, 2)) {
        return -6;
    }
    
    // map_big_array *map_parts = prepare_map_part(rotateParts);
    
    // Controle que les pieces correspondent à leur "entourage"
    for (int p = 0; p < packet->alloc; p++) {
        uint8_t x = dirx[p];
        uint8_t y = diry[p];
        int16_t gridValue = packet->grid[x][y];
        if (gridValue < 0 || gridValue >= rotateParts->size) {
            return -7;
        }
        struct part partXY = rotateParts->parts[gridValue];
        
        int8_t top = 0;
        // TOP
        if (y -1 >= 0)
        {
            if (packet->grid[x][y-1] < 0) {
                top = -1;
            } else {
                top = rotateParts->parts[packet->grid[x][y-1]].bottom;
            }
        }
        if (top != -1 && partXY.top != top) {
            return -9;
        }
        
        // RIGHT
        int8_t right = 0;
        if (x + 1 < ETERN_SIZE)
        {
            if (packet->grid[x+1][y] < 0) {
                right = -1;
            } else {
                right = rotateParts->parts[packet->grid[x+1][y]].left;
            }
        }
        if (right != -1 && partXY.right != right) {
            return -9;
        }
        
        // BOTTOM
        int8_t bottom = 0;
        if (y + 1 < ETERN_SIZE)
        {
            if (packet->grid[x][y+1] < 0) {
                bottom = -1;
            } else {
                bottom = rotateParts->parts[packet->grid[x][y+1]].top;
            }
        }
        if (bottom != -1 && partXY.bottom != bottom) {
            return -9;
        }
        
        // LEFT
        int8_t left = 0;
        if (x - 1 >= 0)
        {
            if (packet->grid[x-1][y] < 0) {
                left = -1;
            } else {
                left = rotateParts->parts[packet->grid[x-1][y]].right;
            }
        }
        if (left != -1 && partXY.left != left) {
            return -9;
        }
    }

	return 0;
}

/**
 * @brief Affiche un `possibility_packet` au format JSON dans les logs.
 *
 * Format : `{"alloc": N, "x": X, "y": Y, "grid": [[...], ...]}`.
 *
 * @param packet Paquet à afficher.
 * @return       0.
 */
int print_possibility_packet(struct possibility_packet *packet)
{

	char *grid = malloc(sizeof(char) * (((ETERN_SIZE*5 + 2) * ETERN_SIZE) + ETERN_SIZE*2)); // 5 = 3chiffres + espace + virgule
	int c = 0;
	grid[c++] = '[';
	for (int y = 0; y < ETERN_SIZE; y++) {
		if (y > 0) {
			grid[c++] = ',';
			grid[c++] = ' ';
		}
		grid[c++] = '[';
		for (int x = 0; x < ETERN_SIZE; x++) {
			if (x > 0) {
				grid[c++] = ',';
				grid[c++] = ' ';
			}
			char str[10];

			sprintf(str, "%i", packet->grid[x][y]);
			for (int i = 0; i < strlen(str); i++)
			{
				grid[c++] = str[i];
			}
		}
		grid[c++] = ']';
	}
	grid[c++] = ']';
	grid[c++] = '\0';
	log_info("{\"alloc\": %i, \"x\": %i, \"y\": %i, \"grid\": ", packet->alloc, packet->x, packet->y);
    log_info("%s", grid);
    log_info("}\n");
	
	free(grid);
	
	return 0;
}

/**
 * @brief Retourne la pièce 139 dans sa rotation i8 (rotation 2, bords 2,15,15,3).
 *
 * Cette pièce est l'indice fixe officiel du puzzle Eternity II placé en (7,8).
 * Quitte le programme si la pièce est introuvable dans la map (configuration invalide).
 *
 * @param mapParts Tableau 4D de lookup.
 * @return         Pointeur vers la pièce 139 r2 dans la map.
 */
struct part* part_139_i8(map_big_array *mapParts)
{
    key_part key = {2,15,15,3};
    struct part *part = get_one_part(mapParts, key);
    if(part == NULL)
    {
        log_error("part 139 not found\n");
        exit(EXIT_FAILURE);
    }
    return part;
}

/**
 * @brief Génère l'ensemble des possibilités initiales et les injecte dans le datamanager.
 *
 * Pour le puzzle 16×16 (ETERN_PARTS == 256), place les indices officiels connus
 * (pièces 139, 208, 255, 181, 249) à leurs positions fixes, puis développe la
 * première case libre pour produire toutes les positions de départ.
 *
 * Ces possibilités initiales sont ensuite distribuées par le serveur aux clients.
 *
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 */
void first_possibility(map_big_array *mapParts, struct array_part *all_rotate_part)
{
    struct part *etern[ETERN_SIZE][ETERN_SIZE];
    int x;
    int y;
    // initialisation
    for (x = 0; x < ETERN_SIZE; x++) {
        for(y=0; y < ETERN_SIZE; y++)
        {
            etern[x][y] = NULL;
        }
    }

#if ETERN_PARTS == 256
    x = 7;
    y = 8;
    int cur_dir = DIR_UP;
    
    struct part *part = part_139_i8(mapParts);
    if(part != NULL) {
        etern[x][y] = part;
        
#if ETERN_WITH_INDICES
        // 208 C3 -- rotation 3
        // 1 13 12 3
        key_part k208 = {13,12,3,1};
        part = get_one_part(mapParts, k208);
        if(part == NULL)
        {
            log_error("part 208 r3 not found\n");
            exit(EXIT_FAILURE);
        }
        etern[2][2] = part;
        
        // 255 C14 -- rotation 3
        // 7 13 11 13
        key_part k255 = {13,11,13,7};
        part = get_one_part(mapParts, k255);
        if(part == NULL)
        {
            log_error("part 255 r3 not found\n");
            exit(EXIT_FAILURE);
        }
        etern[13][2] = part;
        
        // 181 N3-- rotation 3
        // 3 7 15 5
        key_part k181 = {7,15,5,3};
        part = get_one_part(mapParts, k181);
        if(part == NULL)
        {
            log_error("part 181 r3 not found\n");
            exit(EXIT_FAILURE);
        }
        etern[2][13] = part;
        
        // 249 N14 -- rotation 0
        // 8 5 9 10
        key_part k249 = {8,5,9,10};
        part = get_one_part(mapParts, k249);
        if(part == NULL)
        {
            log_error("part 249 r0 not found\n");
            exit(EXIT_FAILURE);
        }
        etern[13][13] = part;
#endif
    } else
    {
        cur_dir = DIR_LEFT;
    }
#else
    x = 1;
    y = 1;
    int cur_dir = DIR_LEFT;
#endif
    
    int16_t idParts[ETERN_PARTS+1][4];
    for(int p=0; p <= ETERN_PARTS;p++) {
        for(int r=0; r < 4;r++) {
            idParts[p][r] = p + ETERN_PARTS * r;
        }
    }
    
    File *possibilities = malloc(sizeof(File));
    init_file_with_cache(possibilities, 0, sizeof(struct possibility_packet));
    key_part *key = malloc(sizeof(key_part));
    
    struct possibility_packet *possibilityPacket = generate_possibility_packet(x, y, etern, cur_dir);
    non_null_possibilities++;
    // alimente key pour indiquer quoi chercher
    what_search_to_key2(all_rotate_part, possibilityPacket, key, mapParts->sizearrayM);
    int max = search_possiblity_light(possibilities, key, possibilityPacket, mapParts, all_rotate_part, idParts);
    free(possibilityPacket);
    
    // Si le résultat à dépasser le plus grand qu'on a trouvé, on trace
    if(max > max_result)
    {
        max_result = max;
        if(max_result >= ETERN_PARTS)
        {
            log_error("Erreur alloc > ETERN_PARTS\n");
        }
        log_info("max result:%i\n",max_result);
    }
    
    while (possibilities->size > 0) {
        struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
        scroll(possibilities,packet);
        log_info("packet->alloc:%i\n",packet->alloc);
        if(packet->alloc > max_result)
        {
            max_result = packet->alloc;
            if(max_result >= ETERN_PARTS)
            {
                log_error("Erreur alloc > ETERN_PARTS\n");
            }
            log_info("max result:%i\n",max_result);
        }
        array_possibility_packet *aposs2 = build_single_array_possibility_packet(packet);
        if(add_possibility(NULL, aposs2))
        {
            log_error("error on add_possibility\n");
			// Pour l'initialisation, on crash car ce n'est vraiment pas normal
            exit(EXIT_FAILURE);
        }
        non_null_possibilities++;
		free_array_possibility_packet(aposs2);
        free(packet);
    }
    free_file(possibilities);
    free(key);
}

/**
 * @brief Compare deux paquets et retourne 0 s'ils sont identiques.
 *
 * @param packet       Premier paquet.
 * @param other_packet Second paquet.
 * @return  0 si identiques, sinon un code négatif indiquant la première différence :
 *          -1 nullité différente, -2 alloc diffèrent, -3 position (x,y) différente,
 *          -4 masque de pièces utilisées différent, -5 grille différente.
 */
int compare_possibility(struct possibility_packet *packet, struct possibility_packet *other_packet) {
    // Test si l'un est null alors est-ce que les deux le sont
    if (packet == NULL || other_packet == NULL) {
        return packet == other_packet ? -1 : 0;
    }

    // Test différence de pieces placées
	if (packet->alloc != other_packet->alloc) {
		return -2;
	}

    // Test de la position
	if (packet->x != other_packet->x || packet->y != other_packet->y) {
		return -3;
	}

    // Test sur les pieces utilisées
	for (int u = 0; u < ETERN_PARTS; u++) {
#ifdef FACES_USED_BITS
		if (is_face_used(packet->b_faceused, u) != is_face_used(other_packet->b_faceused, u)) {
#else
        if (packet->faceused[u] != other_packet->faceused[u]) {
#endif // FACES_USED_BITS
			return -4;
		}
	}

    // Test si les pieces sont placées à la même position (et meme sens)
	for (int x = 0; x < ETERN_SIZE; x++) {
		for (int y = 0; y < ETERN_SIZE; y++) {
			if (packet->grid[x][y] != other_packet->grid[x][y]) {
				return -5;
			}
		}
	}

	return 0;
}
    
/**
 * @brief Indique si `packet` est un préfixe de `other_packet`.
 *
 * Vérifie que toutes les pièces placées dans `packet` (jusqu'à `packet->alloc`)
 * sont identiques à celles d'`other_packet` aux mêmes positions.
 *
 * @param packet       Paquet supposément ancêtre (alloc inférieur).
 * @param other_packet Paquet supposément descendant.
 * @return  1 si `packet` est bien un ancêtre, -1 si alloc ≥, -2 si grilles divergent, 0 si NULL.
 */
int is_origin_of(struct possibility_packet *packet, struct possibility_packet *other_packet) {
    // Test si l'un est null alors biensur que non
    if (packet == NULL || other_packet == NULL) {
        return 0;
    }

    // Le sens est imposé et on ne cherche pas une égalité
    if (packet->alloc >= other_packet->alloc) {
        return -1;
    }

    // Test si les pieces alloués sont les memes
    for (int p = 0; p < packet->alloc; p++) {
        uint8_t x = dirx[p];
        uint8_t y = diry[p];
        if (packet->grid[x][y] != other_packet->grid[x][y]) {
            return -2;
        }
    }
    
    return 1;
}

/**
 * @brief Emballe un unique `possibility_packet` dans un `array_possibility_packet`.
 *
 * Copie le paquet dans un tableau de taille 1, format attendu par `add_possibility`.
 *
 * @param possibility Paquet à emballer (peut être NULL → tableau de taille 0).
 * @return            `array_possibility_packet` alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *build_single_array_possibility_packet(struct possibility_packet *possibility) {
	array_possibility_packet *result = malloc(sizeof(array_possibility_packet));
	if (possibility != NULL) {
		result->size = 1;
        result->possibilities = malloc(sizeof(struct possibility_packet));
        memcpy(&result->possibilities[0], possibility, sizeof(struct possibility_packet));
	} else {
		result->size = 0;
        result->possibilities = NULL;
	}

	return result;
}

/**
 * @brief Libère un `array_possibility_packet` et son tableau de paquets interne.
 * @param possibilities Structure à libérer.
 */
void free_array_possibility_packet(array_possibility_packet *possibilities) {
	if (possibilities->possibilities != NULL) {
		free(possibilities->possibilities);
	}
	free(possibilities);
}
