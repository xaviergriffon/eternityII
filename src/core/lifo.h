#ifndef eternityII_lifo_h
#define eternityII_lifo_h

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Maillon d'une `File` — la charge utile est allouée AVEC le maillon.
 *
 * Auparavant `void *value` pointait vers un bloc séparé : deux allocations par
 * élément, deux en-têtes d'allocateur, et une taille de charge utile
 * nécessairement uniforme (`File.sizeofvalue`). Le tableau souple `data[]`
 * supprime la seconde allocation ET autorise des charges utiles de TAILLES
 * DIFFÉRENTES dans une même file — ce dont dépend le stockage compact du stock
 * (`core/packet_codec.h`), où une possibilité pèse 65 octets en moyenne au lieu
 * de 576.
 *
 * Conséquence volontaire : il n'existe plus de pointeur `value` à
 * déréférencer. Tout code qui lisait le contenu d'un élément passe désormais
 * par `data`/`len`, et le compilateur a dû le signaler partout.
 */
typedef struct ElementList{
    struct ElementList *previous;
	struct ElementList *next;
	/** Octets utiles dans `data`. */
	uint16_t len;
	/** Charge utile, en place. */
	uint8_t data[];
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
	/** Taille d'une valeur en mode UNIFORME. 0 en mode variable. */
	size_t sizeofvalue;
	/** 1 = les valeurs ont des tailles DIFFÉRENTES : `put`/`scroll` (qui
	 *  supposent `sizeofvalue`) y sont refusés, il faut `put_sized`/
	 *  `scroll_sized`. Un refus bruyant plutôt qu'une lecture silencieusement
	 *  tronquée : c'est le garde-fou qui rend sûr le mélange, dans un même
	 *  fichier, de files de stock (variables) et de files de travail
	 *  temporaires (uniformes). */
	int variable;
} File;

/**
 * @brief Initialise une `File` vide.
 *
 * @param suite       File à initialiser.
 * @param sizeofvalue Taille en octets de chaque valeur stockée.
 */
void init_file(File *suite, size_t sizeofvalue);

/**
 * @brief Initialise une file dont les valeurs ont des TAILLES DIFFÉRENTES.
 *
 * `put`/`scroll`/`scroll_fifo` y sont refusés (ils supposent une taille
 * uniforme) : utiliser `put_sized`/`scroll_sized`/`scroll_fifo_sized`.
 */
void init_file_variable(File *suite);

/**
 * @brief Vide une file de tous ses éléments, sans rien recopier.
 *
 * La `File` reste utilisable (mode et `sizeofvalue` conservés) — contrairement
 * à `free_file`, qui libère aussi la structure.
 *
 * Remplace le motif `while (size > 0) scroll(suite, &jetable);`, qui payait une
 * copie par élément pour jeter le résultat — et qui, en mode variable, ne
 * terminait pas : `scroll` y refuse de travailler, donc la boucle tournait
 * indéfiniment sur une file jamais vidée.
 */
void file_clear(File *suite);

/**
 * @brief Ajoute une valeur de `len` octets en fin de file.
 * @return 1 si insérée, 0 sur échec d'allocation ou `len` nul/hors bornes.
 */
int put_sized(File *suite, const void *value, size_t len);

/**
 * @brief Dépile la QUEUE (LIFO) en copiant jusqu'à `destcap` octets.
 * @param out_len Reçoit la taille réelle de la valeur (peut être NULL).
 * @return 1 si extraite, 0 si la file est vide ou si `destcap` est trop petit
 *         (l'élément reste alors en place : jamais de perte silencieuse).
 */
int scroll_sized(File *suite, void *dest, size_t destcap, size_t *out_len);

/// Pendant FIFO de `scroll_sized` (extrait la TÊTE).
int scroll_fifo_sized(File *suite, void *dest, size_t destcap, size_t *out_len);

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
