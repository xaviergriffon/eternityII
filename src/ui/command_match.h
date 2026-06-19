#ifndef eternityII_command_match_h
#define eternityII_command_match_h

/*
 * Appariement approximatif de commandes par distance d'édition. Logique pure
 * (aucune dépendance hors libc), extraite de command_lines.c pour être testable
 * en isolation : la liste des commandes connues est passée en paramètre plutôt
 * que référencée globalement.
 */

/**
 * @brief Distance d'édition de Levenshtein entre deux chaînes (insertions,
 *        suppressions, substitutions).
 *
 * La longueur de `b` est bornée à 62 (les commandes sont courtes), ce qui
 * plafonne la distance retournée en conséquence.
 *
 * @param a Première chaîne.
 * @param b Seconde chaîne.
 * @return  Nombre minimal d'éditions pour transformer `a` en `b`.
 */
int levenshtein(const char *a, const char *b);

/**
 * @brief Renvoie la commande connue la plus proche de `instruction`.
 *
 * Parcourt `commands` (tableau de `n` noms) et retient la plus petite distance
 * de Levenshtein. La suggestion n'est renvoyée que si elle est « suffisamment
 * proche » : au plus 1/3 de la longueur de la commande candidate en éditions,
 * borné à [1, 3]. Sinon renvoie NULL.
 *
 * @param instruction Saisie utilisateur (commande potentiellement mal tapée).
 * @param commands    Tableau des noms de commandes connues.
 * @param n           Nombre de commandes dans le tableau.
 * @return            Nom de la commande la plus proche, ou NULL si aucune ne
 *                    passe le seuil.
 */
const char *closest_command(const char *instruction, const char *const *commands, int n);

#endif /* eternityII_command_match_h */
