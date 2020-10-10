//
//  possibility.c
//  eternityII
//
//  Created by Xavier GRIFFON on 18/09/13.
//  Copyright (c) 2013 Xavier GRIFFON. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "possibility.h"
// #include "etii_opencl.h"

static uint8_t directions[256] = {135,221,210,34,45,255,254,253,237,239,223,222,238,240,224,208,209,241,242,226,225,0,1,2,18,16,32,33,17,15,31,47,46,14,13,29,30,252,251,250,249,248,247,246,245,244,243,192,176,160,144,128,112,96,80,64,48,3,4,5,6,7,8,9,10,11,12,63,79,95,111,127,143,159,175,191,207,236,235,234,233,232,231,230,229,228,227,193,177,161,145,129,113,97,81,65,49,19,20,21,22,23,24,25,26,27,28,62,78,94,110,126,142,158,174,190,206,220,219,218,217,216,215,214,213,212,211,194,178,162,146,130,114,98,82,66,50,35,36,37,38,39,40,41,42,43,44,61,77,93,109,125,141,157,173,189,205,204,203,202,201,200,199,198,197,196,195,179,163,147,131,115,99,83,67,51,52,53,54,55,56,57,58,59,60,76,92,108,124,140,156,172,188,187,186,185,184, 183,182,181,180,164,148,132,116,100,84,68,69,70,71,72,73,74,75,91,107,123,139,155,171,170,169,168,167,166,165,149,133,117,101,85,86,87,88,89,90,106,122,138,154,153,152,151,150,134,118,102,103,104,105,121,137,119,136,120};

static uint8_t dirx[256] = {7,13,2,2,13,15,14,13,13,15,15,14,14,0,0,0,1,1,2,2,1,0,1,2,2,0,0,1,1,15,15,15,14,14,13,13,14,12,11,10,9,8,7,6,5,4,3,0,0,0,0,0,0,0,0,0,0,3,4,5,6,7,8,9,10,11,12,15,15,15,15,15,15,15,15,15,15,12,11,10,9,8,7,6,5,4,3,1,1,1,1,1,1,1,1,1,1,3,4,5,6,7,8,9,10,11,12,14,14,14,14,14,14,14,14,14,14,12,11,10,9,8,7,6,5,4,3,2,2,2,2,2,2,2,2,2,2,3,4,5,6,7,8,9,10,11,12,13,13,13,13,13,13,13,13,13,13,12,11,10,9,8,7,6,5,4,3,3,3,3,3,3,3,3,3,3,4,5,6,7,8,9,10,11,12,12,12,12,12,12,12,12,12,11,10,9,8,7,6,5,4,4,4,4,4,4,4,4,5,6,7,8,9,10,11,11,11,11,11,11,11,10,9,8,7,6,5,5,5,5,5,5,6,7,8,9,10,10,10,10,10,9,8,7,6,6,6,6,7,8,9,9,9,7,8,8};

static uint8_t diry[256] = {8,13,13,2,2,15,15,15,14,14,13,13,14,15,14,13,13,15,15,14,14,0,0,0,1,1,2,2,1,0,1,2,2,0,0,1,1,15,15,15,15,15,15,15,15,15,15,12,11,10,9,8,7,6,5,4,3,0,0,0,0,0,0,0,0,0,0,3,4,5,6,7,8,9,10,11,12,14,14,14,14,14,14,14,14,14,14,12,11,10,9,8,7,6,5,4,3,1,1,1,1,1,1,1,1,1,1,3,4,5,6,7,8,9,10,11,12,13,13,13,13,13,13,13,13,13,13,12,11,10,9,8,7,6,5,4,3,2,2,2,2,2,2,2,2,2,2,3,4,5,6,7,8,9,10,11,12,12,12,12,12,12,12,12,12,12,12,11,10,9,8,7,6,5,4,3,3,3,3,3,3,3,3,3,3,4,5,6,7,8,9,10,11,11,11,11,11,11,11,11,11,10,9,8,7,6,5,4,4,4,4,4,4,4,4,5,6,7,8,9,10,10,10,10,10,10,10,9,8,7,6,5,5,5,5,5,5,6,7,8,9,9,9,9,9,8,7,6,6,6,6,7,8,7,8,7};

int decode_direction()
{
	printf("/nx : ");
	int i;
	for(i=0;i<256;i++) {
		int x = directions[i] % ETERN_SIZE;
		printf("%i,",x);
	}
	
	printf("/ny : ");
	for(i=0;i<256;i++) {
		int x = directions[i] % ETERN_SIZE;
		int y = (directions[i] - x) / ETERN_SIZE;
		printf("%i,",y);
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
			printf("grille : %i not use\n", i);
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
	
	memset(result->faceused, 0, sizeof(result->faceused));
	
	int l;
	for (l = 0; l < ETERN_SIZE; l++)
	{
		int h;
		for(h = 0; h < ETERN_SIZE; h++)
		{
			struct part *part = etern[l][h];
			if(part != NULL)
			{
				result->grid[l][h] = part->id + part->rotation * 256;
                result->faceused[part->id-1] = 1;
				result->alloc++;
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

void what_search_in_grid_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, int8_t x, int8_t y,key_part *key) {
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
		if(possiblity->grid[x][y-1] < 0)
		{
			key->k1 = -1;
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
			key->k2 = -1;
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
			key->k3 = -1;
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
			key->k4 = -1;
		} else
		{
			key->k4 = all_rotate_parts->parts[possiblity->grid[x-1][y]].right;
		}
	}
}

/*
 * Alimente dans key, une représentation de quoi chercher pour l'emplacement.
 */
void what_search_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key) {
    // -2 : non défini
    // -1 toute face
    // 0 bordure
	key->k1 =-2;
	key->k2 =-2;
	key->k3 =-2;
	key->k4 =-2;
	
	int x = possiblity->x;
	int y = possiblity->y;

	// TOP
	if(y -1 < 0)
	{
		key->k1 = 0;
	} else
	{
		if(possiblity->grid[x][y-1] < 0)
		{
			key->k1 = -1;
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
			key->k2 = -1;
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
			key->k3 = -1;
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
			key->k4 = -1;
		} else
		{
			key->k4 = all_rotate_parts->parts[possiblity->grid[x-1][y]].right;
		}
	}
}

key_part what_search(struct array_part *all_rotate_parts, int x, int y, struct possibility_packet *possiblity)
{
	//char *result = malloc(MAX_KEY_LENGTH * sizeof(char));
	key_part result;
	result.k1 =-2;
	result.k2 =-2;
	result.k3 =-2;
	result.k4 =-2;
	// TOP
	if(y -1 < 0)
	{
		result.k1 = 0;
	} else
	{
		if(possiblity->grid[x][y-1] < 0)
		{
			result.k1 = -1;
		} else
		{
			result.k1 = all_rotate_parts->parts[possiblity->grid[x][y-1]].bottom;
		}
	}
	
	// RIGHT
	if(x + 1 >= ETERN_SIZE)
	{
		result.k2 = 0;
	} else
	{
		if(possiblity->grid[x+1][y] < 0)
		{
			result.k2 = -1;
		} else
		{
			result.k2 = all_rotate_parts->parts[possiblity->grid[x+1][y]].left;
		}
	}
	
	// BOTTOM
	if(y + 1 >= ETERN_SIZE)
	{
		result.k3 = 0;
	} else
	{
		if(possiblity->grid[x][y+1] < 0)
		{
			result.k3 = -1;
		} else
		{
			result.k3 = all_rotate_parts->parts[possiblity->grid[x][y+1]].top;
		}
	}
	
	// LEFT
	if(x -1 < 0)
	{
		result.k4 = 0;
	} else
	{
		if(possiblity->grid[x-1][y] < 0)
		{
			result.k4 = -1;
		} else
		{
			result.k4 = all_rotate_parts->parts[possiblity->grid[x-1][y]].right;
		}
	}
	
//	if(result.k1 == -1 && result.k2 == -1 && result.k3 == -1 && result.k4 == -1) {
//		printf("nothing to search x:%i y:%i \n",x,y);
//	}
	
	return result;
}

/*
 int change_dir(int cur_dir, int x, int y, struct possibility_packet *possiblity)
 {
 if (cur_dir == DIR_UP)
 {
 // on continue vers le haut sauf si il n'y a rien à droite
 if(x+1 < ETERN_SIZE && possiblity->grid[x+1][y].k1 < -1)
 {
 cur_dir = DIR_RIGHT;
 }
 } else if (cur_dir == DIR_RIGHT)
 {
 // on continue vers la droite sauf si il y a rien en bas
 if(y+1 < ETERN_SIZE && possiblity->grid[x][y+1].k1 < -1)
 {
 cur_dir = DIR_DOWN;
 }
 } else if (cur_dir == DIR_DOWN)
 {
 // on continue vers le bas sauf si il y a rien à gauche
 if(x-1 >=0 && possiblity->grid[x-1][y].k1 < -1)
 {
 cur_dir = DIR_LEFT;
 }
 } else if (cur_dir == DIR_LEFT)
 {
 // on continue vers la gauche sauf si il y a rien en haut
 if(y-1 >= 0 && possiblity->grid[x][y-1].k1 < -1)
 {
 cur_dir = DIR_UP;
 }
 }
 
 return cur_dir;
 }
 */
/*
int change_dir(int cur_dir, int x, int y, struct possibility_packet *possiblity)
{
    if (cur_dir == DIR_UP)
    {
		// on continue vers le haut sauf si on est à 0 ou si la place est prise
		if(y-1 < 0 || possiblity->grid[x][y-1].k1 > -1)
        {
            cur_dir = DIR_LEFT;
        }
        
	} else if (cur_dir == DIR_LEFT)
	{
		// on continue vers la gauche sauf si on est à 0 ou si la place est prise
		if(x-1 < 0 || possiblity->grid[x-1][y].k1 > -1)
		{
			cur_dir = DIR_DOWN;
		}
		
	} else if (cur_dir == DIR_DOWN)
	{
		// on continue vers le bas sauf si on est au max ou la place est prise
		if(y+1 >= ETERN_SIZE || possiblity->grid[x][y+1].k1 > -1)
		{
			cur_dir = DIR_RIGHT;
		}
    } else if (cur_dir == DIR_RIGHT)
    {
        // on continue vers la droite sauf si est au max ou la place est prise
		if(x+1 >= ETERN_SIZE || possiblity->grid[x+1][y].k1 > -1)
		{
			cur_dir = DIR_UP;
		}
    }
	
	
    return cur_dir;
}
 */

int save_possibility(char *filename, struct possibility_packet *possibility)
{
	FILE *f = fopen(filename, "w");
	if(!f)
	{
		printf("file :%s",filename);
		perror("fopen()");
		exit(EXIT_FAILURE);
	}
	
	fwrite(possibility, sizeof(struct possibility_packet), 1, f);
	
	fclose(f);
	return 0;
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
			if(search->parts[s].id != 0 && possibility->faceused[search->parts[s].id -1] == 0)
			{
                result = 1;
            }
        }
    }
    return result;
}

/*
 * retourne 1 si les place sont encore libre et que des possiblités (> 1) existes
 * 0 si plus aucune piece n'est plaçable ou qu'elles sont toutes placées
 */
int possibility_all_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    int result = 1;
    
	key_part wsearch;
	int c;
    // On parcours
	for(c=possibility->alloc;c < ETERN_PARTS && result == 1;c++) {
		result = 0;
		int8_t x = dirx[c];
		int8_t y = diry[c];
		if(possibility->grid[x][y] == -2) {
			what_search_in_grid_to_key(all_rotate_part, possibility, x, y,&wsearch);
			if(wsearch.k1 > -1 || wsearch.k2 > -1 || wsearch.k3 > -1 || wsearch.k4 > -1) {
				
				struct array_part *search = get_parts_bigarray_with_key(mapParts, &wsearch);
				int s;
				if(search->size > 0)
				{
					
					
					for(s=0; s< search->size && result == 0; s++)
					{
						if(search->parts[s].id != 0 && possibility->faceused[search->parts[s].id -1] == 0)
						{
							if( search->size == 1) {
								possibility->faceused[search->parts[s].id -1] = 1;
								possibility->grid[x][y] = idpart(search->parts[s].id, search->parts[s].rotation);
							}
							result = 1;
						}
					}
					
				}
			}else {
				result = 1;
				break;
			}
		} else {
			result = 1;
		}
		
		
		
	}
	
	return result;
}

int search_possiblity(File *result,struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part)
{
	int max_result=0;
    uint8_t x;
	uint8_t y;
	
	// initialisation
	x = possiblity->x;
	y = possiblity->y;
    
	//cur_dir = possiblity->direcory;
	key_part wsearch = what_search(all_rotate_part, x, y, possiblity);
	
	int8_t p[4] = {wsearch.k1,wsearch.k2,wsearch.k3,wsearch.k4};
	//int next_dir = change_dir(cur_dir, x, y, possiblity);
	struct array_part *search = get_parts_bigarray(mapParts, p);
	int s;

		// test performance
		//struct possibility_packet *poss = malloc(sizeof(*poss));
		
		for(s=0; s< search->size; s++)
		{
			if(search->parts[s].id != 0 && possiblity->faceused[search->parts[s].id -1] == 0)
			{
				struct part part = search->parts[s];
                // On injecte dans la pile afin qu'un copie soit faite puis on utilise la copie
				put (result, possiblity);
				struct possibility_packet *poss = (struct possibility_packet *)result->end->value;
				poss->grid[x][y] = idpart(part.id, part.rotation);
				poss->alloc++;
				if(poss->alloc >= ETERN_PARTS)
				{
					printf("fin de la boucle à %i \n", poss->alloc);
					printf("solution trouvée\n");
					for(x = 0; x < ETERN_SIZE; x++)
					{
						for(y=0;y < ETERN_SIZE; y++)
						{
							//struct part *part = get_one_part(mapParts, poss.grid[x][y]);
                            struct part *part = &all_rotate_part->parts[poss->grid[x][y]];
							printf("%i;%i; ",x,y);
							print_part(part);
						}
					}
					save_possibility("./solution",poss);
					//free(poss);
					//free(wsearch);
					exit(EXIT_SUCCESS);
				}
				if(poss->alloc > max_result)
				{
					max_result = poss->alloc;
				}
				
//				div_t xy = div(directions[poss.alloc], ETERN_SIZE);
//				poss.x = xy.rem;
//				poss.y = xy.quot;
//				poss->x = directions[poss->alloc] % ETERN_SIZE;
//				poss->y = (directions[poss->alloc] - poss->x) / ETERN_SIZE;
				poss->x = dirx[poss->alloc];
				poss->y = diry[poss->alloc];
				
				poss->faceused[part.id -1] = 1;
				//put (result, &poss);
			}
		//free(poss);
	}
	
	//free(wsearch);
	return max_result;
}

/* (ajouter) un élément dans la file */
void put_possibility (File * suite, struct possibility_packet *value){
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
	if(suite->end == NULL){
		suite->start = new_element;
		suite->end = new_element;
	}else {
		suite->end->next = new_element;
		new_element->previous = suite->end;
		suite->end = new_element;
	}
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
	int lastId =0;
	
	struct possibility_packet *currPossibility = possiblity;
	
    // On vérifie si la possibilité à cette position n'est toujours pas connu.
	if(currPossibility->grid[x][y] == -2) {
    
		// TODO : voir pour réviser la recherche avec seulement des id de all_rotate_part
        // liste des pieces répondant à la recherche (key)
        struct array_part *search = get_parts_bigarray_with_key(mapParts, key);
        for(s=0; s< search->size; s++)
        {
            int position = search->parts[s].id -1;
            // Si la piece n'est pas déjà utilisée dans la suite de possibilité, on a donc une possiblité supplémentaire
            if(!currPossibility->faceused[position])
            {
                // Dans le cas où on a déjà généré une possiblité, on libère la piece qui avait été utilisée avant de généré un nouveau jeu
                if(lastId) {
                    currPossibility->faceused[lastId -1] = 0;
                }
                // On ajoute la définition d'une possibilité dans la suite.
                put_possibility(result, currPossibility);
                // On se placce à la fin de la suite qui correspond à la nouvelle définition
                currPossibility = result->end->value;
                // On place la piece
                currPossibility->grid[x][y] = idParts[search->parts[s].id][search->parts[s].rotation];
                // statistique du nombre de piece placée
                currPossibility->alloc = incAlloc;
                
                currPossibility->x = nX;
                currPossibility->y = nY;
                // On indique que la piece est utilisée
                currPossibility->faceused[position] = 1;
                // identifiant de la dernière piece utilisée
                lastId = search->parts[s].id;
                // On vérifie que les emplacements libres ont tous une piece possible
                // Si qu'une possiblité, alors place la piece
                if(possibility_all_has_a_next(currPossibility, mapParts, all_rotate_part) == 0) {
                    // Consomme la suite ou fournie la possiblité actuel si pas d'élément dans la suite
                    scroll_cache(result, currPossibility);
                }
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
		if(possibility_all_has_a_next(currPossibility, mapParts, all_rotate_part) == 0) {
            // pk consommer ?
			//scroll_cache(result, currPossibility);
		}
	}

    // On a au moins placé une piece
	if (lastId) {
		max_result = incAlloc;
		if(max_result >= ETERN_PARTS)
		{
			struct possibility_packet *poss = currPossibility;
			printf("fin de la boucle à %i \n", poss->alloc);
			printf("solution trouvée\n");
			for(x = 0; x < ETERN_SIZE; x++)
			{
				for(y=0;y < ETERN_SIZE; y++)
				{
					//struct part *part = get_one_part(mapParts, poss.grid[x][y]);
					struct part *part = &all_rotate_part->parts[poss->grid[x][y]];
					printf("%i;%i; ",x,y);
					print_part(part);
				}
			}
			save_possibility("./solution",poss);
			//free(poss);
			//free(wsearch);
			exit(EXIT_SUCCESS);
		}
	}
	return max_result;
}

/*
 0 OK
 -1 packet NULL
 -2 (x or y) > ETERN_SIZE or < 0
 -3 directory > or < dir_possibilities
 -4 alloc <= 0
 -5 alloc <> faceused
 */
int check_possibility(struct possibility_packet *packet)
{
	if(packet == NULL) return -1;
	
	if(packet->x < 0 || packet->x >= ETERN_SIZE || packet->y < 0 || packet->y >= ETERN_SIZE) return -2;
	
	//if(packet->direcory < DIR_UP || packet->direcory > DIR_LEFT) return -3;
	
	if(packet->alloc <= 0) return -4;
	
	int i;
	int faceused= 0;
	for(i = 0; i < ETERN_PARTS;i++)
	{
		if(packet->faceused[i] == 1)
		{
			faceused++;
		}
	}
	if(faceused != packet->alloc) return -5;
	
	return 0;
}

int print_possibility_packet(struct possibility_packet *packet)
{

	printf("possibility facesused:%i \n",packet->alloc);
	return 0;
}

/*
File *search_possiblity_light_opencl(etii_cl_instance *instance,struct possibility_packet *possiblity, int nbPossibility) {
	File *result = search_possibility_opencl(instance, possiblity, nbPossibility);
	return result;
}

File *search_possiblity_opencl(etii_cl_instance *instance,struct possibility_packet *possiblity, int nbPossibility,map_big_array *mapParts, struct array_part *all_rotate_part)
{
    File *result = malloc(sizeof(File));
    init_file_with_cache(result, nbPossibility * 60, sizeof(struct possibility_packet));
	
	key_part *wsearch = malloc(sizeof(key_part) * nbPossibility);
	int w;
	for(w = 0; w < nbPossibility; w++)
	{
		uint8_t x = possiblity[w].x;
		uint8_t y = possiblity[w].y;
		//int cur_dir = possiblity[w].direcory;
		key_part ws = what_search(all_rotate_part, x, y, &possiblity[w]);
		memcpy(&wsearch[w], &ws, sizeof(ws));
		//free(ws);
	}
	
	File **fileParts = test_opencl(instance,wsearch, possiblity, nbPossibility);
	int f;
	int16_t *kpart = malloc(sizeof(int16_t));
	for(f=0;f < nbPossibility;f++)
	{
		if(fileParts[f]->size > 0)
		{
			while (fileParts[f]->size > 0) {
				scroll(fileParts[f], kpart);
				if(kpart != NULL)
				{
					put (result, &possiblity[f]);
					struct possibility_packet *poss = result->end->value;

					uint8_t x = possiblity[f].x;
					uint8_t y = possiblity[f].y;
					poss->grid[x][y] = *kpart;
					
					poss->alloc++;

					if(poss->alloc == ETERN_PARTS)
					{
						printf("fin de la boucle à %i \n", poss->alloc);
						printf("solution trouvée\n");
						for(x = 0; x < ETERN_SIZE; x++)
						{
							for(y=0;y < ETERN_SIZE; y++)
							{
								struct part *part = &all_rotate_part->parts[poss->grid[x][y]];
								printf("%i;%i; ",x,y);
								print_part(part);
							}
						}
						save_possibility("./solution",poss);
						free(poss);
						free(wsearch);
						exit(EXIT_SUCCESS);
					}
					
					poss->x = dirx[poss->alloc];
					poss->y = diry[poss->alloc];
					
					poss->faceused[*kpart % 256] = 1;
				}
				
				
			}
			
		}
		free_file(fileParts[f]);
	}
	free(kpart);
	kpart = NULL;
	free(fileParts);
    
	free(wsearch);

    return result;
}
*/
