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
	size_t sizeofvalue;
} File;

/**
 * @brief Initialise une `File` vide.
 *
 * @param suite       File à initialiser.
 * @param sizeofvalue Taille en octets de chaque valeur stockée.
 */
void init_file(File *suite, size_t sizeofvalue);

/**
 * @brief Ajoute un élément en fin de la file (mode FIFO/pile).
 *
 * Copie la valeur dans un nouvel élément alloué dynamiquement.
 *
 * @param suite File cible.
 * @param value Pointeur vers la valeur à copier.
 * @return      1 si l'insertion a réussi, 0 en cas d'erreur.
 */
int put (File * suite, void *value);

/**
 * @brief Déplace un élément juste avant l'élément cible dans la file.
 *
 * @param suite   File propriétaire des deux éléments.
 * @param element Élément à déplacer.
 * @param target  Élément devant lequel insérer.
 */
void move_before(File *suite, Element *element, Element *target);

/**
 * @brief Déplace un élément juste après l'élément cible dans la file.
 *
 * @param suite   File propriétaire des deux éléments.
 * @param element Élément à déplacer.
 * @param target  Élément après lequel insérer.
 */
void move_after(File *suite, Element *element, Element *target);

/**
 * @brief Extrait et copie le dernier élément de la file (mode LIFO).
 *
 * L'élément est retiré de la file et sa valeur copiée dans `dest`.
 *
 * @param suite File source.
 * @param dest  Tampon de destination (au moins `sizeofvalue` octets).
 * @return      1 si un élément a été extrait, 0 si la file est vide.
 */
int scroll (File * suite, void *dest);

/**
 * @brief Supprime un élément de la file et libère sa mémoire.
 *
 * Recâble les pointeurs des voisins, met à jour `suite->start` / `suite->end`
 * si nécessaire, libère `element->value` et `element`, et décrémente
 * `suite->size`.
 *
 * @param suite   File contenant l'élément.
 * @param element Élément à supprimer.
 */
void file_remove_element(File *suite, Element *element);

/**
 * @brief Libère tous les éléments d'une `File` ainsi que la structure elle-même.
 * @param suite File à libérer.
 */
void free_file(File *suite);

#endif
