#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "part.h"
#include "readdata.h"

int err = 0;
int relocated = 0;
int maxrelocate = 0;

void print_part(struct part *p)
{
	printf("part [id:%i,rotation:%i / top:%i,left:%i,bottom:%i,right:%i]\n",p->id,p->rotation, p->top, p->left,p->bottom, p->right);
}

/*
 * Rotation dans le sens des aiguilles d'une montres
 * le départ est donc top
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
    if(tour >= 4)
    {
        tour = tour % 4;
    }
    result->rotation = tour;
    
    int t;
    for(t=0; t < tour; t++)
    {
		int8_t left = result->left;
		result->left = result->bottom;
		result->bottom = result->right;
		result->right = result->top;
        result->top = left;
    }
    
    return result;
}

struct array_part * rotate_all_parts(struct array_part *apart)
{
    struct array_part *result = malloc(sizeof *result);
    result->size = apart->size * 4;
    result->parts = calloc((result->size+4) ,sizeof(struct part));
    
    int p=0;
	// on ne prend pas en compte l'id 0
    int i = 1;
    while(p< result->size)
    {
        struct part *part = &apart->parts[i];
        int r;
        for (r=0; r < 4; r++)
        {
			
			struct part *rotatepart = rotatePart(part, r);
			int position = i+ 256*r;
			memcpy(&result->parts[position], rotatepart, sizeof(struct part));
			//print_part(rotatepart);
            free(rotatepart);
			p++;
        }
        i++;
    }
    return result;
}

int search_max_face(struct array_part *apart)
{
    int maxface = 0;
    int i;
    for (i = 1; i <= apart->size; i++)
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

struct array_part * search_face(struct array_part *apart, int face, int position)
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
        }else if ((position == PART_RIGHT || position == PART_NONE) && (p->right == face || (face == FACE_UNKNOW && p->right != 0)))
        {
            present = 1;
        }else if ((position == PART_BOTTOM || position == PART_NONE) && (p->bottom == face || (face == FACE_UNKNOW && p->bottom != 0)))
        {
            present = 1;
        }else if ((position == PART_LEFT || position == PART_NONE) && (p->left == face || (face == FACE_UNKNOW && p->left != 0)))
        {
            present = 1;
        }
        
        if (present != 0)
        {
			if(curList->value == NULL)
			{
				curList->value = p;
			} else
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
	if(array_size > 0)
	{
		size_t allocsize = array_size * sizeof(struct part);
		struct part *parts = malloc(allocsize);
		result->parts = parts;
	}else
	{
		result->parts = NULL;
	}
	
	curList = list;
	int c = 0;
	result->size = array_size;
	while (curList != NULL) {
		if(curList->value != NULL)
		{
			result->parts[c] = *curList->value;
			c++;
		} else
		{
			if(c>1)
			{
				printf("no value ligne:%i\n", c);
			}
		}
		struct list_part *last = curList;
		curList = curList->next;
		
		free(last);
	}
    return result;
}

unsigned long hashmap_hash_int(unsigned long key){
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
	//key = (key >> 3) * 2654435761;
	
	return key % 1024;
}

unsigned int hash (char *str)
{
    unsigned int hash = *str;//5381
	while (*str != 0)
    {
        int c = *str;
        //hash = hash * 33 + c;
        hash = ((hash << 5) + hash) + c;
        str++;
    }
		
    return hash;
}

int put_part(struct map_part *map, unsigned int key_int, char *key, struct array_part *apart)
{
	int l = key_int % map->size;
	int first = l;
	int r=0;
	while (map->elements[l].key_int != 0 && l < map->sizemap) {
		r++;
		l++;
	}
	if(first != l)
		relocated++;
	if(r > maxrelocate)
		maxrelocate = r;
    if(l >= map->sizemap)
    {
        printf("map trop petite \n");
        exit(EXIT_FAILURE);
    }
	if (map->elements[l].key_int == 0)
	{
		map->elements[l].key_int = key_int;
		map->elements[l].key = key;
		map->elements[l].apart = copy_array_part(apart);
	} else
	{
		printf("Probleme d'emplacement ligne:%i key_int:%i key:%s\n",l, key_int, key);
		err++;
	}

	return l;
}

struct array_part *get_parts(struct map_part *map, char *key)
{
    struct array_part *parts = NULL;
    
    int key_int = hash(key);
    int l = abs(key_int) % map->size;
    struct map_part_element *mpe = NULL;
    while (mpe == NULL && l < map->sizemap) {
        struct map_part_element *temp = &map->elements[l];
		if(temp != NULL && temp->key_int == key_int && strcmp(key, temp->key) == 0)
        {
            mpe = &map->elements[l];
        }
		l++;
	}
    if(mpe != NULL)
    {
        parts = mpe->apart;
    }
    return parts;
}

int8_t convert_p(int8_t p, int maxFace)
{
	int8_t result = p;
	if(result ==-1)
	{
		result = maxFace + p;
	}
	return result;
}

struct array_part *get_parts_bigarray(map_big_array *map,int8_t p[4])
{
    struct array_part *parts = NULL;

	int8_t k1 = convert_p(p[0], map->sizearray);
	int8_t k2 = convert_p(p[1], map->sizearray);
	int8_t k3 = convert_p(p[2], map->sizearray);
	int8_t k4 = convert_p(p[3], map->sizearray);
	parts = map->bigarray[k1][k2][k3][k4];
//	if(parts->size > 0 && parts->parts[0].id <0) {
//		printf("get_parts_bigarray error : size:%i for %i:%i:%i:%i-%i:%i:%i:%i r[0].id = %i\n",parts->size,p[0],p[1],p[2],p[3],k1,k2,k3,k4,parts->parts[0].id );
//	}
    return parts;
}
/*
 * Indique toutes les pieces pouvant correspondre à la recherhce (key)
 */
struct array_part *get_parts_bigarray_with_key(map_big_array *map,key_part *key)
{
    struct array_part *parts = NULL;
	
	int8_t k1 = convert_p(key->k1, map->sizearray);
	int8_t k2 = convert_p(key->k2, map->sizearray);
	int8_t k3 = convert_p(key->k3, map->sizearray);
	int8_t k4 = convert_p(key->k4, map->sizearray);
	parts = map->bigarray[k1][k2][k3][k4];
	//	if(parts->size > 0 && parts->parts[0].id <0) {
	//		printf("get_parts_bigarray error : size:%i for %i:%i:%i:%i-%i:%i:%i:%i r[0].id = %i\n",parts->size,p[0],p[1],p[2],p[3],k1,k2,k3,k4,parts->parts[0].id );
	//	}
    return parts;
}

void check_array(struct array_part *apart) {
	printf("check_array :\n");
	if(apart != NULL) {
		printf("size:%i\n",apart->size);
		for(int i=0;i <apart->size;i++) {
			struct part p=apart->parts[i];
			if(p.id < 0 || p.id > 256) {
				printf("p[%i] id false:%i\n",i,p.id);
				print_part(&p);
			}
		}
	} else {
		printf("array_part NULL\n");
	}
	printf("check_array end\n");
}

map_big_array *buildBigArray(struct array_part *apart,int maxFace)
{
	int sizeBigArray = (maxFace + 2);
	map_big_array *result = malloc(sizeof(map_big_array));
	result->sizearray = sizeBigArray;
	result->bigarray = malloc(sizeof(big_array)*sizeBigArray);
	struct array_part *****big_array = result->bigarray;

	int maxarray = 0;
	int f1;
	for(f1=-1;f1 <= maxFace;f1++)
	{
        struct array_part *arraypart1 = search_face(apart, f1, PART_TOP);
		int p1 = f1;
		if(abs(p1) != p1)
		{
			p1 = maxFace + abs(p1);
		}
		
		big_array[p1] = malloc(sizeof(struct array_part***)*sizeBigArray);
		
		int f2;
		for(f2=-1;f2 <= maxFace;f2++)
		{
			int p2 = f2;
			if(abs(p2) != p2)
			{
				p2 = maxFace + abs(p2);
			}
			big_array[p1][p2] = malloc(sizeof(struct array_part**)*sizeBigArray);
			
            struct array_part *arraypart2 = search_face(arraypart1, f2, PART_RIGHT);
//            if(f1 >=0 && f2>=0 && arraypart2->size > maxarray)
//			{
//				maxarray = arraypart2->size;
//			}
			int f3;
			for(f3=-1;f3 <= maxFace;f3++)
			{
				int p3 = f3;
				if(abs(p3) != p3)
				{
					p3 = maxFace + abs(p3);
				}
				big_array[p1][p2][p3] = malloc(sizeof(struct array_part*)*sizeBigArray);
                
                struct array_part *arraypart3 = search_face(arraypart2, f3, PART_BOTTOM);
                
				int f4;
				for(f4=-1;f4 <= maxFace;f4++)
				{
					int p4 = f4;
					if(abs(p4) != p4)
					{
						p4 = maxFace + abs(p4);
					}
					

					
                    struct array_part *arraypart = search_face(arraypart3, f4, PART_LEFT);
					if(f1 >=0 || f2>=0 || f3>=0 || f4>=0)
					{
						if(arraypart->size > maxarray)
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
	printf("max array:%i\n",maxarray);
	return result;
}

struct map_part *buildMapPart(struct array_part *apart, int maxFace)
{
	err = 0;
	struct map_part *result = malloc(sizeof *result);
	result->size = pow((maxFace +2), 4);
	// Considérant un tot de 50% de croisement du hash, on répercute sur la taille
	result->sizemap = result->size * 1.5;
	long size = (result->sizemap * sizeof(*result->elements));
    printf("taille part : %i\n", apart->size);
    printf("nb mappart : %i\n", result->sizemap);
	printf("alloc : %li\n",size);
	result->elements = calloc(result->sizemap,sizeof(*result->elements));
	int f1;
	unsigned int key_int;
    
	for(f1=-1;f1 <= maxFace;f1++)
	{
		char *c1 = malloc(MAX_KEY_LENGTH * sizeof(char));//calloc('\0', MAX_KEY_LENGTH * sizeof(char));
		sprintf(c1, "%d", f1);
		
        struct array_part *arraypart1 = search_face(apart, f1, PART_TOP);
        
		
		int f2;
		for(f2=-1;f2 <= maxFace;f2++)
		{
			char *c2 = malloc(MAX_KEY_LENGTH * sizeof(char));//calloc('\0', MAX_KEY_LENGTH * sizeof(char));
			sprintf(c2, "%s_%d",c1,f2);
			
            struct array_part *arraypart2 = search_face(arraypart1, f2, PART_RIGHT);
            
			int f3;
			for(f3=-1;f3 <= maxFace;f3++)
			{
				char *c3 = malloc(MAX_KEY_LENGTH * sizeof(char));//calloc('\0', MAX_KEY_LENGTH * sizeof(char));
				sprintf(c3, "%s_%d",c2,f3);
				
                
                struct array_part *arraypart3 = search_face(arraypart2, f3, PART_BOTTOM);
                
				int f4;
				for(f4=-1;f4 <= maxFace;f4++)
				{
					char *c4 = malloc(MAX_KEY_LENGTH * sizeof(char));//calloc('\0', MAX_KEY_LENGTH * sizeof(char));
					sprintf(c4, "%s_%d",c3,f4);
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
	printf("nb erreur :%i\n",err);
	printf("relocalisé : %i\n",relocated);
	printf("max relocate : %i\n",maxrelocate);
	return result;
}

int free_map_part(struct map_part *map_parts)
{

	int i;
	for(i = 0; i < map_parts->sizemap; i++)
	{
        free(map_parts->elements[i].key);
		if(map_parts->elements[i].apart != NULL)
		{
			free_array_part(map_parts->elements[i].apart);
		}

	}
	free(map_parts->elements);
	free(map_parts);
	return 0;
}

int free_array_part(struct array_part *array_parts)
{
    if(array_parts != NULL)
    {
        free(array_parts->parts);
    }
	free(array_parts);
	
	return 0;
}

int free_bigarray(map_big_array *array_parts)
{
	int sizeBigArray = array_parts->sizearray;
	int f1;
	for(f1=0;f1 < sizeBigArray;f1++)
	{
        
		int f2;
		for(f2=0;f2 < sizeBigArray;f2++)
		{
            
			int f3;
			for(f3=0;f3 < sizeBigArray;f3++)
			{
				int f4;
				for(f4=0;f4 < sizeBigArray;f4++)
				{
                    free_array_part(array_parts->bigarray[f1][f2][f3][f4]);
				}
                free(array_parts->bigarray[f1][f2][f3]);
				
			}
            free(array_parts->bigarray[f1][f2]);
			
		}
        free(array_parts->bigarray[f1]);
		
	}
	free(array_parts);
	return 0;
}

int free_map_in_one(struct map_in_one *map)
{
    if(map->position != NULL)
    {
        free(map->position);
    }
    if(map->quantity != NULL)
    {
        free(map->quantity);
    }
    if(map->parts != NULL)
    {
        free(map->parts);
    }
	free(map);
	return 0;
}

struct array_part *copy_array_part(struct array_part *apart)
{
    struct array_part *result = NULL;
    
    if(apart != NULL)
    {
        result = malloc(sizeof(*result));
        result->size = apart->size;
        result->parts = NULL;
        
        if(apart->size > 0)
        {
            int sizeofarray = apart->size * sizeof(struct part);
            result->parts = malloc(sizeofarray);
            int i;
            for(i=0;i<apart->size;i++)
            {
                struct part *part = &apart->parts[i];
                memcpy(&result->parts[i], part, sizeof(*part));
            }
        }
    }
    
    return result;
}

struct part* get_one_part(map_big_array *map_parts, key_part key)
{
    struct part *result = NULL;
	if(key.k1 > -2)
	{
		int8_t p[4] = {key.k1,key.k2,key.k3,key.k4};
		struct array_part *search = get_parts_bigarray(map_parts, p);
		if(search != NULL && search->size == 1)
		{
			result = &search->parts[0];
		}
	}
    
    return result;
}

map_big_array *prepare_map_part(struct array_part *rotateParts)
{
	int maxface = search_max_face(rotateParts);
	map_big_array *mapParts = buildBigArray(rotateParts, maxface);
	
	
	return mapParts;
}

struct map_in_one *regroup_map(map_big_array *map)
{
	struct map_in_one *result = malloc(sizeof(struct map_in_one));
	
	int sizeBigArray = map->sizearray;
	result->nbarrays = pow(sizeBigArray, 4);
	
	result->position = malloc(sizeof(int) * result->nbarrays);
	result->quantity = malloc(sizeof(int) * result->nbarrays);
	result->parts = malloc(sizeof(struct part) * result->nbarrays);
	int nbParts = 0;
	int dsize = sizeBigArray*sizeBigArray;
	int f1;
	for(f1=0;f1 < sizeBigArray;f1++)
	{
		int f2;
		for(f2=0;f2 < sizeBigArray;f2++)
		{
			int f3;
			for(f3=0;f3 < sizeBigArray;f3++)
			{
				int f4;
				for(f4=0;f4 < sizeBigArray;f4++)
				{
					
					int narray = f1*sizeBigArray*dsize+f2*dsize+f3*sizeBigArray+f4;
					result->quantity[narray] = 0;
					result->position[narray] = 0;
                    struct array_part *apart = map->bigarray[f1][f2][f3][f4];
					if(apart != NULL)
					{
						result->quantity[narray] = apart->size;
						result->position[narray] = nbParts;
						int p;
						for(p=0;p < apart->size;p++)
						{
							memcpy(&result->parts[nbParts + p], &apart->parts[p], sizeof(apart->parts[p]));
						}
						if(apart->size > 1024){
							printf("apart->size > 1024\n");
						}
						nbParts+=apart->size;
					}else{
						printf("apart null\n");
					}
				}
			}
		}
	}
	if(nbParts > 0)
	{
		//result->parts = realloc(result->parts, sizeof(struct part) * nbParts);
	} else {
		free(result->parts);
		result->parts = NULL;
	}
	result->nbparts = nbParts;
	printf("nbparts:%i\n",nbParts);
	
	return result;
}

int16_t idpart(int id, int8_t rotation) {
    return id + 256 * rotation;
}

