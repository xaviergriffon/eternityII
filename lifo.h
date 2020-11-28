#ifndef eternityII_lifo_h
#define eternityII_lifo_h

#include <stddef.h>

typedef struct ElementList{
	void *value;
    struct ElementList *previous;
	struct ElementList *next;
} Element;

typedef struct ListeRepere{
	Element *start;
	Element *end;
	int size;
	int cacheSize;
	Element *cacheElement;
    Element * cacheEndPosition;
	long lastPostionCache;
	size_t sizeofvalue;
} File;

/* initialisation */
void init_file_with_cache(File *suite, int cacheSize, size_t sizeofvalue);

/* ENFILER*/
int put (File * suite, void *value);

/* DE_FILER*/
int scroll (File * suite, void *dest);
void *scroll_cache(File * suite);
//int scroll_fifo (File * suite, void *dest);

void free_file(File *suite);

#endif
