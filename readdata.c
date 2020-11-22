#include "readdata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#include "static_variables.h"

struct array_part *read_parts(const char *file)
{
	int np = 0;
	
	FILE *f = fopen(file, "r");
	if(!f)
	{
		printf("read_parts file :%s",file);
		perror("fopen()");
		exit(EXIT_FAILURE);
	}
	
	fscanf(f, "ntiles: %d", &np);
	printf("ntiles:%i\n",np);


    struct part *parts = NULL;
    int NB_PARTS = (np+1);
	if(NULL == (parts = malloc(NB_PARTS * sizeof *parts)))
	{
        // TODO : tracer l'erreur
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
	while(!feof(f))
	{
		int top;
		int left;
		int bottom;
		int right;
		fscanf(f, "%hd %d %d %d %d",
			  &parts[count].id,
			   &top,
			   &left,
			   &bottom,
			   &right);
		
		parts[count].top=(int8_t)top;
		parts[count].left=(int8_t)left;
		parts[count].bottom=(int8_t)bottom;
		parts[count].right=(int8_t)right;
        parts[count].rotation = 0;
		
		//print_part(&parts[count]);
		count++;
	}
	
	fclose(f);
    return array;
}

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
                            printf("nombre de colonne trop important");
                            break;
                        }
                        value = malloc (sizeof (*value) * (size + 1));
                        strncpy (value, &cursor[start], size);
                        value[size] = '\0';
                        int16_t rPart = atoi(value);
                        possibility->grid[x][y] = rPart;
                        if (rPart >= 0) {
                            int nPart = rPart % ETERN_PARTS;
                            possibility->faceused[nPart] = 1;
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
                        printf("fin recherche");
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
        printf("erreur compilation regest %i\n", err);
    }
}

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
							possibility->faceused[p] = 0;
						}
					}

                    char *value = NULL;
                    long long firstEnd = pmatch[0].rm_eo;
                    char *lastValue = NULL;
                    for (int m = 0; m < nmatch; m++) {
                        if (lastValue != NULL) {
                            free(lastValue);
                        }
                        if (value != NULL) {
                            size_t value_size = strlen(value);
                            lastValue = malloc(sizeof(char) * (value_size + 1));
                            strncpy (lastValue, value, value_size);
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
                        printf("fin recherche");
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
        printf("erreur compilation regest %i\n", err);
    }
        
    return possibility;
}
