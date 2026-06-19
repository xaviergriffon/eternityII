#ifndef eternityII_readdata_h
#define eternityII_readdata_h
#include "core/part.h"
#include "core/possibility.h"

/**
 * @brief Lit et parse le fichier CSV de définition des pièces.
 *
 * Format attendu :
 * - Première ligne : `ntiles: N`
 * - Lignes suivantes : `id top right bottom left`
 *
 * La pièce 0 (bordure) est insérée automatiquement avec tous ses bords à 0.
 *
 * @param files Chemin du fichier CSV.
 * @return      Tableau de `N+1` pièces alloué (à libérer avec `free_array_part`).
 *              Quitte le programme en cas d'erreur d'ouverture ou de parse.
 */
struct array_part *read_parts(const char *files);

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
