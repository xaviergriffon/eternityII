#ifndef eternityII_readdata_h
#define eternityII_readdata_h
#include "core/part.h"
#include "core/possibility.h"

/**
 * @brief Lit et parse le fichier CSV de définition des pièces.
 *
 * Format attendu :
 * - Première ligne : `ntiles: N`
 * - Lignes suivantes : `id top left bottom right`
 *
 * La pièce 0 (bordure) est insérée automatiquement avec tous ses bords à 0.
 *
 * @param files Chemin du fichier CSV.
 * @return      Tableau de `N+1` pièces alloué (à libérer avec `free_array_part`).
 *              Quitte le programme en cas d'erreur d'ouverture ou de parse.
 */
struct array_part *read_parts(const char *files);

/**
 * @brief Un indice officiel du puzzle : pièce `id` posée en `(x,y)` avec la rotation `rotation`.
 *
 * `mandatory` distingue l'indice géométrique (toujours posé, indépendamment
 * d'`ETERN_WITH_INDICES`) des indices de coin (posés seulement si
 * `ETERN_WITH_INDICES` est actif). Voir `first_possibility` (possibility.c).
 */
struct board_index
{
	int16_t id;
	uint8_t x;
	uint8_t y;
	uint8_t rotation;
	uint8_t mandatory;
};

struct array_index
{
	int size;
	struct board_index *indices;
};

/**
 * @brief Lit et parse le fichier CSV de définition des indices officiels.
 *
 * Format attendu :
 * - Première ligne : `nindices: N`
 * - Lignes suivantes : `id x y rotation mandatory`
 *
 * @param file Chemin du fichier CSV.
 * @return     Tableau de `N` indices alloué (à libérer avec `free_array_index`).
 *             Quitte le programme en cas d'erreur d'ouverture ou de parse.
 */
struct array_index *read_indices(const char *file);

/**
 * @brief Libère un `struct array_index` alloué par `read_indices`.
 * @param array_indices Tableau à libérer (NULL toléré, ne fait rien).
 */
void free_array_index(struct array_index *array_indices);

/**
 * @brief Reconstruit un `possibility_packet` depuis une chaîne JSON.
 *
 * Parse la chaîne JSON représentant l'état de la grille et alimente
 * le champ `grid` ainsi que le masque `b_faceused` du paquet résultant.
 *
 * @param json_possiblity Chaîne JSON contenant les valeurs de la grille.
 * @return                Paquet alloué (à libérer par l'appelant), ou NULL si le parse échoue.
 */
struct possibility_packet * read_from_json(const char *json_possiblity);

#endif
