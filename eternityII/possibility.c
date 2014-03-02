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
#include "etii_opencl.h"

static int directions[256] = {135,221,210,34,45,255,254,253,237,222,223,239,238,240,224,208,209,226,242,241,225,0,1,2,18,33,32,16,17,13,14,15,31,47,46,29,30,118,119,120,136,152,151,150,134,252,251,250,249,248,247,246,245,244,243,192,176,160,144,128,112,96,80,64,48,3,4,5,6,7,8,9,10,11,12,63,79,95,111,127,143,159,175,191,207,236,235,234,233,232,231,230,229,228,227,193,177,161,145,129,113,97,81,65,49,19,20,21,22,23,24,25,26,27,28,62,78,94,110,126,142,158,174,190,206,220,219,218,217,216,215,214,213,212,211,194,178,162,146,130,114,98,82,66,50,35,36,37,38,39,40,41,42,43,44,61,77,93,109,125,141,157,173,189,205,204,203,202,201,200,199,198,197,196,195,179,163,147,131,115,99,83,67,51,52,53,54,55,56,57,58,59,60,76,92,108,124,140,156,172,188,187,186,185,184,183,182,181,180,164,148,132,116,100,84,68,69,70,71,72,73,74,75,91,107,123,139,155,171,170,169,168,167,166,165,149,133,117,101,85,86,87,88,89,90,106,122,138,154,102,103,104,105,121,137,153};

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
	result->direcory = directory;
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
				result->grid[l][h].k1 = part->top;
				result->grid[l][h].k2 = part->right;
				result->grid[l][h].k3 = part->bottom;
				result->grid[l][h].k4 = part->left;
				result->faceused[part->id-1] = 1;
				result->alloc++;
			} else
			{
				result->grid[l][h].k1 = -2;
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

key_part what_search(map_big_array *map_parts, int x, int y, struct possibility_packet possiblity)
{
	if(x==14 && y == 11 && possiblity.alloc == 9) {
		printf("xy\n");
	}
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
		if(possiblity.grid[x][y-1].k1 < 0)
		{
			result.k1 = -1;
		} else
		{
			result.k1 = possiblity.grid[x][y-1].k3;
		}
	}
	
	// RIGHT
	if(x + 1 >= ETERN_SIZE)
	{
		result.k2 = 0;
	} else
	{
		if(possiblity.grid[x+1][y].k1 < 0)
		{
			result.k2 = -1;
		} else
		{
			result.k2 = possiblity.grid[x+1][y].k4;
		}
	}
	
	// BOTTOM
	if(y + 1 >= ETERN_SIZE)
	{
		result.k3 = 0;
	} else
	{
		if(possiblity.grid[x][y+1].k1 < 0)
		{
			result.k3 = -1;
		} else
		{
			result.k3 = possiblity.grid[x][y+1].k1;
		}
	}
	
	// LEFT
	if(x -1 < 0)
	{
		result.k4 = 0;
	} else
	{
		if(possiblity.grid[x-1][y].k1 < 0)
		{
			result.k4 = -1;
		} else
		{
			result.k4 = possiblity.grid[x-1][y].k2;
		}
	}
	
	if(result.k1 == -1 && result.k2 == -1 && result.k3 == -1 && result.k4 == -1) {
		printf("nothing to search\n");
	}
	
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

int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts)
{
    int result = 0;
    
    // initialisation
	uint8_t x = possibility->x;
	uint8_t y = possibility->y;
    key_part wsearch = what_search(mapParts, x, y, *possibility);
	
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

int search_possiblity(File *result,struct possibility_packet *possiblity, map_big_array *mapParts)
{
	int max_result=0;
    uint8_t x;
	uint8_t y;
	int cur_dir;
	struct part *part = NULL;
	// initialisation
	x = possiblity->x;
	y = possiblity->y;
    
	cur_dir = possiblity->direcory;
	part = NULL;
	
	key_part wsearch = what_search(mapParts, x, y, *possiblity);
	
	int8_t p[4] = {wsearch.k1,wsearch.k2,wsearch.k3,wsearch.k4};
	int next_dir = change_dir(cur_dir, x, y, possiblity);
	struct array_part *search = get_parts_bigarray(mapParts, p);
	int s;
	if(search->size > 0)
	{
		// test performance
		//struct possibility_packet *poss = malloc(sizeof(*poss));
		struct possibility_packet poss;
		for(s=0; s< search->size; s++)
		{
			if(search->parts[s].id != 0 && possiblity->faceused[search->parts[s].id -1] == 0)
			{
				part = &search->parts[s];
                
				memcpy(&poss, possiblity, sizeof(*possiblity));
				poss.grid[x][y].k1 = part->top;
				poss.grid[x][y].k2 = part->right;
				poss.grid[x][y].k3 = part->bottom;
				poss.grid[x][y].k4 = part->left;
				poss.alloc++;
				if(poss.alloc >= ETERN_PARTS)
				{
					printf("fin de la boucle à %i \n", poss.alloc);
					printf("solution trouvée\n");
					for(x = 0; x < ETERN_SIZE; x++)
					{
						for(y=0;y < ETERN_SIZE; y++)
						{
							struct part *part = get_one_part(mapParts, poss.grid[x][y]);
							printf("%i;%i; ",x,y);
							print_part(part);
						}
					}
					save_possibility("./solution",&poss);
					//free(poss);
					//free(wsearch);
					exit(EXIT_SUCCESS);
				}
				if(poss.alloc > max_result)
				{
					max_result = poss.alloc;
				}
				poss.direcory = next_dir;
				if (poss.direcory == DIR_UP)
				{
					poss.y--;
				} else if (poss.direcory == DIR_RIGHT)
				{
					poss.x++;
				} else if (poss.direcory == DIR_DOWN)
				{
					poss.y++;
				} else if (poss.direcory == DIR_LEFT)
				{
					poss.x--;
				}
				
				div_t xy = div(directions[poss.alloc], ETERN_SIZE);
				poss.x = xy.rem;
				poss.y = xy.quot;
				
				poss.faceused[part->id -1] = 1;
				put (result, &poss);
                
			}
		}
		//free(poss);
	}
	
	//free(wsearch);
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
	
	if(packet->direcory < DIR_UP || packet->direcory > DIR_LEFT) return -3;
	
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
	printf("possibility x:%i y:%i facesused:%i directory:%i\n",packet->x,packet->y,packet->alloc,packet->direcory);
	return 0;
}

File *search_possiblity_opencl(etii_cl_instance *instance,struct possibility_packet *possiblity, int nbPossibility,map_big_array *mapParts)
{
    File *result = malloc(sizeof(File));
    init_file_with_cache(result, 300, sizeof(struct possibility_packet));
    
	struct part *part = malloc(sizeof(struct part));
	// initialisation

    
	
	int *next_dir = malloc(sizeof(int)*nbPossibility);
	
	key_part *wsearch = malloc(sizeof(key_part) * nbPossibility);
	int w;
	for(w = 0; w < nbPossibility; w++)
	{
		uint8_t x = possiblity[w].x;
		uint8_t y = possiblity[w].y;
		int cur_dir = possiblity[w].direcory;
		key_part ws = what_search(mapParts, x, y, possiblity[w]);
		memcpy(&wsearch[w], &ws, sizeof(ws));
		//free(ws);
		
		next_dir[w] = change_dir(cur_dir, x, y, &possiblity[w]);
	}
	if(possiblity->alloc >= ETERN_PARTS)
	{
		printf("Erreur alloc > ETERN_PARTS\n");
	}
	
	File **fileParts = test_opencl(instance,wsearch, possiblity, nbPossibility);

	int f;
	for(f=0;f < nbPossibility;f++)
	{
		if(fileParts[f]->size > 0)
		{
//			printf("fileparts:%i size:%i\n",f,fileParts[f]->size);
			while (fileParts[f]->size > 0) {
				scroll(fileParts[f], part);
				if(part != NULL)
				{
					struct possibility_packet *poss = malloc(sizeof(struct possibility_packet));
					memcpy(poss, &possiblity[f], sizeof(struct possibility_packet));
					uint8_t x = possiblity[f].x;
					uint8_t y = possiblity[f].y;
					poss->grid[x][y].k1 = part->top;
					poss->grid[x][y].k2 = part->right;
					poss->grid[x][y].k3 = part->bottom;
					poss->grid[x][y].k4 = part->left;
					if(poss->alloc > ETERN_PARTS)
					{
						printf("Erreur alloc > ETERN_PARTS\n");
					}
					
					poss->alloc++;
					if(poss->alloc > ETERN_PARTS)
					{
						
						printf("Erreur alloc > ETERN_PARTS\n");
					}
					if(poss->alloc == ETERN_PARTS)
					{
						printf("fin de la boucle à %i \n", poss->alloc);
						printf("solution trouvée\n");
						for(x = 0; x < ETERN_SIZE; x++)
						{
							for(y=0;y < ETERN_SIZE; y++)
							{
								struct part *part = get_one_part(mapParts, poss->grid[x][y]);
								printf("%i;%i; ",x,y);
								print_part(part);
							}
						}
						save_possibility("./solution",poss);
						free(poss);
						free(wsearch);
						exit(EXIT_SUCCESS);
					}
					poss->direcory = next_dir[f];
					if (poss->direcory == DIR_UP)
					{
						poss->y--;
					} else if (poss->direcory == DIR_RIGHT)
					{
						poss->x++;
					} else if (poss->direcory == DIR_DOWN)
					{
						poss->y++;
					} else if (poss->direcory == DIR_LEFT)
					{
						poss->x--;
					}
					
					poss->faceused[part->id -1] = 1;
					put (result, poss);
					free(poss);
				}
				
				
			}
			
		}
		free_file(fileParts[f]);
	}
	free(part);
	part = NULL;
	free(fileParts);
    
	free(wsearch);
	free(next_dir);
    return result;
}
