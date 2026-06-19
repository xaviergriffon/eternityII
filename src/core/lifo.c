#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/lifo.h"

/**
 * @brief Initialise une `File` à l'état vide sans cache.
 * @param suite File à initialiser.
 */
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

/**
 * @brief Initialise une `File` avec un cache pré-alloué d'éléments.
 *
 * Les `cacheSize` premiers éléments insérés réutilisent le bloc mémoire pré-alloué,
 * évitant autant d'appels `malloc` au démarrage. Une fois le cache épuisé, les
 * éléments suivants sont alloués dynamiquement.
 *
 * @param suite       File à initialiser.
 * @param cacheSize   Nombre d'éléments pré-alloués dans le cache (0 = aucun cache).
 * @param sizeofvalue Taille en octets de chaque valeur stockée.
 */
void init_file_with_cache(File *suite, unsigned long long cacheSize, size_t sizeofvalue)
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
	unsigned long long e;
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

/**
 * @brief Indique si un élément appartient au bloc de cache pré-alloué de la file.
 * @param file    File propriétaire du cache.
 * @param element Élément à tester.
 * @return        1 si l'élément est dans le cache, 0 sinon.
 */
int inside_cache(File *file, Element *element)
{
    // On vérifie si la position de l'élément se trouve dans la zone mémoire de cacheElemeent
	return file->cacheElement <= element && element <= file->cacheEndPosition;
}

/**
 * @brief Retourne la position d'un élément dans le cache, ou -1 s'il n'en fait pas partie.
 * @param file    File propriétaire du cache.
 * @param element Élément à localiser.
 * @return        Position dans le cache (≥ 0), ou -1 si hors cache.
 */
long position_cache(File *file, Element *element)
{
	if(!inside_cache(file, element))
	{
		return -1;
	}
	
    // Utilisation des pointeurs pour calculer la position
	return file->cacheElement - element;
}

/**
 * @brief Ajoute un élément en fin de file (mode FIFO).
 *
 * Copie `sizeofvalue` octets depuis `value` dans un nouvel élément. Réutilise
 * le cache pré-alloué si possible, sinon alloue dynamiquement.
 *
 * @param suite File cible.
 * @param value Pointeur vers la valeur à copier.
 * @return      1 en cas de succès, 0 si la file n'est pas initialisée.
 */
int put (File * suite, void *value){
	Element *new_element = NULL;
	if(suite->lastPostionCache < suite->cacheSize)
	{
		
		new_element = &suite->cacheElement[suite->lastPostionCache];
		suite->lastPostionCache++;
	} else
	{
		// TODO : voir pour éviter de faire dans le put
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

/**
 * @brief Extrait et copie le dernier élément de la file (mode LIFO).
 *
 * L'élément est retiré de la file et sa valeur copiée dans `dest`.
 * La mémoire de l'élément est libérée (sauf s'il appartient au cache).
 *
 * @param suite File source.
 * @param dest  Tampon de destination (doit avoir au moins `sizeofvalue` octets).
 * @return      1 si un élément a été extrait, 0 si la file est vide.
 */
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

/**
 * @brief Extrait le dernier élément de la file et retourne un pointeur direct vers sa valeur.
 *
 * Contrairement à `scroll`, ne copie pas la valeur : retourne le pointeur interne.
 * L'appelant ne doit pas libérer ce pointeur s'il appartient au cache.
 *
 * @param suite File source.
 * @return      Pointeur vers la valeur de l'élément extrait, ou NULL si la file est vide.
 */
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


/**
 * @brief Extrait et copie le premier élément de la file (mode FIFO).
 *
 * @param suite File source.
 * @param dest  Tampon de destination.
 * @return      1 si un élément a été extrait, 0 si la file est vide.
 */
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
		if ((unsigned long long)position_cache(suite, supp_element) == suite->lastPostionCache -1)
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

/**
 * @brief Détache un élément de sa position dans la liste chaînée sans le libérer.
 *
 * Met à jour les pointeurs `previous` et `next` des voisins, ainsi que `start`
 * et `end` de la file si l'élément était en tête ou en queue.
 * Ne décrémente pas `suite->size`.
 *
 * @param suite   File contenant l'élément (peut être NULL si la file n'est pas connue).
 * @param element Élément à détacher.
 */
void extract_element(File *suite, Element *element) {
	Element *previous = element->previous;
	Element *next = element->next;

	if (previous != NULL) {
		previous->next = next;
	} else {
		// L'élément était le 1er
		if (suite != NULL && suite->start == element) {
			suite->start = next;
		}
	}

	if (next != NULL) {
		next->previous = previous;
	} else {
		// L'élément était le dernier
		if (suite != NULL && suite->end == element) {
			suite->end = previous;
		}
	}
    
    element->previous = NULL;
    element->next = NULL;
}

/**
 * @brief Déplace un élément juste avant un élément cible dans la liste.
 *
 * Extrait `element` de sa position actuelle puis l'insère immédiatement avant `target`.
 * Met à jour `suite->start` si nécessaire.
 *
 * @param suite   File contenant les deux éléments.
 * @param element Élément à déplacer.
 * @param target  Élément devant lequel insérer.
 */
void move_before(File *suite, Element *element, Element *target) {
	if(element != NULL && target != NULL) {
		// On extrait l'élément de ça position actuelle
		extract_element(suite, element);
		
		Element *targetPrevious = target->previous;
		element->previous = targetPrevious;
		if (targetPrevious != NULL) {
			targetPrevious->next = element;
		} else {
			// La cible est la 1ère
			if (suite != NULL && suite->start == target) {
				suite->start = element;
			}
		}

		// On place l'élément avant
		element->next = target;
		target->previous = element;
	}
}

/**
 * @brief Déplace un élément juste après un élément cible dans la liste.
 *
 * Extrait `element` de sa position actuelle puis l'insère immédiatement après `target`.
 * Met à jour `suite->end` si nécessaire.
 *
 * @param suite   File contenant les deux éléments.
 * @param element Élément à déplacer.
 * @param target  Élément après lequel insérer.
 */
void move_after(File *suite, Element *element, Element *target) {
	if(element != NULL && target != NULL) {
		// On extrait l'élément de ça position actuelle
		extract_element(suite, element);

		Element *targetNext = target->next;
		element->next = targetNext;
		if (targetNext != NULL) {
			targetNext->previous = element;
		} else {
			// La cible est dernière
			if (suite != NULL && suite->end == target) {
				suite->end = element;
			}
		}

		// On place l'élément après
		element->previous = target;
		target->next = element;
	}
}

/**
 * @brief Vide et libère complètement une `File` et son cache.
 *
 * Extrait tous les éléments restants, libère le cache pré-alloué si présent,
 * puis libère la structure `File` elle-même.
 *
 * @param suite File à libérer.
 */
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
		unsigned long long c;
		for(c = 0; c < suite->cacheSize;c++)
		{
			free(suite->cacheElement[c].value);
		}
		free(suite->cacheElement);
	}
	free(suite);
}

/**
 * @brief Initialise un tableau dynamique `big_table`.
 *
 * Alloue un bloc initial de `incrementSize` éléments. Le tableau grossit
 * automatiquement par incréments de `incrementSize` lors des insertions.
 *
 * @param table         Tableau à initialiser.
 * @param incrementSize Nombre d'éléments alloués à chaque agrandissement.
 * @param sizeofvalue   Taille en octets de chaque élément.
 */
void init_big_table(big_table *table, int incrementSize, size_t sizeofvalue) {
    table->value = malloc(incrementSize * sizeofvalue);
    table->incrementSize = incrementSize;
    table->size = 0;
    table->realsize = incrementSize;
    table->sizeofvalue = sizeofvalue;
}

/**
 * @brief Retourne un pointeur vers l'élément à la position donnée dans le tableau.
 * @param table    Tableau source.
 * @param position Indice (base 0) de l'élément.
 * @return         Pointeur vers l'élément.
 */
void *big_table_value(big_table *table, unsigned long long position) {
    return ((void *)table->value + (position * table->sizeofvalue));
}

/**
 * @brief Ajoute un élément en fin du tableau dynamique, en l'agrandissant si nécessaire.
 *
 * Si le tableau est plein, réalloue un nouveau bloc copiant le contenu précédent,
 * puis libère l'ancien bloc. L'agrandissement se fait par `incrementSize` éléments.
 *
 * @param table Tableau cible.
 * @param value Pointeur vers la valeur à copier.
 * @return      Pointeur vers la copie insérée dans le tableau.
 */
void *put_big_table(big_table *table, void *value) {
    // Test s'il faut allouer plus de mémoire
	void *oldTableValue = table->value;
    if (table->realsize == table->size) {
        size_t oldSize = table->realsize * table->sizeofvalue;
        table->realsize *= 2;
        void *newValue = malloc(table->sizeofvalue * table->realsize);
        memcpy(newValue, table->value, oldSize);
		table->value = newValue;
    }
    
    void *result = big_table_value(table, table->size);
    table->size++;
    memcpy(result, value, table->sizeofvalue);
	// Si la table a été redimensionnée, on supprime l'ancienne
	// Ceci est fait en dernier pour gérer le cas d'une valeur réinserrée et donc présente
	// dans l'ancienne table
	if (oldTableValue != table->value) {
		free(oldTableValue);
	}
    return result;
}

/**
 * @brief Extrait et copie le dernier élément du tableau (mode LIFO).
 *
 * Décrémente la taille logique du tableau sans réallouer la mémoire.
 *
 * @param table Tableau source.
 * @param dest  Tampon de destination (au moins `sizeofvalue` octets).
 * @return      1 si un élément a été extrait, 0 si le tableau est vide.
 */
int scroll_big_table(big_table *table, void *dest) {
    if (table->size > 0) {
        table->size--;
        memcpy(dest, big_table_value(table, table->size), table->sizeofvalue);
        
        return 1;
    }
    
    return 0;;
}

/**
 * @brief Extrait le dernier élément du tableau et retourne un pointeur direct vers sa valeur.
 *
 * Contrairement à `scroll_big_table`, ne copie pas : retourne le pointeur interne.
 * L'espace mémoire reste valide jusqu'au prochain `put_big_table`.
 *
 * @param table Tableau source.
 * @return      Pointeur vers la valeur extraite, ou NULL si le tableau est vide.
 */
void *scroll_big_table_cache(big_table *table) {
    if (table->size > 0) {
        table->size--;
        return big_table_value(table, table->size);
    }
    return NULL;
}

/**
 * @brief Libère la mémoire d'un `big_table` et de son contenu.
 * @param table Tableau à libérer.
 */
void free_big_table(big_table *table) {
    if (table->value != NULL) {
        free(table->value);
    }
    free(table);
}

/**
 * @brief Libère uniquement le buffer interne d'un `big_table` alloué sur la pile.
 *
 * Contrairement à `free_big_table`, ne libère pas la structure elle-même.
 * À utiliser quand `table` est alloué sur la pile (stack).
 *
 * @param table Tableau dont le buffer interne doit être libéré.
 */
void clear_big_table(big_table *table) {
    if (table->value != NULL) {
        free(table->value);
        table->value = NULL;
    }
}
