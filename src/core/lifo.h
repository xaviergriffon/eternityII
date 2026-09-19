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
	/** Somme des octets de CHARGE UTILE portés par les éléments de la file.
	 *
	 *  Tant que toutes les valeurs font `sizeofvalue` octets, ce compteur vaut
	 *  exactement `size * sizeofvalue` — et un test le vérifie. Il existe pour
	 *  que le plafond RAM (`--stock-max-ram`) puisse être appliqué sur des
	 *  OCTETS plutôt que sur un nombre de possibilités : un plafond exprimé en
	 *  nombre suppose une taille par possibilité constante, hypothèse qui
	 *  cesse d'être vraie dès que la file stocke des enregistrements de taille
	 *  variable.
	 *
	 *  Ne compte QUE la charge utile : le surcoût par élément (structure de
	 *  chaînage, en-têtes d'allocation) dépend de l'allocateur et se calcule
	 *  chez l'appelant (`datamanager_bytes_per_possibility`). */
	unsigned long long bytes;
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
 * @brief Extrait et copie le premier élément de la file (mode FIFO).
 *
 * Contrairement à `scroll` (dépile la QUEUE, `end` — les possibilités les
 * plus récemment ajoutées), extrait la TÊTE (`start`) : les éléments les
 * plus anciens, jamais retouchés tant que la file ne se vide pas
 * complètement. Utilisée par `core/stock_spill.c` (débordement sur disque)
 * pour évincer précisément la donnée FROIDE, jamais celle qu'un GET
 * normal (`scroll`) aurait servie en premier.
 *
 * @param suite File source.
 * @param dest  Tampon de destination (au moins `sizeofvalue` octets).
 * @return      1 si un élément a été extrait, 0 si la file est vide.
 */
int scroll_fifo (File * suite, void *dest);

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
 * @brief Libère un élément DÉJÀ détaché de sa file (chaînage retiré par
 *        l'appelant, `size` déjà ajustée).
 *
 * Existe pour que personne n'ait à savoir comment un élément range sa valeur :
 * `free(e->value); free(e);` écrit à la main devient faux dès que la valeur
 * cesse d'être un bloc séparé.
 */
void free_detached_element(Element *element);

/**
 * @brief Libère tous les éléments d'une `File` ainsi que la structure elle-même.
 * @param suite File à libérer.
 */
void free_file(File *suite);

#endif
