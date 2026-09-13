#include "tools/border_walk.h"

#include <string.h>
#include <stdlib.h>

void border_ring_order(int8_t ring[BORDER_RING_LEN][2])
{
    int i = 0;

    for (int x = 0; x < ETERN_SIZE; x++) {
        ring[i][0] = (int8_t)x;
        ring[i][1] = 0;
        i++;
    }
    for (int y = 1; y < ETERN_SIZE; y++) {
        ring[i][0] = (int8_t)(ETERN_SIZE - 1);
        ring[i][1] = (int8_t)y;
        i++;
    }
    for (int x = ETERN_SIZE - 2; x >= 0; x--) {
        ring[i][0] = (int8_t)x;
        ring[i][1] = (int8_t)(ETERN_SIZE - 1);
        i++;
    }
    for (int y = ETERN_SIZE - 2; y >= 1; y--) {
        ring[i][0] = 0;
        ring[i][1] = (int8_t)y;
        i++;
    }
}

static int bw_is_corner_cell(int x, int y)
{
    return (x == 0 || x == ETERN_SIZE - 1) && (y == 0 || y == ETERN_SIZE - 1);
}

void border_corners_first_order(int8_t order[BORDER_RING_LEN][2])
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    int k = 0;
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        if (bw_is_corner_cell(ring[i][0], ring[i][1])) {
            order[k][0] = ring[i][0];
            order[k][1] = ring[i][1];
            k++;
        }
    }
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        if (!bw_is_corner_cell(ring[i][0], ring[i][1])) {
            order[k][0] = ring[i][0];
            order[k][1] = ring[i][1];
            k++;
        }
    }
}

/* ===========================================================================
 * Chemin rapide : toute la bordure tient dans UN mot de 64 bits.
 *
 * Le jeu réel n'a que 60 pièces de bordure. L'ensemble des pièces encore
 * libres est donc un simple `uint64_t`, et l'ensemble des candidates à une
 * case est un masque précalculé — on remplace, par nœud, la construction de
 * clé (`what_search_in_grid_to_key`), le lookup dans la map et un test
 * `is_face_used` PAR CANDIDAT, par deux `AND` et une boucle `ctz`.
 *
 * C'est la transposition du `bucket_id_mask`/popcount du moteur principal
 * (`src/core/etii_search.c`), dans un cas plus favorable : là-bas 256 pièces
 * demandent 4 mots, ici 60 en demandent un seul.
 *
 * Trois masques par case de l'anneau, indexés sur la POSITION dans l'anneau
 * (pas sur l'ordre de parcours, qui peut poser les coins d'abord) :
 *   - `at`   : pièces dont la forme convient (coin sur un coin, bord sur un
 *              bord) — la rotation est alors forcée par la position, donc
 *              chaque pièce n'a qu'une orientation possible ici ;
 *   - `prev` : + face tournée vers le voisin d'anneau précédent == couleur ;
 *   - `next` : + face tournée vers le voisin d'anneau suivant == couleur.
 * Un voisin non encore posé n'impose rien : on n'intersecte que ce qui est
 * connu. La face intérieure n'entre jamais en jeu — le walker ne pose pas
 * l'intérieur, elle reste joker, exactement comme dans la version map.
 *
 * Repli : si le jeu compte plus de 64 pièces de bordure (ou zéro), `ok` reste
 * à 0 et bw_dfs reprend le chemin générique par la map. Les deux doivent
 * donner le MÊME compte — c'est ce que verrouillent les tests de comptage
 * existants (fixtures à 4 et 12 anneaux) et le recoupement avec la DP sur
 * data/pieces16.csv, qui est un algorithme indépendant.
 */
#define BW_MAX_FAST_PIECES 64

/* Point d'entrée test-only (même schéma que `stock_spill_set_segment_bytes_for_tests`)
   : force le repli sur le chemin générique par la map, pour qu'un test puisse
   comparer les DEUX implémentations sur le même jeu de pièces. Sans cet
   oracle, rien ne dirait qu'elles énumèrent le même ENSEMBLE — les comptes
   de fixtures (4, 12) le vérifient sur deux cas, cette bascule le vérifie
   sur n'importe lequel. */
static int bw_fastmap_enabled = 1;

void border_walk_set_fastmap_enabled_for_tests(int enabled)
{
    bw_fastmap_enabled = enabled;
}

struct bw_fastmap {
    int ok;
    int count;                                   /* nombre de pièces de bordure */
    int nb_colors;                               /* couleurs distinctes + 1 */
    uint64_t at[BORDER_RING_LEN];
    uint64_t *prev;                              /* [BORDER_RING_LEN][nb_colors] */
    uint64_t *next;
    uint16_t rotated[BORDER_RING_LEN][BW_MAX_FAST_PIECES]; /* slot -> id tourné, posé ici */
    int ring_index[ETERN_SIZE][ETERN_SIZE];      /* (x,y) -> position d'anneau, -1 hors anneau */
    int8_t ring[BORDER_RING_LEN][2];
};

/* Face de la pièce `g` tournée vers la case située à l'offset (dx,dy) DEPUIS
   elle. Toujours du point de vue de `g` : si la cible est à sa gauche, c'est
   sa face `left`. Les deux appelants s'en servent dans les deux sens, en
   choisissant le point de vue via l'offset qu'ils passent — l'un depuis la
   case courante vers son voisin, l'autre depuis le voisin vers la case
   courante. */
static inline int8_t bw_face_towards(struct array_part *all, int16_t g, int dx, int dy)
{
    const struct part *p = &all->parts[g];
    if (dx < 0) return p->left;
    if (dx > 0) return p->right;
    if (dy < 0) return p->top;
    return p->bottom;
}

static void bw_fastmap_free(struct bw_fastmap *fm)
{
    free(fm->prev);
    free(fm->next);
    fm->prev = NULL;
    fm->next = NULL;
}

static void bw_fastmap_build(struct bw_fastmap *fm, struct array_part *all)
{
    memset(fm, 0, sizeof *fm);
    if (!bw_fastmap_enabled) {
        return;
    }
    border_ring_order(fm->ring);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            fm->ring_index[x][y] = -1;
        }
    }
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        fm->ring_index[fm->ring[i][0]][fm->ring[i][1]] = i;
    }

    int n = (all->size - 1) / 4;
    int max_color = 0;
    uint16_t border_id[BW_MAX_FAST_PIECES];
    int count = 0;
    for (int id = 1; id <= n; id++) {
        const struct part *p = &all->parts[id];
        int8_t v[4] = { p->top, p->right, p->bottom, p->left };
        int zeros = 0;
        for (int k = 0; k < 4; k++) {
            if (v[k] == 0) zeros++;
            if (v[k] > max_color) max_color = v[k];
        }
        if (zeros != 1 && zeros != 2) {
            continue;
        }
        if (count == BW_MAX_FAST_PIECES) {
            return; /* plus de 64 : repli sur le chemin générique */
        }
        border_id[count++] = (uint16_t)id;
    }
    if (count == 0) {
        return;
    }

    fm->count = count;
    fm->nb_colors = max_color + 1;
    fm->prev = calloc((size_t)BORDER_RING_LEN * (size_t)fm->nb_colors, sizeof *fm->prev);
    fm->next = calloc((size_t)BORDER_RING_LEN * (size_t)fm->nb_colors, sizeof *fm->next);
    if (fm->prev == NULL || fm->next == NULL) {
        bw_fastmap_free(fm);
        return;
    }

    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int x = fm->ring[i][0], y = fm->ring[i][1];
        int ip = (i + BORDER_RING_LEN - 1) % BORDER_RING_LEN;
        int in = (i + 1) % BORDER_RING_LEN;
        int dpx = fm->ring[ip][0] - x, dpy = fm->ring[ip][1] - y;
        int dnx = fm->ring[in][0] - x, dny = fm->ring[in][1] - y;

        for (int slot = 0; slot < count; slot++) {
            uint16_t id = border_id[slot];
            /* La rotation est déterminée par la position : celle qui met une
               face nulle face à chaque bord du plateau adjacent. Vérifié
               empiriquement sur 3 M d'anneaux réels (zéro position où elle
               varie) — mais on la CHERCHE ici plutôt que de la supposer, et
               une pièce sans rotation valide à cette case est simplement
               absente du masque. */
            int found = -1;
            for (uint8_t rot = 0; rot < 4; rot++) {
                const struct part *rp = &all->parts[id_for_rotated_part(id, rot)];
                int ok = 1;
                if (y == 0 && rp->top != 0) ok = 0;
                if (x == ETERN_SIZE - 1 && rp->right != 0) ok = 0;
                if (y == ETERN_SIZE - 1 && rp->bottom != 0) ok = 0;
                if (x == 0 && rp->left != 0) ok = 0;
                if (ok) { found = (int)rot; break; }
            }
            if (found < 0) {
                continue;
            }
            int16_t g = (int16_t)id_for_rotated_part(id, (uint8_t)found);
            fm->rotated[i][slot] = (uint16_t)g;
            fm->at[i] |= (uint64_t)1 << slot;

            int8_t fp = bw_face_towards(all, g, dpx, dpy);
            int8_t fn = bw_face_towards(all, g, dnx, dny);
            if (fp >= 0 && fp < fm->nb_colors) {
                fm->prev[(size_t)i * fm->nb_colors + fp] |= (uint64_t)1 << slot;
            }
            if (fn >= 0 && fn < fm->nb_colors) {
                fm->next[(size_t)i * fm->nb_colors + fn] |= (uint64_t)1 << slot;
            }
        }
    }
    fm->ok = 1;
}

struct bw_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    const struct bw_fastmap *fm;   /* NULL = chemin générique par la map */
    uint64_t used;                 /* miroir 64 bits des slots occupés */
    const int8_t (*order)[2];
    struct possibility_packet state;
    long long count;
    border_ring_found_cb on_found;
    void *user_ctx;
    long long nodes;
    long long progress_since_last;
    const struct border_progress_opts *progress;
};

/* ATTENTION : `border_walk_expand_frontier` (plus bas dans ce fichier)
   réimplémente indépendamment la même logique de candidats à une case (le
   filtre `id <= 0`, le filtre `is_face_used`, le placement via
   `id_for_rotated_part`) — un changement ici doit être répercuté là-bas,
   sinon le total obtenu en parallélisant (frontière + reprise) diverge
   silencieusement du total séquentiel. Les tests
   `border_walk_expand_frontier_then_resume_matches_direct_count` et sa
   variante « coins d'abord » verrouillent que les deux restent en
   lockstep — s'ils échouent après une modification d'un seul des deux
   sites, c'est exactement ce dont il s'agit. */
/* Le décompte de progression (`ctx->nodes`/`ctx->progress`) est signalé en
   POST-ordre, à la toute fin de la fonction — après que la case courante
   (feuille ou nœud interne) a fini tout son travail, fermeture d'anneau
   comprise. Un signal en pré-ordre raterait de peu le nœud qui vient de
   fermer le dernier anneau : `ctx->count` n'y serait pas encore incrémenté
   au moment de l'appel. */
/* Retourne non nul si `on_found` a demandé l'arrêt : l'appelant doit alors
   remonter sans essayer le moindre candidat suivant. L'état du plateau
   (`ctx->state`) reste celui de l'anneau qui a déclenché l'arrêt — on ne
   défait pas les placements en remontant, personne ne le relit après. */
static int bw_dfs(struct bw_ctx *ctx, int i)
{
    int stop = 0;
    if (i == BORDER_RING_LEN) {
        ctx->count++;
        if (ctx->on_found != NULL) {
            stop = (ctx->on_found(&ctx->state, ctx->user_ctx) != 0);
        }
    } else if (ctx->fm != NULL) {
        /* Chemin rapide : deux AND et une boucle ctz, aucun test par
           candidat. Voir le commentaire de `struct bw_fastmap`. */
        const struct bw_fastmap *fm = ctx->fm;
        int8_t x = ctx->order[i][0];
        int8_t y = ctx->order[i][1];
        int pos = fm->ring_index[x][y];
        int ip = (pos + BORDER_RING_LEN - 1) % BORDER_RING_LEN;
        int in = (pos + 1) % BORDER_RING_LEN;

        uint64_t m = fm->at[pos] & ~ctx->used;

        int px = fm->ring[ip][0], py = fm->ring[ip][1];
        int16_t gp = ctx->state.grid[px][py];
        if (gp != -2) {
            int8_t c = bw_face_towards(ctx->all_rotate_parts, gp, x - px, y - py);
            m &= (c >= 0 && c < fm->nb_colors) ? fm->prev[(size_t)pos * fm->nb_colors + c] : 0;
        }
        int nx = fm->ring[in][0], ny = fm->ring[in][1];
        int16_t gn = ctx->state.grid[nx][ny];
        if (gn != -2) {
            int8_t c = bw_face_towards(ctx->all_rotate_parts, gn, x - nx, y - ny);
            m &= (c >= 0 && c < fm->nb_colors) ? fm->next[(size_t)pos * fm->nb_colors + c] : 0;
        }

        while (m != 0) {
            int slot = __builtin_ctzll(m);
            m &= m - 1;

            uint64_t bit = (uint64_t)1 << slot;
            int16_t g = (int16_t)fm->rotated[pos][slot];
            ctx->state.grid[x][y] = g;
            ctx->used |= bit;
            set_face_used(ctx->state.b_faceused, (uint16_t)((((g - 1) % ETERN_PARTS) + 1) - 1), 1);
            ctx->state.alloc = (uint16_t)(i + 1);

            stop = bw_dfs(ctx, i + 1);

            set_face_used(ctx->state.b_faceused, (uint16_t)((((g - 1) % ETERN_PARTS) + 1) - 1), 0);
            ctx->used &= ~bit;
            ctx->state.grid[x][y] = -2;
            ctx->state.alloc = (uint16_t)i;

            if (stop) {
                break;
            }
        }
    } else {
        int8_t x = ctx->order[i][0];
        int8_t y = ctx->order[i][1];

        key_part key;
        what_search_in_grid_to_key(ctx->all_rotate_parts, &ctx->state, x, y, &key,
                                    (int8_t)ctx->map->sizearrayM);
        map_bucket bucket = map_bucket_packed(ctx->map, &key);

        for (int s = 0; s < bucket.size; s++) {
            const struct part *cand = &bucket.parts[s];
            if (cand->id <= 0) {
                continue;
            }
            uint16_t face_idx = (uint16_t)(cand->id - 1);
            if (is_face_used(ctx->state.b_faceused, face_idx)) {
                continue;
            }

            ctx->state.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
            set_face_used(ctx->state.b_faceused, face_idx, 1);
            ctx->state.alloc = (uint16_t)(i + 1);

            stop = bw_dfs(ctx, i + 1);

            set_face_used(ctx->state.b_faceused, face_idx, 0);
            ctx->state.grid[x][y] = -2;
            ctx->state.alloc = (uint16_t)i;

            if (stop) {
                break;
            }
        }
    }

    ctx->nodes++;
    if (ctx->progress != NULL) {
        ctx->progress_since_last++;
        if (ctx->progress_since_last == ctx->progress->interval_nodes) {
            ctx->progress_since_last = 0;
            ctx->progress->on_progress(ctx->nodes, ctx->count, ctx->progress->ctx);
        }
    }
    return stop;
}

/* Cohérence de couleur — MÊME convention que check_possibility()
   (src/core/possibility.c), voir border_walk.h pour le détail et pour
   pourquoi cette fonction n'est pas appelée directement. */
static int bw_check_colors(const struct possibility_packet *p, struct array_part *all)
{
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            int16_t g = p->grid[x][y];
            if (g == -2) {
                continue;
            }
            if (g < 0 || g >= all->size) {
                return BORDER_RING_BAD_PIECE_ID;
            }
            struct part me = all->parts[g];

            /* 0 = hors plateau (la bordure doit y présenter une face nulle),
               -1 = voisin vide, donc rien à comparer. */
            int8_t want_top = 0, want_right = 0, want_bottom = 0, want_left = 0;
            if (y - 1 >= 0) {
                want_top = (p->grid[x][y - 1] < 0) ? -1 : all->parts[p->grid[x][y - 1]].bottom;
            }
            if (x + 1 < ETERN_SIZE) {
                want_right = (p->grid[x + 1][y] < 0) ? -1 : all->parts[p->grid[x + 1][y]].left;
            }
            if (y + 1 < ETERN_SIZE) {
                want_bottom = (p->grid[x][y + 1] < 0) ? -1 : all->parts[p->grid[x][y + 1]].top;
            }
            if (x - 1 >= 0) {
                want_left = (p->grid[x - 1][y] < 0) ? -1 : all->parts[p->grid[x - 1][y]].right;
            }
            if ((want_top != -1 && me.top != want_top) || (want_right != -1 && me.right != want_right) ||
                (want_bottom != -1 && me.bottom != want_bottom) ||
                (want_left != -1 && me.left != want_left)) {
                return BORDER_RING_BAD_COLOR;
            }
        }
    }
    return 0;
}

int border_ring_validate(const struct possibility_packet *ring, struct array_part *all_rotate_parts)
{
    if (possibility_placed_count(ring) != BORDER_RING_LEN) {
        return BORDER_RING_BAD_PLACED_COUNT;
    }

    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(order);

    /* Pas de balayage de l'intérieur : `possibility_placed_count` compte les
       cases non vides du PLATEAU ENTIER, donc « exactement BORDER_RING_LEN
       posées » (ci-dessus) et « les BORDER_RING_LEN cases du pourtour sont
       remplies » (ci-dessous) impliquent déjà qu'aucune case intérieure ne
       l'est. Une case intérieure posée sort en BAD_PLACED_COUNT, ou en
       BAD_EMPTY_CELL si elle a été déplacée depuis le pourtour — les deux
       sont couverts par `border_ring_validate_catches_each_kind_of_corruption`.
       Un contrôle dédié serait inatteignable, donc jamais testé. */
    int seen[ETERN_PARTS + 1];
    memset(seen, 0, sizeof seen);

    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int x = order[i][0], y = order[i][1];
        int16_t v = ring->grid[x][y];
        if (v == -2) {
            return BORDER_RING_BAD_EMPTY_CELL;
        }
        if (v < 0 || v >= all_rotate_parts->size) {
            return BORDER_RING_BAD_PIECE_ID;
        }
        int base = ((v - 1) % ETERN_PARTS) + 1;
        if (seen[base]) {
            return BORDER_RING_BAD_DUPLICATE_ID;
        }
        seen[base] = 1;
        /* `b_faceused` est indexé sur l'id de BASE (0-based), pas sur l'id
           tourné — cf. bw_dfs, qui fait `set_face_used(..., cand->id - 1)`. */
        if (!is_face_used((uint16_t *)ring->b_faceused, (uint16_t)(base - 1))) {
            return BORDER_RING_BAD_FACEUSED;
        }
    }

    return bw_check_colors(ring, all_rotate_parts);
}

long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx,
                                     const struct border_progress_opts *progress)
{
    struct bw_ctx bw;
    bw.map = map;
    bw.all_rotate_parts = all_rotate_parts;
    bw.order = order;

    if (start_state != NULL) {
        bw.state = *start_state;
    } else {
        memset(&bw.state, 0, sizeof bw.state);
        for (int x = 0; x < ETERN_SIZE; x++) {
            for (int y = 0; y < ETERN_SIZE; y++) {
                bw.state.grid[x][y] = -2;
            }
        }
        bw.state.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
    }

    struct bw_fastmap fm;
    bw_fastmap_build(&fm, all_rotate_parts);
    bw.fm = fm.ok ? &fm : NULL;

    /* `used` doit refléter l'état de DÉPART : une reprise depuis un état
       partiel (border_walk_expand_frontier) a déjà des pièces posées. */
    bw.used = 0;
    if (fm.ok) {
        for (int i2 = 0; i2 < BORDER_RING_LEN; i2++) {
            int16_t g = bw.state.grid[fm.ring[i2][0]][fm.ring[i2][1]];
            if (g == -2) {
                continue;
            }
            for (int slot = 0; slot < fm.count; slot++) {
                if (fm.rotated[i2][slot] == (uint16_t)g) {
                    bw.used |= (uint64_t)1 << slot;
                    break;
                }
            }
        }
    }

    bw.count = 0;
    bw.on_found = on_found;
    bw.user_ctx = ctx;
    bw.nodes = 0;
    bw.progress_since_last = 0;
    bw.progress = progress;

    (void)bw_dfs(&bw, start_depth);
    bw_fastmap_free(&fm);
    return bw.count;
}

long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    return border_walk_count_ordered(map, all_rotate_parts, ring, 0, NULL, on_found, ctx, NULL);
}

/* ATTENTION : réimplémente indépendamment la même logique de candidats que
   `bw_dfs` (plus haut dans ce fichier) — voir le commentaire sur `bw_dfs`
   pour pourquoi les deux doivent rester en lockstep. */
long long border_walk_expand_frontier(map_big_array *map,
                                       struct array_part *all_rotate_parts,
                                       const int8_t order[BORDER_RING_LEN][2],
                                       int target_partitions,
                                       border_partial_cb on_partial, void *partial_ctx,
                                       border_ring_found_cb on_complete, void *complete_ctx)
{
    struct possibility_packet *level = malloc(sizeof *level);
    int level_size = 1;
    memset(&level[0], 0, sizeof level[0]);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            level[0].grid[x][y] = -2;
        }
    }
    level[0].min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    long long completed = 0;
    int depth = 0;
    int stop = 0;

    while (!stop && level_size < target_partitions && depth < BORDER_RING_LEN) {
        int8_t x = order[depth][0];
        int8_t y = order[depth][1];

        struct possibility_packet *next_level = NULL;
        int next_size = 0;
        int next_cap = 0;

        for (int e = 0; e < level_size && !stop; e++) {
            struct possibility_packet *base = &level[e];
            key_part key;
            what_search_in_grid_to_key(all_rotate_parts, base, x, y, &key, (int8_t)map->sizearrayM);
            map_bucket bucket = map_bucket_packed(map, &key);

            for (int s = 0; s < bucket.size; s++) {
                const struct part *cand = &bucket.parts[s];
                if (cand->id <= 0) {
                    continue;
                }
                uint16_t face_idx = (uint16_t)(cand->id - 1);
                if (is_face_used(base->b_faceused, face_idx)) {
                    continue;
                }

                struct possibility_packet child = *base;
                child.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
                set_face_used(child.b_faceused, face_idx, 1);
                child.alloc = (uint16_t)(depth + 1);

                if (depth + 1 == BORDER_RING_LEN) {
                    completed++;
                    if (on_complete != NULL && on_complete(&child, complete_ctx) != 0) {
                        stop = 1;
                        break;
                    }
                    continue;
                }

                if (next_size == next_cap) {
                    next_cap = (next_cap == 0) ? 16 : next_cap * 2;
                    next_level = realloc(next_level, (size_t)next_cap * sizeof *next_level);
                }
                next_level[next_size++] = child;
            }
        }

        free(level);
        level = next_level;
        level_size = next_size;
        depth++;

        if (level_size == 0) {
            break;
        }
    }

    /* Arrêt demandé : aucun état partiel n'est livré. Les distribuer quand
       même ferait repartir des workers sur un travail dont l'appelant vient
       précisément de dire qu'il n'en voulait plus. */
    if (on_partial != NULL && !stop) {
        for (int e = 0; e < level_size; e++) {
            on_partial(&level[e], depth, partial_ctx);
        }
    }
    free(level);

    return completed;
}
