#include "core/possibility.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/logger.h"
#include "core/core_static_variables.h"
#include "core/datamanager.h"
#include "core/readdata.h"
#include "core/best_board.h"

// set_face_used / is_face_used : désormais static inline dans possibility.h
// (appelées pour chaque candidat de la boucle chaude de recherche).

/**
 * @brief Affiche dans les logs les coordonnées x et y de chaque position du parcours.
 *
 * Outil de débogage permettant de visualiser l'ordre dans lequel les cases
 * de la grille sont parcourues (tableau `directions`).
 *
 * @return 0.
 */
int decode_direction(void)
{
	log_info("/nx : ");
	int i;
	for(i=0;i < ETERN_PARTS;i++) {
		int x = directions[i] % ETERN_SIZE;
        log_info("%i,",x);
	}
	
    log_info("/ny : ");
	for(i=0;i < ETERN_PARTS;i++) {
		int x = directions[i] % ETERN_SIZE;
		int y = (directions[i] - x) / ETERN_SIZE;
        log_info("%i,",y);
	}
	

	return 0;
}

/**
 * @brief Nombre de cases non vides de la grille (`grid[x][y] != -2`).
 *
 * Définition canonique de `alloc` depuis VERSION 13 (bascule MRV, moteur
 * unique) — voir sa doc dans possibility.h.
 */
int possibility_placed_count(const struct possibility_packet *packet)
{
    int placed = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (packet->grid[x][y] != -2) {
                placed++;
            }
        }
    }
    return placed;
}

int test_directions(void)
{
	int grille[ETERN_PARTS];
	int i;
	for(i=0; i < ETERN_PARTS; i++)
	{
		grille[i] = 0;
	}
	
	for(i=0; i < ETERN_PARTS;i++)
	{
		int x = directions[i];
		grille[x] = 1;
	}
	
	for(i=0; i < ETERN_PARTS;i++)
	{
		if(grille[i] == 0)
		{
            log_info("grille : %i not use\n", i);
			return -1;
		}
	}
	
	return 0;
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
struct possibility_packet *generate_possibility_packet(int x, int y, struct part *etern[ETERN_SIZE][ETERN_SIZE], int directory)
{
	(void)directory;
	struct possibility_packet *result = malloc(sizeof(*result));
	result->x = x;
	result->y = y;
	result->alloc = 0;
	result->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
	result->checked = 0;
	memset(result->b_faceused, 0, sizeof(result->b_faceused));
	int l;
	for (l = 0; l < ETERN_SIZE; l++)
	{
		int h;
		for(h = 0; h < ETERN_SIZE; h++)
		{
			struct part *part = etern[l][h];
			if(part != NULL)
			{
				result->grid[l][h] = id_for_rotated_part(part->id, part->rotation);
                set_face_used(result->b_faceused, part->id-1, 1);
			} else
			{
				result->grid[l][h] = -2;
			}
		}
	}
	return result;
}

/**
 * @brief Calcule la clé de recherche pour une case (x, y) quelconque de la grille.
 *
 * Variante de `what_search_to_key` qui prend des coordonnées explicites au lieu
 * d'utiliser la position courante du paquet. Utilisée par `possibility_all_has_a_next`
 * pour tester chaque case libre de la grille.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant (état de la grille).
 * @param x                Coordonnée x de la case à tester.
 * @param y                Coordonnée y de la case à tester.
 * @param key              Clé résultante (sortie).
 * @param all_face         Valeur représentant « toute face » dans la map (= map->sizearrayM).
 */
void what_search_in_grid_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, int8_t x, int8_t y,key_part *key, int8_t all_face) {
	key->k1 =-2;
	key->k2 =-2;
	key->k3 =-2;
	key->k4 =-2;
		
	// TOP
	if(y -1 < 0)
	{
		key->k1 = 0;
	} else
	{
        // Todo : tester -2 ou -1 (optim)
		if(possiblity->grid[x][y-1] < 0)
		{
			key->k1 = all_face;
		} else
		{
			key->k1 = all_rotate_parts->parts[possiblity->grid[x][y-1]].bottom;
		}
	}
	
	// RIGHT
	if(x + 1 >= ETERN_SIZE)
	{
		key->k2 = 0;
	} else
	{
		if(possiblity->grid[x+1][y] < 0)
		{
			key->k2 = all_face;
		} else
		{
			key->k2 = all_rotate_parts->parts[possiblity->grid[x+1][y]].left;
		}
	}
	
	// BOTTOM
	if(y + 1 >= ETERN_SIZE)
	{
		key->k3 = 0;
	} else
	{
		if(possiblity->grid[x][y+1] < 0)
		{
			key->k3 = all_face;
		} else
		{
			key->k3 = all_rotate_parts->parts[possiblity->grid[x][y+1]].top;
		}
	}
	
	// LEFT
	if(x -1 < 0)
	{
		key->k4 = 0;
	} else
	{
		if(possiblity->grid[x-1][y] < 0)
		{
			key->k4 = all_face;
		} else
		{
			key->k4 = all_rotate_parts->parts[possiblity->grid[x-1][y]].right;
		}
	}
}


/**
 * @brief Calcule la clé de recherche pour la case courante du paquet.
 *
 * Détermine les contraintes de bord imposées par les pièces voisines déjà placées.
 * - k1 = bord TOP requis (bottom du voisin du dessus, 0 si bord de grille, all_face si voisin absent)
 * - k2 = bord RIGHT requis
 * - k3 = bord BOTTOM requis
 * - k4 = bord LEFT requis
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param possiblity       Paquet courant indiquant la position (x, y) à remplir.
 * @param key              Clé résultante (sortie).
 * @param all_face         Valeur représentant « toute face » (= map->sizearrayM).
 */
void what_search_to_key(struct array_part *all_rotate_parts, struct possibility_packet *possiblity, key_part *key, int8_t all_face) {
    int x = possiblity->x;
    int xm = x - 1;
    int xp = x + 1;
    int y = possiblity->y;
    int ym = y - 1;
    int yp = y + 1;

    // TOP
    if(ym < 0)
    {
        key->k1 = 0;
    } else
    {
        int16_t partId = possiblity->grid[x][ym];
        if(partId < 0)
        {
            key->k1 = all_face;
        } else
        {
            key->k1 = all_rotate_parts->parts[partId].bottom;
        }
    }

    // RIGHT
    if(xp >= ETERN_SIZE)
    {
        key->k2 = 0;
    } else
    {
        int16_t partId = possiblity->grid[xp][y];
        if(partId < 0)
        {
            key->k2 = all_face;
        } else
        {
            key->k2 = all_rotate_parts->parts[partId].left;
        }
    }

    // BOTTOM
    if(yp >= ETERN_SIZE)
    {
        key->k3 = 0;
    } else
    {
        int16_t partId = possiblity->grid[x][yp];
        if(partId < 0)
        {
            key->k3 = all_face;
        } else
        {
            key->k3 = all_rotate_parts->parts[partId].top;
        }
    }

    // LEFT
    if(xm < 0)
    {
        key->k4 = 0;
    } else
    {
        int16_t partId = possiblity->grid[xm][y];
        if(partId < 0)
        {
            key->k4 = all_face;
        } else
        {
            key->k4 = all_rotate_parts->parts[partId].right;
        }
    }
}

/**
 * @brief Calcule et retourne la clé de recherche pour une case donnée.
 *
 * Version retournant une `key_part` par valeur au lieu d'écrire dans un pointeur.
 *
 * @param all_rotate_parts Tableau de toutes les rotations.
 * @param x                Coordonnée x de la case.
 * @param y                Coordonnée y de la case.
 * @param possiblity       État courant de la grille.
 * @return                 Clé de recherche (k1=top, k2=right, k3=bottom, k4=left).
 */
key_part what_search(struct array_part *all_rotate_parts, int x, int y, struct possibility_packet *possiblity)
{
	//char *result = malloc(MAX_KEY_LENGTH * sizeof(char));
	key_part result;
    // Toujours valorisé dans les if
    /*
	result.k1 =-2;
	result.k2 =-2;
	result.k3 =-2;
	result.k4 =-2;
     */
    
    int xm = x - 1;
    int xp = x + 1;
    int ym = y - 1;
    int yp = y + 1;
    
	// TOP
	if(ym < 0)
	{
		result.k1 = 0;
	} else
	{
		if(possiblity->grid[x][ym] < 0)
		{
			result.k1 = -1;
		} else
		{
			result.k1 = all_rotate_parts->parts[possiblity->grid[x][ym]].bottom;
		}
	}
	
	// RIGHT
	if(xp >= ETERN_SIZE)
	{
		result.k2 = 0;
	} else
	{
		if(possiblity->grid[xp][y] < 0)
		{
			result.k2 = -1;
		} else
		{
			result.k2 = all_rotate_parts->parts[possiblity->grid[xp][y]].left;
		}
	}
	
	// BOTTOM
	if(yp >= ETERN_SIZE)
	{
		result.k3 = 0;
	} else
	{
		if(possiblity->grid[x][yp] < 0)
		{
			result.k3 = -1;
		} else
		{
			result.k3 = all_rotate_parts->parts[possiblity->grid[x][yp]].top;
		}
	}
	
	// LEFT
	if(xm < 0)
	{
		result.k4 = 0;
	} else
	{
		if(possiblity->grid[xm][y] < 0)
		{
			result.k4 = -1;
		} else
		{
			result.k4 = all_rotate_parts->parts[possiblity->grid[xm][y]].right;
		}
	}
	
//	if(result.k1 == -1 && result.k2 == -1 && result.k3 == -1 && result.k4 == -1) {
//		printf("nothing to search x:%i y:%i \n",x,y);
//	}
	
	return result;
}

/**
 * @brief Sérialise un `possibility_packet` dans un fichier binaire.
 *
 * @param filename    Chemin du fichier de destination.
 * @param possibility Paquet à sauvegarder.
 * @return            0 en cas de succès.
 */
int save_possibility(char *filename, struct possibility_packet *possibility)
{
	FILE *f = fopen(filename, "w");
	if(!f)
	{
		log_errno("save_possibility file :%s ",filename);
		exit(EXIT_FAILURE);
	}
	
	fwrite(possibility, sizeof(struct possibility_packet), 1, f);
	
	fclose(f);
	return 0;
}

int save_solution_csv(const char *filename, const struct possibility_packet *poss,
                      const struct array_part *all_rotate_part)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        log_errno("save_solution_csv file: %s ", filename);
        return -1;
    }

    fprintf(f, "row,col,piece_id,rotation,top,right,bottom,left\n");
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            int16_t idx = poss->grid[x][y];
            if (idx < 0) continue;
            if (all_rotate_part) {
                const struct part *p = &all_rotate_part->parts[idx];
                fprintf(f, "%d,%d,%d,%d,%d,%d,%d,%d\n",
                        x, y, (int)p->id, (int)p->rotation,
                        (int)p->top, (int)p->right, (int)p->bottom, (int)p->left);
            } else {
                fprintf(f, "%d,%d,%d,%d,-1,-1,-1,-1\n",
                        x, y, idx % ETERN_PARTS, idx / ETERN_PARTS);
            }
        }
    }

    fclose(f);
    return 0;
}

/**
 * @brief Affiche la grille solution dans la console et la sauvegarde sur disque.
 *
 * Émet l'événement « SOLUTION FOUND », journalise chaque pièce placée puis écrit
 * le fichier solution (`./solution_<pid>`). NE quitte PAS le processus : la
 * notification éventuelle du serveur et l'arrêt restent à la charge de
 * l'appelant (cf. `checkIfResultFound` pour les chemins sans contexte client).
 *
 * @param poss            Paquet solution (toutes les pièces placées).
 * @param all_rotate_part Tableau de toutes les rotations (pour affichage).
 */
void log_solution(struct possibility_packet *poss, struct array_part *all_rotate_part) {
    // Nom de fichier unique : <pid> distingue les processus de recherche, le
    // compteur atomique distingue plusieurs solutions trouvées par le même
    // processus (mode « continuer après une solution »). Aucun écrasement.
    static unsigned solution_seq = 0;
    unsigned seq = __atomic_fetch_add(&solution_seq, 1, __ATOMIC_RELAXED);
    char fileName[68];
    snprintf(fileName, sizeof fileName, "./solution_%i_%u.csv", (int)getpid(), seq);

    log_info("fin de la boucle à %i \n", poss->alloc);
    log_event("SOLUTION FOUND! (%i pieces) - saved to %s", poss->alloc, fileName);
    for(int x = 0; x < ETERN_SIZE; x++)
    {
        for(int y=0;y < ETERN_SIZE; y++)
        {
            struct part *part = &all_rotate_part->parts[poss->grid[x][y]];
            log_info("%i;%i; ",x,y);
            print_part(part);
        }
    }
    save_solution_csv(fileName, poss, all_rotate_part);
}

/**
 * @brief Vérifie si toutes les 256 pièces sont placées et traite la solution trouvée.
 *
 * Si `alloc >= ETERN_PARTS`, affiche/sauvegarde la grille (`log_solution`) et
 * termine le processus avec EXIT_SUCCESS. Utilisé par les chemins de recherche
 * sans contexte client (donc sans notification possible du serveur). Le chemin
 * de production (`autosearch`) passe par `record_solution` qui informe d'abord
 * le serveur.
 *
 * @param poss            Paquet à vérifier.
 * @param all_rotate_part Tableau de toutes les rotations (pour affichage).
 */
void checkIfResultFound(struct possibility_packet *poss, struct array_part *all_rotate_part) {
    if(poss->alloc >= ETERN_PARTS)
    {
        log_solution(poss, all_rotate_part);
        // Sortie immédiate. NE PAS positionner request=STOP ici : cela
        // réveillerait les threads auxiliaires (alimentation/contrôle), qui
        // écriraient dans les flux stdio au moment où exit() les vide (sortie
        // perdue, voire course sur le tas). L'arrêt propre par signal est géré
        // dans run_mono_client (join des threads) ; le fichier solution est déjà
        // écrit (fclose), donc un exit() franc suffit ici.
        exit(EXIT_SUCCESS);
    }
}

/**
 * @brief Indique si la case courante du paquet a au moins une pièce posable.
 *
 * Vérifie uniquement la case indiquée par `possibility->x` et `possibility->y`.
 *
 * @param possibility    Paquet à tester.
 * @param mapParts       Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return               1 si au moins une pièce disponible peut être placée, 0 sinon.
 */
int possibility_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    int result = 0;
    
    // initialisation
	uint8_t x = possibility->x;
	uint8_t y = possibility->y;
    key_part wsearch = what_search(all_rotate_part, x, y, possibility);
	
	int8_t p[4] = {wsearch.k1,wsearch.k2,wsearch.k3,wsearch.k4};
    struct array_part *search = get_parts_bigarray(mapParts, p);
	int s;
	if(search->size > 0)
	{
		for(s=0; s< search->size && result == 0; s++)
		{
			if(search->parts[s].id != 0 && is_face_used(possibility->b_faceused, search->parts[s].id -1) == 0)
			{
                result = 1;
            }
        }
    }
    return result;
}

/**
 * @brief Vérifie que toutes les cases encore libres de la grille ont au moins une pièce posable.
 *
 * Parcourt EXHAUSTIVEMENT toutes les cases VIDES de `directions[]` (celles
 * déjà remplies sont sautées sans être ré-examinées, où qu'elles soient dans
 * le parcours — `alloc` n'indexe plus une position de curseur, cf.
 * `possibility_placed_count` ; correction par rapport à l'ancien balayage
 * `[alloc, ETERN_PARTS)`, qui ré-étudiait inutilement les cases déjà remplies
 * au-delà du curseur). Une case non contrainte ne stoppe pas le balayage :
 * elle est satisfiable par construction, cf. commentaire inline. Si une case
 * n'admet aucune pièce, retourne 0 (le paquet est sans issue).
 * Optimisation : si une case n'admet qu'une seule pièce, la place immédiatement.
 *
 * Itère ce balayage jusqu'à point fixe (§4.6a) : un balayage place des cases
 * forcées (une seule pièce candidate) en AVANÇANT dans `directions[]`, mais ne
 * revient jamais sur une case DÉJÀ examinée plus tôt dans le MÊME balayage —
 * une case qui avait 2 candidats libres au moment de son examen (donc jamais
 * forcée, jamais revisitée) peut se retrouver sans AUCUN candidat une fois
 * que ses deux candidats ont chacun été consommés par des forçages
 * ultérieurs. Un seul passage ne le détecte pas (cf. le commentaire
 * historique au-dessus de `remove_possibilities_with_no_next`,
 * src/core/datamanager.c, qui documentait déjà ce trou). Tant qu'un passage a
 * forcé au moins une case, on relance un passage complet ; on s'arrête dès
 * qu'un passage ne force plus rien (point fixe atteint) ou trouve une case
 * sans issue.
 *
 * `alloc` est recompté (`possibility_placed_count`), jamais incrémenté à la
 * main, dès qu'au moins un placement forcé a eu lieu — y compris quand le
 * plateau ne se retrouve PAS complet : avant cette correction, `alloc`
 * restait périmé tant que ETERN_PARTS n'était pas atteint, alors que
 * `b_faceused` avait déjà avancé (source du code -5 documenté par
 * `check_possibility`).
 *
 * @param possibility    Paquet à analyser (peut être modifié si des pièces uniques sont placées).
 * @param mapParts       Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param out_cells_studied Si non NULL, reçoit le nombre de cases examinées,
 *                       cumulé sur tous les passages du point fixe.
 * @return               1 si toutes les cases libres ont au moins une suite, 0 sinon.
 */
int possibility_all_has_a_next_counted(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part, unsigned int *out_cells_studied)
{
    int result = 1;

	key_part wsearch;
	int c;
    unsigned int cells_studied = 0;
    int forced_this_pass;
    int any_forced = 0;

    do {
        forced_this_pass = 0;
        result = 1;
        // On parcourt les cases VIDES du parcours (les pleines sont sautées).
        for(c=0;c < ETERN_PARTS && result == 1;c++) {
            int8_t x = dirx[c];
            int8_t y = diry[c];
            if(possibility->grid[x][y] != -2) {
                continue;
            }
            result = 0;
            cells_studied++;
            what_search_in_grid_to_key(all_rotate_part, possibility, x, y,&wsearch, mapParts->sizearrayM);
                if(wsearch.k1 < mapParts->sizearrayM || wsearch.k2 < mapParts->sizearrayM || wsearch.k3 < mapParts->sizearrayM || wsearch.k4 < mapParts->sizearrayM) {

                    struct array_part *search = get_parts_bigarray_with_key(mapParts, &wsearch);
                    int s;
                    if(search->size > 0)
                    {
                        for(s=0; s< search->size && result == 0; s++)
                        {
                            if(search->parts[s].id != 0 && is_face_used(possibility->b_faceused, search->parts[s].id -1) == 0)
                            {
                                if( search->size == 1) {
                                    set_face_used(possibility->b_faceused, search->parts[s].id - 1, 1);
                                    possibility->grid[x][y] = id_for_rotated_part(search->parts[s].id, search->parts[s].rotation);
                                    forced_this_pass = 1;
                                    any_forced = 1;
                                }
                                result = 1;
                            }
                        }

                    } else {
                        // On a rien trouvé, il n'y a donc pas de suite
                        break;
                    }
                }else {
                    /* Case non contrainte (les 4 clés valent all_face : ni bord de
                     * grille, ni voisin posé). Le compartiment "toute face" de la
                     * map contient l'union de toutes les pièces (cf. buildBigArray),
                     * donc cette case est satisfiable par construction tant qu'il
                     * reste au moins une pièce non utilisée -- ce qui est
                     * nécessairement vrai ici puisque le jeu complet n'est pas
                     * épuisé. Ne PAS interrompre le balayage : une case plus loin
                     * dans directions[] peut être contrainte (pièces pré-placées,
                     * indices, trous d'import) et sans issue ; s'arrêter ici
                     * masquerait cette impasse (sous-détection, cf. régression
                     * all_has_a_next_unconstrained_cell_does_not_hide_later_dead_cell).
                     */
                    result = 1;
                }
        }
    } while (result == 1 && forced_this_pass);
    if (any_forced) {
#ifdef DEBUG_RM_NO_NEXT
        log_debug("all has next (%i) allocated %i -> %i\n", result, possibility->alloc, possibility_placed_count(possibility));
#endif // DEBUG_RM_NO_NEXT
        possibility->alloc = (uint16_t)possibility_placed_count(possibility);
        // Placements forcés hors moteur MRV : le score de la dernière case
        // posée n'est pas connu ici (pas de mrv_choose_cell).
        possibility->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
        /* Ne pas appeler checkIfResultFound ici : cette fonction est invoquée
         * depuis des contextes variés (serveur, pruner client). Chaque appelant
         * teste possibility->alloc >= ETERN_PARTS et gère la solution dans son
         * propre contexte (sauvegarde + exit côté client, sauvegarde sans exit
         * côté serveur). */
    }

    if (out_cells_studied != NULL) {
        *out_cells_studied = cells_studied;
    }

	return result;
}

int possibility_all_has_a_next(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    return possibility_all_has_a_next_counted(possibility, mapParts, all_rotate_part, NULL);
}

/**
 * @brief Ajoute un `possibility_packet` en fin de file.
 *
 * Identique à `put` mais typé pour `possibility_packet`. En mode DEBUG_CHECK_POSSIBILITY,
 * valide le paquet avant insertion.
 *
 * @param suite File cible.
 * @param value Paquet à ajouter (copié dans la file).
 */
int put_possibility (File * suite, struct possibility_packet *value){
#ifdef DEBUG_CHECK_POSSIBILITY
    int analyse = check_possibility(value, NULL);
    if (analyse < 0)
    {
        log_error("possibility error : %i\n",analyse);
        log_error(" ---");
        print_possibility_packet(value);
    }
#endif // DEBUG_CHECK_POSSIBILITY
    // création d'un nouvel élément
	Element *new_element = malloc(sizeof(Element));
	if (suite->sizeofvalue <= 0 || (new_element->value = malloc(suite->sizeofvalue))
		== NULL)
	{
		free (new_element);
		return 0;
	}

	new_element->previous = NULL;
	new_element->next = NULL;

	// par précaution du cache on vérifie que qu'il ne s'agit pas de la meme valeur
    memcpy (new_element->value, value, sizeof(struct possibility_packet));

    // On place l'élément dans la suite
	if(suite->start == NULL){
		suite->start = new_element;
	}else {
		suite->end->next = new_element;
		new_element->previous = suite->end;
	}

    suite->end = new_element;
	suite->size++;
	return 1;
}

/**
 * @brief Choisit, parmi les cases encore vides, celle qui admet le moins de
 *        candidats libres (MRV, variante autonome pour le chemin froid).
 *
 * Variante de `mrv_choose_cell` (core/etii_search.c, `static`, boucle chaude
 * de la recherche réelle) : ce chemin est appelé par `search_possiblity_light`
 * hors boucle chaude — l'expansion anti-famine du démarrage serveur
 * (`expand_datas_to_level`) et le paquet genèse (`first_possibility`), tous
 * deux exécutés une poignée de fois par process, jamais par nœud exploré.
 * `mrv_choose_cell` dépend d'un cache de contraintes et d'un miroir 64 bits
 * des pièces utilisées, maintenus INCRÉMENTALEMENT par le moteur de
 * recherche à chaque placement/retrait ; les reconstruire ici pour un seul
 * appel coûterait plus que le balayage direct qu'ils économisent, et
 * `mrv_choose_cell` est `static` dans un autre module (core/) — l'exposer
 * pour ce seul usage introduirait un couplage inter-module pour un chemin
 * froid. On recalcule donc une clé par case vide (`what_search_in_grid_to_key`),
 * au prix d'un balayage complet à chaque appel plutôt que d'une lecture de
 * cache : acceptable hors boucle chaude, comme `forward_check_next_k`.
 *
 * @param possiblity      Paquet courant.
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param out_x/out_y     Case choisie si succès (non modifiés sinon).
 * @return 1 si une case a été choisie, 0 si aucune case vide (plateau complet).
 */
static int light_choose_cell(struct possibility_packet *possiblity, map_big_array *mapParts,
                              struct array_part *all_rotate_part, uint8_t *out_x, uint8_t *out_y)
{
    int best_count = -1;
    uint8_t best_x = 0, best_y = 0;
    int fallback_found = 0;
    uint8_t fallback_x = 0, fallback_y = 0;
    int8_t all_face = (int8_t)mapParts->sizearrayM;
    key_part wsearch;

    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (possiblity->grid[x][y] != -2) {
                continue;
            }
            what_search_in_grid_to_key(all_rotate_part, possiblity, (int8_t)x, (int8_t)y, &wsearch, all_face);
            if (wsearch.k1 == all_face && wsearch.k2 == all_face
                && wsearch.k3 == all_face && wsearch.k4 == all_face) {
                // Case sans aucune contrainte (ni bord de grille, ni voisine
                // posée) : le compartiment "toute face" contient l'union de
                // toutes les pièces à bord non nul (cf. buildBigArray), donc
                // cette case est satisfiable par construction tant qu'il
                // reste une pièce libre -- jamais le minimum, jamais morte.
                // Même règle que mrv_choose_cell (etii_search.c) : ne PAS la
                // compter (elle vaudrait un nombre énorme de candidats et
                // fausserait la comparaison), la garder seulement en repli
                // pour le cas où AUCUNE case contrainte n'existe encore.
                if (!fallback_found) {
                    fallback_found = 1;
                    fallback_x = (uint8_t)x;
                    fallback_y = (uint8_t)y;
                }
                continue;
            }
            struct array_part *search = get_parts_bigarray_with_key(mapParts, &wsearch);
            int count = 0;
            int16_t seen_last = 0;
            for (int s = 0; s < search->size; s++) {
                int16_t id = search->parts[s].id;
                if (id == 0 || id == seen_last || is_face_used(possiblity->b_faceused, id - 1)) {
                    continue;
                }
                // Les rotations d'une même pièce sont contiguës dans un
                // compartiment (cf. search_face) : ce filtre suffit à ne
                // compter chaque id qu'une fois.
                seen_last = id;
                count++;
            }
            if (count == 0) {
                // Case CONTRAINTE sans aucun candidat : sous-arbre mort.
                // Signalé immédiatement, comme mrv_choose_cell -- ne pas la
                // laisser "gagner" la comparaison de minimum (elle le
                // gagnerait toujours avec 0), ce qui masquerait la vraie
                // impasse derrière un choix de case normal.
                return 0;
            }
            if (best_count < 0 || count < best_count) {
                best_count = count;
                best_x = (uint8_t)x;
                best_y = (uint8_t)y;
            }
        }
    }

    if (best_count < 0) {
        if (!fallback_found) {
            return 0; // plateau complet : aucune case vide
        }
        *out_x = fallback_x;
        *out_y = fallback_y;
        return 1;
    }
    *out_x = best_x;
    *out_y = best_y;
    return 1;
}

/**
 * @brief Développe un paquet en ajoutant une pièce sur la case la plus contrainte.
 *
 * Choisit la case vide la plus contrainte (`light_choose_cell`, MRV) puis, pour
 * chaque pièce compatible avec sa clé (et non encore utilisée), crée une copie
 * du paquet avec la pièce placée et l'ajoute dans `result`. `alloc` de chaque
 * enfant est fixé par recomptage (`possibility_placed_count`), pas par
 * incrément d'un curseur : la case choisie diffère d'un enfant à l'autre du
 * même appel n'est jamais le cas ici (une seule case par appel), mais peut
 * différer d'un appel à l'autre (l'appelant ne connaît plus à l'avance la
 * case qui sera développée).
 *
 * @param result       File de destination des nouveaux paquets.
 * @param possiblity   Paquet source à développer.
 * @param mapParts     Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param idParts      Table de pré-calcul des indices de rotation [id][rotation].
 * @return             Nombre de pièces dans le paquet produit, ou 0 si aucune
 *                     pièce posable (ou plateau déjà complet).
 */
int search_possiblity_light(File *result, struct possibility_packet *possiblity, map_big_array *mapParts, struct array_part *all_rotate_part, int16_t idParts[ETERN_PARTS][4])
{
	int max_result=0;
    uint8_t x, y;

    if (!light_choose_cell(possiblity, mapParts, all_rotate_part, &x, &y)) {
        // Plateau déjà complet : rien à développer. checkIfResultFound n'a
        // pas de sens ici (aucun nouveau paquet produit) ; un appelant qui
        // pousse un plateau déjà complet en entrée d'expansion est un cas
        // qui ne se produit pas en pratique (garde amont côté appelants).
        return max_result;
    }

    key_part key;
    what_search_in_grid_to_key(all_rotate_part, possiblity, (int8_t)x, (int8_t)y, &key, mapParts->sizearrayM);

	int s;
	int lastId =-1;

	struct possibility_packet *currPossibility = possiblity;

	// get_parts_bigarray_with_key est zero-copy : elle renvoie un pointeur
	// direct dans la map 4D (&map->flat[...]) et la boucle lit .id/.rotation
	// sur place — aucune copie à éviter ici. Remplacer array_part par un
	// simple tableau d'id imposerait une seconde structure parallèle dans la
	// map pour un gain (densité de cache line) purement théorique et non
	// mesuré : non retenu.
    // liste des pieces répondant à la recherche (key)
    struct array_part *search = get_parts_bigarray_with_key(mapParts, &key);
    for(s=0; s< search->size; s++)
    {
        // bouchon id = 0 de la map (faces nulles) : pas une pièce
        if(search->parts[s].id == 0)
        {
            continue;
        }
        int position = search->parts[s].id -1;
        // Si la piece n'est pas déjà utilisée dans la suite de possibilité, on a donc une possiblité supplémentaire
        if(!is_face_used(currPossibility->b_faceused, position))
        {
            // On ajoute la définition d'une possibilité dans la suite.
            // effectue une copie dans le end->value
            // put_possibility recopie le possibility_packet entier (~540 o) par
            // candidat. La copie est intrinsèque : chaque pièce posée engendre un
            // état de plateau distinct empilé pour exploration ultérieure (appel
            // non récursif, une expansion par position). Les malloc sont déjà
            // évités (cache d'Element de la File).
            // Optimisation possible si le profiling le confirme : remplacer ce
            // memcpy par un système de delta (n'enregistrer que la case modifiée).
            if (!put_possibility(result, currPossibility)) {
                log_error("put_possibility: malloc échoué à la case (%d,%d)\n", x, y);
                break;
            }
            // On se place à la fin de la suite qui correspond à la nouvelle définition
            currPossibility = result->end->value;
            // Dans le cas où on a déjà généré une possiblité, on libère la piece qui avait été utilisée avant de généré un nouveau jeu
            if(lastId>0) {
                set_face_used(currPossibility->b_faceused, lastId - 1, 0);
            }
            // On place la piece
            currPossibility->grid[x][y] = idParts[search->parts[s].id][search->parts[s].rotation];
            // On indique que la piece est utilisée
            set_face_used(currPossibility->b_faceused, position, 1);
            // statistique du nombre de piece placée : recompté, jamais incrémenté
            // à la main (la case choisie n'est pas forcément le curseur du parent).
            currPossibility->alloc = (uint16_t)possibility_placed_count(currPossibility);
            // Expansion en ordre fixe, hors moteur MRV : pas de score à reporter.
            currPossibility->min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
            // identifiant de la dernière piece utilisée

            lastId = search->parts[s].id;

#ifdef DEBUG_CHECK_POSSIBILITY
            int analyse = check_possibility(currPossibility, all_rotate_part);
            if (analyse < 0)
            {
                log_error("possibility error : %i\n",analyse);
                log_error(" ---");
                print_possibility_packet(currPossibility);
            }
#endif // DEBUG_CHECK_POSSIBILITY
            // si toutes les pieces sont placées alors on n'entrera pas dasn le if !faceused et sortira donc
        }
    }

    // On a au moins placé une piece
	if (lastId>-1) {
		max_result = currPossibility->alloc;
        checkIfResultFound(currPossibility, all_rotate_part);
	}
	return max_result;
}

#if FORWARD_CHECK_K > 0
/**
 * @brief Forward-checking court sur les FORWARD_CHECK_K prochaines cases VIDES du parcours.
 *
 * Inspecte les `FORWARD_CHECK_K` premières cases VIDES rencontrées en
 * parcourant `directions[]` dans l'ordre (les cases déjà remplies sont
 * sautées, où qu'elles soient dans le parcours — cf.
 * `possibility_placed_count`, `alloc` n'indexe plus une position de curseur).
 * Variante volontairement simple : « les K premières cases vides du
 * parcours », pas « les K cases les plus contraintes » — cette dernière
 * suppose un score déjà calculé côté appelant, absent de ce chemin froid, cf.
 * docs/autosearch_step.md. Pour chacune, calcule la clé de
 * contraintes à partir de l'état courant du plateau et interroge la
 * `map_big_array`. Si l'une de ces cases n'admet plus aucune pièce candidate
 * disponible (toutes utilisées, ou aucun résultat dans la map), la branche est
 * sans issue et la fonction retourne 0.
 *
 * Cette vérification élague des branches mortes juste après un placement,
 * sans changer l'ordre statique du parcours `directions[]`.
 *
 * ⚠ Version SANS cache — chemins froids uniquement. Chaque case inspectée
 * recalcule sa clé de contraintes via `what_search_in_grid_to_key`. Son seul
 * appelant est `bt_materialize_pending` (etii_search.c, délégation
 * throttlée par `DELEGATE_MIN_INTERVAL_MS`). Le moteur de backtracking chaud
 * (`search_packet_backtracking`) utilise `bt_forward_check` (etii_search.c),
 * même sémantique mais qui lit la clé dans le cache `constraints[][]` maintenu
 * incrémentalement — ne PAS brancher la présente version sur un hot path.
 *
 * Note : entièrement supprimée à la compilation quand `FORWARD_CHECK_K == 0`
 * (debit brut strictement identique au code d'origine).
 *
 * @param possibility    Paquet à tester (avec la pièce qu'on vient de placer).
 * @param mapParts       Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @return 1 si toutes les cases inspectées ont au moins une pièce candidate, 0 sinon.
 */
int forward_check_next_k(struct possibility_packet *possibility, map_big_array *mapParts, struct array_part *all_rotate_part)
{
    int8_t all_face = mapParts->sizearrayM;
    key_part wsearch;
    // Cases VIDES réellement inspectées (statistique « études de prunage »,
    // et index 1-based dans fc_pruned_at[]) : cumulées localement, un seul
    // ajout atomique par appel (boucle chaude).
    unsigned int cells = 0;

    for (int c = 0; c < ETERN_PARTS && (int)cells < FORWARD_CHECK_K; c++) {
        int8_t x = dirx[c];
        int8_t y = diry[c];

        // Si la case a déjà été remplie, on la saute : elle ne compte pas
        // dans la fenêtre des K cases vides inspectées.
        if (possibility->grid[x][y] != -2) {
            continue;
        }
        cells++;

        what_search_in_grid_to_key(all_rotate_part, possibility, x, y, &wsearch, all_face);

        struct array_part *search = get_parts_bigarray_with_key(mapParts, &wsearch);
        if (search == NULL || search->size == 0) {
            // case morte : aucune pièce candidate
            __atomic_fetch_add(&fc_pruned_at[cells], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&fc_cells_studied, cells, __ATOMIC_RELAXED);
            return 0;
        }

        // Vérifier qu'au moins une pièce candidate n'est pas déjà utilisée ailleurs
        int found = 0;
        for (int s = 0; s < search->size; s++) {
            if (search->parts[s].id != 0
                && !is_face_used(possibility->b_faceused, search->parts[s].id - 1)
                ) {
                found = 1;
                break;
            }
        }
        if (!found) {
            // case morte : toutes les pièces candidates sont déjà utilisées
            __atomic_fetch_add(&fc_pruned_at[cells], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&fc_cells_studied, cells, __ATOMIC_RELAXED);
            return 0;
        }
    }

    __atomic_fetch_add(&fc_cells_studied, cells, __ATOMIC_RELAXED);
    return 1;
}
#endif // FORWARD_CHECK_K > 0

/**
 * @brief Valide l'intégrité d'un `possibility_packet`.
 *
 * @param packet      Paquet à valider.
 * @param rotateParts Tableau de toutes les rotations (chargé depuis `parts_files` si NULL).
 * @return  0 si valide, sinon un code négatif :
 *          -1 packet NULL, -2 coordonnées hors grille, -4 alloc > ETERN_PARTS
 *          (alloc = 0 est l'état genèse, valide),
 *          -5 nombre de faces utilisées < alloc, -6 pièce absente à (7,8) (puzzle 256),
 *          -7 valeur de grille invalide, -9 incohérence de bord.
 */
int check_possibility(struct possibility_packet *packet, struct array_part *rotateParts)
{
    /* Allocations locales, uniquement quand rotateParts n'est pas fourni par
       l'appelant : elles doivent être libérées sur TOUS les chemins de sortie
       (voir label cleanup), sans quoi chaque appel avec rotateParts == NULL
       fuit `apart` et le `rotateParts` reconstruit. */
    struct array_part *local_apart = NULL;
    struct array_part *local_rotate = NULL;
    int result;

    if (rotateParts == NULL) {
        local_apart = read_parts(parts_files);
        local_rotate = rotate_all_parts(local_apart);
        rotateParts = local_rotate;
    }

	if(packet == NULL) { result = -1; goto cleanup; }

	/* x et y sont des uint8_t : les tests « < 0 » seraient toujours faux
	   (type non signé) — GCC les signale via -Wtype-limits. */
	if(packet->x >= ETERN_SIZE || packet->y >= ETERN_SIZE) { result = -2; goto cleanup; }

	//if(packet->direcory < DIR_UP || packet->direcory > DIR_LEFT) return -3;

	// alloc = 0 est l'état genèse (aucune case du parcours encore traitée)
	if(packet->alloc > ETERN_PARTS) { result = -4; goto cleanup; }

	int i;
	int faceused= 0;
	for(i = 0; i < ETERN_PARTS;i++)
	{
		if(is_face_used(packet->b_faceused, i) == 1)
		{
			faceused++;
		}
	}
    // faceused > alloc reste légitime pour un paquet produit par le moteur à
    // ordre FIXE : des indices officiels sont posés au-delà du curseur
    // historique de directions[] (cf. docs/autosearch_step.md) — inégalité
    // stricte volontaire, PAS une égalité.
    if(faceused < packet->alloc) {
        result = -5;
        goto cleanup;
    }

#if ETERN_PARTS == 256
    if (packet->grid[7][8] != id_for_rotated_part(139, 2)) {
        result = -6;
        goto cleanup;
    }
#endif // ETERN_PARTS == 256

    // map_big_array *map_parts = prepare_map_part(rotateParts);

    // Controle que les pieces correspondent à leur "entourage" -- TOUTES les
    // cases non vides de la grille (alloc n'indexe plus une position de
    // curseur dans directions[], cf. possibility_placed_count), pas
    // seulement les alloc premières du parcours.
    for (int p = 0; p < ETERN_PARTS; p++) {
        uint8_t x = dirx[p];
        uint8_t y = diry[p];
        if (packet->grid[x][y] == -2) {
            continue;
        }
        int16_t gridValue = packet->grid[x][y];
        if (gridValue < 0 || gridValue >= rotateParts->size) {
            result = -7;
            goto cleanup;
        }
        struct part partXY = rotateParts->parts[gridValue];

        int8_t top = 0;
        // TOP
        if (y -1 >= 0)
        {
            if (packet->grid[x][y-1] < 0) {
                top = -1;
            } else {
                top = rotateParts->parts[packet->grid[x][y-1]].bottom;
            }
        }
        if (top != -1 && partXY.top != top) {
            result = -9;
            goto cleanup;
        }

        // RIGHT
        int8_t right = 0;
        if (x + 1 < ETERN_SIZE)
        {
            if (packet->grid[x+1][y] < 0) {
                right = -1;
            } else {
                right = rotateParts->parts[packet->grid[x+1][y]].left;
            }
        }
        if (right != -1 && partXY.right != right) {
            result = -9;
            goto cleanup;
        }

        // BOTTOM
        int8_t bottom = 0;
        if (y + 1 < ETERN_SIZE)
        {
            if (packet->grid[x][y+1] < 0) {
                bottom = -1;
            } else {
                bottom = rotateParts->parts[packet->grid[x][y+1]].top;
            }
        }
        if (bottom != -1 && partXY.bottom != bottom) {
            result = -9;
            goto cleanup;
        }

        // LEFT
        int8_t left = 0;
        if (x - 1 >= 0)
        {
            if (packet->grid[x-1][y] < 0) {
                left = -1;
            } else {
                left = rotateParts->parts[packet->grid[x-1][y]].right;
            }
        }
        if (left != -1 && partXY.left != left) {
            result = -9;
            goto cleanup;
        }
    }

	result = 0;

cleanup:
    if (local_rotate != NULL) {
        free_array_part(local_rotate);
    }
    if (local_apart != NULL) {
        free_array_part(local_apart);
    }
    return result;
}

/**
 * @brief Construit la représentation JSON de la grille d'un `possibility_packet`.
 *
 * Partagée par print_possibility_packet (destination : les logs) et
 * fprint_possibility_packet (destination : un fichier arbitraire, pour
 * l'export console `print`/`printFile [fichier]`) : même format, deux sorties.
 *
 * @param packet Paquet dont la grille doit être sérialisée.
 * @return       Chaîne malloc'ée (à libérer par l'appelant), jamais NULL
 *               (fatal_error en cas d'échec d'allocation, comme le reste du
 *               module).
 */
static char *build_grid_json(struct possibility_packet *packet)
{
	// grid[x][y] est un int16_t : la case peut contenir un id de pièce TOURNÉ,
	// codé id + ETERN_PARTS*rotation (cf. id_for_rotated_part), donc jusqu'à
	// 4*ETERN_PARTS - 1 (rotation 0..3). En build 256 ça vaut au plus 1023 (4
	// chiffres) ; les valeurs spéciales (vide -2, erreurs négatives type -9 de
	// check_possibility) restent sur 2-3 caractères. Plutôt que de couper au
	// plus juste, on borne large sur la plage réelle du type : un int16_t tient
	// toujours sur au plus 6 caractères, signe compris ("-32768"). Chaque case
	// coûte donc au plus 6 + 2 (le ", " de séparation) octets, et on ajoute la
	// marge des crochets de ligne/grille + le '\0' final.
	enum { STR_INT16_MAX_LEN = 6 }; // "-32768" = 6 caractères, borne large pour tout int16_t
	const size_t cell_budget = STR_INT16_MAX_LEN + 2;         // valeur + ", "
	const size_t row_budget = (cell_budget * ETERN_SIZE) + 2; // cases d'une ligne + "[" + "]"
	const size_t grid_budget = (row_budget * ETERN_SIZE) + 4; // lignes + "[" + "]" + marge + '\0'

	char *grid = malloc(sizeof(char) * grid_budget);
	if (grid == NULL) {
		fatal_error("build_grid_json: malloc failed (%zu bytes)\n", grid_budget);
	}
	int c = 0;
	grid[c++] = '[';
	for (int y = 0; y < ETERN_SIZE; y++) {
		if (y > 0) {
			grid[c++] = ',';
			grid[c++] = ' ';
		}
		grid[c++] = '[';
		for (int x = 0; x < ETERN_SIZE; x++) {
			if (x > 0) {
				grid[c++] = ',';
				grid[c++] = ' ';
			}
			char str[10]; // int16_t : "-32768\0" (7 octets) tient largement dans 10

			sprintf(str, "%i", packet->grid[x][y]);
			for (size_t i = 0; i < strlen(str); i++)
			{
				grid[c++] = str[i];
			}
		}
		grid[c++] = ']';
	}
	grid[c++] = ']';
	grid[c++] = '\0';
	return grid;
}

/**
 * @brief Affiche un `possibility_packet` au format JSON dans les logs.
 *
 * Format : `{"alloc": N, "x": X, "y": Y, "grid": [[...], ...]}`.
 *
 * @param packet Paquet à afficher.
 * @return       0.
 */
int print_possibility_packet(struct possibility_packet *packet)
{
	char *grid = build_grid_json(packet);
	log_info("{\"alloc\": %i, \"x\": %i, \"y\": %i, \"grid\": ", packet->alloc, packet->x, packet->y);
    log_info("%s", grid);
    log_info("}\n");

	free(grid);

	return 0;
}

/**
 * @brief Variante de print_possibility_packet au niveau ERREUR (voir possibility.h) :
 *        persiste dans events.log, contrairement à log_info. Un seul appel à
 *        log_error (pas trois comme print_possibility_packet) pour que la
 *        ligne complète — alloc/x/y/grille — arrive intacte dans events.log,
 *        où chaque log_error écrit sa propre ligne horodatée.
 */
int log_error_possibility_packet(struct possibility_packet *packet)
{
	char *grid = build_grid_json(packet);
	log_error("{\"alloc\": %i, \"x\": %i, \"y\": %i, \"grid\": %s}\n", packet->alloc, packet->x, packet->y, grid);

	free(grid);

	return 0;
}

/**
 * @brief Écrit un `possibility_packet` au format JSON dans un fichier arbitraire.
 *
 * Même format que print_possibility_packet (destinée aux logs), utilisée par
 * l'export console `print`/`printFile`/`printAnalysed [fichier]` pour éviter
 * qu'un gros stock ne déborde silencieusement le pad de sortie ncurses ou ne
 * défile hors de portée en ANSI.
 *
 * @param out    Flux ouvert en écriture (l'appelant garde la responsabilité
 *               de l'ouvrir et de le refermer).
 * @param packet Paquet à écrire.
 * @return       0 en cas de succès, -1 si une écriture a échoué (ex. : disque
 *               plein) — l'appelant doit alors considérer le fichier incomplet.
 */
int fprint_possibility_packet(FILE *out, struct possibility_packet *packet)
{
	char *grid = build_grid_json(packet);
	int rc = 0;
	if (fprintf(out, "{\"alloc\": %i, \"x\": %i, \"y\": %i, \"grid\": %s}\n",
	            packet->alloc, packet->x, packet->y, grid) < 0) {
		rc = -1;
	}
	free(grid);
	return rc;
}

/**
 * @brief Génère l'ensemble des possibilités initiales et les injecte dans le datamanager.
 *
 * Pour le puzzle 16×16 (ETERN_PARTS == 256), place les indices officiels lus
 * depuis `indices_file` (CSV `id x y rotation mandatory`, voir readdata.h),
 * puis développe la première case libre pour produire toutes les positions
 * de départ.
 *
 * Ces possibilités initiales sont ensuite distribuées par le serveur aux clients.
 *
 * @param mapParts        Tableau 4D de lookup.
 * @param all_rotate_part Tableau de toutes les rotations.
 */
void first_possibility(map_big_array *mapParts, struct array_part *all_rotate_part)
{
    struct part *etern[ETERN_SIZE][ETERN_SIZE];
    int x;
    int y;
    // initialisation
    for (x = 0; x < ETERN_SIZE; x++) {
        for(y=0; y < ETERN_SIZE; y++)
        {
            etern[x][y] = NULL;
        }
    }

#if ETERN_PARTS == 256
    int cur_dir = DIR_UP;

    // Indices officiels du puzzle, lus depuis indices_file (data/indices.csv
    // par défaut) plutôt que codés en dur : voir docs/architecture.md et le
    // format attendu dans readdata.h (read_indices). La case sera enjambée
    // par le parcours directions[] (niveau sans décision), pour chaque indice.
    struct array_index *indices = read_indices(indices_file);
    for (int i = 0; i < indices->size; i++) {
        struct board_index *hint = &indices->indices[i];
#if !ETERN_WITH_INDICES
        // Seul l'indice géométrique (mandatory) reste posé sans ETERN_WITH_INDICES.
        if (!hint->mandatory) {
            continue;
        }
#endif
        int position = hint->id + ETERN_PARTS * hint->rotation;
        if (position < 0 || position >= all_rotate_part->size
            || all_rotate_part->parts[position].id != hint->id) {
            fatal_error("indices : pièce %i rotation %i introuvable\n", hint->id, hint->rotation);
        }
        etern[hint->x][hint->y] = &all_rotate_part->parts[position];
    }
    free_array_index(indices);
#else
    int cur_dir = DIR_LEFT;
#endif

    // x/y du paquet genèse : sans objet pour search_possiblity_light (qui
    // choisit lui-même la case la plus contrainte, cf. light_choose_cell) —
    // conservés à directions[0] uniquement pour peupler ces champs devenus
    // inutilisés du possibility_packet (cf. sa doc, possibility.h).
    x = dirx[0];
    y = diry[0];


    int16_t idParts[ETERN_PARTS+1][4];
    for(int p=0; p <= ETERN_PARTS;p++) {
        for(int r=0; r < 4;r++) {
            idParts[p][r] = p + ETERN_PARTS * r;
        }
    }

    File *possibilities = malloc(sizeof(File));
    init_file(possibilities, sizeof(struct possibility_packet));

    struct possibility_packet *possibilityPacket = generate_possibility_packet(x, y, etern, cur_dir);
    non_null_possibilities++;
    int max = search_possiblity_light(possibilities, possibilityPacket, mapParts, all_rotate_part, idParts);
    free(possibilityPacket);
    
    // Si le résultat à dépasser le plus grand qu'on a trouvé, on trace
    if(max > max_result)
    {
        max_result = max;
        if(max_result >= ETERN_PARTS)
        {
            log_error("Erreur alloc > ETERN_PARTS\n");
        }
        log_info("max result:%i\n",max_result);
    }
    
    while (possibilities->size > 0) {
        struct possibility_packet *packet = malloc(sizeof(struct possibility_packet));
        scroll(possibilities,packet);
        log_info("packet->alloc:%i\n",packet->alloc);
        // Contrôle au démarrage : chaque possibilité initiale doit respecter
        // l'invariant de parcours et être valide
        int analyse = check_possibility(packet, all_rotate_part);
        if (analyse < 0) {
            // Pour l'initialisation, on crash car ce n'est vraiment pas normal :
            // on dump d'abord le paquet fautif, puis fatal_error trace + sort.
            log_error_possibility_packet(packet);
            fatal_error("first_possibility : paquet initial incohérent (check : %i)\n", analyse);
        }
        if(packet->alloc > max_result)
        {
            max_result = packet->alloc;
            if(max_result >= ETERN_PARTS)
            {
                log_error("Erreur alloc > ETERN_PARTS\n");
            }
            log_info("max result:%i\n",max_result);
        }
        // Genèse : seule étape de possibility.c produisant un plateau concret
        // (celui de search_possiblity_light ci-dessus n'en a pas un dédié).
        // Toujours appelé (best_board_try_record gate lui-même sur >) : côté
        // serveur, first_possibility est la seule source locale du record.
        best_board_try_record(&g_server_best_board, packet, packet->alloc);
        array_possibility_packet *aposs2 = build_single_array_possibility_packet(packet);
        if(add_possibility(NULL, aposs2))
        {
			// Pour l'initialisation, on crash car ce n'est vraiment pas normal
            fatal_error("error on add_possibility\n");
        }
        non_null_possibilities++;
		free_array_possibility_packet(aposs2);
        free(packet);
    }
    free_file(possibilities);
}

/**
 * @brief Compare deux paquets et retourne 0 s'ils sont identiques.
 *
 * @param packet       Premier paquet.
 * @param other_packet Second paquet.
 * @return  0 si identiques, sinon un code négatif indiquant la première différence :
 *          -1 nullité différente, -2 alloc diffèrent, -3 position (x,y) différente,
 *          -4 masque de pièces utilisées différent, -5 grille différente.
 */
int compare_possibility(struct possibility_packet *packet, struct possibility_packet *other_packet) {
    // Test si l'un est null alors est-ce que les deux le sont
    if (packet == NULL || other_packet == NULL) {
        return packet == other_packet ? -1 : 0;
    }

    // Test différence de pieces placées
	if (packet->alloc != other_packet->alloc) {
		return -2;
	}

    // Test de la position
	if (packet->x != other_packet->x || packet->y != other_packet->y) {
		return -3;
	}

    // Test sur les pieces utilisées
	for (int u = 0; u < ETERN_PARTS; u++) {
		if (is_face_used(packet->b_faceused, u) != is_face_used(other_packet->b_faceused, u)) {
			return -4;
		}
	}

    // Test si les pieces sont placées à la même position (et meme sens)
	for (int x = 0; x < ETERN_SIZE; x++) {
		for (int y = 0; y < ETERN_SIZE; y++) {
			if (packet->grid[x][y] != other_packet->grid[x][y]) {
				return -5;
			}
		}
	}

	return 0;
}
    
/**
 * @brief Indique si `packet` est un ancêtre (inclusion de plateaux) de `other_packet`.
 *
 * Vérifie que toutes les cases NON VIDES de `packet` sont identiques à
 * celles d'`other_packet` aux mêmes positions — inclusion de plateaux
 * (peu importe l'ordre de parcours dans lequel elles ont été posées), pas
 * un préfixe de `directions[]` : `alloc` n'indexe plus une position de
 * curseur, cf. `possibility_placed_count`.
 *
 * @param packet       Paquet supposément ancêtre (alloc inférieur).
 * @param other_packet Paquet supposément descendant.
 * @return  1 si `packet` est bien un ancêtre, -1 si alloc ≥, -2 si grilles divergent, 0 si NULL.
 */
int is_origin_of(struct possibility_packet *packet, struct possibility_packet *other_packet) {
    // Test si l'un est null alors biensur que non
    if (packet == NULL || other_packet == NULL) {
        return 0;
    }

    // Le sens est imposé et on ne cherche pas une égalité
    if (packet->alloc >= other_packet->alloc) {
        return -1;
    }

    // Test si toutes les cases NON VIDES de packet sont identiques dans other_packet
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (packet->grid[x][y] == -2) {
                continue;
            }
            if (packet->grid[x][y] != other_packet->grid[x][y]) {
                return -2;
            }
        }
    }

    return 1;
}

/**
 * @brief Emballe un unique `possibility_packet` dans un `array_possibility_packet`.
 *
 * Copie le paquet dans un tableau de taille 1, format attendu par `add_possibility`.
 *
 * @param possibility Paquet à emballer (peut être NULL → tableau de taille 0).
 * @return            `array_possibility_packet` alloué (à libérer avec `free_array_possibility_packet`).
 */
array_possibility_packet *build_single_array_possibility_packet(struct possibility_packet *possibility) {
	array_possibility_packet *result = malloc(sizeof(array_possibility_packet));
	if (possibility != NULL) {
		result->size = 1;
        result->possibilities = malloc(sizeof(struct possibility_packet));
        memcpy(&result->possibilities[0], possibility, sizeof(struct possibility_packet));
	} else {
		result->size = 0;
        result->possibilities = NULL;
	}

	return result;
}

/**
 * @brief Libère un `array_possibility_packet` et son tableau de paquets interne.
 * @param possibilities Structure à libérer.
 */
void free_array_possibility_packet(array_possibility_packet *possibilities) {
	if (possibilities->possibilities != NULL) {
		free(possibilities->possibilities);
	}
	free(possibilities);
}
