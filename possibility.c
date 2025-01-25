#include "possibility.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "static_variables.h"
#include "datamanager.h"
#include "readdata.h"

#ifdef FACES_USED_BITS
void set_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part, uint8_t boolean) {
    //uint16_t groupe = part / 16;
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    //int8_t n = part % 16;
    int8_t n = part - (groupe << 4);
    number = (number & ~(1 << n)) | (boolean << n);
    faceused[groupe] = number;
}

uint8_t is_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part) {
    //uint16_t groupe = part / 16;
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    //int8_t n = part % 16;
    int8_t n = part - (groupe << 4);
    return (number >> n) & 1;
}
#endif // FACES_USED_BITS

int decode_direction()
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

int test_directions()
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

struct possibility_packet *crypt_to_network(struct possibility_packet *packet)
{
    
	return NULL;
}

struct possibility_packet *decrypt_from_network(struct possibility_packet *packet)
{
	return NULL;
}

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
/*
 * Alimente dans key, une représentation de quoi chercher pour l'emplacement.
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

void checkIfResultFound(struct possibility_packet *poss, struct array_part *all_rotate_part) {
    if(poss->alloc >= ETERN_PARTS)
    {
        log_info("fin de la boucle à %i \n", poss->alloc);
        log_info("solution trouvée\n");
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

/*
 * retourne 1 si les place sont encore libre et que des possiblités (> 1) existes
 * 0 si plus aucune piece n'est plaçable
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

/* (ajouter) un élément dans la file */
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
        // ?? à quoi correspond % 256
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

/*
 0 OK
 -1 packet NULL
 -2 (x or y) > ETERN_SIZE or < 0
 -3 directory > or < dir_possibilities
 -4 alloc <= 0
 -5 alloc > faceused
 -6 x7 y8 bad part
 -7 allocated part bad value
 */
int check_possibility(struct possibility_packet *packet, struct array_part *rotateParts)
{
    if (rotateParts == NULL) {
        struct array_part *apart= read_parts(partsFiles);
        
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
        
        // on commence vers le haut
        // et sur l'angle en bas à droite
        x = ETERN_SIZE -1;
        y = ETERN_SIZE -1;
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
    getted_possibility_not_null++;
    // alimente key pour indiquer quoi chercher
    what_search_to_key2(all_rotate_part, possibilityPacket, key, mapParts->sizearrayM);
    int max = search_possiblity_light(possibilities, key, possibilityPacket, mapParts, all_rotate_part, idParts);
    
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
        getted_possibility_not_null++;
		free_array_possibility_packet(aposs2);
        free(packet);
    }
    free_file(possibilities);
    free(key);
}

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

void free_array_possibility_packet(array_possibility_packet *possibilities) {
	if (possibilities->possibilities != NULL) {
		free(possibilities->possibilities);
	}
	free(possibilities);
}
