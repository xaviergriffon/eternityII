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

typedef struct BigTable {
    void *value;
    unsigned long long size;
    unsigned long long realsize;
    size_t sizeofvalue;
    int incrementSize;
} big_table;

/**
 * @brief Initialise une `File` vide.
 *
 * @param suite       File à initialiser.
 * @param sizeofvalue Taille en octets de chaque valeur stockée.
 */
void init_file(File *suite, size_t sizeofvalue);

/**
 * @brief Initialise un tableau dynamique `big_table`.
 *
 * Alloue un bloc initial de `incrementSize` éléments. Le tableau grossit
 * automatiquement lors des insertions.
 *
 * @param table         Tableau à initialiser.
 * @param incrementSize Nombre d'éléments alloués à chaque agrandissement.
 * @param sizeofvalue   Taille en octets de chaque élément.
 */
void init_big_table(big_table *table, int incrementSize, size_t sizeofvalue);

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
 * @brief Ajoute un élément en fin du tableau dynamique, en l'agrandissant si nécessaire.
 *
 * Contrat d'échec (OOM) : **NULL = échec, table inchangée.** Si l'allocation
 * nécessaire (initiale ou d'agrandissement) échoue, la fonction retourne NULL
 * sans modifier `table` (buffer, `size` et `realsize` restent tels qu'avant
 * l'appel). Tout appelant DOIT tester le retour avant de le déréférencer ;
 * en cas de NULL, la valeur à insérer n'a pas été prise en compte.
 *
 * @param table Tableau cible.
 * @param value Pointeur vers la valeur à copier.
 * @return      Pointeur vers la copie insérée dans le tableau, ou NULL si
 *              l'allocation nécessaire a échoué (table inchangée).
 */
void *put_big_table(big_table *table, void *value);

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
 * @brief Extrait et copie le dernier élément du tableau dynamique (mode LIFO).
 *
 * Décrémente la taille logique du tableau sans réallouer la mémoire.
 *
 * @param table Tableau source.
 * @param dest  Tampon de destination (au moins `sizeofvalue` octets).
 * @return      1 si un élément a été extrait, 0 si le tableau est vide.
 */
int scroll_big_table(big_table *table, void *dest);

/**
 * @brief Extrait le dernier élément du tableau et retourne un pointeur direct vers sa valeur.
 *
 * Ne copie pas : retourne le pointeur interne. L'espace reste valide jusqu'au
 * prochain `put_big_table`.
 *
 * @param table Tableau source.
 * @return      Pointeur vers la valeur extraite, ou NULL si le tableau est vide.
 */
void *scroll_big_table_cache(big_table *table);

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

/**
 * @brief Libère la mémoire d'un `big_table` alloué sur le tas et de son buffer interne.
 * @param table Tableau à libérer.
 */
void free_big_table(big_table *table);

/**
 * @brief Libère uniquement le buffer interne d'un `big_table` alloué sur la pile.
 *
 * Contrairement à `free_big_table`, ne libère pas la structure elle-même.
 * À utiliser quand `table` est déclaré sur la pile (stack).
 *
 * @param table Tableau dont le buffer interne doit être libéré.
 */
void clear_big_table(big_table *table);

#endif
