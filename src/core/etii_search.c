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
 * Chaque case explorée depuis le paquet racine occupe un niveau de pile. Un
 * niveau mémorise la case concernée, la liste de candidats de la map (pointeur
 * stable, la map est en lecture seule pendant la recherche) et la position de
 * reprise : le plateau lui-même est partagé et modifié en place.
 *
 * La case (`x`, `y`) est stockée plutôt que déduite de `dirx[depth]/diry[depth]`
 * : en ordre FIXE elle vaut exactement cela, mais en ordre DYNAMIQUE (MRV,
 * §4.7 de `docs/conception/elagage_recherche.md`) elle est choisie à chaque
 * nœud par `mrv_choose_cell`. C'est ce qui permet aux mécanismes de délégation
 * (`bt_count_pending`, `bt_materialize_pending`, `bt_flush_pending`) d'être
 * strictement les mêmes pour les deux moteurs — une seule sémantique de
 * délégation, testée une seule fois.
 */
typedef struct {
    /** Liste des candidats pour cette case (NULL = case pré-remplie ou sans issue, aucune décision). */
    struct array_part *search;
    /** Prochain indice de candidat à essayer dans `search` lors d'un retour sur ce niveau. */
    int next_s;
    /** Indice faceused (id-1) de la pièce actuellement placée à ce niveau, -1 si aucune. */
    int16_t placed_pos;
    /** Colonne de la case de ce niveau. */
    uint8_t x;
    /** Ligne de la case de ce niveau. */
    uint8_t y;
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
 * Inspecte les VOISINES géométriques de la pièce qu'on vient de placer en
 * `(cx, cy)` — au plus 4 (haut/droite/bas/gauche, cf. `bt_propagate_place`) —
 * plutôt que les `FORWARD_CHECK_K` prochaines cases du parcours `directions[]`.
 * Seul un placement modifie la clé de ses voisines directes ; une case du
 * parcours peut se trouver à des dizaines de cases de distance sans jamais
 * être une voisine (mesuré sur le puzzle 256 : 39 % des relations de
 * voisinage ne sont jamais couvertes par l'ancienne fenêtre K=6, retard de
 * détection médian 8 niveaux, max 152 — cf.
 * `docs/conception/elagage_recherche.md` §3.1/§4.1). Moins cher aussi : 1,9
 * case voisine à inspecter en moyenne contre 6 avec l'ancienne fenêtre.
 *
 * Ne lit PAS `FORWARD_CHECK_K` : ce paramètre ne borne plus que l'activation
 * du forward-checking (`#if FORWARD_CHECK_K > 0`), pas la taille d'une
 * fenêtre — le nombre de voisines est une propriété de la grille (4 au plus),
 * indépendante de tout réglage. La variante `forward_check_next_k`
 * (possibility.c) garde, elle, l'ancienne sémantique de fenêtre : elle ne
 * sert que les chemins froids (`bt_materialize_pending`, throttlé, et les
 * tests), pour lesquels une fenêtre de parcours reste un contrat valide et
 * testé indépendamment. Tout nouveau code de la boucle chaude doit passer
 * par ici.
 *
 * @param constraints Cache de contraintes maintenu par le backtracking.
 * @param board       Plateau courant (grille + masque des pièces utilisées).
 * @param mapParts    Table de lookup.
 * @param cx          Colonne de la pièce qu'on vient de placer.
 * @param cy          Ligne de la pièce qu'on vient de placer.
 * @return            1 si toutes les voisines vides ont au moins une pièce candidate, 0 sinon.
 */
static int bt_forward_check(key_part constraints[ETERN_SIZE][ETERN_SIZE],
                            struct possibility_packet *board,
                            map_big_array *mapParts, int cx, int cy)
{
    // Même ordre que bt_propagate_place/undo (haut, droite, bas, gauche) :
    // au plus 4 voisines, celles qui tombent dans la grille.
    int8_t nx[4];
    int8_t ny[4];
    int n = 0;
    if (cy > 0)              { nx[n] = (int8_t)cx;     ny[n] = (int8_t)(cy - 1); n++; }
    if (cx < ETERN_SIZE - 1) { nx[n] = (int8_t)(cx + 1); ny[n] = (int8_t)cy;     n++; }
    if (cy < ETERN_SIZE - 1) { nx[n] = (int8_t)cx;     ny[n] = (int8_t)(cy + 1); n++; }
    if (cx > 0)              { nx[n] = (int8_t)(cx - 1); ny[n] = (int8_t)cy;     n++; }

    // Cases réellement inspectées (statistique « études de prunage ») :
    // cumulées localement, un seul ajout atomique par appel (boucle chaude).
    unsigned int cells = 0;
    // Rang (1..4) de la voisine dans CETTE énumération, indexant fc_pruned_at
    // à l'élagage : ce n'est plus une distance de parcours, cf. son commentaire
    // dans static_variables.h.
    int rank = 0;

    for (int i = 0; i < n; i++) {
        int8_t x = nx[i];
        int8_t y = ny[i];

        // Case déjà remplie : on saute
        if (board->grid[x][y] != -2) {
            continue;
        }
        cells++;
        rank++;

        // Lookup via l'index COMPACT (`packed`) et non `flat` : à ce stade la
        // très grande majorité des accès ne sert qu'à lire une taille, et
        // `packed` divise par ~3,8 le volume balayé (cf. map_bucket_packed).
        // Résultat rigoureusement identique à get_parts_bigarray_with_key.
        map_bucket search = map_bucket_packed(mapParts, &constraints[x][y]);
        if (search.size == 0) {
            // case morte : aucune pièce candidate
            __atomic_fetch_add(&fc_pruned_at[rank], 1, __ATOMIC_RELAXED);
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
            __atomic_fetch_add(&fc_pruned_at[rank], 1, __ATOMIC_RELAXED);
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
 * @brief Re-canonise un paquet produit par une exploration en ordre DYNAMIQUE.
 *
 * Un paquet exploré en ordre MRV (§4.7) a des cases remplies un peu partout :
 * la profondeur de pile qui l'a produit n'a plus aucun rapport avec le curseur
 * de parcours `directions[]`. Or `alloc` EST ce curseur (cf. sa documentation
 * canonique dans `possibility.h`), et le seul invariant qu'un consommateur —
 * serveur, pruner, ou client à ordre fixe — est en droit d'attendre est :
 * toutes les cases d'index `< alloc` dans `directions[]` sont remplies. On
 * rétablit donc `alloc` = index de la PREMIÈRE case vide du parcours, ce que
 * `normalize_possibility_packet` sait déjà faire (et qui recale `x`/`y` dans
 * la foulée) : le paquet redevient indiscernable d'un paquet produit en ordre
 * fixe. Les cases remplies au-delà d'`alloc` sont exactement le cas déjà prévu
 * et documenté (« indices fixes », sautés par un niveau sans décision) —
 * aucune extension du format, donc aucun bump de `VERSION`.
 *
 * @param pkt Paquet à canoniser (modifié : `alloc`, `x`, `y`).
 * @return    1 si le plateau est COMPLET (aucune case vide : c'est une
 *            solution, pas un travail à déléguer), 0 sinon.
 */
static int bt_canonicalize_packet(struct possibility_packet *pkt)
{
    // normalize_possibility_packet n'abaisse `alloc` que s'il dépasse le
    // premier trou : on part donc du maximum pour qu'il le recale toujours.
    pkt->alloc = ETERN_PARTS;
    normalize_possibility_packet(pkt);
    return pkt->alloc >= ETERN_PARTS;
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
 * @param dynamic_order 0 : ordre fixe — `alloc` vaut la profondeur du niveau,
 *                      qui EST le curseur de parcours (invariant conservé de
 *                      proche en proche depuis un paquet racine canonique).
 *                      1 : ordre dynamique (MRV) — la profondeur de pile n'a
 *                      plus aucun rapport avec le curseur `directions[]`, le
 *                      paquet est donc RE-CANONISÉ (`bt_canonicalize_packet`)
 *                      avant d'être émis, ce qui le rend indiscernable, pour
 *                      tout autre client ou pour le serveur, d'un paquet
 *                      produit en ordre fixe — cf. §4.7 (« re-canonisation aux
 *                      frontières de délégation ») : c'est ce qui évite tout
 *                      bump de `VERSION`.
 * @return           Nombre de paquets effectivement matérialisés.
 */
static int bt_materialize_pending(client_possibility_t *client,
                                  const struct possibility_packet *board,
                                  const bt_level *stack, int top, int start_depth,
                                  int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                                  struct possibility_packet *out, int max_out,
                                  int *new_next_s, int dynamic_order)
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
            scratch.grid[lvl->x][lvl->y] = -2;
            BOARD_SET_FACE(&scratch, lvl->placed_pos, 0);
        }
        if (lvl->search != NULL) {
            uint8_t cx = lvl->x;
            uint8_t cy = lvl->y;
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
                if (dynamic_order) {
                    if (bt_canonicalize_packet(pkt)) {
                        // Plateau complet : même traitement qu'en ordre fixe.
                        record_solution(client, pkt);
                        continue;
                    }
                } else {
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
                }
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
 * @param dynamic_order Ordre de parcours du moteur appelant, transmis tel quel
 *                      à `bt_materialize_pending` (cf. sa doc).
 */
static void bt_delegate_if_needed(client_possibility_t *client,
                                  const struct possibility_packet *board,
                                  bt_level *stack, int top, int start_depth,
                                  int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                                  int dynamic_order)
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
                                        quota, new_next_s, dynamic_order);
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
 * @param dynamic_order Ordre de parcours du moteur appelant (cf.
 *                      `bt_materialize_pending`) : en ordre dynamique, le
 *                      paquet du chemin courant est lui aussi re-canonisé.
 */
static void bt_flush_pending(client_possibility_t *client,
                             struct possibility_packet *board,
                             bt_level *stack, int top, int start_depth,
                             int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                             int dynamic_order)
{
    unsigned long long pending = bt_count_pending(board, stack, top);

    array_possibility_packet *aposs = malloc(sizeof(array_possibility_packet));
    aposs->possibilities = malloc(sizeof(struct possibility_packet) * (pending + 1));
    int new_next_s[ETERN_PARTS];
    aposs->size = bt_materialize_pending(client, board, stack, top, start_depth,
                                         idParts, aposs->possibilities,
                                         (int)pending, new_next_s, dynamic_order);

    // Le chemin courant lui-même : prochaine case à étudier avec le plateau actuel
    struct possibility_packet *cur = &aposs->possibilities[aposs->size];
    memcpy(cur, board, sizeof(*cur));
    int complete = 0;
    if (dynamic_order) {
        complete = bt_canonicalize_packet(cur);
    } else {
        cur->alloc = start_depth + top + 1;
        cur->x = dirx[cur->alloc];
        cur->y = diry[cur->alloc];
    }
    // Des pièces ont pu être placées depuis la racine : contrôle pruner caduc
    cur->checked = 0;
    // Un plateau complet (cas défensif, ordre dynamique uniquement) n'est pas
    // un travail : rien à explorer au-delà, la solution a déjà été signalée.
    if (!complete) {
        aposs->size++;
    }

    if (add_possibility(client, aposs)) {
        log_error("Error on add_possibility \n");
    }
    free_array_possibility_packet(aposs);
}

/**
 * @brief Nombre de mots de 64 bits couvrant le masque des pièces utilisées.
 *
 * Dérivé de `FACES_USED_SIZE` (le masque du paquet, en groupes de 16 bits) et
 * non de `ETERN_PARTS` : 4 groupes du paquet tiennent dans un mot, le dernier
 * mot peut être partiellement rempli. Garantit `MRV_USED_WORDS >=
 * map->id_mask_words` pour toute map dont les ids tiennent dans le masque du
 * paquet — c'est-à-dire toute map de production.
 */
#define MRV_USED_WORDS ((FACES_USED_SIZE + 3) / 4)

/**
 * @brief Construit le miroir 64 bits du masque des pièces utilisées du plateau.
 *
 * `possibility_packet.b_faceused` est un masque en groupes de 16 bits (bit
 * `p & 15` du groupe `p >> 4`, pour `p = id - 1`) : parfait pour un test
 * unitaire, trop étroit pour compter par `popcount`. Ce miroir regroupe 4
 * groupes par mot de 64 bits — explicitement, par décalage, jamais par
 * réinterprétation de la mémoire du paquet (qui dépendrait de l'endianness de
 * la machine).
 *
 * Le miroir est ensuite maintenu en place par `mrv_used_set` / `mrv_used_clear`
 * à chaque pose/retrait, exactement comme le cache de contraintes : jamais
 * reconstruit dans la boucle chaude.
 *
 * @param used  Miroir à remplir (`MRV_USED_WORDS` mots).
 * @param board Plateau source.
 */
static void mrv_used_init(uint64_t used[MRV_USED_WORDS], const struct possibility_packet *board)
{
    for (int w = 0; w < MRV_USED_WORDS; w++) {
        used[w] = 0;
    }
    for (int g = 0; g < FACES_USED_SIZE; g++) {
        used[g / 4] |= (uint64_t)board->b_faceused[g] << ((g % 4) * 16);
    }
}

/** @brief Marque la pièce d'indice `position` (= id - 1) utilisée dans le miroir. */
static inline void mrv_used_set(uint64_t used[MRV_USED_WORDS], int position)
{
    used[position / 64] |= (uint64_t)1 << (position % 64);
}

/** @brief Marque la pièce d'indice `position` (= id - 1) libre dans le miroir. */
static inline void mrv_used_clear(uint64_t used[MRV_USED_WORDS], int position)
{
    used[position / 64] &= ~((uint64_t)1 << (position % 64));
}

/**
 * @brief Nombre de pièces ENCORE LIBRES candidates à une case, pour le choix MRV.
 *
 * Chemin rapide : `popcount` du masque d'ids du compartiment (`bucket_id_mask`,
 * construit une fois avec la map) contre le miroir des pièces utilisées —
 * indépendant de la TAILLE du compartiment, alors que le prototype de mesure
 * parcourait toutes ses entrées (jusqu'à plusieurs centaines). C'est le premier
 * des deux verrous de coût identifiés par §4.7 ; l'autre est la restriction du
 * balayage aux cases de frontière (`mrv_choose_cell`).
 *
 * Repli (map bâtie à la main dans un test, index compact absent, ou masque plus
 * large que le miroir) : comptage par parcours, résultat identique. Les deux
 * chemins comptent des IDENTIFIANTS distincts, jamais des entrées : une pièce
 * présente sous deux rotations dans le même compartiment compte une fois. Ce
 * choix est indifférent aux deux usages : `== 0` (case morte) est équivalent
 * dans les deux comptages, et le reste n'est qu'un critère d'ORDRE.
 *
 * @param map   Table de lookup.
 * @param key   Clé de la case.
 * @param board Plateau (masque des pièces utilisées, pour le repli).
 * @param used  Miroir 64 bits du même masque.
 * @return      Nombre de pièces distinctes candidates et libres (0 = case morte).
 */
static inline int mrv_free_candidates(const map_big_array *map, const key_part *key,
                                      struct possibility_packet *board,
                                      const uint64_t used[MRV_USED_WORDS])
{
    const uint64_t *mask = map_bucket_id_mask(map, key);
    if (mask != NULL && map->id_mask_words <= MRV_USED_WORDS) {
        return map_mask_free_count(mask, map->id_mask_words, used);
    }
    map_bucket bucket = map_bucket_packed(map, key);
    int count = 0;
    int16_t seen_last = 0;
    for (int s = 0; s < bucket.size; s++) {
        int16_t id = bucket.parts[s].id;
        if (id == 0 || id == seen_last || BOARD_FACE_USED(board, id - 1)) {
            continue;
        }
        // Les rotations d'une même pièce sont contiguës dans un compartiment
        // (cf. search_face) : ce filtre suffit à ne compter chaque id qu'une
        // fois sur le chemin de repli. Un doublon non contigu ne fausserait de
        // toute façon qu'un critère d'ordre, jamais le test de mort.
        seen_last = id;
        count++;
    }
    return count;
}

/**
 * @brief Choisit la case vide la plus contrainte (MRV, « minimum remaining
 *        values ») — §4.7 de `docs/conception/elagage_recherche.md`.
 *
 * Deux différences avec le prototype de mesure, qui coûtait un parcours de
 * compartiment pour CHACUNE des cases vides du plateau (jusqu'à 256) :
 *
 * 1. **Restriction aux cases de FRONTIÈRE** : une case dont les 4 côtés valent
 *    `all_face` n'est contrainte par rien (ni bord de plateau, ni voisine
 *    posée) et offre donc, par construction, toutes les pièces libres — elle ne
 *    peut jamais être le minimum tant qu'une case contrainte existe. Le test
 *    est une lecture du cache de contraintes déjà maintenu par le moteur, pas
 *    un lookup. Sur le puzzle 256 la frontière compte 29 cases en moyenne
 *    (max 52) contre 256 cases balayées par le prototype (§3.2).
 *    Il existe TOUJOURS une case de frontière tant qu'une case vide existe :
 *    la première case vide dans l'ordre lexicographique a soit un bord de
 *    plateau, soit une voisine de rang inférieur nécessairement remplie. Le
 *    repli `fallback` couvre malgré tout ce cas, plutôt que de le supposer.
 * 2. **Comptage par `popcount`** au lieu d'un parcours du compartiment
 *    (`mrv_free_candidates`).
 *
 * Le balayage reste COMPLET (pas d'arrêt anticipé sur une case à 1 candidat) :
 * sa détection de case morte, où qu'elle soit sur la frontière, est un
 * sous-produit gratuit et strictement plus large que le forward-check local —
 * on ne la sacrifie pas pour quelques cycles.
 *
 * @param board       Plateau courant.
 * @param constraints Cache de contraintes du moteur.
 * @param mapParts    Table de lookup.
 * @param used        Miroir 64 bits des pièces utilisées.
 * @param all_face    Valeur « toute face » (= map->sizearrayM).
 * @param out_x/out_y Remplis avec la case choisie si succès ; non modifiés sinon.
 * @return 1 si une case a été choisie, 0 si au moins une case vide n'a AUCUN
 *         candidat (branche morte détectée par le balayage lui-même).
 */
static int mrv_choose_cell(struct possibility_packet *board,
                           key_part constraints[ETERN_SIZE][ETERN_SIZE],
                           map_big_array *mapParts,
                           const uint64_t used[MRV_USED_WORDS],
                           int8_t all_face,
                           uint8_t *out_x, uint8_t *out_y)
{
    int best_count = -1;
    uint8_t best_x = 0, best_y = 0;
    int fallback_found = 0;
    uint8_t fallback_x = 0, fallback_y = 0;

    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (board->grid[x][y] != -2) {
                continue;
            }
            const key_part *key = &constraints[x][y];
            if (key->k1 == all_face && key->k2 == all_face
                && key->k3 == all_face && key->k4 == all_face) {
                // Case sans aucune contrainte : jamais le minimum (cf. doc).
                if (!fallback_found) {
                    fallback_found = 1;
                    fallback_x = (uint8_t)x;
                    fallback_y = (uint8_t)y;
                }
                continue;
            }
            int count = mrv_free_candidates(mapParts, key, board, used);
            if (count == 0) {
                // Case sans issue : sous-arbre mort, inutile de continuer.
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
            // Aucune case vide : l'appelant vérifie le plateau complet avant
            // d'appeler, ce retour n'est donc pas atteint en pratique.
            return 0;
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
 * @brief Issue de `search_packet_backtracking_core`.
 */
typedef enum {
    BT_CORE_EXHAUSTED = 0, /**< Sous-arbre entièrement exploré : mort, prouvé (solutions éventuelles déjà signalées). */
    BT_CORE_STOPPED = 1,   /**< REQUEST_STOP : arrêt demandé. */
    BT_CORE_BUDGET = 2,    /**< `node_budget` épuisé avant exhaustivité : statut indéterminé. */
} bt_core_result_t;

/**
 * @brief Cœur du backtracking in-place, factorisé pour servir deux usages : la
 *        recherche réelle illimitée (`search_packet_backtracking`) et la preuve
 *        de fermeture bornée en nœuds du pruner (`search_packet_backtracking_budgeted`,
 *        §4.6b de `docs/conception/elagage_recherche.md`).
 *
 * Un unique plateau (copie locale du paquet racine) est modifié en place.
 * Avancer = écrire une case et positionner un bit ; reculer = effacer la case,
 * libérer le bit et passer au candidat suivant du niveau. Aucune copie de
 * `possibility_packet` ni allocation dans la boucle chaude. L'ordre de parcours
 * (`directions[]`), le forward-checking et les statistiques de nœuds
 * (`counters`, `fc_attempts`/`fc_pruned`, `max_result`) sont identiques quel
 * que soit l'appelant : le même arbre est exploré par le même code, seuls le
 * plafond de nœuds et la délégation réseau diffèrent — c'est ce qui garantit
 * qu'une fermeture prouvée par la variante bornée est une VRAIE preuve
 * (aucune divergence de comportement possible entre les deux usages).
 *
 * @param client         Contexte du thread client.
 * @param root           Paquet racine à explorer (non modifié).
 * @param idParts        Table de pré-calcul des indices de rotation [id][rotation].
 * @param node_budget    Nombre maximal de nœuds à explorer avant de renoncer
 *                        (`BT_CORE_BUDGET`) ; `<= 0` = illimité (la recherche
 *                        réelle ne s'arrête jamais sur ce critère).
 * @param allow_delegate 1 : délégation périodique du surplus de travail
 *                        (`bt_delegate_if_needed`) et renvoi au serveur du
 *                        travail restant à l'arrêt (`bt_flush_pending`), comme
 *                        avant cette factorisation — usage recherche réelle.
 *                        0 : ni l'un ni l'autre. Déléguer une partie du
 *                        sous-arbre romprait la preuve de fermeture elle-même
 *                        (le budget n'aurait plus exploré tout ce qu'il
 *                        prétend avoir fermé) — usage preuve bornée du pruner,
 *                        où un arrêt (REQUEST_STOP) doit se contenter
 *                        d'abandonner l'exploration locale : l'appelant
 *                        retombe alors sur le comportement d'avant cette PR
 *                        (possibilité originale conservée intacte, `checked`).
 * @param out_nodes      Optionnel (NULL si non désiré) : reçoit le nombre de
 *                        nœuds explorés, quel que soit le statut de retour —
 *                        coût de la preuve, pour instrumentation/mesure.
 * @return               `BT_CORE_EXHAUSTED`, `BT_CORE_STOPPED` ou `BT_CORE_BUDGET`.
 */
static bt_core_result_t search_packet_backtracking_core(client_possibility_t *client,
                                      struct possibility_packet *root,
                                      int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                                      long node_budget,
                                      int allow_delegate,
                                      unsigned long long *out_nodes)
{
    // Plateau unique modifié en place
    struct possibility_packet board;
    memcpy(&board, root, sizeof(board));

    // Cache de contraintes : clé de recherche de chaque case, maintenue
    // incrémentalement à chaque placement/retrait
    const int8_t all_face = (int8_t)client->map_part->sizearrayM;
    key_part constraints[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(constraints, &board, client->all_rotate_part, all_face);

    // Miroir du masque des pièces utilisées : nécessaire au seul balayage
    // global de case morte (`global_dead_check`), donc entretenu uniquement
    // quand il est armé — l'ordre fixe historique ne paie rien.
    uint64_t used[MRV_USED_WORDS];
    if (global_dead_check) {
        mrv_used_init(used, &board);
    }

    // Pile de décisions : un niveau par case explorée depuis la racine
    bt_level stack[ETERN_PARTS];
    int top = -1;
    const int start_depth = board.alloc;
    int noCheckDelegate = 0;
    // Date de la dernière délégation (0 = jamais : la première est autorisée)
    struct timespec last_delegate = {0, 0};

    // Nœuds explorés (miroir local des incréments de `counters`, pour
    // `out_nodes` et le plafond `node_budget` — jamais lu hors de cette pile).
    unsigned long long nodes = 1;
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
            // REQUEST_STOP : renvoi du travail restant au serveur (recherche
            // réelle seulement — cf. doc de allow_delegate ci-dessus)
            if (allow_delegate) {
                bt_flush_pending(client, &board, stack, top, start_depth, idParts, 0);
            }
            if (out_nodes != NULL) {
                *out_nodes = nodes;
            }
            return BT_CORE_STOPPED;
        }

        if (allow_delegate) {
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
                    bt_delegate_if_needed(client, &board, stack, top, start_depth, idParts, 0);
                    last_delegate = now;
                }
            }
        }

        uint8_t x = dirx[depth];
        uint8_t y = diry[depth];

        top++;
        stack[top].next_s = 0;
        stack[top].placed_pos = -1;
        // En ordre fixe, la case du niveau EST dirx[depth]/diry[depth] : on la
        // mémorise quand même, pour que la pile ait la même forme qu'en ordre
        // dynamique et que la délégation soit rigoureusement le même code.
        stack[top].x = x;
        stack[top].y = y;

        if (board.grid[x][y] != -2) {
            // Case déjà remplie (indice du paquet d'origine) : niveau sans décision
            stack[top].search = NULL;
            counters[client->compteur]++;
            nodes++;
            if (depth + 1 > max_result) {
                max_result = depth + 1;
                best_board_try_record(&g_search_best_board, &board, (uint16_t)(depth + 1));
            }
            if (node_budget > 0 && nodes >= (unsigned long long)node_budget) {
                if (out_nodes != NULL) {
                    *out_nodes = nodes;
                }
                return BT_CORE_BUDGET;
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
            uint8_t cx = lvl->x;
            uint8_t cy = lvl->y;

            // Annulation du placement courant du niveau (reprise après backtrack)
            if (lvl->placed_pos >= 0) {
                board.grid[cx][cy] = -2;
                BOARD_SET_FACE(&board, lvl->placed_pos, 0);
                if (global_dead_check) {
                    mrv_used_clear(used, lvl->placed_pos);
                }
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
                    if (global_dead_check) {
                        mrv_used_set(used, position);
                    }
                    bt_propagate_place(constraints, cx, cy, &search->parts[s]);
                    board.alloc = d + 1;
#if FORWARD_CHECK_K > 0
                    // Forward-checking : on inspecte les voisines de la case
                    // qu'on vient de remplir pour détecter une impasse immédiate
                    if (d + 1 < ETERN_PARTS) {
                        __atomic_fetch_add(&fc_attempts, 1, __ATOMIC_RELAXED);
                        if (!bt_forward_check(constraints, &board, client->map_part, cx, cy)) {
                            board.grid[cx][cy] = -2;
                            BOARD_SET_FACE(&board, position, 0);
                            if (global_dead_check) {
                                mrv_used_clear(used, position);
                            }
                            bt_propagate_undo(constraints, cx, cy, all_face);
                            __atomic_fetch_add(&fc_pruned, 1, __ATOMIC_RELAXED);
                            continue;
                        }
                    }
#endif // FORWARD_CHECK_K > 0
                    // Balayage GLOBAL de case morte (expérience d'ablation, cf.
                    // `global_dead_check`) : exactement le sous-produit gratuit
                    // du choix de case MRV, mais ici on jette le choix et on ne
                    // garde que le test de mort — c'est ce qui sépare l'effet de
                    // l'ORDRE de celui de la PORTÉE de la détection.
                    if (global_dead_check && d + 1 < ETERN_PARTS) {
                        uint8_t gx, gy;
                        if (!mrv_choose_cell(&board, constraints, client->map_part,
                                             used, all_face, &gx, &gy)) {
                            board.grid[cx][cy] = -2;
                            BOARD_SET_FACE(&board, position, 0);
                            mrv_used_clear(used, position);
                            bt_propagate_undo(constraints, cx, cy, all_face);
#if FORWARD_CHECK_K > 0
                            // `fc_pruned` n'existe que si le forward-checking est
                            // compilé (cf. static_variables.h) ; la statistique est
                            // partagée avec lui parce que les deux comptent la même
                            // chose — un placement rejeté — mais le mécanisme, lui,
                            // ne dépend pas de FORWARD_CHECK_K.
                            __atomic_fetch_add(&fc_pruned, 1, __ATOMIC_RELAXED);
#endif // FORWARD_CHECK_K > 0
                            continue;
                        }
                    }
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
            if (out_nodes != NULL) {
                *out_nodes = nodes;
            }
            return BT_CORE_EXHAUSTED;
        }

        // Statistique possibilité étudiée + meilleur résultat
        counters[client->compteur]++;
        nodes++;
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
        if (node_budget > 0 && nodes >= (unsigned long long)node_budget) {
            if (out_nodes != NULL) {
                *out_nodes = nodes;
            }
            return BT_CORE_BUDGET;
        }
    }
}


/**
 * @brief Nombre de cases réellement remplies d'un plateau.
 *
 * Le moteur à ordre dynamique ne peut pas lire `board.alloc` pour cela : sur un
 * paquet re-canonisé (`bt_canonicalize_packet`), `alloc` est le curseur de
 * parcours, c'est-à-dire une BORNE INFÉRIEURE du nombre de pièces posées, pas
 * ce nombre. On compte donc les cases, une fois, à l'entrée du moteur.
 *
 * @param board Plateau à compter.
 * @return      Nombre de cases non vides.
 */
static int mrv_count_placed(const struct possibility_packet *board)
{
    int placed = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (board->grid[x][y] != -2) {
                placed++;
            }
        }
    }
    return placed;
}

/**
 * @brief Recherche à ordre de variable DYNAMIQUE (MRV) — §4.7 de
 *        `docs/conception/elagage_recherche.md`.
 *
 * Même contrat que `search_packet_backtracking_core` (même plateau unique
 * modifié en place, même pile de décisions `bt_level`, même forward-check après
 * placement, mêmes statistiques, même délégation) — une seule chose change :
 * la case traitée à chaque niveau est choisie par `mrv_choose_cell` (la plus
 * contrainte) au lieu d'être imposée par `dirx[depth]/diry[depth]`.
 *
 * Ce qui découle de ce seul changement, et qui distingue ce moteur du prototype
 * de mesure de la PR 9 (lequel ne déléguait jamais) :
 * - la profondeur de pile n'est plus le curseur de parcours, donc les paquets
 *   délégués sont RE-CANONISÉS avant émission (`bt_materialize_pending` avec
 *   `dynamic_order = 1`) — un client à ordre fixe, un pruner ou un `.back`
 *   n'y voient que du feu, d'où l'absence de bump de `VERSION` ;
 * - le nombre de pièces posées est compté explicitement (`mrv_count_placed`)
 *   puis maintenu, au lieu d'être lu dans `alloc` : un paquet reçu peut être
 *   troué (cases remplies au-delà du curseur), y compris s'il vient d'un autre
 *   client MRV ;
 * - `board.alloc` n'est mis à jour que là où il est OBSERVÉ (record de
 *   `max_result`, solution, délégation), jamais comme compteur de boucle.
 *
 * @param client         Contexte du thread client.
 * @param root           Paquet racine à explorer (non modifié).
 * @param idParts        Table de pré-calcul des indices de rotation.
 * @param node_budget    Plafond de nœuds (`<= 0` = illimité), même sémantique
 *                       que `search_packet_backtracking_core`.
 * @param allow_delegate 1 : délégation périodique + renvoi du travail restant
 *                       à l'arrêt ; 0 : ni l'un ni l'autre.
 * @param out_nodes      Optionnel : nombre de nœuds explorés.
 * @return               `BT_CORE_EXHAUSTED`, `BT_CORE_STOPPED` ou `BT_CORE_BUDGET`.
 */
static bt_core_result_t search_packet_backtracking_mrv(client_possibility_t *client,
                                                       struct possibility_packet *root,
                                                       int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                                                       long node_budget,
                                                       int allow_delegate,
                                                       unsigned long long *out_nodes)
{
    struct possibility_packet board;
    memcpy(&board, root, sizeof(board));

    const int8_t all_face = (int8_t)client->map_part->sizearrayM;
    key_part constraints[ETERN_SIZE][ETERN_SIZE];
    bt_init_constraints(constraints, &board, client->all_rotate_part, all_face);

    uint64_t used[MRV_USED_WORDS];
    mrv_used_init(used, &board);

    bt_level stack[ETERN_PARTS];
    int top = -1;
    // `start_depth` n'a plus de rôle d'ordonnancement ici : il ne sert qu'aux
    // fonctions de délégation partagées, dont la branche `dynamic_order` ne
    // l'utilise pas. Le vrai compteur de progression est `placed_count`.
    const int start_depth = board.alloc;
    int placed_count = mrv_count_placed(&board);
    int noCheckDelegate = 0;
    struct timespec last_delegate = {0, 0};

    unsigned long long nodes = 1;
    counters[client->compteur]++;

    for (;;) {
        if (placed_count >= ETERN_PARTS) {
            board.alloc = (uint16_t)placed_count;
            // Toutes les pièces sont placées : enregistre + signale au serveur.
            // Avec --stop-on-solution, record_solution ne revient pas (exit).
            record_solution(client, &board);
            goto backtrack;
        }

        if (request != REQUEST_CONTINUE) {
            useconds_t pause_us = request_is_pause(request);
            if (pause_us > 0) {
                usleep(pause_us);
                continue;
            }
            if (allow_delegate) {
                bt_flush_pending(client, &board, stack, top, start_depth, idParts, 1);
            }
            if (out_nodes != NULL) {
                *out_nodes = nodes;
            }
            return BT_CORE_STOPPED;
        }

        if (allow_delegate) {
            noCheckDelegate++;
            if (noCheckDelegate == DELEGATE_CHECK_INTERVAL_NODES) {
                noCheckDelegate = 0;
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long long elapsed_ms = (now.tv_sec - last_delegate.tv_sec) * 1000LL
                                     + (now.tv_nsec - last_delegate.tv_nsec) / 1000000LL;
                if (elapsed_ms >= DELEGATE_MIN_INTERVAL_MS) {
                    bt_delegate_if_needed(client, &board, stack, top, start_depth, idParts, 1);
                    last_delegate = now;
                }
            }
        }

        top++;
        stack[top].next_s = 0;
        stack[top].placed_pos = -1;

        uint8_t x, y;
        if (mrv_choose_cell(&board, constraints, client->map_part, used, all_face, &x, &y)) {
            stack[top].x = x;
            stack[top].y = y;
            stack[top].search = get_parts_bigarray_with_key(client->map_part, &constraints[x][y]);
        } else {
            // Case sans issue détectée par le balayage : niveau sans aucun
            // candidat à essayer — le backtrack normal (search == NULL) le
            // traite exactement comme un niveau épuisé.
            stack[top].x = 0;
            stack[top].y = 0;
            stack[top].search = NULL;
        }

backtrack:;
        int placed = 0;
        while (top >= 0) {
            bt_level *lvl = &stack[top];
            uint8_t cx = lvl->x;
            uint8_t cy = lvl->y;

            if (lvl->placed_pos >= 0) {
                board.grid[cx][cy] = -2;
                BOARD_SET_FACE(&board, lvl->placed_pos, 0);
                mrv_used_clear(used, lvl->placed_pos);
                bt_propagate_undo(constraints, cx, cy, all_face);
                lvl->placed_pos = -1;
                placed_count--;
            }

            if (lvl->search != NULL) {
                struct array_part *search = lvl->search;
                for (int s = lvl->next_s; s < search->size; s++) {
                    if (search->parts[s].id == 0) {
                        continue;
                    }
                    int position = search->parts[s].id - 1;
                    if (BOARD_FACE_USED(&board, position)) {
                        continue;
                    }
                    board.grid[cx][cy] = idParts[search->parts[s].id][search->parts[s].rotation];
                    BOARD_SET_FACE(&board, position, 1);
                    mrv_used_set(used, position);
                    bt_propagate_place(constraints, cx, cy, &search->parts[s]);
                    placed_count++;
#if FORWARD_CHECK_K > 0
                    if (placed_count < ETERN_PARTS) {
                        __atomic_fetch_add(&fc_attempts, 1, __ATOMIC_RELAXED);
                        if (!bt_forward_check(constraints, &board, client->map_part, cx, cy)) {
                            board.grid[cx][cy] = -2;
                            BOARD_SET_FACE(&board, position, 0);
                            mrv_used_clear(used, position);
                            bt_propagate_undo(constraints, cx, cy, all_face);
                            placed_count--;
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
            top--;
        }

        if (top < 0) {
            if (out_nodes != NULL) {
                *out_nodes = nodes;
            }
            return BT_CORE_EXHAUSTED;
        }

        counters[client->compteur]++;
        nodes++;
        // `alloc` porte ici le NOMBRE de pièces posées, la convention déjà
        // retenue par max_result/best_board (cf. la note du site symétrique de
        // search_packet_backtracking_core) — jamais un curseur de parcours :
        // tout paquet SORTANT est re-canonisé par bt_canonicalize_packet.
        board.alloc = (uint16_t)placed_count;
        if (board.alloc > max_result) {
            max_result = board.alloc;
            best_board_try_record(&g_search_best_board, &board, board.alloc);
#ifdef DEBUG_CHECK_POSSIBILITY
            log_info("max result:%i\n", max_result);
#endif // DEBUG_CHECK_POSSIBILITY
        }
#ifdef DEBUG_CHECK_POSSIBILITY
        {
            uint64_t expected[MRV_USED_WORDS];
            mrv_used_init(expected, &board);
            for (int w = 0; w < MRV_USED_WORDS; w++) {
                if (expected[w] != used[w]) {
                    log_error("miroir des pièces utilisées désynchronisé (mot %i)\n", w);
                }
            }
        }
#endif // DEBUG_CHECK_POSSIBILITY
        if (node_budget > 0 && nodes >= (unsigned long long)node_budget) {
            if (out_nodes != NULL) {
                *out_nodes = nodes;
            }
            return BT_CORE_BUDGET;
        }
    }
}

/**
 * @brief Explore en profondeur le sous-arbre d'un paquet racine par backtracking in-place.
 *
 * Fine enveloppe de `search_packet_backtracking_core` (illimité, délégation
 * autorisée) préservant la signature/le contrat historiques de cette fonction :
 * seule la recherche réelle (`autosearch_step`) l'appelle. Bascule vers le
 * moteur à ordre dynamique (`search_packet_backtracking_mrv`, §4.7) quand
 * `mrv_enabled` est levé — même contrat de retour, même délégation : le choix
 * de l'ordre est invisible de l'appelant.
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
    if (mrv_enabled) {
        bt_core_result_t r = search_packet_backtracking_mrv(client, root, idParts, -1, 1, NULL);
        return (r == BT_CORE_STOPPED) ? 1 : 0;
    }
    bt_core_result_t r = search_packet_backtracking_core(client, root, idParts, -1, 1, NULL);
    return (r == BT_CORE_STOPPED) ? 1 : 0;
}

/**
 * @brief Preuve de fermeture bornée en nœuds du sous-arbre d'une possibilité (§4.6b
 *        de `docs/conception/elagage_recherche.md`).
 *
 * Rejoue `root` par le même backtracking que la recherche réelle
 * (`search_packet_backtracking_core`), plafonné à `node_budget` nœuds et sans
 * délégation (`allow_delegate = 0`, cf. sa doc). Une condition nécessaire
 * exacte, pas une heuristique : si `BT_CORE_EXHAUSTED` est retourné, le
 * sous-arbre entier a été parcouru par le même code que la recherche fait
 * foi — aucun faux positif possible, exactement la même garantie qu'une
 * fermeture découverte par la recherche elle-même. `BT_CORE_BUDGET` et
 * `BT_CORE_STOPPED` signifient seulement « statut indéterminé dans ce budget » :
 * l'appelant doit alors traiter la possibilité comme avant cette PR (aucune
 * conclusion, ni positive ni négative, n'en découle).
 *
 * @param client      Contexte du thread client (pruner).
 * @param root        Possibilité à contrôler (non modifiée).
 * @param idParts     Table de pré-calcul des indices de rotation [id][rotation].
 * @param node_budget Plafond de nœuds (`<= 0` : appelant ne doit pas appeler
 *                    cette fonction — la budgétisation est désactivée en amont).
 * @param out_nodes   Optionnel (NULL si non désiré) : nœuds explorés, coût de
 *                    la preuve pour instrumentation.
 * @return            `BT_CORE_EXHAUSTED`, `BT_CORE_BUDGET` ou `BT_CORE_STOPPED`.
 */
static bt_core_result_t search_packet_backtracking_budgeted(client_possibility_t *client,
                                      struct possibility_packet *root,
                                      int16_t idParts[ETERN_PARTS + 1][PART_SIZES],
                                      long node_budget,
                                      unsigned long long *out_nodes)
{
    return search_packet_backtracking_core(client, root, idParts, node_budget, 0, out_nodes);
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
 * Depuis §4.6b (`docs/conception/elagage_recherche.md`) : une possibilité que
 * le contrôle superficiel juge vivante mais pas encore `checked` est en plus
 * soumise à `search_packet_backtracking_budgeted`, une preuve de fermeture par
 * backtracking RÉEL borné en nœuds (`pruner_dfs_budget`). Si le budget suffit
 * à épuiser tout le sous-arbre, la possibilité est prouvée morte au même titre
 * qu'une trouvaille de la recherche elle-même — éliminée, jamais redistribuée.
 * Sinon (budget épuisé, ou arrêt demandé en cours de preuve), comportement
 * inchangé : conservée, marquée `checked`. `pruner_dfs_budget <= 0` désactive
 * entièrement ce contrôle supplémentaire (même convention que `limit 0`).
 *
 * @return 1 pour poursuivre la boucle, 0 pour s'arrêter (REQUEST_STOP).
 */
static int autoprune_step(client_possibility_t *client)
{
    // Table de pré-calcul des rotations, nécessaire uniquement à la preuve de
    // fermeture bornée ci-dessous (search_packet_backtracking_budgeted).
    // Recalculée à chaque appel (un par lot, pas par possibilité) : coût
    // négligeable (ETERN_PARTS+1 * PART_SIZES affectations d'int16_t) au
    // regard d'un aller-retour TCP de lot.
    int16_t idParts[ETERN_PARTS + 1][PART_SIZES];
    init_id_parts(idParts);

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
        if (!work.checked && has_next && pruner_dfs_budget > 0)
        {
            // Vivant selon le contrôle superficiel mais pas encore `checked` :
            // tenter la preuve de fermeture bornée avant de se résigner à
            // conserver. Jamais tentée quand `work.checked` est déjà vrai (cf.
            // §4.6b : ce drapeau court-circuite tout, comportement d'avant
            // cette PR préservé à l'identique) ni quand le contrôle
            // superficiel a déjà tranché « mort » (has_next == 0, cas déjà
            // traité ci-dessous sans le coût d'un backtracking).
            unsigned long long dfs_nodes = 0;
            bt_core_result_t dfs = search_packet_backtracking_budgeted(
                client, &work, idParts, pruner_dfs_budget, &dfs_nodes);
            pruner_dfs_nodes += dfs_nodes;
            if (dfs == BT_CORE_EXHAUSTED)
            {
                // Sous-arbre entier prouvé mort (ou déjà entièrement soldé :
                // toute solution qu'il contenait a été signalée au passage par
                // search_packet_backtracking_core) : éliminée, comme une
                // branche morte du contrôle superficiel.
                pruner_dfs_closed++;
                pruner_removed++;
                a++;
                continue;
            }
            // BT_CORE_BUDGET ou BT_CORE_STOPPED : statut indéterminé, on
            // retombe sur le comportement historique ci-dessous.
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
