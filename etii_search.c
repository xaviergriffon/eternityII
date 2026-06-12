#include "etii_search.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "static_variables.h"
#include "etii_client.h"
#include "datamanager.h"
#include "possibility.h"

// Accès au masque des pièces utilisées, indépendant de FACES_USED_BITS
#ifdef FACES_USED_BITS
#define BOARD_FACE_USED(b, pos)   is_face_used((b)->b_faceused, (pos))
#define BOARD_SET_FACE(b, pos, v) set_face_used((b)->b_faceused, (pos), (v))
#else
#define BOARD_FACE_USED(b, pos)   ((b)->faceused[(pos)])
#define BOARD_SET_FACE(b, pos, v) ((b)->faceused[(pos)] = (v))
#endif // FACES_USED_BITS

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
    if(db->size > max_stock_by_thread)
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
 * @brief Délègue au serveur les possibilités excédant `max_stock_by_thread` dans un `big_table`.
 *
 * Variante de `checkAndDelegatePossibilitiesIfNeeded` utilisant un `big_table`
 * au lieu d'une `File`.
 *
 * @param client_possibility Contexte du thread client.
 * @param bt                 Table de grande capacité dont on contrôle la taille.
 */
void checkAndDelegatePossibilitiesIfNeeded_with_big_table(client_possibility_t *client_possibility, big_table *bt) {
    if(bt->size > max_stock_by_thread)
    {
        array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
        unsigned long long remains = bt->size - max_stock_by_thread;
        aposs->possibilities = malloc(sizeof(struct possibility_packet) * (max_stock_by_thread));
        aposs->size = 0;
        while(bt->size > remains)
        {
            scroll_big_table(bt, &aposs->possibilities[aposs->size]);

            aposs->size++;
        }
        if(add_possibility(client_possibility, aposs))
        {
            log_error("error on add_possibility — remise en table locale\n");
            for(int i = aposs->size - 1; i >= 0; i--) {
                put_big_table(bt, &aposs->possibilities[i]);
            }
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
 * @brief Matérialise en paquets les frères non explorés de la pile, du plus haut de l'arbre vers le bas.
 *
 * Reconstruit l'état du plateau à chaque niveau (annulation puis ré-application
 * des placements sur une copie de travail) et produit, pour chaque candidat
 * restant, le `possibility_packet` que l'ancienne implémentation aurait poussé
 * dans sa file : pièce placée, `alloc` incrémenté, position sur la case suivante,
 * forward-checking appliqué. Les niveaux les moins profonds sont matérialisés en
 * premier : on cède le haut de l'arbre (gros sous-arbres) et on garde le bas.
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

    // Retour à l'état racine
    for (int i = top; i >= 0; i--) {
        if (stack[i].placed_pos >= 0) {
            int d = start_depth + i;
            scratch.grid[dirx[d]][diry[d]] = -2;
            BOARD_SET_FACE(&scratch, stack[i].placed_pos, 0);
        }
    }

    int count = 0;
    int i;
    for (i = 0; i <= top && count < max_out; i++) {
        int d = start_depth + i;
        const bt_level *lvl = &stack[i];
        new_next_s[i] = lvl->next_s;
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
                    // Solution complète : sauvegarde et quitte le processus
                    checkIfResultFound(pkt, client->all_rotate_part);
                }
                pkt->x = dirx[d + 1];
                pkt->y = diry[d + 1];
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
        // Ré-application du placement courant du niveau
        if (lvl->placed_pos >= 0) {
            scratch.grid[dirx[d]][diry[d]] = board->grid[dirx[d]][diry[d]];
            BOARD_SET_FACE(&scratch, lvl->placed_pos, 1);
        }
    }
    // Niveaux non parcourus (limite atteinte) : positions de reprise inchangées
    for (; i <= top; i++) {
        new_next_s[i] = stack[i].next_s;
    }
    return count;
}

/**
 * @brief Délègue au serveur une partie du stock implicite si celui-ci dépasse `max_stock_by_thread`.
 *
 * Équivalent backtracking de `checkAndDelegatePossibilitiesIfNeeded_with_big_table` :
 * le stock est compté dans la pile de décisions, et au plus `max_stock_by_thread`
 * frères non explorés (les moins profonds) sont matérialisés puis envoyés.
 * Les niveaux délégués ne sont marqués consommés qu'après un envoi réussi.
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
    if (pending <= (unsigned long long)max_stock_by_thread) {
        return;
    }

    array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
    aposs->possibilities = malloc(sizeof(struct possibility_packet) * max_stock_by_thread);
    int new_next_s[ETERN_PARTS];
    aposs->size = bt_materialize_pending(client, board, stack, top, start_depth,
                                         idParts, aposs->possibilities,
                                         max_stock_by_thread, new_next_s);
    if (aposs->size > 0) {
        if (add_possibility(client, aposs)) {
            // Échec d'envoi : la pile n'est pas marquée, le travail reste local
            log_error("error on add_possibility\n");
        } else {
            for (int i = 0; i <= top; i++) {
                stack[i].next_s = new_next_s[i];
            }
            lastfilesize[client->compteur] = pending - aposs->size;
        }
    }
    free_array_possibility_packet(aposs);
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

    // Pile de décisions : un niveau par case explorée depuis la racine
    bt_level stack[ETERN_PARTS];
    int top = -1;
    const int start_depth = board.alloc;
    key_part key;
    int noCheckDelegate = 0;

    // Statistique : le paquet racine compte comme une possibilité étudiée
    counters[client->compteur]++;

    for (;;) {
        // Prochaine case du parcours à remplir
        int depth = start_depth + top + 1;

        if (depth >= ETERN_PARTS) {
            board.alloc = depth;
            // Toutes les pièces sont placées : sauvegarde et quitte le processus
            checkIfResultFound(&board, client->all_rotate_part);
        }

        if (request != REQUEST_CONTINUE) {
            if (request == REQUEST_PAUSE) {
                usleep(MICRO_SHORT_SLEEP);
                continue;
            }
            // REQUEST_STOP : renvoi du travail restant au serveur
            bt_flush_pending(client, &board, stack, top, start_depth, idParts);
            return 1;
        }

        noCheckDelegate++;
        // TODO : voir pour calculer 1/2s (vitesse/s / 2)
        if (noCheckDelegate == 1000000) {
            // Si trop d'étude à faire pour 1 thread, alors on délègue une partie
            bt_delegate_if_needed(client, &board, stack, top, start_depth, idParts);
            noCheckDelegate = 0;
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
            }
            continue;
        }

        board.x = x;
        board.y = y;
        board.alloc = depth;
        what_search_to_key2(client->all_rotate_part, &board, &key, client->map_part->sizearrayM);
        stack[top].search = get_parts_bigarray_with_key(client->map_part, &key);

        // Place le prochain candidat du niveau courant, sinon remonte (backtrack)
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
                    board.alloc = d + 1;
#if FORWARD_CHECK_K > 0
                    // Forward-checking : on inspecte les FORWARD_CHECK_K prochaines cases
                    // pour détecter une impasse immédiate
                    if (d + 1 < ETERN_PARTS) {
                        __atomic_fetch_add(&fc_attempts, 1, __ATOMIC_RELAXED);
                        if (!forward_check_next_k(&board, client->map_part, client->all_rotate_part)) {
                            board.grid[cx][cy] = -2;
                            BOARD_SET_FACE(&board, position, 0);
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
    for(int p=0; p <= ETERN_PARTS; p++) {
        int base = p;
        for(int r=0; r < PART_SIZES; r++) {
            idParts[p][r] = base;
            base += ETERN_PARTS;
        }
    }
#ifdef DEBUG_THREAD
    log_info("START search thread %i\n", client->pid);
#endif // DEBUG_THREAD
    // Boucle infinie pour maintenir le thread
    while(1)
    {
        // Attente d'un jeu de possibilité
        while ((client->works == 0 || client->aposs == NULL) && (request == REQUEST_CONTINUE || request == REQUEST_PAUSE))
        {
            usleep(MICRO_SLEEP);
        }

        // Consommation des possibilités demandées : un backtracking par paquet racine
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
            int leftovers = stopped;
            // Renvoie au serveur les paquets racines non encore traités
            if (client->aposs != NULL && a < client->aposs->size)
            {
                array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
                aposs->possibilities = malloc(sizeof(struct possibility_packet) * (client->aposs->size - a));
                aposs->size = 0;
                for (; a < client->aposs->size; a++)
                {
                    memcpy(&aposs->possibilities[aposs->size], &client->aposs->possibilities[a], sizeof(struct possibility_packet));
                    aposs->size++;
                }
                // En cas d'erreur, les possibilités sont remises en locale.
                if (add_possibility(client, aposs))
                {
                    log_error("Error on add_possibility \n");
                }
                free_array_possibility_packet(aposs);
                leftovers = 1;
            }
            if (leftovers)
            {
                send_possibility_analysed(client);
            }
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
            break;
        } else {
            usleep(MICRO_SHORT_SLEEP);
        }
    }
#ifdef DEBUG_THREAD
    log_info("END search thread %i\n", client->pid);
#endif // DEBUG_THREAD

    return NULL;
}
