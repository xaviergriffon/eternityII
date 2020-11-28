#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lifo.h"

void init_file(File * suite){
	suite->start = NULL;
	suite->end = NULL;
	suite->size = 0;
	suite->cacheSize = 0;
	suite->cacheElement = NULL;
    suite->cacheEndPosition = NULL;
	suite->sizeofvalue = 0;
	suite->lastPostionCache = 0;
}

void init_file_with_cache(File *suite, int cacheSize, size_t sizeofvalue)
{
	init_file(suite);
	suite->cacheSize = cacheSize;
	if(cacheSize > 0)
	{
        size_t cacheElementSize = sizeof(Element) * cacheSize;
		suite->cacheElement = malloc(cacheElementSize);
        suite->cacheEndPosition = &suite->cacheElement[cacheSize - 1];
		
	}
	suite->sizeofvalue = sizeofvalue;
	int e;
	for(e = 0; e < cacheSize; e++)
	{
		suite->cacheElement[e].value = malloc(sizeofvalue);
		// Construction de la suite
		if(e>0) {
			suite->cacheElement[e].previous = &suite->cacheElement[e-1];
			suite->cacheElement[e-1].next = NULL;
		} else {
			suite->cacheElement[e].previous = &suite->cacheElement[e];
		}
		
		if(e == cacheSize -1 ) {
			suite->cacheElement[e].next = NULL;
		}
	}
}

int inside_cache(File *file, Element *element)
{
    // On vérifie si la position de l'élément se trouve dans la zone mémoire de cacheElemeent
	return file->cacheElement <= element && element <= file->cacheEndPosition;
}

long position_cache(File *file, Element *element)
{
	if(!inside_cache(file, element))
	{
		return -1;
	}
	
    // Utilisation des pointeurs pour calculer la position
	return file->cacheElement - element;
}

/* (ajouter) un élément dans la file */
int put (File * suite, void *value){
	Element *new_element = NULL;
	if(suite->lastPostionCache < suite->cacheSize)
	{
		
		new_element = &suite->cacheElement[suite->lastPostionCache];
		suite->lastPostionCache++;
	} else
	{
		
        if (suite->sizeofvalue <= 0) {
			free (new_element);
			return 0;
        } else {
            new_element = malloc(sizeof(Element));
            new_element->value = malloc(suite->sizeofvalue);
        }
	}

	new_element->previous = NULL;
	new_element->next = NULL;
	
	// par précaution du cache on vérifie que qu'il ne s'agit pas de la meme valeur
	memcpy (new_element->value, value, suite->sizeofvalue);
	
	if(suite->end == NULL){
		suite->start = new_element;
		suite->end = new_element;
	}else {
		suite->end->next = new_element;
		new_element->previous = suite->end;
		suite->end = new_element;
	}
	suite->size++;
	return 1;
}

int scroll (File * suite, void *dest){
	Element *supp_element;
	if (suite->size == 0 || suite->end == NULL)
		return 0;
	supp_element = suite->end;
	if(supp_element->previous != NULL)
	{
		supp_element->previous->next = NULL;
	}
	suite->end = supp_element->previous;
	void *result = supp_element->value;
	memcpy(dest, result, suite->sizeofvalue);
	
    // TODO : simplement tester si il s'agit de suite->cacheElement[suite->lastPostionCache]
    /*
    long position = position_cache(suite, supp_element);
    if(position > -1)
    {
        if (position == suite->lastPostionCache -1)
        {
            suite->lastPostionCache--;
        }
     */
    if (suite->lastPostionCache > 0 && &suite->cacheElement[suite->lastPostionCache -1] == supp_element) {
        suite->lastPostionCache--;
	} else
	{
		free (result);
		free (supp_element);
	}

	suite->size--;

	if(suite->size ==0)
	{
		suite->start = NULL;
		suite->end = NULL;
	}
	
	return 1;
}

// TODO : Revoir cette méthode
// Consomme la suite ou fournie le cache si pas d'élément dans la suite
void *scroll_cache(File * suite){
	Element *supp_element;
    // TODO : hors sécu, est-ce qu'on doit tester end ? size devrait êtr suffisant
	if (suite->size == 0 || suite->end == NULL)
		return NULL;
	supp_element = suite->end;
	if(supp_element->previous != NULL)
	{
		supp_element->previous->next = NULL;
	}
	suite->end = supp_element->previous;
	void *result = supp_element->value;

    // TODO : simplement tester si il s'agit de suite->cacheElement[suite->lastPostionCache]
    /*
	long position = position_cache(suite, supp_element);
	if(position > -1)
	{
		if (position == suite->lastPostionCache -1)
		{
			suite->lastPostionCache--;
		}
     */
    if (suite->lastPostionCache > 0 && &suite->cacheElement[suite->lastPostionCache -1] == supp_element) {
        suite->lastPostionCache--;
	} else
	{
        // TODO : SI n'est pas dans le cache, alors vider mémoire
        /*
		memcpy(cache, result, suite->sizeofvalue);
		free (result);
		free (supp_element);
		result = cache;
         */
	}
	
	suite->size--;
	
	if(suite->size ==0)
	{
		suite->start = NULL;
		suite->end = NULL;
	}
	
	return result;
}


int scroll_fifo (File * suite, void *dest){
	Element *supp_element;
	if (suite->size == 0)
		return 0;
	supp_element = suite->start;
	if(supp_element->next != NULL)
	{
		supp_element->next->previous = NULL;
	}
	suite->start = supp_element->next;
	void *result = supp_element->value;
	memcpy(dest, result, suite->sizeofvalue);
	
	if(inside_cache(suite, supp_element))
	{
		if (position_cache(suite, supp_element) == suite->lastPostionCache -1)
		{
			suite->lastPostionCache--;
		}
	} else
	{
		free (result);
		free (supp_element);
	}
	suite->size--;
	
	return 1;
}

void free_file(File *suite)
{
	void *value = malloc(suite->sizeofvalue);
	while(suite->size >0)
	{
		scroll(suite,value);
	}
	free(value);
	if(suite->cacheSize > 0)
	{
		int c;
		for(c = 0; c < suite->cacheSize;c++)
		{
			free(suite->cacheElement[c].value);
		}
		free(suite->cacheElement);
	}
	free(suite);
}

void init_big_table(big_table *table, int incrementSize, size_t sizeofvalue) {
    table->value = malloc(sizeofvalue * incrementSize);
    table->incrementSize = incrementSize;
    table->size = 0;
    table->realsize = incrementSize;
    table->lastPositionUsed = -1;
    table->sizeofvalue = sizeofvalue;
}

void *big_table_value(big_table *table, int position) {
    return ((char *)table->value + (table->lastPositionUsed * table->sizeofvalue));
}

void *put_big_table(big_table *table, void *value) {
    // Test s'il faut allouer plus de mémoire
    if (table->realsize == table->size) {
        printf("recalcul de la taille de big_table\n");
        size_t oldSize = table->realsize * table->sizeofvalue;
        table->realsize = table->realsize + table->incrementSize;
        void *newValue = malloc(table->sizeofvalue * table->realsize);
        memcpy(newValue, table->value, oldSize);
        free(table->value);
        table->value = newValue;
    }
    table->lastPositionUsed++;
    table->size++;
    void *result = big_table_value(table, table->lastPositionUsed);
    memcpy(result, value, table->sizeofvalue);
    
    return result;
}

int scroll_big_table(big_table *table, void *dest) {
    if (table->size > 0) {
        memcpy(dest, big_table_value(table, table->lastPositionUsed), table->sizeofvalue);
        table->lastPositionUsed--;
        table->size--;
        
        return 1;
    }
    
    return 0;;
}

void *scroll_big_table_cache(big_table *table) {
    void *result = NULL;
    if (table->size > 0) {
        result = big_table_value(table, table->lastPositionUsed);
        table->lastPositionUsed--;
        table->size--;
    }
    return result;
}

void free_big_table(big_table *table) {
    if (table->value != NULL) {
        free(table->value);
    }
    free(table);
}
