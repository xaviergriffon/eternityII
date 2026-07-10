#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/lifo.h"
#include "ui/logger.h"

/**
 * @brief Initialise une `File` vide.
 * @param suite       File à initialiser.
 * @param sizeofvalue Taille en octets de chaque valeur stockée.
 */
void init_file(File *suite, size_t sizeofvalue){
	suite->start = NULL;
	suite->end = NULL;
	suite->size = 0;
	suite->sizeofvalue = sizeofvalue;
}

/**
 * @brief Ajoute un élément en fin de file (mode FIFO).
 *
 * Copie `sizeofvalue` octets depuis `value` dans un nouvel élément alloué
 * dynamiquement.
 *
 * @param suite File cible.
 * @param value Pointeur vers la valeur à copier.
 * @return      1 en cas de succès, 0 si la file n'est pas initialisée.
 */
int put (File * suite, void *value){
	if (suite->sizeofvalue <= 0) {
		return 0;
	}
	Element *new_element = malloc(sizeof(Element));
	if (new_element == NULL) {
		return 0;
	}
	new_element->value = malloc(suite->sizeofvalue);
	if (new_element->value == NULL) {
		free(new_element);
		return 0;
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

	free (result);
	free (supp_element);

	suite->size--;

	if(suite->size ==0)
	{
		suite->start = NULL;
		suite->end = NULL;
	}
	
	return 1;
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

	free (result);
	free (supp_element);
	suite->size--;

	if(suite->size == 0)
	{
		suite->start = NULL;
		suite->end = NULL;
	}

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
 * @brief Supprime un élément de la file et libère sa mémoire si hors cache.
 *
 * @param suite   File contenant l'élément.
 * @param element Élément à supprimer.
 */
void file_remove_element(File *suite, Element *element) {
    extract_element(suite, element);
    free(element->value);
    free(element);
    suite->size--;
}

/**
 * @brief Vide et libère complètement une `File` ainsi que la structure elle-même.
 *
 * Extrait et libère tous les éléments restants, puis libère la `File`.
 *
 * Si le tampon temporaire utilisé pour dépiler (`scroll`) ne peut être alloué
 * (OOM), la `File` et ses éléments restants sont tout de même libérés (fuite
 * évitée), simplement sans passer par `scroll`.
 *
 * @param suite File à libérer.
 */
void free_file(File *suite)
{
	void *value = malloc(suite->sizeofvalue);
	if (value == NULL) {
		log_error("free_file: malloc a échoué (sizeofvalue=%zu) — libération directe des éléments\n",
		          suite->sizeofvalue);
		Element *current = suite->start;
		while (current != NULL) {
			Element *next = current->next;
			free(current->value);
			free(current);
			current = next;
		}
		free(suite);
		return;
	}
	while(suite->size >0)
	{
		scroll(suite,value);
	}
	free(value);
	free(suite);
}
