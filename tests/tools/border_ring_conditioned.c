/**
 * @file border_ring_conditioned.c
 * @brief Mesure A de l'étude « recherche par anneaux intérieurs avec
 * bordure différée » (plan `etudies-les-besoins-la-toasty-starfish.md`,
 * voir aussi docs/conception/recherche_interieur_budget_couleur.md pour
 * l'antériorité).
 *
 * Question posée : `border_ring_dp` (branche non mergée
 * `border-mass-walker-design`) a mesuré que la masse INCONDITIONNELLE des
 * anneaux de bordure valides dépasse 9,2×10^18 — mais son DP laisse
 * explicitement la couleur intérieure de chaque pièce de bord en wildcard,
 * jamais vérifiée. Si l'on CONNAÎT le premier anneau intérieur (52 cases),
 * chacune des 56 positions de bordure non-coin a une couleur intérieure
 * exigée et FIXÉE POSITIONNELLEMENT (pas juste comptée en agrégat comme le
 * check déjà mesuré et clos dans recherche_interieur_budget_couleur.md).
 * Combien de complétions de bordure restent alors valides ?
 *
 * Outil de comptage seul : n'écrit rien, ne modifie aucun résultat, ne
 * dépend d'aucune branche non mergée (géométrie et DP réimplémentés ici,
 * indépendamment de border_ring_dp/border_walk).
 *
 * Usage :
 *   make border-ring-conditioned
 *   tests/tools/border_ring_conditioned data/pieces.csv eternityII.back [options]
 *   tests/tools/border_ring_conditioned data/pieces.csv eternityII.back --selftest
 *
 * Options :
 *   --max-roots <n>     racines à premier anneau complet échantillonnées (défaut 50)
 *   --node-budget <n>   budget de nœuds DFS par racine (défaut 5000000)
 *   --leaf-cap <n>      arrête le comptage d'une racine une fois ce nombre de
 *                       complétions trouvées (défaut 2000000) — suffisant pour
 *                       conclure "loin de 9,2e18" sans épuiser le budget
 *   --reverse-max <n>        bordures réelles complètes examinées en passe 4
 *                            (défaut : toutes)
 *   --reverse-node-budget <n> budget de nœuds DFS par bordure, passe 4 (défaut 3000000)
 *   --reverse-leaf-cap <n>    complétions d'anneau à trouver par bordure avant
 *                            d'arrêter, passe 4 (défaut 3 — l'existence suffit)
 *   --witness-node-budget <n> budget de nœuds du DFS-témoin, passe 4 (défaut 30000000)
 *   --witness-leaf-cap <n>    plafond de complétions du DFS-témoin, passe 4 (défaut 5000000)
 *   --selftest          valide la géométrie/l'appariement sur les données
 *                       réelles du stock (pas de DFS pleine échelle), puis
 *                       s'arrête
 *
 * Passe 4 (contrôle inverse) : sens opposé des passes 1-3. Prend chaque
 * bordure RÉELLE complète du stock (60/60), tente de remplir le premier
 * anneau intérieur à partir de zéro (sans regarder l'intérieur réellement
 * posé dans ce paquet), puis reboucle l'anneau obtenu dans la méthode des
 * passes 1-3 (extraction de demande + DFS de bordure) pour vérifier que
 * cette méthode retrouve bien la bordure réelle qui a servi à le
 * construire — sinon un 0/230 en passes 2/3 pourrait être un bug de
 * méthode plutôt qu'un verrou réel du puzzle.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/part.h"
#include "core/possibility.h"
#include "core/readdata.h"

/* ===========================================================================
 * Géométrie de l'anneau de bordure (60 cases, 4 coins + 56 de bord),
 * réimplémentée ici en autonomie — pas de dépendance à border_walk.h
 * (branche border-mass-walker-design, non mergée, absente de master).
 * ------------------------------------------------------------------------- */

#define BRC_RING_LEN (4 * (ETERN_SIZE - 1))

enum { BRC_TOP = 0, BRC_RIGHT = 1, BRC_BOTTOM = 2, BRC_LEFT = 3 };
static const int brc_dx[4] = { 0, 1, 0, -1 };
static const int brc_dy[4] = { -1, 0, 1, 0 };
static const int brc_opposite[4] = { 2, 3, 0, 1 };

typedef struct {
    int8_t x, y;
    int8_t is_corner;
    uint8_t outward_mask; /* bits BRC_TOP.. : faces devant valoir 0 */
    int8_t inward_dir;    /* direction vers le 1er anneau intérieur, -1 si coin */
    int8_t prev_dir;      /* direction vers g_ring[i-1] */
    int8_t next_dir;      /* direction vers g_ring[i+1] */
} brc_ring_pos_t;

static brc_ring_pos_t g_ring[BRC_RING_LEN];

static int brc_dir_between(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    for (int s = 0; s < 4; s++) {
        if (brc_dx[s] == dx && brc_dy[s] == dy) {
            return s;
        }
    }
    fprintf(stderr, "border_ring_conditioned : cases (%d,%d) et (%d,%d) non adjacentes — geometrie cassee\n",
            x0, y0, x1, y1);
    exit(1);
}

static void brc_build_ring(void)
{
    int n = 0;
    for (int x = 0; x < ETERN_SIZE; x++) { g_ring[n].x = (int8_t)x; g_ring[n].y = 0; n++; }
    for (int y = 1; y < ETERN_SIZE; y++) { g_ring[n].x = (int8_t)(ETERN_SIZE - 1); g_ring[n].y = (int8_t)y; n++; }
    for (int x = ETERN_SIZE - 2; x >= 0; x--) { g_ring[n].x = (int8_t)x; g_ring[n].y = (int8_t)(ETERN_SIZE - 1); n++; }
    for (int y = ETERN_SIZE - 2; y >= 1; y--) { g_ring[n].x = 0; g_ring[n].y = (int8_t)y; n++; }
    if (n != BRC_RING_LEN) {
        fprintf(stderr, "border_ring_conditioned : geometrie de l'anneau cassee (%d cases, attendu %d)\n",
                n, BRC_RING_LEN);
        exit(1);
    }

    for (int i = 0; i < BRC_RING_LEN; i++) {
        int x = g_ring[i].x, y = g_ring[i].y;
        uint8_t outward = 0;
        if (y == 0) outward |= (1u << BRC_TOP);
        if (x == ETERN_SIZE - 1) outward |= (1u << BRC_RIGHT);
        if (y == ETERN_SIZE - 1) outward |= (1u << BRC_BOTTOM);
        if (x == 0) outward |= (1u << BRC_LEFT);
        g_ring[i].outward_mask = outward;
        int ngrey = 0;
        for (int s = 0; s < 4; s++) {
            if (outward & (1u << s)) ngrey++;
        }
        g_ring[i].is_corner = (ngrey == 2);
        g_ring[i].inward_dir = -1;
        if (!g_ring[i].is_corner) {
            for (int s = 0; s < 4; s++) {
                if (outward & (1u << s)) continue;
                int nx = x + brc_dx[s], ny = y + brc_dy[s];
                if (nx < 0 || nx >= ETERN_SIZE || ny < 0 || ny >= ETERN_SIZE) continue;
                int nb_is_border = (nx == 0 || nx == ETERN_SIZE - 1 || ny == 0 || ny == ETERN_SIZE - 1);
                if (!nb_is_border) {
                    g_ring[i].inward_dir = (int8_t)s;
                }
            }
        }
    }
    for (int i = 0; i < BRC_RING_LEN; i++) {
        int prev = (i - 1 + BRC_RING_LEN) % BRC_RING_LEN;
        int next = (i + 1) % BRC_RING_LEN;
        g_ring[i].prev_dir = (int8_t)brc_dir_between(g_ring[i].x, g_ring[i].y, g_ring[prev].x, g_ring[prev].y);
        g_ring[i].next_dir = (int8_t)brc_dir_between(g_ring[i].x, g_ring[i].y, g_ring[next].x, g_ring[next].y);
    }
}

static inline int brc_is_border_cell(int x, int y)
{
    return x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1;
}

/* ===========================================================================
 * Classification des pièces (forme, invariante par rotation) + table des
 * candidats valides par position (couleurs, orientation déjà résolue par
 * rotation).
 * ------------------------------------------------------------------------- */

typedef struct {
    int8_t is_corner_shape;
    int8_t is_border_shape;
    int8_t is_interior_shape;
    int8_t inner_colour; /* face opposee a l'unique face grise, -1 si pas une piece de bord */
} brc_shape_t;

static brc_shape_t g_shape[ETERN_PARTS + 1];

/* Offre : nombre de pieces de bord par couleur interieure exigee (1..6 sur
   data/pieces.csv) — le budget qu'une construction d'anneau « consciente de
   la bordure » doit respecter EN CONSTRUISANT, pas seulement verifier apres
   coup (cf. brc_gen_iring_dfs, ctx->border_aware). */
static int g_offre[MAX_FACE_MAP];

static void brc_compute_shapes(struct array_part *rot)
{
    memset(g_offre, 0, sizeof g_offre);
    for (int id = 1; id <= ETERN_PARTS; id++) {
        struct part *p = &rot->parts[id]; /* rotation 0 : forme invariante par rotation */
        int8_t f[4] = { p->top, p->right, p->bottom, p->left };
        int ngrey = 0, grey_slot = -1;
        for (int s = 0; s < 4; s++) {
            if (f[s] == 0) { ngrey++; grey_slot = s; }
        }
        g_shape[id].is_corner_shape = (ngrey == 2);
        g_shape[id].is_border_shape = (ngrey == 1);
        g_shape[id].is_interior_shape = (ngrey == 0);
        g_shape[id].inner_colour = -1;
        if (ngrey == 1) {
            g_shape[id].inner_colour = f[(grey_slot + 2) % 4];
            g_offre[g_shape[id].inner_colour]++;
        }
    }
}

/** Une pièce (id+rotation) candidate à une position de l'anneau (bordure ou
 * premier anneau intérieur — structure partagée par les deux DFS). */
typedef struct {
    int16_t rotated_id; /* index grid[][] direct : id + ETERN_PARTS*rotation */
    int8_t id;
    int8_t faces[4];
} brc_candidate_t;

#define BRC_MAX_CANDIDATES_PER_POS 128

/* ===========================================================================
 * Géométrie du PREMIER ANNEAU INTÉRIEUR (52 cases, pourtour de [1,ETERN_SIZE-2]²)
 * — même topologie cyclique que la bordure, un cran plus à l'intérieur.
 * Sert uniquement à SYNTHÉTISER un anneau intérieur réel et complet, quand
 * aucune racine du stock n'en a un (mesuré : c'est le cas — cf. §2.3 de
 * l'étude, MRV pose la bordure au fil de l'eau plutôt que par anneaux
 * complets). Seules pièces intérieures (0 face grise) y sont candidates ;
 * aucune contrainte de couleur envers la bordure ou l'intérieur profond
 * n'est imposée pendant la synthèse (ces faces-là ne servent qu'après,
 * relues par `brc_extract_demand`, exactement comme pour un anneau réel).
 * ------------------------------------------------------------------------- */

#define BRC_IRING_LEN (4 * (ETERN_SIZE - 3))

typedef struct {
    int8_t x, y;
    int8_t prev_dir, next_dir;
    int8_t border_dirs[2]; /* directions pointant vers une case de bordure (0, 1 ou 2) */
    int8_t n_border_dirs;
} brc_iring_pos_t;

static brc_iring_pos_t g_iring[BRC_IRING_LEN];

static void brc_build_square_coords(int lo, int hi, int8_t *xs, int8_t *ys, int len)
{
    int n = 0;
    for (int x = lo; x <= hi; x++) { xs[n] = (int8_t)x; ys[n] = (int8_t)lo; n++; }
    for (int y = lo + 1; y <= hi; y++) { xs[n] = (int8_t)hi; ys[n] = (int8_t)y; n++; }
    for (int x = hi - 1; x >= lo; x--) { xs[n] = (int8_t)x; ys[n] = (int8_t)hi; n++; }
    for (int y = hi - 1; y >= lo + 1; y--) { xs[n] = (int8_t)lo; ys[n] = (int8_t)y; n++; }
    if (n != len) {
        fprintf(stderr, "border_ring_conditioned : geometrie carree cassee (%d cases, attendu %d)\n", n, len);
        exit(1);
    }
}

static void brc_build_iring(void)
{
    int8_t xs[BRC_IRING_LEN], ys[BRC_IRING_LEN];
    brc_build_square_coords(1, ETERN_SIZE - 2, xs, ys, BRC_IRING_LEN);
    for (int i = 0; i < BRC_IRING_LEN; i++) {
        g_iring[i].x = xs[i];
        g_iring[i].y = ys[i];
    }
    for (int i = 0; i < BRC_IRING_LEN; i++) {
        int prev = (i - 1 + BRC_IRING_LEN) % BRC_IRING_LEN;
        int next = (i + 1) % BRC_IRING_LEN;
        g_iring[i].prev_dir = (int8_t)brc_dir_between(g_iring[i].x, g_iring[i].y, g_iring[prev].x, g_iring[prev].y);
        g_iring[i].next_dir = (int8_t)brc_dir_between(g_iring[i].x, g_iring[i].y, g_iring[next].x, g_iring[next].y);

        g_iring[i].n_border_dirs = 0;
        for (int s = 0; s < 4; s++) {
            int nx = g_iring[i].x + brc_dx[s], ny = g_iring[i].y + brc_dy[s];
            if (brc_is_border_cell(nx, ny)) {
                g_iring[i].border_dirs[g_iring[i].n_border_dirs++] = (int8_t)s;
            }
        }
    }
}

typedef struct {
    long long node_budget;
    long long nodes_used;
    int8_t used[ETERN_PARTS + 1];
    int order[ETERN_PARTS + 1]; /* ordre (mélangé) de balayage des ids intérieurs */
    int n_interior_ids;
    brc_candidate_t placed[BRC_IRING_LEN];
    /* Mode « conscient de la bordure » : n'accepte une pose que si elle ne
       fait dépasser aucun budget de couleur (g_offre) sur les demi-arêtes
       tournées vers la bordure — condition nécessaire de Hall, appliquée EN
       CONSTRUISANT, pas vérifiée après coup. */
    int border_aware;
    int used_colour_count[MAX_FACE_MAP];
} brc_gen_ctx_t;

static int brc_gen_iring_dfs(struct array_part *rot, int pos_idx, brc_gen_ctx_t *ctx)
{
    ctx->nodes_used++;
    if (ctx->nodes_used > ctx->node_budget) {
        return 0;
    }
    if (pos_idx == BRC_IRING_LEN) {
        return 1;
    }
    const brc_iring_pos_t *pos = &g_iring[pos_idx];
    for (int oi = 0; oi < ctx->n_interior_ids; oi++) {
        int id = ctx->order[oi];
        if (ctx->used[id]) continue;
        for (int r = 0; r < 4; r++) {
            struct part *p = &rot->parts[id + ETERN_PARTS * r];
            brc_candidate_t c;
            c.rotated_id = (int16_t)(id + ETERN_PARTS * r);
            c.id = (int8_t)id;
            c.faces[0] = p->top; c.faces[1] = p->right; c.faces[2] = p->bottom; c.faces[3] = p->left;

            if (pos_idx > 0) {
                const brc_candidate_t *prevp = &ctx->placed[pos_idx - 1];
                if (c.faces[pos->prev_dir] != prevp->faces[g_iring[pos_idx - 1].next_dir]) continue;
            }
            if (pos_idx == BRC_IRING_LEN - 1) {
                const brc_candidate_t *firstp = &ctx->placed[0];
                if (c.faces[pos->next_dir] != firstp->faces[g_iring[0].prev_dir]) continue;
            }

            if (ctx->border_aware) {
                int ok = 1;
                for (int b = 0; b < pos->n_border_dirs && ok; b++) {
                    int8_t colour = c.faces[pos->border_dirs[b]];
                    int already = ctx->used_colour_count[colour];
                    /* Compte aussi les autres demi-arêtes DE CETTE MÊME pose
                       vers cette couleur (cas des 4 coins d'anneau, 2 demi-
                       arêtes) avant de comparer à l'offre. */
                    int this_piece_same_colour = 0;
                    for (int b2 = 0; b2 < b; b2++) {
                        if (c.faces[pos->border_dirs[b2]] == colour) this_piece_same_colour++;
                    }
                    if (already + this_piece_same_colour + 1 > g_offre[colour]) ok = 0;
                }
                if (!ok) continue;
            }

            ctx->used[id] = 1;
            ctx->placed[pos_idx] = c;
            if (ctx->border_aware) {
                for (int b = 0; b < pos->n_border_dirs; b++) {
                    ctx->used_colour_count[(int)c.faces[pos->border_dirs[b]]]++;
                }
            }
            if (brc_gen_iring_dfs(rot, pos_idx + 1, ctx)) return 1;
            if (ctx->border_aware) {
                for (int b = 0; b < pos->n_border_dirs; b++) {
                    ctx->used_colour_count[(int)c.faces[pos->border_dirs[b]]]--;
                }
            }
            ctx->used[id] = 0;
        }
    }
    return 0;
}

/**
 * @brief Synthétise un premier anneau intérieur (52 cases) valide et
 * complet, écrit dans `out` (bordure et intérieur profond laissés à -2).
 * `seed` mélange l'ordre de balayage des pièces intérieures pour produire
 * des échantillons différents. `border_aware` : n'accepte une pose que si
 * elle respecte le budget de couleur de bordure restant (condition
 * nécessaire de Hall) — sinon génération « aveugle » (ne regarde que la
 * cohérence de l'anneau avec lui-même). @return 1 si trouvé, 0 sinon
 * (budget épuisé — plus probable en mode conscient, plus contraint).
 */
static int brc_generate_iring(struct array_part *rot, unsigned int seed, long long node_budget, int border_aware,
                               struct possibility_packet *out)
{
    brc_gen_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.node_budget = node_budget;
    ctx.border_aware = border_aware;
    ctx.n_interior_ids = 0;
    for (int id = 1; id <= ETERN_PARTS; id++) {
        if (g_shape[id].is_interior_shape) {
            ctx.order[ctx.n_interior_ids++] = id;
        }
    }
    /* Fisher-Yates, PRNG local (rand_r) : ne dépend pas de l'état global de
       rand(), reproductible à seed égal. */
    unsigned int state = seed;
    for (int i = ctx.n_interior_ids - 1; i > 0; i--) {
        int j = rand_r(&state) % (i + 1);
        int tmp = ctx.order[i];
        ctx.order[i] = ctx.order[j];
        ctx.order[j] = tmp;
    }

    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            out->grid[x][y] = -2;
        }
    }
    if (!brc_gen_iring_dfs(rot, 0, &ctx)) {
        return 0;
    }
    for (int i = 0; i < BRC_IRING_LEN; i++) {
        out->grid[g_iring[i].x][g_iring[i].y] = ctx.placed[i].rotated_id;
    }
    return 1;
}

/**
 * @brief Contrôle INDÉPENDANT d'un anneau intérieur généré par
 * `brc_generate_iring` — ne relit rien de son contexte interne (`ctx`),
 * uniquement les pièces réellement écrites dans `ring_pkt->grid`. Vérifie :
 * (1) qu'aucune pièce (identifiant réel, pas `rotated_id` — deux rotations
 * de la même pièce comptent comme UN seul usage) n'apparaît deux fois dans
 * les 52 cases de l'anneau ; (2) que chaque paire de cases voisines le long
 * de l'anneau (fermeture du cycle comprise) montre bien la même couleur sur
 * l'arête qu'elles partagent. Échec bruyant (message + 0) plutôt que
 * silencieux : un anneau qui réutiliserait une pièce ou romprait une
 * adjacence rendrait toute la mesure sans objet.
 * @return 1 si l'anneau est valide, 0 sinon (message d'erreur déjà émis).
 */
static int brc_verify_generated_ring(const struct possibility_packet *ring_pkt, struct array_part *rot)
{
    int8_t seen[ETERN_PARTS + 1] = {0};
    for (int i = 0; i < BRC_IRING_LEN; i++) {
        int16_t v = ring_pkt->grid[g_iring[i].x][g_iring[i].y];
        if (v == -2) {
            fprintf(stderr,
                    "border_ring_conditioned : VERIF ECHEC — case (%d,%d) de l'anneau genere non posee\n",
                    g_iring[i].x, g_iring[i].y);
            return 0;
        }
        int id = rot->parts[v].id;
        if (id < 1 || id > ETERN_PARTS) {
            fprintf(stderr,
                    "border_ring_conditioned : VERIF ECHEC — case (%d,%d), identifiant de piece invalide (%d)\n",
                    g_iring[i].x, g_iring[i].y, id);
            return 0;
        }
        if (seen[id]) {
            fprintf(stderr,
                    "border_ring_conditioned : VERIF ECHEC — piece %d utilisee plus d'une fois dans l'anneau"
                    " genere (derniere occurrence en (%d,%d))\n",
                    id, g_iring[i].x, g_iring[i].y);
            return 0;
        }
        seen[id] = 1;
    }
    for (int i = 0; i < BRC_IRING_LEN; i++) {
        int prev = (i - 1 + BRC_IRING_LEN) % BRC_IRING_LEN;
        struct part *pc = &rot->parts[ring_pkt->grid[g_iring[i].x][g_iring[i].y]];
        struct part *pp = &rot->parts[ring_pkt->grid[g_iring[prev].x][g_iring[prev].y]];
        int8_t fc[4] = { pc->top, pc->right, pc->bottom, pc->left };
        int8_t fp[4] = { pp->top, pp->right, pp->bottom, pp->left };
        if (fc[g_iring[i].prev_dir] != fp[g_iring[prev].next_dir]) {
            fprintf(stderr,
                    "border_ring_conditioned : VERIF ECHEC — adjacence rompue dans l'anneau genere,"
                    " position %d (%d,%d)\n",
                    i, g_iring[i].x, g_iring[i].y);
            return 0;
        }
    }
    return 1;
}

/* ===========================================================================
 * PASSE 4 (contrôle inverse) : partant d'une bordure RÉELLE et COMPLÈTE
 * (60/60, mesuré : 345/13739 sur eternityII.back), remplir le premier
 * anneau intérieur À PARTIR DE ZÉRO — sans regarder l'intérieur réellement
 * posé dans ce paquet (ignoré), sans indice : seule la bordure fixe
 * contraint, plus la cohérence de l'anneau avec lui-même (adjacence +
 * non-réutilisation de pièce). C'est le sens INVERSE des passes 1-3
 * (anneau connu -> bordure) : mesure si l'asymétrie observée (anneau ->
 * bordure toujours infaisable) est bien directionnelle, et pas un artefact
 * du DFS de bordure lui-même.
 * ------------------------------------------------------------------------- */

typedef struct {
    long long node_budget;
    long long nodes_used;
    long long leaf_cap;
    long long leaves_found;
    int budget_exceeded;
    int8_t used[ETERN_PARTS + 1];
    int order[ETERN_PARTS + 1];
    int n_interior_ids;
    brc_candidate_t placed[BRC_IRING_LEN];
    struct part *border_face_cache[ETERN_SIZE][ETERN_SIZE]; /* NULL si pas bordure */
} brc_fill_ctx_t;

static void brc_fill_dfs(struct array_part *rot, int pos_idx, brc_fill_ctx_t *ctx)
{
    if (ctx->leaves_found >= ctx->leaf_cap) return;
    ctx->nodes_used++;
    if (ctx->nodes_used > ctx->node_budget) {
        ctx->budget_exceeded = 1;
        return;
    }
    if (pos_idx == BRC_IRING_LEN) {
        ctx->leaves_found++;
        return;
    }

    const brc_iring_pos_t *pos = &g_iring[pos_idx];
    for (int oi = 0; oi < ctx->n_interior_ids; oi++) {
        int id = ctx->order[oi];
        if (ctx->used[id]) continue;
        for (int r = 0; r < 4; r++) {
            struct part *p = &rot->parts[id + ETERN_PARTS * r];
            brc_candidate_t c;
            c.rotated_id = (int16_t)(id + ETERN_PARTS * r);
            c.id = (int8_t)id;
            c.faces[0] = p->top; c.faces[1] = p->right; c.faces[2] = p->bottom; c.faces[3] = p->left;

            if (pos_idx > 0) {
                const brc_candidate_t *prevp = &ctx->placed[pos_idx - 1];
                if (c.faces[pos->prev_dir] != prevp->faces[g_iring[pos_idx - 1].next_dir]) continue;
            }
            if (pos_idx == BRC_IRING_LEN - 1) {
                const brc_candidate_t *firstp = &ctx->placed[0];
                if (c.faces[pos->next_dir] != firstp->faces[g_iring[0].prev_dir]) continue;
            }
            int ok = 1;
            for (int b = 0; b < pos->n_border_dirs && ok; b++) {
                int dir = pos->border_dirs[b];
                int nx = pos->x + brc_dx[dir], ny = pos->y + brc_dy[dir];
                struct part *bp = ctx->border_face_cache[nx][ny];
                int8_t bface[4] = { bp->top, bp->right, bp->bottom, bp->left };
                if (c.faces[dir] != bface[brc_opposite[dir]]) ok = 0;
            }
            if (!ok) continue;

            ctx->used[id] = 1;
            ctx->placed[pos_idx] = c;
            brc_fill_dfs(rot, pos_idx + 1, ctx);
            ctx->used[id] = 0;
            if (ctx->leaves_found >= ctx->leaf_cap || ctx->budget_exceeded) return;
        }
    }
}

/**
 * @brief Remplit `out` (anneau seul, bordure et intérieur profond laissés
 * à -2) à partir d'une bordure réelle connue et déjà vérifiée complète
 * (`border_pkt`) — les 60 pièces de bordure sont exclues des candidats.
 * @return 1 si au moins une complétion trouvée (`out` rempli), 0 sinon —
 * `*budget_exceeded_out` distingue alors « prouvé infaisable » (DFS
 * exhaustif, 0) de « inconclusif » (budget de nœuds épuisé, 1).
 */
static int brc_fill_iring_from_border(struct array_part *rot, const struct possibility_packet *border_pkt,
                                       long long node_budget, long long leaf_cap,
                                       struct possibility_packet *out, int *budget_exceeded_out)
{
    brc_fill_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.node_budget = node_budget;
    ctx.leaf_cap = leaf_cap;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (!brc_is_border_cell(x, y)) continue;
            struct part *bp = &rot->parts[border_pkt->grid[x][y]];
            ctx.border_face_cache[x][y] = bp;
            ctx.used[bp->id] = 1; /* pieces de bordure indisponibles pour l'anneau */
        }
    }
    ctx.n_interior_ids = 0;
    for (int id = 1; id <= ETERN_PARTS; id++) {
        if (g_shape[id].is_interior_shape && !ctx.used[id]) {
            ctx.order[ctx.n_interior_ids++] = id;
        }
    }
    brc_fill_dfs(rot, 0, &ctx);
    *budget_exceeded_out = ctx.budget_exceeded;
    if (ctx.leaves_found == 0) return 0;
    for (int x = 0; x < ETERN_SIZE; x++)
        for (int y = 0; y < ETERN_SIZE; y++) out->grid[x][y] = -2;
    for (int i = 0; i < BRC_IRING_LEN; i++) {
        out->grid[g_iring[i].x][g_iring[i].y] = ctx.placed[i].rotated_id;
    }
    return 1;
}

typedef struct {
    brc_candidate_t cand[BRC_MAX_CANDIDATES_PER_POS];
    int n;
} brc_candidate_list_t;

/** Construit la liste des candidats pour la position `pos_idx`, filtrés par
 * couleur intérieure exigée si `demand >= 0` (aucun filtre si `demand < 0`). */
static void brc_candidates_for_position(struct array_part *rot, int pos_idx, int demand, brc_candidate_list_t *out)
{
    const brc_ring_pos_t *pos = &g_ring[pos_idx];
    out->n = 0;
    for (int id = 1; id <= ETERN_PARTS; id++) {
        if (pos->is_corner) {
            if (!g_shape[id].is_corner_shape) continue;
        } else {
            if (!g_shape[id].is_border_shape) continue;
        }
        for (int r = 0; r < 4; r++) {
            struct part *p = &rot->parts[id + ETERN_PARTS * r];
            int8_t f[4] = { p->top, p->right, p->bottom, p->left };
            uint8_t mask = 0;
            for (int s = 0; s < 4; s++) if (f[s] == 0) mask |= (uint8_t)(1u << s);
            if (mask != pos->outward_mask) continue;
            if (!pos->is_corner && demand >= 0 && f[pos->inward_dir] != demand) continue;
            if (out->n >= BRC_MAX_CANDIDATES_PER_POS) {
                fprintf(stderr, "border_ring_conditioned : BRC_MAX_CANDIDATES_PER_POS trop petit\n");
                exit(1);
            }
            brc_candidate_t *c = &out->cand[out->n++];
            c->rotated_id = (int16_t)(id + ETERN_PARTS * r);
            c->id = (int8_t)id;
            memcpy(c->faces, f, sizeof f);
        }
    }
}

/* ===========================================================================
 * DFS de comptage, position par position dans l'ordre de l'anneau — le
 * repli de fermeture du cycle (position 59 vs position 0) est vérifié à la
 * toute dernière étape, comme un DFS de bordure classique.
 * ------------------------------------------------------------------------- */

typedef struct {
    long long node_budget;
    long long leaf_cap;
    long long nodes_used;
    unsigned long long leaves_found;
    int budget_exceeded;
    int cap_reached;
    int8_t used[ETERN_PARTS + 1];
    brc_candidate_t placed[BRC_RING_LEN];
} brc_dfs_ctx_t;

static void brc_dfs(struct array_part *rot, const int demand[BRC_RING_LEN], int pos_idx, brc_dfs_ctx_t *ctx)
{
    if (ctx->budget_exceeded || ctx->cap_reached) {
        return;
    }
    ctx->nodes_used++;
    if (ctx->nodes_used > ctx->node_budget) {
        ctx->budget_exceeded = 1;
        return;
    }
    if (pos_idx == BRC_RING_LEN) {
        ctx->leaves_found++;
        if ((long long)ctx->leaves_found >= ctx->leaf_cap) {
            ctx->cap_reached = 1;
        }
        return;
    }

    const brc_ring_pos_t *pos = &g_ring[pos_idx];
    brc_candidate_list_t cands;
    brc_candidates_for_position(rot, pos_idx, pos->is_corner ? -1 : demand[pos_idx], &cands);

    for (int k = 0; k < cands.n; k++) {
        const brc_candidate_t *c = &cands.cand[k];
        if (ctx->used[c->id]) continue;

        if (pos_idx > 0) {
            const brc_candidate_t *prevp = &ctx->placed[pos_idx - 1];
            if (c->faces[pos->prev_dir] != prevp->faces[g_ring[pos_idx - 1].next_dir]) continue;
        }
        if (pos_idx == BRC_RING_LEN - 1) {
            const brc_candidate_t *firstp = &ctx->placed[0];
            if (c->faces[pos->next_dir] != firstp->faces[g_ring[0].prev_dir]) continue;
        }

        ctx->used[c->id] = 1;
        ctx->placed[pos_idx] = *c;
        brc_dfs(rot, demand, pos_idx + 1, ctx);
        ctx->used[c->id] = 0;

        if (ctx->budget_exceeded || ctx->cap_reached) return;
    }
}

/* ===========================================================================
 * Extraction de la demande positionnelle depuis un plateau réel : pour
 * chaque position de bordure non-coin, la couleur qu'affiche, en face
 * d'elle, la case du premier anneau intérieur SI elle est posée.
 * ------------------------------------------------------------------------- */

static int brc_first_ring_complete(const struct possibility_packet *b)
{
    for (int x = 1; x <= ETERN_SIZE - 2; x++) {
        for (int y = 1; y <= ETERN_SIZE - 2; y++) {
            int on_ring = (x == 1 || x == ETERN_SIZE - 2 || y == 1 || y == ETERN_SIZE - 2);
            if (!on_ring) continue;
            if (b->grid[x][y] == -2) return 0;
        }
    }
    return 1;
}

/** demand[i] = couleur exigée en position i (-1 pour les coins). */
static void brc_extract_demand(const struct possibility_packet *b, struct array_part *rot, int demand[BRC_RING_LEN])
{
    for (int i = 0; i < BRC_RING_LEN; i++) {
        if (g_ring[i].is_corner) {
            demand[i] = -1;
            continue;
        }
        int nx = g_ring[i].x + brc_dx[g_ring[i].inward_dir];
        int ny = g_ring[i].y + brc_dy[g_ring[i].inward_dir];
        int16_t rid = b->grid[nx][ny];
        struct part *p = &rot->parts[rid];
        int8_t f[4] = { p->top, p->right, p->bottom, p->left };
        demand[i] = f[brc_opposite[g_ring[i].inward_dir]];
    }
}

/* ===========================================================================
 * --selftest : validations sur données réelles, sans dépendre d'un anneau
 * complet (rare/absent dans le stock disponible) — voir le commentaire
 * d'en-tête du fichier.
 * ------------------------------------------------------------------------- */

static int brc_selftest(const char *back_path, struct array_part *rot)
{
    FILE *f = fopen(back_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "border_ring_conditioned : ouverture de %s impossible\n", back_path);
        return 1;
    }

    long long roots = 0;
    long long shape_checks = 0, inward_checks = 0, adjacency_checks = 0;
    struct possibility_packet pkt;
    while (fread(&pkt, sizeof pkt, 1, f) == 1) {
        roots++;
        for (int i = 0; i < BRC_RING_LEN; i++) {
            int x = g_ring[i].x, y = g_ring[i].y;
            int16_t rid = pkt.grid[x][y];
            if (rid == -2) continue;
            struct part *p = &rot->parts[rid];
            int8_t face[4] = { p->top, p->right, p->bottom, p->left };
            uint8_t mask = 0;
            for (int s = 0; s < 4; s++) if (face[s] == 0) mask |= (uint8_t)(1u << s);
            if (mask != g_ring[i].outward_mask) {
                fprintf(stderr,
                        "border_ring_conditioned : SELFTEST ECHEC — case (%d,%d) piece %d, masque gris %u"
                        " attendu %u\n",
                        x, y, rid, mask, g_ring[i].outward_mask);
                fclose(f);
                return 1;
            }
            shape_checks++;

            if (g_ring[i].is_corner) continue;
            int nx = x + brc_dx[g_ring[i].inward_dir], ny = y + brc_dy[g_ring[i].inward_dir];
            if (pkt.grid[nx][ny] == -2) continue;
            struct part *pin = &rot->parts[pkt.grid[nx][ny]];
            int8_t fin[4] = { pin->top, pin->right, pin->bottom, pin->left };
            int8_t expect = fin[brc_opposite[g_ring[i].inward_dir]];
            if (face[g_ring[i].inward_dir] != expect) {
                fprintf(stderr,
                        "border_ring_conditioned : SELFTEST ECHEC — case (%d,%d) couleur interieure %d,"
                        " voisine (%d,%d) exige %d\n",
                        x, y, face[g_ring[i].inward_dir], nx, ny, expect);
                fclose(f);
                return 1;
            }
            inward_checks++;

            int px = x + brc_dx[g_ring[i].prev_dir], py = y + brc_dy[g_ring[i].prev_dir];
            if (pkt.grid[px][py] != -2) {
                struct part *pp = &rot->parts[pkt.grid[px][py]];
                int8_t fp[4] = { pp->top, pp->right, pp->bottom, pp->left };
                int prev_idx = (i - 1 + BRC_RING_LEN) % BRC_RING_LEN;
                if (fp[g_ring[prev_idx].next_dir] != face[g_ring[i].prev_dir]) {
                    fprintf(stderr,
                            "border_ring_conditioned : SELFTEST ECHEC — adjacence bordure rompue en (%d,%d)\n",
                            x, y);
                    fclose(f);
                    return 1;
                }
                adjacency_checks++;
            }
        }
    }
    fclose(f);
    printf("selftest : %lld racines, %lld verifs de forme, %lld verifs de couleur interieure,"
           " %lld verifs d'adjacence bordure — toutes passees\n",
           roots, shape_checks, inward_checks, adjacency_checks);

    /* Témoin négatif : sans aucun épinglage, le DFS ne doit PAS converger
       sous un budget modeste (la masse inconditionnelle dépasse 9,2e18,
       cf. border-mass-walker-design) — sinon la logique d'appariement est
       trop stricte quelque part (bug), pas juste lente. */
    int demand_open[BRC_RING_LEN];
    for (int i = 0; i < BRC_RING_LEN; i++) demand_open[i] = -1;
    brc_dfs_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.node_budget = 300000;
    ctx.leaf_cap = 1000000000LL;
    brc_dfs(rot, demand_open, 0, &ctx);
    if (!ctx.budget_exceeded) {
        fprintf(stderr,
                "border_ring_conditioned : SELFTEST ECHEC — le DFS sans épinglage a convergé sous 300000"
                " noeuds (%llu completions) ; attendu : budget epuise (masse inconditionnelle astronomique)\n",
                ctx.leaves_found);
        return 1;
    }
    printf("selftest : temoin negatif OK — DFS sans epinglage n'a pas convergé sous 300000 noeuds"
           " (comme attendu)\n");

    /* Témoin positif : sur une racine réelle à bordure ENTIÈREMENT posée
       (mesuré : 345/13739 sur eternityII.back), la table de candidats de
       `brc_candidates_for_position` — celle qu'utilise le DFS à chaque pas —
       doit CONTENIR la pièce réellement utilisée à chaque position, et la
       chaîne d'adjacence prev/fermeture doit tenir de bout en bout. Valide
       le DFS lui-même (génération de candidats + fermeture du cycle), pas
       seulement la géométrie, sans dépendre d'une recherche aléatoire. */
    FILE *f2 = fopen(back_path, "rb");
    if (f2 == NULL) {
        fprintf(stderr, "border_ring_conditioned : reouverture de %s impossible\n", back_path);
        return 1;
    }
    struct possibility_packet full_pkt;
    int found_full_border = 0;
    while (fread(&full_pkt, sizeof full_pkt, 1, f2) == 1) {
        int all_border = 1;
        for (int i = 0; i < BRC_RING_LEN && all_border; i++) {
            if (full_pkt.grid[g_ring[i].x][g_ring[i].y] == -2) all_border = 0;
        }
        if (all_border) { found_full_border = 1; break; }
    }
    fclose(f2);
    if (!found_full_border) {
        fprintf(stderr, "border_ring_conditioned : SELFTEST ECHEC — aucune racine a bordure complete"
                        " dans le stock, temoin positif impossible\n");
        return 1;
    }
    for (int i = 0; i < BRC_RING_LEN; i++) {
        int16_t rid = full_pkt.grid[g_ring[i].x][g_ring[i].y];
        brc_candidate_list_t cands;
        brc_candidates_for_position(rot, i, -1, &cands);
        int found = 0;
        for (int k = 0; k < cands.n; k++) if (cands.cand[k].rotated_id == rid) { found = 1; break; }
        if (!found) {
            fprintf(stderr,
                    "border_ring_conditioned : SELFTEST ECHEC — temoin positif, piece reelle %d absente"
                    " des candidats generes en position %d (%d,%d)\n",
                    rid, i, g_ring[i].x, g_ring[i].y);
            return 1;
        }
    }
    for (int i = 0; i < BRC_RING_LEN; i++) {
        int prev = (i - 1 + BRC_RING_LEN) % BRC_RING_LEN;
        struct part *pc = &rot->parts[full_pkt.grid[g_ring[i].x][g_ring[i].y]];
        struct part *pp = &rot->parts[full_pkt.grid[g_ring[prev].x][g_ring[prev].y]];
        int8_t fc[4] = { pc->top, pc->right, pc->bottom, pc->left };
        int8_t fp[4] = { pp->top, pp->right, pp->bottom, pp->left };
        if (fc[g_ring[i].prev_dir] != fp[g_ring[prev].next_dir]) {
            fprintf(stderr,
                    "border_ring_conditioned : SELFTEST ECHEC — temoin positif, chaine d'adjacence rompue"
                    " en position %d\n",
                    i);
            return 1;
        }
    }
    printf("selftest : temoin positif OK — bordure reelle entierement posee (60/60), toutes les pieces"
           " reelles sont dans les candidats generes et la chaine d'adjacence (fermeture comprise) tient\n");
    return 0;
}

/* ===========================================================================
 * main
 * ------------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <pieces.csv> <stock.back> [--max-roots n] [--node-budget n]"
                     " [--leaf-cap n] [--synth-rings n] [--synth-rings-aware n] [--gen-node-budget n]"
                     " [--reverse-max n] [--reverse-node-budget n] [--reverse-leaf-cap n]"
                     " [--witness-node-budget n] [--witness-leaf-cap n]"
                     " [--selftest]\n",
            prog);
}

typedef struct {
    long long sampled, zero, budget_exceeded, cap_reached;
    unsigned long long min_leaves, max_leaves;
    int have_min_max;
} brc_agg_t;

static void brc_agg_update(brc_agg_t *agg, const brc_dfs_ctx_t *ctx)
{
    agg->sampled++;
    if (ctx->leaves_found == 0) agg->zero++;
    if (ctx->budget_exceeded) agg->budget_exceeded++;
    if (ctx->cap_reached) agg->cap_reached++;
    if (!agg->have_min_max) {
        agg->min_leaves = agg->max_leaves = ctx->leaves_found;
        agg->have_min_max = 1;
    } else {
        if (ctx->leaves_found < agg->min_leaves) agg->min_leaves = ctx->leaves_found;
        if (ctx->leaves_found > agg->max_leaves) agg->max_leaves = ctx->leaves_found;
    }
}

static void brc_agg_print(const char *label, const brc_agg_t *agg)
{
    printf("\n=== %s ===\n", label);
    printf("racines echantillonnees            : %lld\n", agg->sampled);
    if (agg->sampled > 0) {
        printf("completions minimum observees      : %llu\n", agg->min_leaves);
        printf("completions maximum observees      : %llu\n", agg->max_leaves);
        printf("racines a 0 completion (infaisable): %lld\n", agg->zero);
        printf("racines plafonnees (>= --leaf-cap) : %lld\n", agg->cap_reached);
        printf("racines budget de noeuds epuise    : %lld\n", agg->budget_exceeded);
    } else {
        printf("aucun echantillon\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    const char *pieces_path = argv[1];
    const char *back_path = argv[2];
    long long max_roots = 50;
    long long node_budget = 5000000;
    long long leaf_cap = 2000000;
    long long synth_rings = 20;
    long long synth_rings_aware = 20;
    long long gen_node_budget = 2000000;
    long long reverse_max = 1000000000LL;
    long long reverse_node_budget = 3000000;
    long long reverse_leaf_cap = 3;
    long long witness_node_budget = 30000000;
    long long witness_leaf_cap = 5000000;
    int do_selftest = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--max-roots") == 0 && i + 1 < argc) {
            max_roots = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--node-budget") == 0 && i + 1 < argc) {
            node_budget = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--leaf-cap") == 0 && i + 1 < argc) {
            leaf_cap = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--synth-rings") == 0 && i + 1 < argc) {
            synth_rings = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--synth-rings-aware") == 0 && i + 1 < argc) {
            synth_rings_aware = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--gen-node-budget") == 0 && i + 1 < argc) {
            gen_node_budget = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--reverse-max") == 0 && i + 1 < argc) {
            reverse_max = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--reverse-node-budget") == 0 && i + 1 < argc) {
            reverse_node_budget = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--reverse-leaf-cap") == 0 && i + 1 < argc) {
            reverse_leaf_cap = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--witness-node-budget") == 0 && i + 1 < argc) {
            witness_node_budget = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--witness-leaf-cap") == 0 && i + 1 < argc) {
            witness_leaf_cap = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--selftest") == 0) {
            do_selftest = 1;
        } else {
            fprintf(stderr, "option inconnue : %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    struct array_part *apart = read_parts(pieces_path);
    struct array_part *rot = rotate_all_parts(apart);
    brc_build_ring();
    brc_build_iring();
    brc_compute_shapes(rot);

    if (do_selftest) {
        return brc_selftest(back_path, rot);
    }

    /* --- Passe 1 : racines REELLES du stock a premier anneau complet. ---- */
    FILE *f = fopen(back_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "border_ring_conditioned : ouverture de %s impossible\n", back_path);
        return 1;
    }
    long long roots_scanned = 0, roots_ring_complete = 0;
    brc_agg_t real_agg = {0};
    struct possibility_packet pkt;
    while (real_agg.sampled < max_roots && fread(&pkt, sizeof pkt, 1, f) == 1) {
        roots_scanned++;
        if (!brc_first_ring_complete(&pkt)) continue;
        roots_ring_complete++;

        int demand[BRC_RING_LEN];
        brc_extract_demand(&pkt, rot, demand);
        brc_dfs_ctx_t ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.node_budget = node_budget;
        ctx.leaf_cap = leaf_cap;
        brc_dfs(rot, demand, 0, &ctx);
        brc_agg_update(&real_agg, &ctx);
        printf("racine reelle #%lld (scannee #%lld) : -> %llu completion(s)%s%s\n",
               real_agg.sampled, roots_scanned, ctx.leaves_found,
               ctx.cap_reached ? " [plafond atteint]" : "",
               ctx.budget_exceeded ? " [budget de noeuds epuise]" : "");
    }
    fclose(f);

    printf("\nracines scannees                    : %lld\n", roots_scanned);
    printf("racines REELLES a premier anneau complet : %lld\n", roots_ring_complete);
    brc_agg_print("mesure A (1/2) : racines reelles du stock, premier anneau complet", &real_agg);
    if (roots_ring_complete == 0) {
        printf("\n(aucune racine reelle n'a de premier anneau complet dans l'echantillon scanne —\n"
               " coherent avec le fait que MRV pose bordure et anneau au fil de l'eau, pas par\n"
               " anneaux complets. Voir la passe 2, anneaux synthetiques, ci-dessous.)\n");
    }

    /* --- Passe 2 : anneaux SYNTHETIQUES AVEUGLES, valides et complets, ---- *
     * generes sans aucun egard pour la rareté des couleurs de bordure. ----- */
    brc_agg_t synth_agg = {0};
    long long synth_gen_failed = 0;
    for (long long s = 0; s < synth_rings; s++) {
        struct possibility_packet ring_pkt;
        if (!brc_generate_iring(rot, (unsigned int)(0x9E3779B9u * (unsigned int)(s + 1)), gen_node_budget, 0,
                                 &ring_pkt)) {
            synth_gen_failed++;
            continue;
        }
        if (!brc_verify_generated_ring(&ring_pkt, rot)) {
            return 1;
        }
        int demand[BRC_RING_LEN];
        brc_extract_demand(&ring_pkt, rot, demand);
        brc_dfs_ctx_t ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.node_budget = node_budget;
        ctx.leaf_cap = leaf_cap;
        brc_dfs(rot, demand, 0, &ctx);
        brc_agg_update(&synth_agg, &ctx);
        printf("anneau synthetique aveugle #%lld : -> %llu completion(s)%s%s\n",
               s + 1, ctx.leaves_found,
               ctx.cap_reached ? " [plafond atteint]" : "",
               ctx.budget_exceeded ? " [budget de noeuds epuise]" : "");
    }
    if (synth_gen_failed > 0) {
        printf("\n(generation d'anneau synthetique aveugle echouee %lld fois sur %lld)\n", synth_gen_failed,
               synth_rings);
    }
    brc_agg_print("mesure A (2/3) : anneaux synthetiques AVEUGLES, valides et complets", &synth_agg);

    /* --- Passe 3 : anneaux SYNTHETIQUES CONSCIENTS DE LA BORDURE — le vrai -*
     * test du mecanisme propose (le budget de couleur guide la pose du     -*
     * premier anneau, il n'est pas verifie apres coup). ---------------------*/
    brc_agg_t aware_agg = {0};
    long long aware_gen_failed = 0;
    for (long long s = 0; s < synth_rings_aware; s++) {
        struct possibility_packet ring_pkt;
        if (!brc_generate_iring(rot, (unsigned int)(0x2545F491u * (unsigned int)(s + 1)), gen_node_budget, 1,
                                 &ring_pkt)) {
            aware_gen_failed++;
            continue;
        }
        if (!brc_verify_generated_ring(&ring_pkt, rot)) {
            return 1;
        }
        int demand[BRC_RING_LEN];
        brc_extract_demand(&ring_pkt, rot, demand);

        /* Garde-fou : le quota impose PENDANT la generation doit se retrouver
           tel quel dans la demande extraite — sinon brc_gen_iring_dfs a un
           bug (le mode conscient ne servirait a rien silencieusement). */
        int demand_count[MAX_FACE_MAP] = {0};
        for (int i = 0; i < BRC_RING_LEN; i++) {
            if (demand[i] >= 0) demand_count[demand[i]]++;
        }
        for (int c = 0; c < MAX_FACE_MAP; c++) {
            if (demand_count[c] > g_offre[c]) {
                fprintf(stderr,
                        "border_ring_conditioned : BUG — anneau #%lld conscient de la bordure viole quand"
                        " meme le quota (couleur %d : demande %d > offre %d)\n",
                        s + 1, c, demand_count[c], g_offre[c]);
                return 1;
            }
        }

        brc_dfs_ctx_t ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.node_budget = node_budget;
        ctx.leaf_cap = leaf_cap;
        brc_dfs(rot, demand, 0, &ctx);
        brc_agg_update(&aware_agg, &ctx);
        printf("anneau synthetique conscient de la bordure #%lld : -> %llu completion(s)%s%s\n",
               s + 1, ctx.leaves_found,
               ctx.cap_reached ? " [plafond atteint]" : "",
               ctx.budget_exceeded ? " [budget de noeuds epuise]" : "");
    }
    if (aware_gen_failed > 0) {
        printf("\n(generation d'anneau conscient de la bordure echouee %lld fois sur %lld — budget de"
               " synthese insuffisant : la contrainte de quota rend la construction elle-meme plus dure)\n",
               aware_gen_failed, synth_rings_aware);
    }
    brc_agg_print("mesure A (3/3) : anneaux synthetiques CONSCIENTS de la bordure (quota respecte en construisant)",
                  &aware_agg);

    /* --- Passe 4 : CONTROLE INVERSE — bordures REELLES connues -> anneau. */
    FILE *f4 = fopen(back_path, "rb");
    if (f4 == NULL) {
        fprintf(stderr, "border_ring_conditioned : ouverture de %s impossible\n", back_path);
        return 1;
    }
    long long rev_border_complete = 0, rev_examined = 0;
    long long rev_solvable = 0, rev_infeasible_proven = 0, rev_inconclusive = 0;
    int have_witness = 0;
    struct possibility_packet witness_border_pkt, witness_ring_pkt;
    struct possibility_packet pkt4;
    while (fread(&pkt4, sizeof pkt4, 1, f4) == 1) {
        int bc = 1;
        for (int x = 0; x < ETERN_SIZE && bc; x++)
            for (int y = 0; y < ETERN_SIZE && bc; y++)
                if (brc_is_border_cell(x, y) && pkt4.grid[x][y] == -2) bc = 0;
        if (!bc) continue;
        rev_border_complete++;
        if (rev_examined >= reverse_max) continue;
        rev_examined++;

        struct possibility_packet ring_out;
        int budget_exceeded = 0;
        int found = brc_fill_iring_from_border(rot, &pkt4, reverse_node_budget, reverse_leaf_cap, &ring_out,
                                                &budget_exceeded);
        if (found) {
            rev_solvable++;
            if (!have_witness) {
                have_witness = 1;
                witness_border_pkt = pkt4;
                witness_ring_pkt = ring_out;
            }
        } else if (budget_exceeded) {
            rev_inconclusive++;
        } else {
            rev_infeasible_proven++;
        }
    }
    fclose(f4);

    printf("\n=== mesure A (4/4) : controle inverse, bordures REELLES connues -> anneau ===\n");
    printf("bordures reelles completes (60/60) dans le stock : %lld\n", rev_border_complete);
    printf("bordures examinees                                : %lld\n", rev_examined);
    printf("  -> anneau trouve (solvable)                     : %lld\n", rev_solvable);
    printf("  -> prouve infaisable (DFS exhaustif)             : %lld\n", rev_infeasible_proven);
    printf("  -> inconclusif (budget de noeuds epuise)         : %lld\n", rev_inconclusive);

    /* Témoin : reprendre l'anneau ainsi obtenu et le donner à la méthode
       des passes 1-3 (extraction de demande + DFS de bordure). Ferme la
       boucle : si la bordure réelle qui a servi à construire cet anneau
       n'était PAS retrouvée par le DFS de bordure, ce serait un bug de la
       méthode elle-même, pas un verrou du puzzle — pas seulement pour cet
       anneau-ci, mais pour tout le 0/230 des passes 2/3. */
    if (have_witness) {
        if (!brc_verify_generated_ring(&witness_ring_pkt, rot)) {
            fprintf(stderr, "border_ring_conditioned : TEMOIN ECHEC — anneau du controle inverse invalide\n");
            return 1;
        }
        int demand[BRC_RING_LEN];
        brc_extract_demand(&witness_ring_pkt, rot, demand);

        int witness_ok = 1;
        for (int i = 0; i < BRC_RING_LEN; i++) {
            int16_t rid = witness_border_pkt.grid[g_ring[i].x][g_ring[i].y];
            struct part *p = &rot->parts[rid];
            int8_t face[4] = { p->top, p->right, p->bottom, p->left };
            if (!g_ring[i].is_corner) {
                brc_candidate_list_t cands;
                brc_candidates_for_position(rot, i, demand[i], &cands);
                int found_cand = 0;
                for (int k = 0; k < cands.n; k++) {
                    if (cands.cand[k].rotated_id == rid) { found_cand = 1; break; }
                }
                if (!found_cand) {
                    fprintf(stderr,
                            "border_ring_conditioned : TEMOIN ECHEC — piece reelle en position %d absente"
                            " des candidats pour la demande extraite (%d)\n",
                            i, demand[i]);
                    witness_ok = 0;
                }
            }
            if (i > 0) {
                int16_t prid = witness_border_pkt.grid[g_ring[i - 1].x][g_ring[i - 1].y];
                struct part *pp = &rot->parts[prid];
                int8_t pf[4] = { pp->top, pp->right, pp->bottom, pp->left };
                if (face[g_ring[i].prev_dir] != pf[g_ring[i - 1].next_dir]) {
                    fprintf(stderr, "border_ring_conditioned : TEMOIN ECHEC — adjacence bordure rompue en"
                                     " position %d\n",
                            i);
                    witness_ok = 0;
                }
            }
            if (i == BRC_RING_LEN - 1) {
                int16_t frid = witness_border_pkt.grid[g_ring[0].x][g_ring[0].y];
                struct part *fp = &rot->parts[frid];
                int8_t ff[4] = { fp->top, fp->right, fp->bottom, fp->left };
                if (face[g_ring[i].next_dir] != ff[g_ring[0].prev_dir]) {
                    fprintf(stderr,
                            "border_ring_conditioned : TEMOIN ECHEC — fermeture du cycle de bordure rompue\n");
                    witness_ok = 0;
                }
            }
        }
        printf("\ntemoin (4/4) : bordure reelle vs demande extraite de l'anneau genere : %s\n",
               witness_ok ? "OK - satisfait exactement la demande et la chaine d'adjacence"
                          : "ECHEC (voir messages ci-dessus)");
        if (!witness_ok) {
            return 1;
        }

        brc_dfs_ctx_t wctx;
        memset(&wctx, 0, sizeof wctx);
        wctx.node_budget = witness_node_budget;
        wctx.leaf_cap = witness_leaf_cap;
        brc_dfs(rot, demand, 0, &wctx);
        printf("temoin (4/4) : DFS de completion (methode des passes 1-3) sur cet anneau -> %llu"
               " completion(s) trouvee(s) sur %lld noeuds%s%s\n",
               wctx.leaves_found, wctx.nodes_used,
               wctx.budget_exceeded ? " [budget de noeuds epuise -- inconclusif]" : "",
               wctx.cap_reached ? " [plafond atteint -- il y en a au moins autant]" : "");
        if (wctx.leaves_found == 0) {
            fprintf(stderr,
                    "border_ring_conditioned : TEMOIN ECHEC — le DFS de bordure ne retrouve pas la bordure"
                    " reelle connue (0 completion) : bug de methode, pas verrou du puzzle\n");
            return 1;
        }
    } else {
        printf("\n(pas de temoin possible : aucune bordure reelle n'a permis de completer un anneau)\n");
    }

    printf("\nrappel : la masse INCONDITIONNELLE (couleur interieure wildcard) depasse 9.2e18\n"
           "(border_ring_dp, branche border-mass-walker-design).\n");
    return 0;
}
