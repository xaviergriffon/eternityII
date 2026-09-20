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
	suite->bytes = 0;
	suite->sizeofvalue = sizeofvalue;
	suite->variable = 0;
}

void init_file_variable(File *suite){
	init_file(suite, 0);
	suite->variable = 1;
}

/* Attache `element` en queue et met les compteurs à jour. Le chaînage est le
   même pour les deux modes : seule la charge utile diffère. */
static void append_element(File *suite, Element *element){
	element->previous = NULL;
	element->next = NULL;
	if(suite->end == NULL){
		suite->start = element;
		suite->end = element;
	}else {
		suite->end->next = element;
		element->previous = suite->end;
		suite->end = element;
	}
	suite->size++;
	suite->bytes += (unsigned long long)element->len;
}

int put_sized(File *suite, const void *value, size_t len){
	if (len == 0 || len > UINT16_MAX) {
		return 0;
	}
	Element *new_element = malloc(sizeof(Element) + len);
	if (new_element == NULL) {
		return 0;
	}
	new_element->len = (uint16_t)len;
	memcpy(new_element->data, value, len);
	append_element(suite, new_element);
	return 1;
}

/* Détache `element` de la queue (LIFO) ou de la tête (FIFO) et rend sa
   charge utile dans `dest`. Facteur commun de scroll_sized/scroll_fifo_sized :
   les deux ne diffèrent que par le bout choisi. */
static int detach_and_copy(File *suite, Element *supp_element, void *dest, size_t destcap,
                           size_t *out_len){
	if (supp_element->len > destcap) {
		// Tampon trop petit : on ne tronque pas et on ne retire RIEN — une
		// possibilité à moitié copiée serait pire qu'un refus.
		return 0;
	}
	memcpy(dest, supp_element->data, supp_element->len);
	if (out_len != NULL) {
		*out_len = supp_element->len;
	}
	suite->bytes -= (unsigned long long)supp_element->len;
	suite->size--;
	free(supp_element);
	if(suite->size == 0)
	{
		suite->start = NULL;
		suite->end = NULL;
	}
	return 1;
}

int scroll_sized(File *suite, void *dest, size_t destcap, size_t *out_len){
	if (suite->size == 0 || suite->end == NULL) {
		return 0;
	}
	Element *supp_element = suite->end;
	if (supp_element->len > destcap) {
		return 0;
	}
	if(supp_element->previous != NULL)
	{
		supp_element->previous->next = NULL;
	}
	suite->end = supp_element->previous;
	return detach_and_copy(suite, supp_element, dest, destcap, out_len);
}

int scroll_fifo_sized(File *suite, void *dest, size_t destcap, size_t *out_len){
	if (suite->size == 0 || suite->start == NULL) {
		return 0;
	}
	Element *supp_element = suite->start;
	if (supp_element->len > destcap) {
		return 0;
	}
	if(supp_element->next != NULL)
	{
		supp_element->next->previous = NULL;
	}
	suite->start = supp_element->next;
	return detach_and_copy(suite, supp_element, dest, destcap, out_len);
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
	if (suite->variable || suite->sizeofvalue <= 0) {
		// Mode variable : la taille ne peut pas être devinée. Refus BRUYANT
		// plutôt qu'une valeur tronquée à `sizeofvalue` — un appelant qui a
		// oublié de passer par put_sized doit le voir tout de suite.
		return 0;
	}
	return put_sized(suite, value, suite->sizeofvalue);
}

int scroll (File * suite, void *dest){
	if (suite->variable || suite->sizeofvalue <= 0) {
		return 0;
	}
	return scroll_sized(suite, dest, suite->sizeofvalue, NULL);
}

int scroll_fifo (File * suite, void *dest){
	if (suite->variable || suite->sizeofvalue <= 0) {
		return 0;
	}
	return scroll_fifo_sized(suite, dest, suite->sizeofvalue, NULL);
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
void file_clear(File *suite) {
	Element *current = suite->start;
	while (current != NULL) {
		Element *next = current->next;
		free(current);
		current = next;
	}
	suite->start = NULL;
	suite->end = NULL;
	suite->size = 0;
	suite->bytes = 0;
}

void free_detached_element(Element *element) {
    // Une seule libération : la charge utile est allouée AVEC le maillon.
    free(element);
}

void file_remove_element(File *suite, Element *element) {
    extract_element(suite, element);
    suite->bytes -= (unsigned long long)element->len;
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
	// Plus de tampon temporaire à allouer : la charge utile part avec le
	// maillon, il n'y a rien à recopier pour libérer.
	Element *current = suite->start;
	while (current != NULL) {
		Element *next = current->next;
		free(current);
		current = next;
	}
	suite->start = NULL;
	suite->end = NULL;
	suite->size = 0;
	suite->bytes = 0;
	free(suite);
}
