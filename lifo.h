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
	unsigned long long size;
	unsigned long long cacheSize;
	Element *cacheElement;
    Element * cacheEndPosition;
	unsigned long long lastPostionCache;
	size_t sizeofvalue;
} File;

typedef struct BigTable {
    void *value;
    unsigned long long size;
    unsigned long long realsize;
    size_t sizeofvalue;
    int incrementSize;
} big_table;

/* initialisation */
void init_file_with_cache(File *suite, unsigned long long cacheSize, size_t sizeofvalue);
void init_big_table(big_table *table, int incrementSize, size_t sizeofvalue);

// Ajout d'une valeur dans la suite
int put (File * suite, void *value);

void *put_big_table(big_table *table, void *value);

// Positionne l'élément avant la cible
void move_before(File *suite, Element *element, Element *target);
// Positionne l'élément après la cible
void move_after(File *suite, Element *element, Element *target);

// Extrait un élément de la suite
int scroll (File * suite, void *dest);
void *scroll_cache(File * suite);
//int scroll_fifo (File * suite, void *dest);

int scroll_big_table(big_table *table, void *dest);
void *scroll_big_table_cache(big_table *table);

void free_file(File *suite);
void free_big_table(big_table *table);
void clear_big_table(big_table *table);
#endif
