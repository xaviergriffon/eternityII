#ifndef eternityII_possibility_h
#define eternityII_possibility_h

#include <stdint.h>

#include "core/part.h"
#include "core/lifo.h"
#include "core/packed.h"
#include "app/static_variables.h"

#define DIR_UP 1
#define DIR_RIGHT 2
#define DIR_DOWN 3
#define DIR_LEFT 4
 
 struct possibility_packet
 {
 uint8_t x;
 uint8_t y;
 int16_t grid[ETERN_SIZE][ETERN_SIZE];
 uint16_t alloc;
 uint16_t b_faceused[FACES_USED_SIZE] __attribute__ ((aligned (16)));
 /// 1 si un client pruner a vérifié que toutes les cases vides ont encore au
 /// moins une pièce candidate. Remis à 0 sur tout paquet issu d'une expansion
 /// (le contrôle ne vaut que pour l'état exact du plateau).
 uint8_t checked;
 } __attribute__((__packed__));

typedef struct
{
	int size;
	struct possibility_packet *possibilities;
} array_possibility_packet;

/**
 * @brief Marque ou démarque une pièce comme utilisée dans le masque de bits.
 *
 * Stocke l'information sous forme de bitmask compact (1 bit par pièce, groupes de 16).
 * Inline : appelée pour chaque candidat de la boucle chaude de recherche.
 *
 * @param faceused Masque de bits des pièces utilisées (tableau de FACES_USED_SIZE uint16_t).
 * @param part     Identifiant de la pièce (base 0, i.e. id-1).
 * @param boolean  1 = utilisée, 0 = libre.
 */
static inline void set_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part, uint8_t boolean) {
    uint16_t groupe = part >> 4;
    uint16_t number = faceused[groupe];
    int8_t n = part - (groupe << 4);
    number = (number & ~(1 << n)) | (boolean << n);
    faceused[groupe] = number;
}

/**
 * @brief Indique si une pièce est marquée comme utilisée dans le masque de bits.
 *
 * Inline : appelée pour chaque candidat de la boucle chaude de recherche.
 *
 * @param faceused Masque de bits des pièces utilisées.
 * @param part     Identifiant de la pièce (base 0).
 * @return         1 si utilisée, 0 si libre.
 */
static inline uint8_t is_face_used(uint16_t faceused[FACES_USED_SIZE], uint16_t part) {
    uint16_t groupe = part >> 4;
    return (faceused[groupe] >> (part - (groupe << 4))) & 1;
}
/**
 * @brief Crée un `possibility_packet` représentant l'état actuel de la grille.
 *
 * Encode chaque pièce placée dans `etern` sous la forme d'un indice de rotation
 * (`id_for_rotated_part`) et construit le masque des pièces utilisées.
 * Les cases vides sont encodées à -2.
 *
 * @param x         Coordonnée x de la prochaine case à remplir.
 * @param y         Coordonnée y de la prochaine case à remplir.
 * @param etern     Grille 2D de pointeurs vers les pièces placées (NULL = vide).
 * @param directory Direction de parcours courante (constante DIR_*).
 * @return          Paquet alloué représentant cet état (à libérer par l'appelant).
 */
struct possibility_packet *generate_possibility_packet(int x, int y, struct part *etern[ETERN_SIZE][ETERN_SIZE], int directory);

/**
 * @brief Calcule la clé de recherche pour la case courante (retour par valeur).
 *
 * Détermine les contraintes de bord de la case `(x, y)` à partir de l'état
 * de la grille. Les voisins absents valent -1 (toute couleur), les bords du
 * plateau valent 0 (bordure).
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param x                Coordonnée x de la case.
 * @param y                Coordonnée y de la case.
 * @param possiblity       Paquet courant (grille et pièces utilisées).
 * @return                 Clé de recherche (top, right, bottom, left).
 */
key_part what_search(struct array_part *all_rotate_parts, int x, int y, struct possibility_packet *possiblity);

/**
 * @brief Calcule la clé de recherche pour la case courante (version sortie pointeur, sans all_face).
 *
 * Identique à `what_search_to_key2` mais utilise -1 pour les voisins absents.
 * Destinée à la map par hachage textuel.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant.
 * @param key              Clé résultante (sortie).
 */
void what_search_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key);

/**
 * @brief Calcule la clé de recherche pour la case courante (version sortie pointeur, avec all_face).
 *
 * Utilise `all_face` (= `sizearrayM` de la map) pour encoder les voisins absents,
 * permettant un accès direct dans le tableau 4D plat.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant.
 * @param key              Clé résultante (sortie).
 * @param all_face         Valeur à utiliser pour un voisin absent (= index « toute couleur »).
 */
void what_search_to_key2(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key, int8_t all_face);

/**
 * @brief Calcule la clé de recherche pour la case `(x, y)` arbitraire dans la grille.
 *
 * Variante de `what_search_to_key2` permettant de tester une case quelconque
 * (pas nécessairement la case courante du paquet).
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant.
 * @param x                Coordonnée x de la case à tester.
 * @param y                Coordonnée y de la case à tester.
 * @param key              Clé résultante (sortie).
 * @param all_face         Valeur à utiliser pour un voisin absent.
 */
void what_search_in_grid_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, int8_t x, int8_t y, key_part *key, int8_t all_face);

/**
 * @brief Affiche et sauvegarde une solution complète (sans quitter le processus).
 *
 * Émet l'événement « SOLUTION FOUND », journalise la grille et écrit
 * `./solution_<pid>`. L'appelant garde la main (notification serveur, arrêt…).
 *
 * @param poss            Paquet solution (toutes les pièces placées).
 * @param all_rotate_part Tableau de toutes les rotations (pour l'affichage).
 */
void log_solution(struct possibility_packet *poss, struct array_part *all_rotate_part);

/**
 * @brief Détecte si la grille est complète et sauvegarde la solution.
 *
 * Si `poss->alloc >= ETERN_PARTS`, affiche et sérialise la solution dans
 * `./solution_<pid>` puis termine le processus avec `exit(EXIT_SUCCESS)`.
 *
 * @param poss            Paquet courant à tester.
 * @param all_rotate_part Tableau de toutes les rotations (pour l'affichage).
 */
void checkIfResultFound(struct possibility_packet *poss, struct array_part *all_rotate_part);

/**
 * @brief Indique si la case courante du paquet a au moins une pièce posable.
 *
 * Vérifie uniquement la case `(possibility->x, possibility->y)`.
 *
 * @param possibility     Paquet à tester.
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return                1 si au moins une pièce disponible est posable, 0 sinon.
 */
int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);

/**
 * @brief Vérifie que toutes les cases encore libres ont au moins une pièce posable.
 *
 * Parcourt les cases de `possibility->alloc` jusqu'à ETERN_PARTS. Si une case
 * n'admet aucune pièce, retourne 0 (impasse). Optimisation : si une case n'admet
 * qu'une seule pièce, la place immédiatement dans le paquet.
 *
 * @param possibility     Paquet à analyser (peut être modifié).
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return                1 si toutes les cases libres ont au moins une suite, 0 sinon.
 */
int possibility_all_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);

/**
 * @brief Forward-checking sur les `FORWARD_CHECK_K` prochaines cases.
 *
 * Après avoir sélectionné une pièce candidate, vérifie que les prochaines
 * cases de parcours ont encore au moins un candidat disponible. Si l'une est
 * morte, la branche est abandonnée.
 *
 * @param possibility     Paquet courant (après placement de la pièce candidate).
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return                1 si toutes les cases dans la fenêtre ont un candidat, 0 sinon.
 */
int forward_check_next_k(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part);

/**
 * @brief Développe un paquet en générant tous les successeurs valides (version basique).
 *
 * Pour chaque pièce compatible avec la case courante, crée une copie du paquet
 * avec la pièce placée et l'ajoute dans `result` (une `File`).
 *
 * @param result          File de destination des nouveaux paquets.
 * @param possiblity      Paquet source à développer.
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return                Nombre de successeurs générés.
 */
int search_possiblity(File *result, struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part);

/**
 * @brief Développe un paquet en générant tous les successeurs valides (version optimisée, sortie `File`).
 *
 * Variante de `search_possiblity` utilisant une clé pré-calculée et une table
 * d'indices de rotation pour éviter des recalculs dans la boucle chaude.
 *
 * @param result          File de destination des nouveaux paquets.
 * @param key             Clé de recherche pour la case courante (pré-calculée).
 * @param possiblity      Paquet source à développer.
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param idParts         Table de pré-calcul des indices de rotation [id][rotation].
 * @return                Nombre de pièces dans le meilleur paquet produit, ou 0 si aucune.
 */
int search_possiblity_light(File *result, key_part *key, struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part, int16_t idParts[ETERN_PARTS][4]);

/**
 * @brief Développe un paquet en générant tous les successeurs valides (version optimisée, sortie `big_table`).
 *
 * Identique à `search_possiblity_light` mais utilise un `big_table` comme
 * tampon de résultat pour réduire les allocations dynamiques.
 *
 * @param result          Tableau dynamique de destination.
 * @param key             Clé de recherche pour la case courante (pré-calculée).
 * @param possiblity      Paquet source à développer.
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param idParts         Table de pré-calcul des indices de rotation [id][rotation].
 * @return                Nombre de pièces dans le meilleur paquet produit, ou 0 si aucune.
 */
int search_possiblity_light_with_big_table(big_table *result, key_part *key, struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part, int16_t idParts[ETERN_PARTS][4]);

/**
 * @brief Affiche un `possibility_packet` au format JSON dans les logs.
 *
 * Format : `{"alloc": N, "x": X, "y": Y, "grid": [[...], ...]}`.
 *
 * @param packet Paquet à afficher.
 * @return       0.
 */
int print_possibility_packet(struct possibility_packet *packet);

/**
 * @brief Sérialise un `possibility_packet` dans un fichier binaire.
 *
 * @param filename    Chemin du fichier de destination.
 * @param possibility Paquet à sauvegarder.
 * @return            0 en cas de succès.
 */
int save_possibility(char *filename, struct possibility_packet *possibility);

/**
 * @brief Sérialise une solution dans un fichier CSV lisible.
 *
 * En-tête : `row,col,piece_id,rotation,top,right,bottom,left`.
 * Si `all_rotate_part` est NULL, les quatre colonnes de couleur de bord
 * sont écrites comme `-1`.
 *
 * @param filename        Chemin du fichier de destination.
 * @param poss            Paquet solution.
 * @param all_rotate_part Tableau de toutes les rotations, ou NULL.
 * @return                0 en cas de succès, -1 en cas d'erreur.
 */
int save_solution_csv(const char *filename, const struct possibility_packet *poss,
                      const struct array_part *all_rotate_part);

/**
 * @brief Génère et enregistre le paquet genèse (état initial du puzzle).
 *
 * Place les indices fixes officiels (pièce 139 r2 en (7,8), etc.) et crée
 * le premier `possibility_packet` à la case `directions[0]`.
 *
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 */
void first_possibility(map_big_array *mapParts, struct array_part *all_rotate_part);

/**
 * @brief Valide la cohérence d'un `possibility_packet`.
 *
 * Codes de retour :
 *  - 0  : OK
 *  - -1 : paquet NULL
 *  - -2 : x ou y ≥ ETERN_SIZE
 *  - -3 : direction hors bornes
 *  - -4 : alloc > ETERN_PARTS (alloc = 0 est l'état genèse, valide)
 *  - -5 : alloc incohérent avec le masque faceused
 *
 * @param packet      Paquet à vérifier.
 * @param rotateParts Tableau de toutes les rotations (peut être NULL pour un contrôle partiel).
 * @return            0 si valide, code d'erreur négatif sinon.
 */
int check_possibility(struct possibility_packet *packet, struct array_part *rotateParts);

/**
 * @brief Corrige un paquet dont le curseur `(x, y)` / `alloc` est désynchronisé.
 *
 * Recule `alloc` sur la première case vide du parcours et y repositionne `(x, y)`.
 *
 * @param packet Paquet à normaliser (modifié en place).
 * @return       0 si le paquet était déjà conforme, 1 s'il a été corrigé.
 */
int normalize_possibility_packet(struct possibility_packet *packet);

/**
 * @brief Vérifie que le tableau `directions` couvre bien toutes les cases de la grille.
 *
 * Chaque index 0..ETERN_PARTS−1 doit apparaître exactement une fois.
 *
 * @return 0 si le tableau est valide, -1 si une case est manquante.
 */
int test_directions(void);

/**
 * @brief Affiche dans les logs les coordonnées (x, y) encodées dans `directions`.
 * @return 0.
 */
int decode_direction(void);

/**
 * @brief Compare deux paquets champ par champ.
 *
 * @param packet       Premier paquet.
 * @param other_packet Second paquet.
 * @return  0 si identiques, sinon un code négatif indiquant la première différence :
 *          -1 nullité différente, -2 alloc diffèrent, -3 position (x,y) différente,
 *          -4 masque de pièces utilisées différent, -5 grille différente.
 */
int compare_possibility(struct possibility_packet *packet, struct possibility_packet *other_packet);

/**
 * @brief Indique si `packet` est un préfixe (ancêtre) de `other_packet`.
 *
 * Vérifie que toutes les pièces placées dans `packet` (jusqu'à `alloc`)
 * sont identiques à celles d'`other_packet` aux mêmes positions.
 *
 * @param packet       Paquet supposément ancêtre (alloc inférieur).
 * @param other_packet Paquet supposément descendant.
 * @return  1 si ancêtre confirmé, -1 si alloc ≥, -2 si grilles divergent, 0 si NULL.
 */
int is_origin_of(struct possibility_packet *packet, struct possibility_packet *other_packet);

/**
 * @brief Emballe un unique `possibility_packet` dans un `array_possibility_packet`.
 *
 * Copie le paquet dans un tableau de taille 1, format attendu par `add_possibility`.
 *
 * @param possibility Paquet à emballer (NULL → tableau de taille 0).
 * @return            `array_possibility_packet` alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *build_single_array_possibility_packet(struct possibility_packet *possibility);

/**
 * @brief Libère un `array_possibility_packet` et son tableau de paquets interne.
 * @param possibilities Structure à libérer.
 */
void free_array_possibility_packet(array_possibility_packet *possibilities);
#endif
