#include "core/etii_search.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "ui/logger.h"
#include "app/static_variables.h"
#include "app/etii_client.h"
#include "core/datamanager.h"
#include "core/possibility.h"
#include "core/best_board.h"

#ifdef WITH_CUDA
#include "app/gpu_pruner.h"
#endif // WITH_CUDA

// Accès au masque des pièces utilisées
#define BOARD_FACE_USED(b, pos)   is_face_used((b)->b_faceused, (pos))
#define BOARD_SET_FACE(b, pos, v) set_face_used((b)->b_faceused, (pos), (v))

/**
 * @brief Délègue au serveur les possibilités excédant `max_stock_by_thread` dans une `File`.
 *
 * Extrait (`scroll`) les éléments en surplus de `db` vers un `array_possibility_packet`,
 * les pousse via `add_possibility`, puis libère le tableau temporaire.
 *
 * @param client_possibility Contexte du thread client (socket, mutex, etc.).
 * @param db                 File locale dont on contrôle la taille.
 */
void checkAndDelegatePossibilitiesIfNeeded(client_possibility_t *client_possibility, File *db) {
    if(db->size > (unsigned long long)max_stock_by_thread)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        unsigned long long remains = db->size - max_stock_by_thread;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (max_stock_by_thread));
        aposs->size = 0;
        while(db->size > remains)
        {
            scroll(db, &aposs->possibilities[aposs->size]);

            aposs->size++;
        }
        // En cas d'erreur les possibilités sont remises en locale
        if(add_possibility(client_possibility, aposs))
        {
            log_error("error on add_possibility\n");
        }
        free_array_possibility_packet(aposs);
    }
}

/**
 * @brief Niveau de la pile de décisions du backtracking in-place.
 *
 * Chaque case du parcours `directions[]` explorée depuis le paquet racine
 * occupe un niveau de pile. Un niveau mémorise uniquement la liste de candidats
 * de la map (pointeur stable, la map est en lecture seule pendant la recherche)
 * et la position de reprise : le plateau lui-même est partagé et modifié en place.
 */
typedef struct {
    /** Liste des candidats pour cette case (NULL = case pré-remplie, aucune décision). */
    struct array_part *search;
    /** Prochain indice de candidat à essayer dans `search` lors d'un retour sur ce niveau. */
    int next_s;
    /** Indice faceused (id-1) de la pièce actuellement placée à ce niveau, -1 si aucune. */
    int16_t placed_pos;
} bt_level;

/**
 * @brief Initialise le cache de contraintes : la clé de recherche de chaque case de la grille.
 *
 * Pour chaque case, `constraints[x][y]` contient la clé (k1=top, k2=right,
 * k3=bottom, k4=left) que `what_search_in_grid_to_key` calculerait : 0 pour un
 * bord de grille, `all_face` pour un voisin vide, sinon la couleur imposée par
 * le voisin placé. Le cache est ensuite maintenu incrémentalement à chaque
 * placement/retrait (`bt_propagate_place`/`bt_propagate_undo`), ce qui rend le
 * calcul de clé de la boucle chaude gratuit (une lecture de 4 octets).
 *
 * @param constraints     Cache à initialiser.
 * @param board           Plateau courant.
 * @param all_rotate_part Tableau de toutes les rotations.
 * @param all_face        Valeur « toute face » (= map->sizearrayM).
 */
static void bt_init_constraints(key_part constraints[ETERN_SIZE][ETERN_SIZE],
                                struct possibility_packet *board,
                                struct array_part *all_rotate_part, int8_t all_face)
{
    for (int cx = 0; cx < ETERN_SIZE; cx++) {
        for (int cy = 0; cy < ETERN_SIZE; cy++) {
            what_search_in_grid_to_key(all_rotate_part, board, (int8_t)cx, (int8_t)cy, &constraints[cx][cy], all_face);
        }
    }
}

/**
 * @brief Propage les couleurs d'une pièce placée en (cx, cy) vers les clés de ses voisines.
 * @param constraints Cache de contraintes.
 * @param cx          Colonne de la pièce placée.
 * @param cy          Ligne de la pièce placée.
 * @param p           Pièce placée (rotation déjà appliquée).
 */
static inline void bt_propagate_place(key_part constraints[ETERN_SIZE][ETERN_SIZE], int cx, int cy, const struct part *p)
{
    if (cy > 0)              constraints[cx][cy - 1].k3 = p->top;
    if (cx < ETERN_SIZE - 1) constraints[cx + 1][cy].k4 = p->right;
    if (cy < ETERN_SIZE - 1) constraints[cx][cy + 1].k1 = p->bottom;
    if (cx > 0)              constraints[cx - 1][cy].k2 = p->left;
}

/**
 * @brief Annule la propagation d'une pièce retirée de (cx, cy) : ses voisines redeviennent libres de ce côté.
 * @param constraints Cache de contraintes.
 * @param cx          Colonne de la pièce retirée.
 * @param cy          Ligne de la pièce retirée.
 * @param all_face    Valeur « toute face ».
 */
static inline void bt_propagate_undo(key_part constraints[ETERN_SIZE][ETERN_SIZE], int cx, int cy, int8_t all_face)
{
    if (cy > 0)              constraints[cx][cy - 1].k3 = all_face;
    if (cx < ETERN_SIZE - 1) constraints[cx + 1][cy].k4 = all_face;
    if (cy < ETERN_SIZE - 1) constraints[cx][cy + 1].k1 = all_face;
    if (cx > 0)              constraints[cx - 1][cy].k2 = all_face;
}

#if FORWARD_CHECK_K > 0
/**
 * @brief Forward-checking de la boucle chaude, basé sur le cache de contraintes.
 *
 * Même sémantique que `forward_check_next_k` (possibility.c) mais sans recalcul
 * de clé : la clé de chaque case inspectée est lue directement dans le cache
 * `constraints[][]` maintenu par `bt_propagate_place`/`bt_propagate_undo`.
 * C'est LA version du hot path (`search_packet_backtracking`) ; la variante
 * sans cache `forward_check_next_k` ne sert que les chemins froids
 * (`bt_materialize_pending`, throttlé, et les tests) — tout nouveau code de la
 * boucle chaude doit passer par ici.
 *
 * @param constraints Cache de contraintes maintenu par le backtracking.
 * @param board       Plateau courant (grille + masque des pièces utilisées).
 * @param mapParts    Table de lookup.
 * @param alloc       Indice de la première case non remplie du parcours.
 * @return            1 si toutes les cases inspectées ont au moins une pièce candidate, 0 sinon.
 */
static int bt_forward_check(key_part constraints[ETERN_SIZE][ETERN_SIZE],
                            struct possibility_packet *board,
                            map_big_array *mapParts, int alloc)
{
    int end = alloc + FORWARD_CHECK_K;
    if (end > ETERN_PARTS) {
        end = ETERN_PARTS;
    }

    // Cases réellement inspectées (statistique « études de prunage ») :
    // cumulées localement, un seul ajout atomique par appel (boucle chaude).
    unsigned int cells = 0;

    for (int c = alloc; c < end; c++) {
        int8_t x = dirx[c];
        int8_t y = diry[c];

        // Case déjà remplie (pièce placée à l'avance) : on saute
        if (board->grid[x][y] != -2) {
            continue;
        }
        cells++;

        // Lookup via l'index COMPACT (`packed`) et non `flat` : à ce stade la
        // très grande majorité des accès ne sert qu'à lire une taille, et
        // `packed` divise par ~3,8 le volume balayé (cf. map_bucket_packed).
        // Résultat rigoureusement identique à get_parts_bigarray_with_key.
        map_bucket search = map_bucket_packed(mapParts, &constraints[x][y]);
        if (search.size == 0) {
            // case morte : aucune pièce candidate
            __atomic_fetch_add(&fc_pruned_at[c - alloc + 1], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&fc_cells_studied, cells, __ATOMIC_RELAXED);
            return 0;
        }

        // Vérifier qu'au moins une pièce candidate n'est pas déjà utilisée ailleurs
        int found = 0;
        for (int s = 0; s < search.size; s++) {
            if (search.parts[s].id != 0 && !BOARD_FACE_USED(board, search.parts[s].id - 1)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            // case morte : toutes les pièces candidates sont déjà utilisées
            __atomic_fetch_add(&fc_pruned_at[c - alloc + 1], 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&fc_cells_studied, cells, __ATOMIC_RELAXED);
            return 0;
        }
    }

    __atomic_fetch_add(&fc_cells_studied, cells, __ATOMIC_RELAXED);
    return 1;
}
#endif // FORWARD_CHECK_K > 0

/**
 * @brief Compte les possibilités en attente (frères non explorés) dans la pile de décisions.
 *
 * Pour chaque niveau, les candidats restants sont évalués avec le masque des
 * pièces utilisées tel qu'il était à ce niveau (placements des niveaux plus
 * profonds annulés sur une copie de travail). C'est l'équivalent du `bt.size`
 * de l'ancienne file : le stock local implicite du thread.
 *
 * @param board Plateau courant (non modifié).
 * @param stack Pile de décisions.
 * @param top   Indice du dernier niveau occupé (-1 si pile vide).
 * @return      Nombre de possibilités encore à explorer (hors chemin courant).
 */
static unsigned long long bt_count_pending(const struct possibility_packet *board, const bt_level *stack, int top)
{
    struct possibility_packet scratch;
    memcpy(&scratch, board, sizeof(scratch));

    // Retour à l'état racine : annulation des placements du chemin courant
    for (int i = top; i >= 0; i--) {
        if (stack[i].placed_pos >= 0) {
            BOARD_SET_FACE(&scratch, stack[i].placed_pos, 0);
        }
    }

    unsigned long long pending = 0;
    for (int i = 0; i <= top; i++) {
        const bt_level *lvl = &stack[i];
        if (lvl->search != NULL) {
            for (int s = lvl->next_s; s < lvl->search->size; s++) {
                int16_t id = lvl->search->parts[s].id;
                if (id != 0 && !BOARD_FACE_USED(&scratch, id - 1)) {
                    pending++;
                }
            }
        }
        // Ré-application du placement du niveau pour passer à l'état du niveau suivant
        if (lvl->placed_pos >= 0) {
            BOARD_SET_FACE(&scratch, lvl->placed_pos, 1);
        }
    }
    return pending;
}

/**
 * @brief Enregistre une solution complète trouvée par un thread de recherche.
 *
 * Toujours :
 *   1. `log_solution` : affiche la grille (routée vers la console du parent) et
 *      écrit un fichier solution unique.
 *   2. `send_solution` : signale la solution au serveur via TCP (synchrone,
 *      acquittée). Ce passage bloquant joue aussi le rôle de barrière : il laisse
 *      au processus parent le temps de vider les datagrammes IPC de l'étape 1.
 *
 * Avec `--stop-on-solution`, on termine ensuite le processus (`exit`) ; sinon on
 * rend la main à l'appelant, qui poursuit l'exploration pour trouver d'autres
 * solutions.
 *
 * @param client Contexte du thread (socket serveur + table des rotations).
 * @param poss   Paquet solution (toutes les pièces placées).
 */
static void record_solution(client_possibility_t *client, struct possibility_packet *poss)
{
    log_solution(poss, client->all_rotate_part);
    send_solution(client, poss);
    if (stop_on_solution) {
        exit(EXIT_SUCCESS);
    }
}

/**
 * @brief Matérialise en paquets les frères non explorés de la pile, du plus profond vers la racine.
 *
 * Reconstruit l'état du plateau à chaque niveau (annulation progressive des
 * placements sur une copie de travail) et produit, pour chaque candidat
 * restant, le `possibility_packet` que l'ancienne implémentation aurait poussé
 * dans sa file : pièce placée, `alloc` incrémenté, position sur la case suivante,
 * forward-checking appliqué. Les niveaux les plus profonds sont matérialisés en
 * premier — comme l'ancienne file LIFO qui expédiait les nœuds les plus récents :
 * on cède le bas de l'arbre (petites unités de travail vite consommées, stock
 * serveur maigre) et le haut reste local.
 *
 * Les positions de reprise consommées sont retournées dans `new_next_s` mais ne
 * sont PAS appliquées à la pile : l'appelant ne les applique qu'une fois l'envoi
 * réussi, sinon le travail reste local.
 *
 * @param client     Contexte du thread client.
 * @param board      Plateau courant (non modifié).
 * @param stack      Pile de décisions.
 * @param top        Indice du dernier niveau occupé.
 * @param start_depth Profondeur (alloc) du paquet racine.
 * @param idParts    Table de pré-calcul des indices de rotation [id][rotation].
 * @param out        Tableau de destination des paquets matérialisés.
 * @param max_out    Nombre maximal de paquets à produire.
 * @param new_next_s Sortie : nouveau `next_s` par niveau si l'envoi réussit.
 * @return           Nombre de paquets effectivement matérialisés.
 */
static int bt_materialize_pending(client_possibility_t *client,
                                  const struct possibility_packet *board,
                                  const bt_level *stack, int top, int start_depth,
                                  int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                                  struct possibility_packet *out, int max_out,
                                  int *new_next_s)
{
    struct possibility_packet scratch;
    memcpy(&scratch, board, sizeof(scratch));

    int count = 0;
    int i;
    for (i = top; i >= 0 && count < max_out; i--) {
        int d = start_depth + i;
        const bt_level *lvl = &stack[i];
        new_next_s[i] = lvl->next_s;
        // Annulation du placement du niveau : scratch = état au moment du choix
        if (lvl->placed_pos >= 0) {
            scratch.grid[dirx[d]][diry[d]] = -2;
            BOARD_SET_FACE(&scratch, lvl->placed_pos, 0);
        }
        if (lvl->search != NULL) {
            uint8_t cx = dirx[d];
            uint8_t cy = diry[d];
            int s = lvl->next_s;
            for (; s < lvl->search->size && count < max_out; s++) {
                struct part *cand = &lvl->search->parts[s];
                if (cand->id == 0) {
                    continue;
                }
                int position = cand->id - 1;
                if (BOARD_FACE_USED(&scratch, position)) {
                    // Pièce prise par un ancêtre : ce candidat est définitivement
                    // imposable à ce niveau, il peut être consommé
                    continue;
                }
                struct possibility_packet *pkt = &out[count];
                memcpy(pkt, &scratch, sizeof(*pkt));
                pkt->grid[cx][cy] = idParts[cand->id][cand->rotation];
                BOARD_SET_FACE(pkt, position, 1);
                pkt->alloc = d + 1;
                if (pkt->alloc >= ETERN_PARTS) {
                    // Solution complète parmi les frères : enregistre + signale.
                    // Avec --stop-on-solution, record_solution ne revient pas.
                    // Sinon on ne la matérialise pas (rien à explorer au-delà ;
                    // dirx[d+1] serait hors borne) et on passe au candidat suivant.
                    record_solution(client, pkt);
                    continue;
                }
                pkt->x = dirx[d + 1];
                pkt->y = diry[d + 1];
                // Nouvel état de plateau : le contrôle pruner ne vaut plus
                pkt->checked = 0;
#if FORWARD_CHECK_K > 0
                __atomic_fetch_add(&fc_attempts, 1, __ATOMIC_RELAXED);
                if (!forward_check_next_k(pkt, client->map_part, client->all_rotate_part)) {
                    // Branche morte : consommée sans être envoyée (comme à l'expansion)
                    __atomic_fetch_add(&fc_pruned, 1, __ATOMIC_RELAXED);
                    continue;
                }
#endif // FORWARD_CHECK_K > 0
                count++;
            }
            new_next_s[i] = s;
        }
    }
    // Niveaux non parcourus (limite atteinte) : positions de reprise inchangées
    for (; i >= 0; i--) {
        new_next_s[i] = stack[i].next_s;
    }
    return count;
}

/**
 * @brief Quota de délégation d'un thread de recherche (fonction pure).
 *
 * Deux régimes :
 * - au-dessus du seuil (`pending > max_stock`) : règle historique, on cède
 *   `max_stock` paquets, quelle que soit la faim ;
 * - sous le seuil : délégation ANTICIPÉE uniquement si le serveur a faim
 *   (`hunger > 0`, publié par la sonde INST_NEED_WORK du thread
 *   d'alimentation). On cède alors au plus la moitié du stock implicite
 *   (jamais le chemin courant), borné par la faim — un thread presque à sec
 *   ne se vide pas pour un serveur affamé.
 *
 * @param pending   Stock implicite du thread (frères non explorés).
 * @param max_stock Seuil de délégation (`max_stock_by_thread`).
 * @param hunger    Faim du serveur (≤ 0 = rassasié ou inconnue).
 * @return          Nombre de paquets à céder (0 = aucune délégation).
 */
static int bt_delegation_quota(unsigned long long pending, int max_stock, int hunger)
{
    if (pending > (unsigned long long)max_stock) {
        return max_stock;
    }
    if (hunger <= 0 || pending < 2) {
        return 0;
    }
    unsigned long long half = pending / 2;
    if ((unsigned long long)hunger < half) {
        return hunger;
    }
    return (int)half;
}

/**
 * @brief Garantit que le buffer de délégation pré-alloué du thread peut contenir
 *        `capacity` paquets.
 *
 * Le tampon `client->delegate_buf` est réutilisé d'une délégation à l'autre pour
 * éviter un malloc/free sur le chemin (semi-)chaud. `max_stock_by_thread` étant
 * ajustable à chaud (console `maxStockByThread`, CLI), le tampon n'est agrandi
 * que lorsque la limite augmente — jamais sur le régime nominal. La première
 * délégation l'alloue (`delegate_buf == NULL`).
 *
 * @param client   Contexte du thread (porte le buffer et sa capacité).
 * @param capacity Nombre de paquets requis (= `max_stock_by_thread`).
 * @return         0 si le buffer est utilisable, -1 sur échec d'allocation
 *                 (l'ancien buffer reste valide).
 */
static int bt_ensure_delegate_buf(client_possibility_t *client, int capacity)
{
    if (client->delegate_buf != NULL && client->delegate_buf_capacity >= capacity) {
        return 0;
    }
    struct possibility_packet *grown =
        realloc(client->delegate_buf, sizeof(struct possibility_packet) * capacity);
    if (grown == NULL) {
        return -1;
    }
    client->delegate_buf = grown;
    client->delegate_buf_capacity = capacity;
    return 0;
}

/**
 * @brief Délègue au serveur une partie du stock implicite si celui-ci dépasse
 *        `max_stock_by_thread` — ou, depuis la v8, si le serveur a faim
 *        (`server_hunger` > 0 : délégation anticipée sous le seuil).
 *
 * Équivalent backtracking de `checkAndDelegatePossibilitiesIfNeeded` :
 * le stock est compté dans la pile de décisions, et au plus
 * `bt_delegation_quota(...)` frères non explorés (les moins profonds) sont
 * matérialisés puis envoyés. Les niveaux délégués ne sont marqués consommés
 * qu'après un envoi réussi ; la faim est alors décrémentée du nombre envoyé.
 *
 * Le tableau de paquets matérialisés est écrit dans le buffer pré-alloué du
 * thread (`client->delegate_buf`) plutôt que dans un bloc malloc/free à chaque
 * appel ; le conteneur `array_possibility_packet` (8 octets) reste sur la pile.
 * `add_possibility` copie chaque paquet (file locale via `put`, ou envoi TCP
 * synchrone), donc le buffer reste réutilisable après l'appel.
 *
 * @param client      Contexte du thread client.
 * @param board       Plateau courant.
 * @param stack       Pile de décisions (modifiée si l'envoi réussit).
 * @param top         Indice du dernier niveau occupé.
 * @param start_depth Profondeur du paquet racine.
 * @param idParts     Table de pré-calcul des indices de rotation.
 */
static void bt_delegate_if_needed(client_possibility_t *client,
                                  const struct possibility_packet *board,
                                  bt_level *stack, int top, int start_depth,
                                  int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    unsigned long long pending = bt_count_pending(board, stack, top);
    // Statistique du nombre de possibilités en étude
    lastfilesize[client->compteur] = pending;
    // Faim du serveur publiée par la sonde INST_NEED_WORK du thread
    // d'alimentation : autorise une délégation anticipée sous le seuil.
    int hunger = __atomic_load_n(&server_hunger, __ATOMIC_RELAXED);
    int quota = bt_delegation_quota(pending, max_stock_by_thread, hunger);
    if (quota <= 0) {
        return;
    }

    if (bt_ensure_delegate_buf(client, quota) != 0) {
        // Échec d'allocation : on saute cette délégation, le travail reste local
        log_error("error on delegate buffer allocation\n");
        return;
    }
    array_possibility_packet aposs = { .size = 0, .possibilities = client->delegate_buf };
    int new_next_s[ETERN_PARTS];
    aposs.size = bt_materialize_pending(client, board, stack, top, start_depth,
                                        idParts, aposs.possibilities,
                                        quota, new_next_s);
    if (aposs.size > 0) {
        if (add_possibility(client, &aposs)) {
            // Échec d'envoi : la pile n'est pas marquée, le travail reste local
            log_error("error on add_possibility\n");
        } else {
            for (int i = 0; i <= top; i++) {
                stack[i].next_s = new_next_s[i];
            }
            lastfilesize[client->compteur] = pending - aposs.size;
            if (hunger > 0) {
                // Faim consommée localement : évite que tous les threads du
                // processus cèdent pour la même annonce avant la prochaine
                // sonde (qui republiera la valeur fraîche du serveur).
                __atomic_fetch_sub(&server_hunger, (int)aposs.size, __ATOMIC_RELAXED);
            }
        }
    }
}

/**
 * @brief Renvoie au serveur tout le travail restant (arrêt demandé).
 *
 * Matérialise l'intégralité des frères non explorés de la pile, plus un paquet
 * représentant le chemin courant (prochaine case à étudier avec l'état actuel
 * du plateau), et envoie le tout via `add_possibility`.
 *
 * @param client      Contexte du thread client.
 * @param board       Plateau courant.
 * @param stack       Pile de décisions.
 * @param top         Indice du dernier niveau occupé.
 * @param start_depth Profondeur du paquet racine.
 * @param idParts     Table de pré-calcul des indices de rotation.
 */
static void bt_flush_pending(client_possibility_t *client,
                             struct possibility_packet *board,
                             bt_level *stack, int top, int start_depth,
                             int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    unsigned long long pending = bt_count_pending(board, stack, top);

    array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
    aposs->possibilities = malloc(sizeof(struct possibility_packet) * (pending + 1));
    int new_next_s[ETERN_PARTS];
    aposs->size = bt_materialize_pending(client, board, stack, top, start_depth,
                                         idParts, aposs->possibilities,
                                         (int)pending, new_next_s);

    // Le chemin courant lui-même : prochaine case à étudier avec le plateau actuel
    struct possibility_packet *cur = &aposs->possibilities[aposs->size];
    memcpy(cur, board, sizeof(*cur));
    cur->alloc = start_depth + top + 1;
    cur->x = dirx[cur->alloc];
    cur->y = diry[cur->alloc];
    // Des pièces ont pu être placées depuis la racine : contrôle pruner caduc
    cur->checked = 0;
    aposs->size++;

    if (add_possibility(client, aposs)) {
        log_error("Error on add_possibility \n");
    }
    free_array_possibility_packet(aposs);
}

/**
 * @brief Explore en profondeur le sous-arbre d'un paquet racine par backtracking in-place.
 *
 * Remplace la file de paquets de l'ancienne implémentation : un unique plateau
 * (copie locale du paquet racine) est modifié en place. Avancer = écrire une case
 * et positionner un bit ; reculer = effacer la case, libérer le bit et passer au
 * candidat suivant du niveau. Aucune copie de `possibility_packet` ni allocation
 * dans la boucle chaude. L'ordre de parcours (`directions[]`), le forward-checking
 * et les statistiques sont identiques à l'ancienne version : le même arbre est
 * exploré, seul le support mémoire change.
 *
 * Le format paquet n'est utilisé qu'aux frontières : délégation périodique du
 * surplus de travail (`bt_delegate_if_needed`) et arrêt (`bt_flush_pending`).
 *
 * @param client  Contexte du thread client.
 * @param root    Paquet racine à explorer (non modifié).
 * @param idParts Table de pré-calcul des indices de rotation [id][rotation].
 * @return        0 si le sous-arbre est entièrement exploré, 1 si arrêt demandé
 *                (le travail restant a été renvoyé au serveur).
 */
static int search_packet_backtracking(client_possibility_t *client,
                                      struct possibility_packet *root,
                                      int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    // Plateau unique modifié en place
    struct possibility_packet board;
    memcpy(&board, root, sizeof(board));

    // Cache de contraintes : clé de recherche de chaque case, maintenue
    // incrémentalement à chaque placement/retrait
    const int8_t all_face = (int8_t)client->map_part->sizearrayM;
    key_part constraints[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(constraints, &board, client->all_rotate_part, all_face);

    // Pile de décisions : un niveau par case explorée depuis la racine
    bt_level stack[ETERN_PARTS];
    int top = -1;
    const int start_depth = board.alloc;
    int noCheckDelegate = 0;
    // Date de la dernière délégation (0 = jamais : la première est autorisée)
    struct timespec last_delegate = {0, 0};

    // Statistique : le paquet racine compte comme une possibilité étudiée
    counters[client->compteur]++;

    for (;;) {
        // Prochaine case du parcours à remplir
        int depth = start_depth + top + 1;

        if (depth >= ETERN_PARTS) {
            board.alloc = depth;
            // Toutes les pièces sont placées : enregistre + signale au serveur.
            // Avec --stop-on-solution, record_solution ne revient pas (exit).
            // Sinon, on backtrack pour continuer à chercher d'autres solutions
            // (saut direct dans la remontée : pas de niveau ETERN_PARTS à empiler,
            // dirx/diry n'ont que ETERN_PARTS cases).
            record_solution(client, &board);
            goto backtrack;
        }

        if (request != REQUEST_CONTINUE) {
            useconds_t pause_us = request_is_pause(request);
            if (pause_us > 0) {
                usleep(pause_us);
                continue;
            }
            // REQUEST_STOP : renvoi du travail restant au serveur
            bt_flush_pending(client, &board, stack, top, start_depth, idParts);
            return 1;
        }

        // Volontairement == et non % : noCheckDelegate n'est écrit qu'ici (+1
        // puis remise à zéro au seuil), il ne peut donc jamais sauter par-dessus
        // le seuil. Les chemins qui esquivent cet incrément (continue sur
        // REQUEST_PAUSE, goto backtrack après une solution) ne font que retarder
        // son atteinte.
        noCheckDelegate++;
        if (noCheckDelegate == DELEGATE_CHECK_INTERVAL_NODES) {
            noCheckDelegate = 0;
            // La fréquence de délégation est bornée en temps et non en nombre de
            // nœuds : une délégation = jusqu'à max_stock_by_thread aller-retours
            // TCP synchrones exécutés par ce thread. Indexée sur les nœuds, elle
            // croîtrait avec la vitesse du moteur et mangerait le gain.
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long elapsed_ms = (now.tv_sec - last_delegate.tv_sec) * 1000LL
                                 + (now.tv_nsec - last_delegate.tv_nsec) / 1000000LL;
            if (elapsed_ms >= DELEGATE_MIN_INTERVAL_MS) {
                // Si trop d'étude à faire pour 1 thread, alors on délègue une partie
                bt_delegate_if_needed(client, &board, stack, top, start_depth, idParts);
                last_delegate = now;
            }
        }

        uint8_t x = dirx[depth];
        uint8_t y = diry[depth];

        top++;
        stack[top].next_s = 0;
        stack[top].placed_pos = -1;

        if (board.grid[x][y] != -2) {
            // Case déjà remplie (indice du paquet d'origine) : niveau sans décision
            stack[top].search = NULL;
            counters[client->compteur]++;
            if (depth + 1 > max_result) {
                max_result = depth + 1;
                best_board_try_record(&g_search_best_board, &board, (uint16_t)(depth + 1));
            }
            continue;
        }

        board.x = x;
        board.y = y;
        board.alloc = depth;
#ifdef DEBUG_CHECK_POSSIBILITY
        // Contrôle de cohérence du cache de contraintes face au recalcul complet
        {
            key_part recomputed;
            what_search_in_grid_to_key(client->all_rotate_part, &board, (int8_t)x, (int8_t)y, &recomputed, all_face);
            if (recomputed.k1 != constraints[x][y].k1 || recomputed.k2 != constraints[x][y].k2
                || recomputed.k3 != constraints[x][y].k3 || recomputed.k4 != constraints[x][y].k4) {
                log_error("constraints cache mismatch (%i,%i) : cache %i/%i/%i/%i recalc %i/%i/%i/%i\n",
                          x, y,
                          constraints[x][y].k1, constraints[x][y].k2, constraints[x][y].k3, constraints[x][y].k4,
                          recomputed.k1, recomputed.k2, recomputed.k3, recomputed.k4);
            }
        }
#endif // DEBUG_CHECK_POSSIBILITY
        stack[top].search = get_parts_bigarray_with_key(client->map_part, &constraints[x][y]);

backtrack:;
        // Place le prochain candidat du niveau courant, sinon remonte (backtrack).
        // Atteint aussi par `goto` après une solution (mode « continuer ») : on
        // repart du niveau courant, dont le placement gagnant sera annulé pour
        // essayer le candidat suivant.
        int placed = 0;
        while (top >= 0) {
            bt_level *lvl = &stack[top];
            int d = start_depth + top;
            uint8_t cx = dirx[d];
            uint8_t cy = diry[d];

            // Annulation du placement courant du niveau (reprise après backtrack)
            if (lvl->placed_pos >= 0) {
                board.grid[cx][cy] = -2;
                BOARD_SET_FACE(&board, lvl->placed_pos, 0);
                bt_propagate_undo(constraints, cx, cy, all_face);
                lvl->placed_pos = -1;
            }

            if (lvl->search != NULL) {
                struct array_part *search = lvl->search;
                for (int s = lvl->next_s; s < search->size; s++) {
                    if (search->parts[s].id == 0) {
                        continue;
                    }
                    int position = search->parts[s].id - 1;
                    // Si la piece n'est pas déjà utilisée, on a une possibilité supplémentaire
                    if (BOARD_FACE_USED(&board, position)) {
                        continue;
                    }
                    // On place la piece
                    board.grid[cx][cy] = idParts[search->parts[s].id][search->parts[s].rotation];
                    BOARD_SET_FACE(&board, position, 1);
                    bt_propagate_place(constraints, cx, cy, &search->parts[s]);
                    board.alloc = d + 1;
#if FORWARD_CHECK_K > 0
                    // Forward-checking : on inspecte les FORWARD_CHECK_K prochaines cases
                    // pour détecter une impasse immédiate
                    if (d + 1 < ETERN_PARTS) {
                        __atomic_fetch_add(&fc_attempts, 1, __ATOMIC_RELAXED);
                        if (!bt_forward_check(constraints, &board, client->map_part, d + 1)) {
                            board.grid[cx][cy] = -2;
                            BOARD_SET_FACE(&board, position, 0);
                            bt_propagate_undo(constraints, cx, cy, all_face);
                            __atomic_fetch_add(&fc_pruned, 1, __ATOMIC_RELAXED);
                            continue;
                        }
                    }
#endif // FORWARD_CHECK_K > 0
                    lvl->next_s = s + 1;
                    lvl->placed_pos = position;
                    placed = 1;
                    break;
                }
            }

            if (placed) {
                break;
            }
            // Niveau épuisé (ou case pré-remplie lors d'une remontée) : backtrack
            top--;
        }

        if (top < 0) {
            // Le sous-arbre du paquet racine est entièrement exploré
            return 0;
        }

        // Statistique possibilité étudiée + meilleur résultat
        counters[client->compteur]++;
        if (board.alloc > max_result) {
            max_result = board.alloc;
            best_board_try_record(&g_search_best_board, &board, board.alloc);
#ifdef DEBUG_CHECK_POSSIBILITY
            log_info("max result:%i\n", max_result);
#endif // DEBUG_CHECK_POSSIBILITY
        }
#ifdef DEBUG_CHECK_POSSIBILITY
        int analyse = check_possibility(&board, client->all_rotate_part);
        if (analyse < 0)
        {
            log_error("possibility error : %i\n", analyse);
            log_error(" ---");
            print_possibility_packet(&board);
        }
#endif // DEBUG_CHECK_POSSIBILITY
    }
}

static void init_id_parts(int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    for (int16_t p = 0; p <= ETERN_PARTS; p++) {
        int16_t base = p;
        for (int r = 0; r < PART_SIZES; r++) {
            idParts[p][r] = base;
            base = (int16_t)(base + ETERN_PARTS);
        }
    }
}

/**
 * @brief À l'arrêt (REQUEST_STOP), renvoie au serveur — ou au stock local si
 *        `server_ip == NULL` — les paquets racines `[from..size)` non traités.
 *
 * Extrait du corps de boucle d'autosearch pour être testable hors boucle infinie.
 */
static void requeue_unprocessed_packets(client_possibility_t *client, int from)
{
    if (client->aposs == NULL || from >= client->aposs->size) {
        return;
    }
    array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
    aposs->possibilities = malloc(sizeof(struct possibility_packet) * (client->aposs->size - from));
    aposs->size = 0;
    for (; from < client->aposs->size; from++)
    {
        memcpy(&aposs->possibilities[aposs->size], &client->aposs->possibilities[from], sizeof(struct possibility_packet));
        aposs->size++;
    }
    // En cas d'erreur, les possibilités sont remises en locale.
    if (add_possibility(client, aposs))
    {
        log_error("Error on add_possibility \n");
    }
    free_array_possibility_packet(aposs);
}

/**
 * @brief Exécute un tour de la boucle de recherche d'autosearch.
 *
 * Attend du travail, consomme les paquets racines de `client->aposs` (un
 * backtracking chacun), gère l'arrêt (REQUEST_STOP : renvoi du travail restant +
 * acquittement) puis nettoie le cycle. Extrait du corps de `while(1)` pour être
 * testable hors de la boucle infinie.
 *
 * @return 1 pour poursuivre la boucle, 0 pour s'arrêter (REQUEST_STOP).
 */
static int autosearch_step(client_possibility_t *client,
                           int16_t idParts[ETERN_PARTS + 1][PART_SIZES])
{
    // Attente d'un jeu de possibilité. `works` et `aposs` sont écrits ensemble
    // sous works_mutex par feed_one_thread (etii_client.c) ; on les relit tous
    // les deux sous ce même mutex, le usleep restant hors verrou pour ne pas
    // bloquer le feed. Les lire sans verrou serait un comportement indéfini en
    // C11 (accès concurrent non synchronisé à un objet non-atomique) : sur x86
    // le modèle mémoire fort masque le problème en pratique, mais sur ARM (le
    // GPU pruner tourne sur Jetson) rien ne garantit qu'un thread observant
    // `works == 1` voie déjà la mise à jour de `aposs` — une réorganisation
    // mémoire pourrait exposer un pointeur non encore publié. Le mutex donne
    // la paire acquire/release nécessaire, sans requérir de types _Atomic.
    // Même motif dans autoprune_step et (sous WITH_CUDA) autoprune_gpu.
    int works_snapshot;
    array_possibility_packet *aposs_snapshot;
    pthread_mutex_lock(&client->works_mutex);
    works_snapshot = client->works;
    aposs_snapshot = client->aposs;
    pthread_mutex_unlock(&client->works_mutex);
    while ((works_snapshot == 0 || aposs_snapshot == NULL) && request_keeps_running(request))
    {
        usleep(MICRO_SLEEP);
        pthread_mutex_lock(&client->works_mutex);
        works_snapshot = client->works;
        aposs_snapshot = client->aposs;
        pthread_mutex_unlock(&client->works_mutex);
    }

    // Consommation des possibilités demandées : un backtracking par paquet racine.
    // On continue d'utiliser client->aposs (et non aposs_snapshot) : une fois la
    // valeur observée non-NULL sous verrou, seul ce même thread la remet à NULL
    // (en fin de step) — aucune course possible sur les lectures qui suivent.
    int a = 0;
    int stopped = 0;
    while (client->aposs != NULL && a < client->aposs->size && !stopped)
    {
        stopped = search_packet_backtracking(client, &client->aposs->possibilities[a], idParts);
        a++;
    }

    if (request == REQUEST_STOP)
    {
#ifdef DEBUG_THREAD
        log_info("thread %i stop\n", client->pid);
#endif // DEBUG_THREAD
        // Renvoie au serveur les paquets racines non encore traités
        requeue_unprocessed_packets(client, a);
        // Acquittement inconditionnel : le thread d'alimentation est arrêté et
        // n'acquittera plus — sans cet envoi, le travail terminé de ce cycle
        // resterait « en analyse » sur le serveur pour toujours.
        send_possibility_analysed(client);
    }

    // A faire tout le temps ou juste si on arrete ?
    if (client->aposs != NULL) {
        free_array_possibility_packet(client->aposs);
        client->aposs = NULL;
    }
    pthread_mutex_lock(&client->works_mutex);
    client->works = 0;
    pthread_mutex_unlock(&client->works_mutex);
    lastfilesize[client->compteur] = 0;

    if (request == REQUEST_STOP) {
        return 0;
    }
    return 1;
}

/**
 * @brief Thread de recherche principale (worker de résolution du puzzle).
 *
 * Attend que `client->works == 1` puis consomme toutes les `possibility_packet`
 * du tableau `client->aposs`. Chaque paquet racine est exploré par backtracking
 * in-place (`search_packet_backtracking`) : un seul plateau par thread, aucune
 * copie de paquet dans la boucle chaude. Le surplus de travail est délégué au
 * serveur toutes les 1 000 000 itérations, et le travail restant lui est renvoyé
 * lors d'un arrêt (REQUEST_STOP).
 *
 * @param userdata Pointeur vers un `client_possibility_t` alloué par le parent.
 * @return         NULL.
 */
void *autosearch (void *userdata)
{
    client_possibility_t *client = userdata;
    int16_t idParts[ETERN_PARTS+1][PART_SIZES];
    init_id_parts(idParts);
#ifdef DEBUG_THREAD
    log_info("START search thread %i\n", client->pid);
#endif // DEBUG_THREAD
    // Boucle infinie pour maintenir le thread ; autosearch_step renvoie 0 sur REQUEST_STOP
    while (autosearch_step(client, idParts))
    {
        usleep(MICRO_SHORT_SLEEP);
    }
#ifdef DEBUG_THREAD
    log_info("END search thread %i\n", client->pid);
#endif // DEBUG_THREAD

    // Libération du buffer de délégation pré-alloué (paresseusement par
    // bt_delegate_if_needed ; NULL si ce thread n'a jamais délégué).
    if (client->delegate_buf != NULL) {
        free(client->delegate_buf);
        client->delegate_buf = NULL;
        client->delegate_buf_capacity = 0;
    }

    return NULL;
}

/**
 * @brief Exécute un tour de la boucle du pruner (autoprune).
 *
 * Attend du travail, contrôle chaque paquet de `client->aposs`
 * (`possibility_all_has_a_next` : vivant -> renvoyé marqué `checked`, mort ->
 * éliminé), gère l'arrêt (REQUEST_STOP : renvoi du lot non traité + acquittement)
 * puis nettoie le cycle. Extrait du corps de `while(1)` pour être testable hors
 * de la boucle infinie.
 *
 * @return 1 pour poursuivre la boucle, 0 pour s'arrêter (REQUEST_STOP).
 */
static int autoprune_step(client_possibility_t *client)
{
    // Attente d'un jeu de possibilité : works/aposs relus ensemble sous
    // works_mutex (cf. commentaire détaillé dans autosearch_step ci-dessus —
    // même risque théorique de réordonnancement mémoire, notamment sur ARM).
    int works_snapshot;
    array_possibility_packet *aposs_snapshot;
    pthread_mutex_lock(&client->works_mutex);
    works_snapshot = client->works;
    aposs_snapshot = client->aposs;
    pthread_mutex_unlock(&client->works_mutex);
    while ((works_snapshot == 0 || aposs_snapshot == NULL) && request_keeps_running(request))
    {
        usleep(MICRO_SLEEP);
        pthread_mutex_lock(&client->works_mutex);
        works_snapshot = client->works;
        aposs_snapshot = client->aposs;
        pthread_mutex_unlock(&client->works_mutex);
    }

    int a = 0;
    while (client->aposs != NULL && a < client->aposs->size && request != REQUEST_STOP)
    {
        useconds_t pause_us = request_is_pause(request);
        if (pause_us > 0)
        {
            usleep(pause_us);
            continue;
        }
        // Copie de travail : l'original doit rester intact pour l'acquittement
        struct possibility_packet work;
        memcpy(&work, &client->aposs->possibilities[a], sizeof(work));
        // Statistique possibilité étudiée (compteur de coups : une par
        // possibilité, sémantique historique). Les cases examinées par le
        // contrôle alimentent le flux DISJOINT `pruner_cells_studied`
        // (« dont prunage/s » et « études/s » des rapports check), avec un
        // minimum d'une case par possibilité (plateau déjà complet : le
        // balayage ne fait rien).
        counters[client->compteur]++;
        unsigned int cells_studied = 0;
        int has_next = possibility_all_has_a_next_counted(&work, client->map_part, client->all_rotate_part, &cells_studied);
        pruner_cells_studied += (cells_studied > 0) ? cells_studied : 1;
        if (work.alloc >= ETERN_PARTS)
        {
            // Plateau complété par les placements forcés du contrôle : solution.
            // Comme le pruner GPU : record_solution notifie le serveur
            // (INST_SOLUTION, synchrone) et ne sort qu'avec --stop-on-solution ;
            // sinon on poursuit le lot. Le plateau complet n'est pas remis en
            // circulation (plus rien à explorer). L'ancien checkIfResultFound
            // sortait inconditionnellement : serveur jamais prévenu (pas d'arrêt
            // ni de backup avec --stop-on-solution côté serveur), lot jamais
            // acquitté, mode « continuer » ignoré.
            record_solution(client, &work);
            a++;
            continue;
        }
        if (work.checked || has_next)
        {
            work.checked = 1;
            pruner_checked++;
            array_possibility_packet *alive = build_single_array_possibility_packet(&work);
            if (add_possibility(client, alive))
            {
                log_error("error on add_possibility (pruner)\n");
            }
            free_array_possibility_packet(alive);
        } else
        {
            // Branche morte : éliminée du stock
            pruner_removed++;
        }
        a++;
    }
    lastfilesize[client->compteur] = 0;

    if (request == REQUEST_STOP)
    {
#ifdef DEBUG_THREAD
        log_info("prune thread %i stop\n", client->pid);
#endif // DEBUG_THREAD
        // Renvoie au serveur les possibilités non encore vérifiées, telles quelles
        requeue_unprocessed_packets(client, a);
        // Acquittement inconditionnel : le lot traité (jusqu'à PRUNER_BATCH_SIZE
        // possibilités) doit être purgé du suivi « en analyse » du serveur,
        // le thread d'alimentation ne le fera plus après l'arrêt.
        send_possibility_analysed(client);
    }

    if (client->aposs != NULL) {
        free_array_possibility_packet(client->aposs);
        client->aposs = NULL;
    }
    pthread_mutex_lock(&client->works_mutex);
    client->works = 0;
    pthread_mutex_unlock(&client->works_mutex);

    if (request == REQUEST_STOP) {
        return 0;
    }
    return 1;
}

/**
 * @brief Thread de vérification d'un client pruner (mode `pruner`).
 *
 * Consomme les `possibility_packet` non vérifiées fournies par le serveur
 * (INST_GET_TO_CHECK, par lots de PRUNER_BATCH_SIZE). Pour chacune, contrôle
 * via `possibility_all_has_a_next` que toutes les cases vides restantes ont
 * encore au moins une pièce candidate :
 * - morte : éliminée (compteur `pruner_removed`) — elle ne retournera jamais
 *   dans le stock du serveur ;
 * - vivante : renvoyée au serveur marquée `checked = 1` (pool dédié, servi en
 *   priorité aux clients de recherche). Le contrôle place au passage les
 *   pièces forcées (cases à candidat unique) et détecte les solutions
 *   complètes.
 *
 * Le contrôle travaille sur une copie : l'original reste intact pour
 * l'acquittement INST_POSSIBILITY_ANALYSED (comparaison par contenu côté
 * serveur).
 *
 * @param userdata Pointeur vers un `client_possibility_t` alloué par le parent.
 * @return         NULL.
 */
void *autoprune (void *userdata)
{
    client_possibility_t *client = userdata;
#ifdef DEBUG_THREAD
    log_info("START prune thread %i\n", client->pid);
#endif // DEBUG_THREAD
    // Boucle infinie pour maintenir le thread ; autoprune_step renvoie 0 sur REQUEST_STOP
    while (autoprune_step(client))
    {
        usleep(MICRO_SHORT_SLEEP);
    }
#ifdef DEBUG_THREAD
    log_info("END prune thread %i\n", client->pid);
#endif // DEBUG_THREAD

    return NULL;
}

#ifdef WITH_CUDA
/**
 * @brief Thread de vérification d'un client pruner GPU (option `--gpu` du mode `pruner`).
 *
 * Variante de `autoprune` (etii_search.c) : au lieu de contrôler chaque paquet
 * individuellement via `possibility_all_has_a_next`, tout le lot
 * `client->aposs` est contrôlé par un seul `gpu_pruner_check_batch`. Le reste du
 * flux (renvoi des vivants marqués `checked = 1`, élimination des morts,
 * statistiques, gestion de l'arrêt) est identique au pruner CPU, qui reste
 * l'implémentation de référence.
 *
 * `gpu_pruner_init` doit avoir été appelé dans ce processus au préalable
 * (cf. run_mono_client).
 *
 * Le lot est traité de façon atomique (tout ou rien) : à la différence du
 * pruner CPU qui peut s'arrêter au milieu du lot, on contrôle l'ensemble en un
 * appel GPU (quelques microsecondes pour un lot de PRUNER_BATCH_SIZE). Sur
 * REQUEST_STOP avant traitement, le lot entier est renvoyé au serveur tel quel.
 *
 * @param userdata Pointeur vers un `client_possibility_t` alloué par le parent.
 * @return         NULL.
 */
void *autoprune_gpu (void *userdata)
{
    client_possibility_t *client = userdata;
#ifdef DEBUG_THREAD
    log_info("START gpu prune thread %i\n", client->pid);
#endif // DEBUG_THREAD
    while (1)
    {
        // Attente d'un jeu de possibilité : works/aposs relus ensemble sous
        // works_mutex (cf. commentaire détaillé dans autosearch_step —
        // même risque théorique de réordonnancement mémoire, notamment sur
        // ARM/Jetson, plateforme cible de ce pruner GPU).
        int works_snapshot;
        array_possibility_packet *aposs_snapshot;
        pthread_mutex_lock(&client->works_mutex);
        works_snapshot = client->works;
        aposs_snapshot = client->aposs;
        pthread_mutex_unlock(&client->works_mutex);
        while ((works_snapshot == 0 || aposs_snapshot == NULL) && request_keeps_running(request))
        {
            usleep(MICRO_SLEEP);
            pthread_mutex_lock(&client->works_mutex);
            works_snapshot = client->works;
            aposs_snapshot = client->aposs;
            pthread_mutex_unlock(&client->works_mutex);
        }

        int processed = 0;
        if (client->aposs != NULL && request != REQUEST_STOP)
        {
            // Respect d'une éventuelle limitation de débit
            useconds_t pause_us;
            while ((pause_us = request_is_pause(request)) > 0)
            {
                usleep(pause_us);
            }

            if (request != REQUEST_STOP)
            {
                int n = client->aposs->size;
                // Statistique : tout le lot est étudié (une par possibilité)
                counters[client->compteur] += n;

                // Lot de taille configurable (jusqu'à pruner_batch_size, borné par
                // PRUNER_BATCH_MAX) : verdicts vivant/mort alloués selon n, pas une
                // taille fixe (l'ancien tableau pile PRUNER_BATCH_SIZE débordait dès
                // qu'un lot dépassait 100).
                uint8_t *alive = malloc((size_t)n * sizeof(uint8_t));
                // Cases examinées par paquet, remontées par le kernel : une étude
                // de prunage par case (flux `pruner_cells_studied`).
                // NULL toléré : le comptage retombe alors sur 1 case par paquet.
                uint32_t *cells = malloc((size_t)n * sizeof(uint32_t));
                if (alive == NULL)
                {
                    log_error("gpu pruner : allocation alive (%d) impossible — lot renvoyé au serveur\n", n);
                    free(cells);
                }
                else
                {

#ifdef GPU_PRUNER_VERIFY
                // Vérification croisée : on garde une copie de l'entrée avant
                // mutation GPU pour rejouer le contrôle CPU et comparer.
                struct possibility_packet *snapshot =
                    malloc(sizeof(struct possibility_packet) * n);
                memcpy(snapshot, client->aposs->possibilities,
                       sizeof(struct possibility_packet) * n);
#endif // GPU_PRUNER_VERIFY

                gpu_pruner_check_batch(client->aposs->possibilities, n, alive, cells);

                // Statistique : les cases examinées alimentent le flux DISJOINT
                // `pruner_cells_studied` (« dont prunage/s » des rapports check),
                // avec un minimum d'une case par paquet (court-circuit `checked`,
                // échec kernel ou allocation `cells` impossible). Le compteur de
                // coups garde sa sémantique historique : une par possibilité.
                unsigned long long batch_cells = 0;
                for (int a = 0; a < n; a++)
                {
                    unsigned long long c = (cells != NULL && cells[a] > 0) ? cells[a] : 1;
                    batch_cells += c;
                }
                pruner_cells_studied += batch_cells;

#ifdef GPU_PRUNER_VERIFY
                for (int a = 0; a < n; a++)
                {
                    struct possibility_packet cpu;
                    memcpy(&cpu, &snapshot[a], sizeof(cpu));
                    int cpu_alive = cpu.checked
                        ? 1
                        : possibility_all_has_a_next(&cpu, client->map_part, client->all_rotate_part);
                    // Pas de checkIfResultFound ici : un plateau complété est
                    // traité par la boucle principale ci-dessous (record_solution,
                    // qui notifie le serveur) ; sortir pendant la vérification
                    // croisée tuerait le processus avant cet enregistrement.
                    if ((cpu_alive ? 1 : 0) != (alive[a] ? 1 : 0))
                    {
                        log_error("gpu_pruner VERIFY: divergence vivant/mort paquet %d : gpu=%d cpu=%d\n",
                                  a, alive[a], cpu_alive);
                    }
                    else if (cpu_alive)
                    {
                        int cmp = compare_possibility(&cpu, &client->aposs->possibilities[a]);
                        if (cmp != 0)
                        {
                            log_error("gpu_pruner VERIFY: divergence de contenu (%d) paquet %d\n", cmp, a);
                        }
                    }
                }
                free(snapshot);
#endif // GPU_PRUNER_VERIFY

                for (int a = 0; a < n; a++)
                {
                    if (alive[a])
                    {
                        struct possibility_packet *pk = &client->aposs->possibilities[a];
                        // Plateau complété par placements forcés : solution trouvée.
                        // Avec --stop-on-solution, record_solution ne revient pas.
                        // Sinon on l'enregistre et on ne la remet pas en circulation
                        // (un plateau complet n'a plus rien à explorer).
                        if (pk->alloc >= ETERN_PARTS)
                        {
                            record_solution(client, pk);
                            continue;
                        }
                        pruner_checked++;
                        array_possibility_packet *alivearr = build_single_array_possibility_packet(pk);
                        if (add_possibility(client, alivearr))
                        {
                            log_error("error on add_possibility (gpu pruner)\n");
                        }
                        free_array_possibility_packet(alivearr);
                    }
                    else
                    {
                        // Branche morte : éliminée du stock
                        pruner_removed++;
                    }
                }
                free(alive);
                free(cells);
                processed = 1;
                }
            }
        }
        lastfilesize[client->compteur] = 0;

        if (request == REQUEST_STOP)
        {
#ifdef DEBUG_THREAD
            log_info("gpu prune thread %i stop\n", client->pid);
#endif // DEBUG_THREAD
            if (client->aposs != NULL && !processed)
            {
                // Lot non traité : renvoyé au serveur tel quel
                array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
                aposs->possibilities = malloc(sizeof(struct possibility_packet) * client->aposs->size);
                aposs->size = 0;
                for (int a = 0; a < client->aposs->size; a++)
                {
                    memcpy(&aposs->possibilities[aposs->size], &client->aposs->possibilities[a], sizeof(struct possibility_packet));
                    aposs->size++;
                }
                if (add_possibility(client, aposs))
                {
                    log_error("Error on add_possibility \n");
                }
                free_array_possibility_packet(aposs);
            }
            // Acquittement inconditionnel du lot (cf. autoprune)
            send_possibility_analysed(client);
        }

        if (client->aposs != NULL) {
            free_array_possibility_packet(client->aposs);
            client->aposs = NULL;
        }
        pthread_mutex_lock(&client->works_mutex);
        client->works = 0;
        pthread_mutex_unlock(&client->works_mutex);

        if (request == REQUEST_STOP) {
            break;
        } else {
            usleep(MICRO_SHORT_SLEEP);
        }
    }
#ifdef DEBUG_THREAD
    log_info("END gpu prune thread %i\n", client->pid);
#endif // DEBUG_THREAD

    return NULL;
}
#endif // WITH_CUDA
