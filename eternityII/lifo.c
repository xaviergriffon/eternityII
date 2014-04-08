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
	suite->sizeofvalue = 0;
	suite->lastPostionCache = 0;
}

void init_file_with_cache(File *suite, int cacheSize, size_t sizeofvalue)
{
	init_file(suite);
	suite->cacheSize = cacheSize;
	if(cacheSize > 0)
	{
		suite->cacheElement = malloc(sizeof(Element) * cacheSize);
		
	}
	suite->sizeofvalue = sizeofvalue;
	int e;
	for(e = 0; e < cacheSize; e++)
	{
		suite->cacheElement[e].next = NULL;
		suite->cacheElement[e].previous = NULL;
		suite->cacheElement[e].value = malloc(sizeofvalue);
	}
}

int inside_cache(File *file, Element *element)
{
	return file->cacheElement <= element && element < file->cacheElement+file->cacheSize;
}

long position_cache(File *file, Element *element)
{
	if(!inside_cache(file, element))
	{
		return -1;
	}
	
	return element - file->cacheElement;
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
		new_element = malloc(sizeof(Element));
		if (suite->sizeofvalue <= 0 || (new_element->value = malloc(suite->sizeofvalue))
			== NULL)
		{
			free (new_element);
			return 0;
		}
	}

	new_element->previous = NULL;
	new_element->next = NULL;
	
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
	
	long position = position_cache(suite, supp_element);
	if(position > -1)
	{
		if (position == suite->lastPostionCache -1)
		{
			suite->lastPostionCache--;
		}
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

