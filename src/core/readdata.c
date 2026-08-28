#include "core/readdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#include "ui/logger.h"
#include "app/static_variables.h"

/**
 * @brief Lit et parse le fichier CSV de définition des pièces.
 *
 * Format attendu :
 * - Première ligne : `ntiles: N`
 * - Lignes suivantes : `id top right bottom left`
 *
 * La pièce 0 (bordure) est insérée automatiquement avec tous ses bords à 0.
 *
 * @param file Chemin du fichier CSV.
 * @return     Tableau de `N+1` pièces alloué (à libérer avec `free_array_part`).
 *             Quitte le programme en cas d'erreur d'ouverture ou de parse.
 */
struct array_part *read_parts(const char *file)
{
	int np = 0;
	
	FILE *f = fopen(file, "r");
	if(!f)
	{
		log_errno("read_parts file :%s ",file);
		exit(EXIT_FAILURE);
	}
	
	if (fscanf(f, "ntiles: %d", &np) == 1)
	{
		log_info("ntiles:%i\n",np);
	} else
	{
		log_error("read_parts: format ntiles invalide dans %s\n", file);
		exit(EXIT_FAILURE);
	}


    struct part *parts = NULL;
    int NB_PARTS = (np+1);
	if(NULL == (parts = malloc(NB_PARTS * sizeof *parts)))
	{
		log_error("read_parts: malloc échoué pour %d pièces\n", NB_PARTS);
		exit(EXIT_FAILURE);
	}
    struct array_part *array = malloc(sizeof(struct array_part));
    array->size = np;
    array->parts = parts;
    
	parts[0].id = 0;
	parts[0].top = 0;
	parts[0].left = 0;
	parts[0].bottom = 0;
	parts[0].right = 0;
    parts[0].rotation = 0;
	int count = 1;
	for(;;)
	{
		int16_t id;
		int top;
		int left;
		int bottom;
		int right;
		int nread = fscanf(f, "%hd %d %d %d %d",
			  &id,
			   &top,
			   &left,
			   &bottom,
			   &right);

		if (nread == EOF)
		{
			// Fin de fichier atteinte proprement : on sort de la boucle.
			break;
		}
		if (nread != 5)
		{
			// Ligne malformée (moins de 5 champs lus).
			log_error("read_parts: ligne malformée dans %s (%d champs lus sur 5)", file, nread);
			exit(EXIT_FAILURE);
		}
		if (count > np)
		{
			// Plus de pièces que ntiles ne l'annonce : refus avant tout écrasement hors limites.
			log_error("read_parts: trop de pièces dans %s (attendu %d)", file, np);
			exit(EXIT_FAILURE);
		}

		parts[count].id=id;
		parts[count].top=(int8_t)top;
		parts[count].left=(int8_t)left;
		parts[count].bottom=(int8_t)bottom;
		parts[count].right=(int8_t)right;
		parts[count].rotation = 0;

		//print_part(&parts[count]);
		count++;
	}

	if (count != np + 1)
	{
		// Moins de pièces que ntiles ne l'annonce.
		log_error("read_parts: %d pièces lues dans %s (attendu %d)", count - 1, file, np);
		exit(EXIT_FAILURE);
	}

	fclose(f);
    return array;
}

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
struct array_index *read_indices(const char *file)
{
	int ni = 0;

	FILE *f = fopen(file, "r");
	if (!f)
	{
		log_errno("read_indices file :%s ", file);
		exit(EXIT_FAILURE);
	}

	if (fscanf(f, "nindices: %d", &ni) == 1)
	{
		log_info("nindices:%i\n", ni);
	} else
	{
		log_error("read_indices: format nindices invalide dans %s\n", file);
		exit(EXIT_FAILURE);
	}

	struct board_index *indices = NULL;
	if (ni > 0 && NULL == (indices = malloc(ni * sizeof *indices)))
	{
		log_error("read_indices: malloc échoué pour %d indices\n", ni);
		exit(EXIT_FAILURE);
	}
	struct array_index *array = malloc(sizeof(struct array_index));
	array->size = ni;
	array->indices = indices;

	int count = 0;
	for (;;)
	{
		int16_t id;
		int x;
		int y;
		int rotation;
		int mandatory;
		int nread = fscanf(f, "%hd %d %d %d %d", &id, &x, &y, &rotation, &mandatory);

		if (nread == EOF)
		{
			// Fin de fichier atteinte proprement : on sort de la boucle.
			break;
		}
		if (nread != 5)
		{
			log_error("read_indices: ligne malformée dans %s (%d champs lus sur 5)", file, nread);
			exit(EXIT_FAILURE);
		}
		if (count >= ni)
		{
			// Plus d'indices que nindices ne l'annonce : refus avant tout écrasement hors limites.
			log_error("read_indices: trop d'indices dans %s (attendu %d)", file, ni);
			exit(EXIT_FAILURE);
		}

		indices[count].id = id;
		indices[count].x = (uint8_t)x;
		indices[count].y = (uint8_t)y;
		indices[count].rotation = (uint8_t)rotation;
		indices[count].mandatory = (uint8_t)mandatory;
		count++;
	}

	if (count != ni)
	{
		// Moins d'indices que nindices ne l'annonce.
		log_error("read_indices: %d indices lus dans %s (attendu %d)", count, file, ni);
		exit(EXIT_FAILURE);
	}

	fclose(f);
	return array;
}

/**
 * @brief Libère un `struct array_index` alloué par `read_indices`.
 * @param array_indices Tableau à libérer (NULL toléré, ne fait rien).
 */
void free_array_index(struct array_index *array_indices)
{
	if (array_indices == NULL)
	{
		return;
	}
	free(array_indices->indices);
	free(array_indices);
}

/**
 * @brief Parse une chaîne JSON représentant la grille et alimente `possibility->grid`.
 *
 * Extrait tous les entiers de `str_value` (regex `(-*[0-9]+)`) et les range
 * colonne par colonne dans la grille. Met également à jour le masque `b_faceused`
 * pour chaque pièce présente (valeur ≥ 0).
 *
 * @param possibility Paquet à remplir (grid et b_faceused modifiés en sortie).
 * @param str_value   Chaîne JSON contenant les valeurs de la grille.
 */
void compute_grid(struct possibility_packet *possibility, char *str_value) {

    const char *str_regex = "(-*[0-9]+)";
    regex_t preg;
    int err = regcomp (&preg, str_regex, REG_EXTENDED);
    
    if (err == 0) {
        int match;
        size_t nmatch = 0;
        regmatch_t *pmatch = NULL;
        
        nmatch = preg.re_nsub;
        pmatch = malloc (sizeof (*pmatch) * nmatch);
        if (pmatch)
        {
            const char *cursor = str_value;
            int x = 0;
            int y = 0;
            do {
                /* analyse de la chaine */
                match = regexec (&preg, cursor, nmatch, pmatch, 0);
                
                /* vérifie si la chaine est trouvée */
                if (match == 0)
                {
                    char *value = NULL;
                    long long start = pmatch[0].rm_so;
                    long long end = pmatch[0].rm_eo;
                    size_t size = end - start;
                    
                    if (size > 0) {
                        // Par sécurité on vérifie qu'on dépasse pas le tableau
                        // fait au début car la dernière valeur relevé passera le compteur au dessus lors du y>=...
                        if (y >= ETERN_SIZE) {
                            log_error("nombre de colonne trop important");
                            break;
                        }
                        value = malloc (sizeof (*value) * (size + 1));
                        strncpy (value, &cursor[start], size);
                        value[size] = '\0';
                        int16_t rPart = atoi(value);
                        possibility->grid[x][y] = rPart;
                        if (rPart >= 0) {
                            int nPart = rPart % ETERN_PARTS;
                            set_face_used(possibility->b_faceused, nPart, 1);
                        }
                        free(value);
                        x++;
                        if (x >= ETERN_SIZE) {
                            y++;
                            x = 0;
                        }
                    }
                    
                    cursor += end ;
                    if (strlen(cursor) == 0) {
                        log_info("fin recherche");
                        break;
                    }
                    

                }
                /* chaine non retrouvée */
                else if (match == REG_NOMATCH)
                {
                    printf ("%s groupe non trouvé\n", str_value);
                    break;
                }
                /*  erreur lors de la recherche */
                else if (match != 0)
                {
                    char *text;
                    size_t size;
                    
                    /* recupération de la taille de l'erreur */
                    size = regerror (err, &preg, NULL, 0);
                    text = malloc (sizeof (*text) * size);
                    if (text)
                    {
                        /* récupération du message d'erreur */
                        regerror (err, &preg, text, size);
                        fprintf (stderr, "%s\n", text);
                        free (text);
                    }
                    break;
                }
            } while (match == 0);
            free(pmatch);
            /* libreation de l'expression compiliée */
            regfree (&preg);
        }
    } else {
        log_error("erreur compilation regest %i\n", err);
    }
}

/**
 * @brief Désérialise un `possibility_packet` depuis une chaîne JSON.
 *
 * Extrait les champs `alloc`, `x`, `y` et `grid` via une regex. La grille
 * est parsée en appelant `compute_grid`. Utilisé par la commande `loadjson`.
 *
 * @param json_possiblity Chaîne JSON au format produit par `print_possibility_packet`.
 * @return                Paquet alloué (à libérer par l'appelant), ou NULL en cas d'erreur.
 */
struct possibility_packet * read_from_json(const char *json_possiblity) {
    regex_t preg;
    const char *str_regex = "\"([^\"]*)\": ([^,{\\[]+|\\[(\\[[^[]+\\][, ]*)+\\]|\\[[^]]+\\])";
    

	struct possibility_packet *possibility = NULL;
	
    // Compilation de la regex
    int err = regcomp (&preg, str_regex, REG_EXTENDED);
    if (err == 0)
    {
        int match;
        size_t nmatch = 0;
        regmatch_t *pmatch = NULL;
        
        nmatch = preg.re_nsub;
        pmatch = malloc (sizeof (*pmatch) * nmatch);
        if (pmatch)
        {
            const char *cursor = json_possiblity;
            do {
                /* analyse de la chaine */
                match = regexec (&preg, cursor, nmatch, pmatch, 0);
                
                /* vérifie si la chaine est trouvée */
                if (match == 0)
                {
					// Initialisation de la possibility si au moins 1 match
					if (possibility == NULL) {
						possibility = malloc(sizeof(struct possibility_packet));
						// Initialisation des pieces utilisées
						for (int p = 0; p < ETERN_PARTS; p++) {
                            set_face_used(possibility->b_faceused, p, 0);
						}
					}

                    char *value = NULL;
                    long long firstEnd = pmatch[0].rm_eo;
                    char *lastValue = NULL;
                    for (size_t m = 0; m < nmatch; m++) {
                        if (lastValue != NULL) {
                            free(lastValue);
                        }
                        if (value != NULL) {
                            size_t value_size = strlen(value);
                            lastValue = malloc(sizeof(char) * (value_size + 1));
                            memcpy (lastValue, value, value_size);
                            lastValue[value_size] = '\0';
                            
                            free(value);
                        }
                        long long start = pmatch[m].rm_so;
                        long long end = pmatch[m].rm_eo;
                        size_t size = end - start;
                        
                        value = malloc (sizeof (*value) * (size + 1));
                        strncpy (value, &cursor[start], size);
                        value[size] = '\0';
                        printf ("%s\n", value);
                        if (lastValue != NULL) {
                            if (strcmp(lastValue, "alloc") == 0) {
                                possibility->alloc = atoi(value);
                            } else if (strcmp(lastValue, "x") == 0) {
                                possibility->x = atoi(value);
                            } else if (strcmp(lastValue, "y") == 0) {
                                possibility->y = atoi(value);
                            } else if (strcmp(lastValue, "grid") == 0) {
                                compute_grid(possibility, value);
                            }
                        }
                    }
                    
                    if (lastValue != NULL) {
                        free(lastValue);
                    }
                    
                    if (value != NULL) {
                        free(value);
                    }
                    
                    cursor += firstEnd + 1;
                    if (strlen(cursor) == 0) {
                        log_info("fin recherche");
                        break;
                    }
                }
                /* chaine non retrouvée */
                else if (match == REG_NOMATCH)
                {
                    printf ("%s groupe non trouvé\n", json_possiblity);
                    break;
                }
                /*  erreur lors de la recherche */
                else
                {
                    char *text;
                    size_t size;
                    
                    /* recupération de la taille de l'erreur */
                    size = regerror (err, &preg, NULL, 0);
                    text = malloc (sizeof (*text) * size);
                    if (text)
                    {
                        /* récupération du message d'erreur */
                        regerror (err, &preg, text, size);
                        fprintf (stderr, "%s\n", text);
                        free (text);
                    }
                    break;
                }
            } while (match == 0);
            free(pmatch);
            /* libreation de l'expression compiliée */
            regfree (&preg);
        }
    } else {
        log_error("erreur compilation regest %i\n", err);
    }
        
    return possibility;
}
