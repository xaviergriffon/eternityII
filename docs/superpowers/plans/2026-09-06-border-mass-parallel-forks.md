# `border_mass` Multi-Core Forks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add fork-based multi-core parallelism to `border_mass`: place the 4 corner pieces first (drastically pruning the search early), expand the search tree breadth-first into a set of independent partial states, and fork one worker process per CPU core to finish each partition and sum the results.

**Architecture:** Three new, purely-additive functions in the already-tested pure core (`tests/tools/border_walk.h`/`.c`) — a reordering function, a generalized (parameterized-order, resumable) version of the existing counting DFS, and a breadth-first frontier expansion — plus a thin extension to the CLI driver (`tests/tools/border_mass.c`) that builds the map once, expands the frontier, and forks/pipes/waits. No existing public function's signature or behavior changes.

**Tech Stack:** C (gnu99), POSIX `fork()`/`pipe()`/`waitpid()`/`sysconf()`, the existing `greatest` test framework.

**Spec:** [docs/superpowers/specs/2026-09-06-border-mass-parallel-design.md](../specs/2026-09-06-border-mass-parallel-design.md)

## Global Constraints

- No changes to `src/core/part.c`, `src/core/readdata.c`, or `src/core/etii_search.c`.
- `border_ring_order()` and `border_walk_count()`'s existing signatures and behavior are frozen — already merged (PR #298), already reviewed, other code may depend on them exactly as they are. Every new capability is a new function.
- Every new pure-core test must pass under both compile-time puzzle sizes (`ETERN_PARTS=16`/`ETERN_SIZE=4` and default `ETERN_PARTS=256`/`ETERN_SIZE=16`).
- No custom signal handling in `border_mass.c` — `Ctrl-C` propagates to the whole process group by default; this is the intended behavior, not a gap (see spec, "Interruption").
- No reuse of `fork_gate.c`/`fork_orchestrator.c` — `border_mass` is single-threaded before forking, so their quiescence coordination solves a problem this tool doesn't have (see spec's fork-safety invariant table).
- Commit messages: brief, one line, no `Co-Authored-By` trailer.
- This plan's branch (`border-mass-parallel-forks`) is stacked on `border-mass-walker-design` (PR #298, still open) — `border_walk.h`/`.c` and `border_mass.c` already exist on it in the form shown by the "Current state" blocks below.

---

## File Structure

| File | Responsibility |
|---|---|
| `tests/tools/border_walk.h` | Adds `border_corners_first_order`, `border_walk_count_ordered`, `border_partial_cb`, `border_walk_expand_frontier` declarations. `border_ring_order`/`border_walk_count` unchanged. |
| `tests/tools/border_walk.c` | Adds the three implementations; `border_walk_count`'s body becomes a one-line call into `border_walk_count_ordered`. |
| `tests/tools/test_border_walk.c` | New tests for all three additions, reusing the existing `bw_make_rotate_parts`/`bw_required_face`/`bw_on_found`/`bw_found_record` helpers already in this file. |
| `tests/tools/border_mass.c` | Adds `--forks N` parsing, core-count detection, the fork/pipe/wait orchestration, per-worker progress logging. |
| `docs/tests_et_ci.md`, `tests/README.md` | Document `--forks`. |

---

### Task 1: `border_walk_count_ordered` (parameterized, resumable DFS)

**Files:**
- Modify: `tests/tools/border_walk.h`
- Modify: `tests/tools/border_walk.c`
- Modify: `tests/tools/test_border_walk.c`

**Interfaces:**
- Consumes: existing `BORDER_RING_LEN`, `border_ring_order`, `border_ring_found_cb`, and the existing puzzle-data API (`what_search_in_grid_to_key`, `map_bucket_packed`, `set_face_used`, `is_face_used`, `id_for_rotated_part`) — all unchanged.
- Produces: `long long border_walk_count_ordered(map_big_array *map, struct array_part *all_rotate_parts, const int8_t order[BORDER_RING_LEN][2], int start_depth, const struct possibility_packet *start_state, border_ring_found_cb on_found, void *ctx);` in `tests/tools/border_walk.h`. Tasks 2 and 3 call this exact signature.

**Current state of `tests/tools/border_walk.h`** (for exact context — do not guess, this is what Step 1 below modifies):

```c
/**
 * @file border_walk.h
 * @brief Énumération exhaustive des anneaux de bordure valides du plateau.
 *
 * Cœur pur de l'outil `tests/tools/border_mass.c` (voir
 * docs/superpowers/specs/2026-09-06-masse-bordure-design.md) : aucune
 * entrée/sortie, testable en isolation. Réutilise tel quel l'infrastructure
 * de lookup de `core/part.h`/`core/possibility.h` — aucune fonction nouvelle
 * n'y est nécessaire.
 */
#ifndef eternityII_border_walk_h
#define eternityII_border_walk_h

#include "core/part.h"
#include "core/possibility.h"

/** @brief Nombre de cases du pourtour du plateau (4 coins + bords). */
#define BORDER_RING_LEN (4 * (ETERN_SIZE - 1))

/**
 * @brief Remplit `ring` avec les BORDER_RING_LEN cases du pourtour, dans le
 * sens horaire, en partant de `(0,0)`.
 *
 * Ordre : ligne du haut (`(0,0)` → `(ETERN_SIZE-1,0)`), colonne droite
 * (`(ETERN_SIZE-1,1)` → `(ETERN_SIZE-1,ETERN_SIZE-1)`), ligne du bas
 * (`(ETERN_SIZE-2,ETERN_SIZE-1)` → `(0,ETERN_SIZE-1)`), colonne gauche
 * (`(0,ETERN_SIZE-2)` → `(0,1)`). La dernière case, `(0,1)`, est donc
 * adjacente à `(0,0)` — la fermeture du cycle.
 *
 * @param ring Tableau de sortie, `BORDER_RING_LEN` couples `(x,y)`.
 */
void border_ring_order(int8_t ring[BORDER_RING_LEN][2]);

/**
 * @brief Appelé pour chaque anneau de bordure fermé trouvé par
 * `border_walk_count`. `ring_state` n'est valide que pendant l'appel (le
 * DFS continue son backtracking juste après) — le copier si on veut le
 * garder.
 */
typedef void (*border_ring_found_cb)(const struct possibility_packet *ring_state, void *ctx);

/**
 * @brief Énumère par recherche exhaustive tous les anneaux de bordure
 * valides, ancrés au coin `(0,0)`.
 *
 * Ne pose jamais de case intérieure : `what_search_in_grid_to_key` traite
 * alors le côté intérieur d'une pièce de bord comme joker, exactement le
 * comportement voulu. La fermeture du cycle (dernière case posée, `(0,1)`,
 * adjacente à `(0,0)` déjà posé) est vérifiée par ce même mécanisme, sans
 * code dédié.
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`).
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param on_found         Appelé pour chaque anneau trouvé (peut être NULL).
 * @param ctx              Passé tel quel à `on_found`.
 * @return                 N — la masse totale directement. Aucun voisin
 *                         n'est encore posé à la toute première case : le DFS
 *                         explore donc déjà les 4 coins possibles comme point
 *                         d'ouverture en (0,0), retrouvant chaque anneau
 *                         abstrait une fois par coin. Pas de ×4 à appliquer
 *                         en aval.
 */
long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx);

#endif /* eternityII_border_walk_h */
```

**Current state of `tests/tools/border_walk.c`** (for exact context):

```c
#include "tools/border_walk.h"

#include <string.h>

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

struct bw_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    int8_t ring[BORDER_RING_LEN][2];
    struct possibility_packet state;
    long long count;
    border_ring_found_cb on_found;
    void *user_ctx;
};

static void bw_dfs(struct bw_ctx *ctx, int i)
{
    if (i == BORDER_RING_LEN) {
        ctx->count++;
        if (ctx->on_found != NULL) {
            ctx->on_found(&ctx->state, ctx->user_ctx);
        }
        return;
    }

    int8_t x = ctx->ring[i][0];
    int8_t y = ctx->ring[i][1];

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

        bw_dfs(ctx, i + 1);

        set_face_used(ctx->state.b_faceused, face_idx, 0);
        ctx->state.grid[x][y] = -2;
        ctx->state.alloc = (uint16_t)i;
    }
}

long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx)
{
    struct bw_ctx bw;
    bw.map = map;
    bw.all_rotate_parts = all_rotate_parts;
    border_ring_order(bw.ring);

    memset(&bw.state, 0, sizeof bw.state);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            bw.state.grid[x][y] = -2;
        }
    }
    bw.state.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;

    bw.count = 0;
    bw.on_found = on_found;
    bw.user_ctx = ctx;

    bw_dfs(&bw, 0);
    return bw.count;
}
```

- [ ] **Step 1: Rewrite `border_walk.h`'s counting section**

Replace the `border_walk_count` declaration and its Doxygen comment (the whole block from `/**\n * @brief Énumère par recherche exhaustive...` down to the closing `long long border_walk_count(...)` line) with:

```c
/**
 * @brief Variante de `border_walk_count` acceptant un ordre de parcours
 * explicite et un état de départ optionnel — permet de reprendre depuis un
 * état partiel produit par `border_walk_expand_frontier` (parallélisation
 * par forks) ou de rejouer le même DFS dans un ordre différent (coins
 * d'abord, `border_corners_first_order`).
 *
 * `border_walk_count` (ci-dessous) est un simple appel à cette fonction avec
 * `border_ring_order()` et un état vide — comportement inchangé, aucun test
 * existant à retoucher.
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`).
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param order            Ordre de parcours des BORDER_RING_LEN cases
 *                         (`border_ring_order` ou `border_corners_first_order`).
 * @param start_depth      Index dans `order` à partir duquel poser des
 *                         pièces (0 pour repartir d'un plateau vide).
 * @param start_state      Si non NULL, état du plateau déjà posé jusqu'à
 *                         `start_depth` (copié — jamais modifié). Si NULL,
 *                         part d'un plateau vide (équivalent à
 *                         `start_depth = 0`).
 * @param on_found         Appelé pour chaque anneau trouvé (peut être NULL).
 * @param ctx              Passé tel quel à `on_found`.
 * @return                 Nombre d'anneaux trouvés en complétant depuis
 *                         `start_state`/`start_depth`.
 */
long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx);

/**
 * @brief Énumère par recherche exhaustive tous les anneaux de bordure
 * valides, ancrés au coin `(0,0)`, dans l'ordre `border_ring_order`.
 *
 * Ne pose jamais de case intérieure : `what_search_in_grid_to_key` traite
 * alors le côté intérieur d'une pièce de bord comme joker, exactement le
 * comportement voulu. La fermeture du cycle (dernière case posée, `(0,1)`,
 * adjacente à `(0,0)` déjà posé) est vérifiée par ce même mécanisme, sans
 * code dédié.
 *
 * @param map              Table de lookup pré-calculée (`prepare_map_part`).
 * @param all_rotate_parts Tableau de toutes les rotations (`rotate_all_parts`).
 * @param on_found         Appelé pour chaque anneau trouvé (peut être NULL).
 * @param ctx              Passé tel quel à `on_found`.
 * @return                 N — la masse totale directement. Aucun voisin
 *                         n'est encore posé à la toute première case : le DFS
 *                         explore donc déjà les 4 coins possibles comme point
 *                         d'ouverture en (0,0), retrouvant chaque anneau
 *                         abstrait une fois par coin. Pas de ×4 à appliquer
 *                         en aval.
 */
long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx);
```

- [ ] **Step 2: Rewrite `border_walk.c`'s counting section**

Replace everything from `struct bw_ctx {` through the end of the existing `border_walk_count` function with:

```c
struct bw_ctx {
    map_big_array *map;
    struct array_part *all_rotate_parts;
    const int8_t (*order)[2];
    struct possibility_packet state;
    long long count;
    border_ring_found_cb on_found;
    void *user_ctx;
};

static void bw_dfs(struct bw_ctx *ctx, int i)
{
    if (i == BORDER_RING_LEN) {
        ctx->count++;
        if (ctx->on_found != NULL) {
            ctx->on_found(&ctx->state, ctx->user_ctx);
        }
        return;
    }

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

        bw_dfs(ctx, i + 1);

        set_face_used(ctx->state.b_faceused, face_idx, 0);
        ctx->state.grid[x][y] = -2;
        ctx->state.alloc = (uint16_t)i;
    }
}

long long border_walk_count_ordered(map_big_array *map,
                                     struct array_part *all_rotate_parts,
                                     const int8_t order[BORDER_RING_LEN][2],
                                     int start_depth,
                                     const struct possibility_packet *start_state,
                                     border_ring_found_cb on_found, void *ctx)
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

    bw.count = 0;
    bw.on_found = on_found;
    bw.user_ctx = ctx;

    bw_dfs(&bw, start_depth);
    return bw.count;
}

long long border_walk_count(map_big_array *map,
                             struct array_part *all_rotate_parts,
                             border_ring_found_cb on_found, void *ctx)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    return border_walk_count_ordered(map, all_rotate_parts, ring, 0, NULL, on_found, ctx);
}
```

- [ ] **Step 3: Run the existing tests to confirm zero regression**

Run: `make test 2>&1 | tail -40`
Expected: PASS — all 5 existing `border_walk_suite` tests still green in both `test-16` and `test-256` (they call `border_walk_count`, whose observable behavior is unchanged — it now delegates to `border_walk_count_ordered` internally, but produces identical results).

- [ ] **Step 4: Write the new resume test**

In `tests/tools/test_border_walk.c`, add before `SUITE(border_walk_suite)`:

```c
/* border_walk_count_ordered avec start_depth=0/start_state=NULL doit se
   comporter EXACTEMENT comme border_walk_count — non-régression du
   refactor qui a fait de border_walk_count un simple appel à cette
   fonction. */
TEST border_walk_count_ordered_matches_border_walk_count_from_scratch(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    long long n = border_walk_count_ordered(map, all, ring, 0, NULL, NULL, NULL);
    ASSERT_EQ_FMT(4LL, n, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Reprendre depuis un état partiel (le premier coin déjà posé avec SA pièce
   spécifique, pas n'importe laquelle des 4) doit trouver exactement 1
   solution, pas 4 — fixer quelle pièce ouvre l'anneau élimine les 3 autres
   placements rotationnels. Construit l'état partiel à la main : la pièce
   d'id 1, rotation 0, est par construction (bw_required_face) l'UNIQUE
   pièce qui satisfait exactement la clé de la case ring[0] sans rotation
   (voir le commentaire de bw_required_face). */
TEST border_walk_count_ordered_resumes_from_a_partial_state(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    struct possibility_packet start;
    memset(&start, 0, sizeof start);
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            start.grid[x][y] = -2;
        }
    }
    start.min_candidats = POSSIBILITY_MIN_CANDIDATS_UNKNOWN;
    start.grid[ring[0][0]][ring[0][1]] = (int16_t)id_for_rotated_part(1, 0);
    set_face_used(start.b_faceused, 0, 1); /* pièce id 1, base 0 */
    start.alloc = 1;

    long long n = border_walk_count_ordered(map, all, ring, 1, &start, NULL, NULL);
    ASSERT_EQ_FMT(1LL, n, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
```

Then add both new tests to the `SUITE` block:

```c
SUITE(border_walk_suite)
{
    RUN_TEST(border_ring_order_has_the_right_length_and_starts_at_origin);
    RUN_TEST(border_ring_order_visits_every_perimeter_cell_exactly_once);
    RUN_TEST(border_ring_order_is_a_closed_walk_of_adjacent_cells);
    RUN_TEST(border_walk_count_returns_zero_without_any_border_shaped_piece);
    RUN_TEST(border_walk_count_finds_the_crafted_ring_once_per_opening_corner);
    RUN_TEST(border_walk_count_ordered_matches_border_walk_count_from_scratch);
    RUN_TEST(border_walk_count_ordered_resumes_from_a_partial_state);
}
```

- [ ] **Step 5: Run the tests**

Run: `make test 2>&1 | tail -60`
Expected: PASS — 7/7 `border_walk_suite` tests, both `test-16` and `test-256`.

- [ ] **Step 6: Commit**

```bash
git add tests/tools/border_walk.h tests/tools/border_walk.c tests/tools/test_border_walk.c
git commit -m "Ajoute border_walk_count_ordered (ordre et reprise paramétrables)"
```

---

### Task 2: `border_corners_first_order`

**Files:**
- Modify: `tests/tools/border_walk.h`
- Modify: `tests/tools/border_walk.c`
- Modify: `tests/tools/test_border_walk.c`

**Interfaces:**
- Consumes: `BORDER_RING_LEN`, `border_ring_order` (unchanged), `border_walk_count_ordered` (Task 1).
- Produces: `void border_corners_first_order(int8_t order[BORDER_RING_LEN][2]);` in `tests/tools/border_walk.h`. Task 4's CLI driver calls this.

- [ ] **Step 1: Write the failing tests**

In `tests/tools/test_border_walk.c`, add before `SUITE(border_walk_suite)`:

```c
static int bw_is_corner(int x, int y)
{
    return (x == 0 || x == ETERN_SIZE - 1) && (y == 0 || y == ETERN_SIZE - 1);
}

TEST border_corners_first_order_visits_the_same_cells_as_border_ring_order(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    int8_t order[BORDER_RING_LEN][2];
    border_ring_order(ring);
    border_corners_first_order(order);

    int seen[ETERN_SIZE][ETERN_SIZE];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        ASSERT_EQ_FMT(0, seen[order[i][0]][order[i][1]], "%d");
        seen[order[i][0]][order[i][1]] = 1;
    }
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        ASSERT_EQ_FMT(1, seen[ring[i][0]][ring[i][1]], "%d");
    }
    PASS();
}

TEST border_corners_first_order_puts_all_4_corners_first(void)
{
    int8_t order[BORDER_RING_LEN][2];
    border_corners_first_order(order);

    for (int i = 0; i < 4; i++) {
        ASSERT(bw_is_corner(order[i][0], order[i][1]));
    }
    for (int i = 4; i < BORDER_RING_LEN; i++) {
        ASSERT(!bw_is_corner(order[i][0], order[i][1]));
    }
    PASS();
}

/* Le réordonnement ne doit rien changer au compte : chaque coin peut
   toujours ouvrir la recherche indépendamment (aucun voisin posé à la toute
   première case, quel que soit l'ordre), donc le même anneau unique est
   toujours retrouvé 4 fois. */
TEST border_corners_first_order_does_not_change_the_count(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    int8_t order[BORDER_RING_LEN][2];
    border_corners_first_order(order);

    long long n = border_walk_count_ordered(map, all, order, 0, NULL, NULL, NULL);
    ASSERT_EQ_FMT(4LL, n, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
```

Add all three to the `SUITE` block (after `border_walk_count_ordered_resumes_from_a_partial_state`):

```c
    RUN_TEST(border_corners_first_order_visits_the_same_cells_as_border_ring_order);
    RUN_TEST(border_corners_first_order_puts_all_4_corners_first);
    RUN_TEST(border_corners_first_order_does_not_change_the_count);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make test 2>&1 | tail -40`
Expected: FAIL to link — `undefined reference to 'border_corners_first_order'`.

- [ ] **Step 3: Declare and implement**

In `tests/tools/border_walk.h`, add after the `border_ring_order` declaration (before the `border_ring_found_cb` typedef):

```c
/**
 * @brief Remplit `order` avec les BORDER_RING_LEN cases du pourtour, les 4
 * coins en premier (dans leur ordre `border_ring_order`), puis les cases de
 * bord dans leur ordre `border_ring_order` habituel.
 *
 * Ne change aucune propriété de comptage de `border_walk_count_ordered` : le
 * raisonnement « N est déjà la masse » (aucun voisin posé à la toute
 * première case, donc n'importe lequel des 4 coins peut ouvrir la
 * recherche) tient toujours — les 4 coins sont simplement tous posés tôt au
 * lieu qu'un seul le soit et que les 3 autres se déduisent en refermant le
 * cycle. Sert à réduire drastiquement le facteur de branchement initial :
 * très peu de pièces ont 2 faces nulles adjacentes (4 sur le jeu 256
 * pièces réel), contre des dizaines de candidats pour une case de bord
 * arbitraire.
 *
 * @param order Tableau de sortie, `BORDER_RING_LEN` couples `(x,y)`.
 */
void border_corners_first_order(int8_t order[BORDER_RING_LEN][2]);
```

In `tests/tools/border_walk.c`, add after `border_ring_order`'s closing brace:

```c
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
```

- [ ] **Step 4: Run the tests**

Run: `make test 2>&1 | tail -60`
Expected: PASS — 10/10 `border_walk_suite` tests, both `test-16` and `test-256`.

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_walk.h tests/tools/border_walk.c tests/tools/test_border_walk.c
git commit -m "Ajoute border_corners_first_order (coins en premier)"
```

---

### Task 3: `border_walk_expand_frontier`

**Files:**
- Modify: `tests/tools/border_walk.h`
- Modify: `tests/tools/border_walk.c`
- Modify: `tests/tools/test_border_walk.c`

**Interfaces:**
- Consumes: `BORDER_RING_LEN`, `border_ring_found_cb`, `border_walk_count_ordered` (Task 1). Uses the same low-level puzzle-data calls as `bw_dfs` (`what_search_in_grid_to_key`, `map_bucket_packed`, `set_face_used`, `is_face_used`, `id_for_rotated_part`) — this is deliberate, minor duplication of the "try candidates at one cell" logic rather than a forced shared abstraction: `bw_dfs` mutates one shared state in place and backtracks (recursive DFS), while frontier expansion builds new independent copies level by level (iterative BFS) — different enough control flow that sharing a helper would need its own indirection layer for little benefit.
- Produces: `typedef void (*border_partial_cb)(const struct possibility_packet *partial_state, int depth, void *ctx);` and `long long border_walk_expand_frontier(map_big_array *map, struct array_part *all_rotate_parts, const int8_t order[BORDER_RING_LEN][2], int target_partitions, border_partial_cb on_partial, void *partial_ctx, border_ring_found_cb on_complete, void *complete_ctx);` in `tests/tools/border_walk.h`. Task 4's CLI driver calls this.

- [ ] **Step 1: Write the failing tests**

In `tests/tools/test_border_walk.c`, add before `SUITE(border_walk_suite)`:

```c
struct bw_frontier_collect {
    struct possibility_packet states[BORDER_RING_LEN + 1];
    int depths[BORDER_RING_LEN + 1];
    int count;
};

static void bw_collect_partial(const struct possibility_packet *partial_state, int depth, void *ctx)
{
    struct bw_frontier_collect *c = (struct bw_frontier_collect *)ctx;
    c->states[c->count] = *partial_state;
    c->depths[c->count] = depth;
    c->count++;
}

/* La toute première case (un coin) n'a aucun voisin posé : n'importe lequel
   des 4 coins-pièces peut l'occuper. L'expansion à un seul niveau (target
   petit) doit donc produire exactement 4 états partiels, tous à la
   profondeur 1 — et reprendre chacun avec border_walk_count_ordered doit
   donner un total de 4, identique à border_walk_count. */
TEST border_walk_expand_frontier_then_resume_matches_direct_count(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    struct bw_frontier_collect collect;
    memset(&collect, 0, sizeof collect);

    long long completed_during_expansion =
        border_walk_expand_frontier(map, all, ring, 2, bw_collect_partial, &collect, NULL, NULL);

    ASSERT_EQ_FMT(0LL, completed_during_expansion, "%lld");
    ASSERT_EQ_FMT(4, collect.count, "%d");
    for (int i = 0; i < collect.count; i++) {
        ASSERT_EQ_FMT(1, collect.depths[i], "%d");
    }

    long long total = completed_during_expansion;
    for (int i = 0; i < collect.count; i++) {
        total += border_walk_count_ordered(map, all, ring, collect.depths[i], &collect.states[i], NULL, NULL);
    }
    ASSERT_EQ_FMT(4LL, total, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Cas dégénéré : la fixture n'a qu'un seul anneau abstrait possible, donc le
   facteur de branchement au-delà du premier niveau est minuscule (au plus 1
   candidat par case, les couleurs étant uniques par arête) — un target de
   partitions énorme force l'expansion à épuiser tout l'arbre : toutes les
   complétions sont comptées via on_complete, la frontière finale est vide. */
TEST border_walk_expand_frontier_exhausts_the_tree_when_target_is_too_high(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    struct bw_frontier_collect collect;
    memset(&collect, 0, sizeof collect);

    long long completed = border_walk_expand_frontier(map, all, ring, 10000,
                                                        bw_collect_partial, &collect, NULL, NULL);

    ASSERT_EQ_FMT(4LL, completed, "%lld");
    ASSERT_EQ_FMT(0, collect.count, "%d");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
```

Add both to the `SUITE` block:

```c
    RUN_TEST(border_walk_expand_frontier_then_resume_matches_direct_count);
    RUN_TEST(border_walk_expand_frontier_exhausts_the_tree_when_target_is_too_high);
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make test 2>&1 | tail -40`
Expected: FAIL to link — `undefined reference to 'border_walk_expand_frontier'`.

- [ ] **Step 3: Declare and implement**

In `tests/tools/border_walk.h`, add after `border_walk_count`'s declaration, before the closing `#endif`:

```c
/**
 * @brief Appelé pour chaque état partiel de la frontière finale produite par
 * `border_walk_expand_frontier`. `partial_state` n'est valide que pendant
 * l'appel — le copier si on veut le garder (c'est un besoin réel : ces
 * états sont ensuite distribués à des workers forkés).
 */
typedef void (*border_partial_cb)(const struct possibility_packet *partial_state,
                                   int depth, void *ctx);

/**
 * @brief Étend le plateau en largeur (BFS, un niveau entier à la fois)
 * jusqu'à ce que le nombre d'états partiels atteigne `target_partitions`,
 * ou que `BORDER_RING_LEN` soit atteint (jeu de pièces trop petit pour
 * produire assez de partitions).
 *
 * Développe toujours un niveau ENTIER avant de tester la cible — jamais
 * coupé en cours de route — pour ne pas biaiser la frontière vers les
 * premières branches explorées dans `order`.
 *
 * Analogue en miniature de `expand_datas_to_level` (`core/datamanager.c`) :
 * même idée (peupler un ensemble d'états à répartir), sans stock ni
 * persistance — tout tient en mémoire, le temps de l'appel.
 *
 * @param map                Table de lookup pré-calculée.
 * @param all_rotate_parts   Tableau de toutes les rotations.
 * @param order              Ordre de parcours (`border_ring_order` ou
 *                           `border_corners_first_order`).
 * @param target_partitions  Nombre d'états partiels visés (peut être
 *                           dépassé : un niveau entier est toujours
 *                           développé en une fois).
 * @param on_partial         Appelé pour chaque état partiel de la frontière
 *                           finale, avec sa profondeur (l'index dans
 *                           `order` à partir duquel `border_walk_count_ordered`
 *                           doit reprendre). Peut être NULL.
 * @param partial_ctx        Passé tel quel à `on_partial`.
 * @param on_complete        Appelé pour chaque anneau complet trouvé
 *                           PENDANT l'expansion (jeu de pièces trop petit
 *                           pour atteindre `target_partitions` sans épuiser
 *                           l'arbre) — ne jamais compter ces anneaux une
 *                           deuxième fois côté appelant. Peut être NULL.
 * @param complete_ctx       Passé tel quel à `on_complete`.
 * @return                   Nombre d'anneaux comptés via `on_complete`
 *                           pendant l'expansion elle-même (0 dans le cas
 *                           courant où la frontière est atteinte avant
 *                           d'épuiser l'arbre).
 */
long long border_walk_expand_frontier(map_big_array *map,
                                       struct array_part *all_rotate_parts,
                                       const int8_t order[BORDER_RING_LEN][2],
                                       int target_partitions,
                                       border_partial_cb on_partial, void *partial_ctx,
                                       border_ring_found_cb on_complete, void *complete_ctx);
```

In `tests/tools/border_walk.c`, add `#include <stdlib.h>` at the top (needed for `malloc`/`realloc`/`free`), and add this function at the end of the file:

```c
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

    while (level_size < target_partitions && depth < BORDER_RING_LEN) {
        int8_t x = order[depth][0];
        int8_t y = order[depth][1];

        struct possibility_packet *next_level = NULL;
        int next_size = 0;
        int next_cap = 0;

        for (int e = 0; e < level_size; e++) {
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
                    if (on_complete != NULL) {
                        on_complete(&child, complete_ctx);
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
    }

    if (on_partial != NULL) {
        for (int e = 0; e < level_size; e++) {
            on_partial(&level[e], depth, partial_ctx);
        }
    }
    free(level);

    return completed;
}
```

- [ ] **Step 4: Run the tests**

Run: `make test 2>&1 | tail -60`
Expected: PASS — 12/12 `border_walk_suite` tests, both `test-16` and `test-256`.

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_walk.h tests/tools/border_walk.c tests/tools/test_border_walk.c
git commit -m "Ajoute border_walk_expand_frontier (expansion BFS pour partitionner)"
```

---

### Task 4: `border_mass.c` — `--forks N` and fork/pipe/wait orchestration

**Files:**
- Modify: `tests/tools/border_mass.c`

**Interfaces:**
- Consumes: `border_corners_first_order`, `border_walk_count_ordered`, `border_partial_cb`, `border_walk_expand_frontier` (Tasks 1-3) — exact signatures as declared in `tests/tools/border_walk.h` after Task 3.

**Current state of `tests/tools/border_mass.c`** (for exact context — this task modifies it, do not guess the starting point):

```c
/*
 * border_mass — mesure la masse totale des anneaux de bordure valides.
 *
 * Énumère par recherche exhaustive tous les anneaux de bordure valides
 * (BORDER_RING_LEN cases du pourtour du plateau), ancrés au coin (0,0), et
 * rapporte N — la masse totale directement. Aucun voisin n'est encore posé à
 * la toute première case : le DFS explore donc déjà les 4 coins possibles
 * comme point d'ouverture, retrouvant chaque anneau abstrait une fois par
 * coin — pas de ×4 supplémentaire à appliquer (constaté empiriquement
 * pendant l'implémentation, cf. docs/superpowers/specs/2026-09-06-masse-bordure-design.md
 * pour le raisonnement complet). Cela suppose qu'aucun indice officiel ne
 * touche le bord (vérifié ci-dessous) — sinon un seul coin serait valide,
 * pas 4.
 *
 * Toute la logique d'énumération vit dans tests/tools/border_walk.c, testée
 * unitairement ; ce fichier n'est que l'enveloppe d'entrées/sorties.
 *
 * Usage :
 *   make border-mass
 *   tests/tools/border_mass data/pieces.csv data/indices.csv
 */
#include <stdio.h>

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"

static int border_mass_check_indices_not_on_border(const struct array_index *indices)
{
    for (int i = 0; i < indices->size; i++) {
        int x = indices->indices[i].x;
        int y = indices->indices[i].y;
        if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1 ||
            x >= ETERN_SIZE || y >= ETERN_SIZE) {
            fprintf(stderr,
                    "border_mass : l'indice officiel id=%d est en (%d,%d), sur le bord — "
                    "un seul coin pourrait alors ouvrir la recherche, ce chiffre ne serait "
                    "plus la masse totale, refus de continuer\n",
                    indices->indices[i].id, x, y);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <pieces.csv> <indices.csv>\n", argv[0]);
        return 2;
    }

    struct array_index *indices = read_indices(argv[2]);
    int indices_ok = (border_mass_check_indices_not_on_border(indices) == 0);
    free_array_index(indices);
    if (!indices_ok) {
        return 1;
    }

    struct array_part *apart = read_parts(argv[1]);
    struct array_part *all = rotate_all_parts(apart);
    map_big_array *map = prepare_map_part(all);
    if (map == NULL) {
        fprintf(stderr, "border_mass : construction de la map de lookup impossible\n");
        return 1;
    }

    long long n = border_walk_count(map, all, NULL, NULL);
    printf("masse totale des anneaux de bordure valides : %lld\n", n);

    return 0;
}
```

- [ ] **Step 1: Rewrite the file**

Replace the whole file with:

```c
/*
 * border_mass — mesure la masse totale des anneaux de bordure valides.
 *
 * Énumère par recherche exhaustive tous les anneaux de bordure valides
 * (BORDER_RING_LEN cases du pourtour du plateau), ancrés au coin (0,0), et
 * rapporte N — la masse totale directement. Aucun voisin n'est encore posé à
 * la toute première case : le DFS explore donc déjà les 4 coins possibles
 * comme point d'ouverture, retrouvant chaque anneau abstrait une fois par
 * coin — pas de ×4 supplémentaire à appliquer. Cela suppose qu'aucun indice
 * officiel ne touche le bord (vérifié ci-dessous).
 *
 * `--forks N` (défaut : nombre de cœurs détecté) parallélise : le plateau
 * est posé dans l'ordre « coins d'abord » (border_corners_first_order —
 * très peu de pièces ont 2 faces nulles adjacentes, donc le facteur de
 * branchement des 4 premières étapes est minuscule), étendu en largeur
 * jusqu'à N*8 états partiels (border_walk_expand_frontier), puis distribué
 * en round-robin à N process forkés — chacun termine sa part avec
 * border_walk_count_ordered et renvoie son sous-total au parent par un
 * pipe dédié. La map de lookup est construite UNE SEULE FOIS avant tout
 * fork (héritée en COW par les enfants), comme le fait déjà main.c pour le
 * serveur/client réel.
 *
 * Aucune coordination façon fork_gate.c : border_mass est mono-thread avant
 * de forker ses workers, donc le problème que fork_gate.c résout (un thread
 * du parent qui tourne encore pendant le fork()) ne se pose pas ici — voir
 * docs/superpowers/specs/2026-09-06-border-mass-parallel-design.md.
 *
 * Aucun gestionnaire de signal : Ctrl-C envoie SIGINT à tout le groupe de
 * process (parent + enfants forkés, aucun setpgid/setsid n'est appelé) —
 * comportement par défaut du terminal, suffisant, volontairement pas
 * réimplémenté.
 *
 * Toute la logique d'énumération vit dans tests/tools/border_walk.c, testée
 * unitairement ; ce fichier n'est que l'enveloppe d'entrées/sorties et
 * l'orchestration fork/pipe/wait, non testée unitairement (comme
 * gen_root.c) — vérifiée par smoke test manuel (--forks 1 vs --forks 4 sur
 * le jeu 16 pièces, même total).
 *
 * Usage :
 *   make border-mass
 *   tests/tools/border_mass [--forks N] data/pieces.csv data/indices.csv
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "core/readdata.h"
#include "core/part.h"
#include "core/possibility.h"
#include "core/core_static_variables.h"
#include "tools/border_walk.h"

static int border_mass_check_indices_not_on_border(const struct array_index *indices)
{
    for (int i = 0; i < indices->size; i++) {
        int x = indices->indices[i].x;
        int y = indices->indices[i].y;
        if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1 ||
            x >= ETERN_SIZE || y >= ETERN_SIZE) {
            fprintf(stderr,
                    "border_mass : l'indice officiel id=%d est en (%d,%d), sur le bord — "
                    "un seul coin pourrait alors ouvrir la recherche, ce chiffre ne serait "
                    "plus la masse totale, refus de continuer\n",
                    indices->indices[i].id, x, y);
            return -1;
        }
    }
    return 0;
}

struct bm_partition {
    struct possibility_packet state;
    int depth;
};

struct bm_collect_ctx {
    struct bm_partition *partitions;
    int count;
    int cap;
};

static void bm_collect_partial(const struct possibility_packet *partial_state, int depth, void *ctx_)
{
    struct bm_collect_ctx *ctx = (struct bm_collect_ctx *)ctx_;
    if (ctx->count == ctx->cap) {
        ctx->cap = (ctx->cap == 0) ? 16 : ctx->cap * 2;
        ctx->partitions = realloc(ctx->partitions, (size_t)ctx->cap * sizeof *ctx->partitions);
    }
    ctx->partitions[ctx->count].state = *partial_state;
    ctx->partitions[ctx->count].depth = depth;
    ctx->count++;
}

static int bm_default_forks(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) {
        n = 1;
    }
    return (int)n;
}

/* Traite les partitions worker_id, worker_id+nb_workers, worker_id+2*nb_workers,
   ... — round-robin plutôt qu'un bloc contigu, pour ne pas concentrer un
   déséquilibre de charge entre sous-arbres voisins sur un seul worker. */
static long long bm_run_worker(int worker_id, int nb_workers,
                                map_big_array *map, struct array_part *all,
                                const int8_t order[BORDER_RING_LEN][2],
                                const struct bm_partition *partitions, int nb_partitions)
{
    long long total = 0;
    int done = 0;
    int assigned = 0;
    for (int p = worker_id; p < nb_partitions; p += nb_workers) {
        assigned++;
    }
    for (int p = worker_id; p < nb_partitions; p += nb_workers) {
        long long sub = border_walk_count_ordered(map, all, order, partitions[p].depth,
                                                    &partitions[p].state, NULL, NULL);
        total += sub;
        done++;
        fprintf(stderr, "border_mass[worker %d] : partition %d/%d terminee, sous-total %lld\n",
                worker_id, done, assigned, total);
    }
    return total;
}

int main(int argc, char **argv)
{
    int forks = bm_default_forks();
    int argi = 1;
    if (argc >= 2 && strcmp(argv[1], "--forks") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s [--forks N] <pieces.csv> <indices.csv>\n", argv[0]);
            return 2;
        }
        forks = atoi(argv[2]);
        if (forks < 1) {
            forks = 1;
        }
        argi = 3;
    }
    if (argc - argi != 2) {
        fprintf(stderr, "usage: %s [--forks N] <pieces.csv> <indices.csv>\n", argv[0]);
        return 2;
    }
    const char *pieces_path = argv[argi];
    const char *indices_path = argv[argi + 1];

    struct array_index *indices = read_indices(indices_path);
    int indices_ok = (border_mass_check_indices_not_on_border(indices) == 0);
    free_array_index(indices);
    if (!indices_ok) {
        return 1;
    }

    struct array_part *apart = read_parts(pieces_path);
    struct array_part *all = rotate_all_parts(apart);
    map_big_array *map = prepare_map_part(all);
    if (map == NULL) {
        fprintf(stderr, "border_mass : construction de la map de lookup impossible\n");
        return 1;
    }

    int8_t order[BORDER_RING_LEN][2];
    border_corners_first_order(order);

    struct bm_collect_ctx collect;
    memset(&collect, 0, sizeof collect);
    long long completed_during_expansion =
        border_walk_expand_frontier(map, all, order, forks * 8,
                                     bm_collect_partial, &collect, NULL, NULL);

    fprintf(stderr, "border_mass : %d partitions, %d worker(s)\n", collect.count, forks);

    if (collect.count == 0) {
        printf("masse totale des anneaux de bordure valides : %lld\n", completed_during_expansion);
        free(collect.partitions);
        return 0;
    }

    int (*pipes)[2] = malloc((size_t)forks * sizeof *pipes);
    pid_t *pids = malloc((size_t)forks * sizeof *pids);

    for (int w = 0; w < forks; w++) {
        if (pipe(pipes[w]) != 0) {
            fprintf(stderr, "border_mass : pipe() a echoue pour le worker %d\n", w);
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "border_mass : fork() a echoue pour le worker %d\n", w);
            return 1;
        }
        if (pid == 0) {
            close(pipes[w][0]);
            long long sub = bm_run_worker(w, forks, map, all, order, collect.partitions, collect.count);
            dprintf(pipes[w][1], "%lld\n", sub);
            close(pipes[w][1]);
            exit(0);
        }
        close(pipes[w][1]);
        pids[w] = pid;
    }

    long long total = completed_during_expansion;
    for (int w = 0; w < forks; w++) {
        char buf[64];
        ssize_t n = read(pipes[w][0], buf, sizeof buf - 1);
        close(pipes[w][0]);
        int status;
        waitpid(pids[w], &status, 0);
        if (n <= 0) {
            fprintf(stderr, "border_mass : aucun resultat lu du worker %d\n", w);
            continue;
        }
        buf[n] = '\0';
        total += strtoll(buf, NULL, 10);
    }

    printf("masse totale des anneaux de bordure valides : %lld\n", total);

    free(pipes);
    free(pids);
    free(collect.partitions);
    return 0;
}
```

- [ ] **Step 2: Build**

Run: `make border-mass 2>&1 | tail -30`
Expected: compiles cleanly under `-Werror`.

- [ ] **Step 3: Smoke test — `--forks 1` and `--forks 4` agree, on the small real puzzle**

`data/indices.csv` holds the 256-piece indices (coordinates in `{2,7,8,13}`), which don't fit inside the 4×4 board `pieces16.csv` maps to — every one of them would trip the "out of grid" branch of the precondition check (`x >= ETERN_SIZE`) and the tool would correctly refuse to run. Rather than fight that mismatch, create a trivial empty indices file for this smoke test only (an empty indices file vacuously satisfies "no official index sits on a border cell"):

Run: `printf 'nindices: 0\n' > /tmp/etii_smoke_indices.csv`

Then:

Run: `tests/tools/border_mass --forks 1 data/pieces16.csv /tmp/etii_smoke_indices.csv`
Expected: last line `masse totale des anneaux de bordure valides : 4`, exits 0.

Run: `tests/tools/border_mass --forks 4 data/pieces16.csv /tmp/etii_smoke_indices.csv`
Expected: same total, `4` (worker progress lines on stderr may interleave, that's expected — see the file's header comment).

Run: `rm /tmp/etii_smoke_indices.csv` to clean up.

- [ ] **Step 4: Commit**

```bash
git add tests/tools/border_mass.c
git commit -m "Ajoute --forks a border_mass (parallelisation multi-coeur)"
```

---

### Task 5: Documentation

**Files:**
- Modify: `docs/tests_et_ci.md` (the `border_mass` section added by PR #298)
- Modify: `tests/README.md` (the `border_mass` paragraph added by PR #298)

**Interfaces:** None — documentation only.

- [ ] **Step 1: Update `docs/tests_et_ci.md`**

Find the `## Outil \`border_mass\` (\`make border-mass\`)` section. Immediately after its `\`\`\`sh ... \`\`\`` usage block (and before the "Mesuré empiriquement..." paragraph already added by PR #298, if present — insert before it), add:

```markdown
`--forks N` (défaut : nombre de cœurs détecté) parallélise la recherche :
le plateau est posé dans l'ordre « coins d'abord » plutôt que l'ordre
séquentiel (très peu de pièces ont 2 faces nulles adjacentes — 4 sur le
jeu 256 pièces réel — donc le facteur de branchement des 4 premières étapes
est minuscule), la recherche est étendue en largeur jusqu'à `N*8` états
partiels puis distribuée à `N` process forkés. Chaque worker journalise sa
progression sur `stderr` (une ligne par partition terminée). Voir
[docs/superpowers/specs/2026-09-06-border-mass-parallel-design.md](superpowers/specs/2026-09-06-border-mass-parallel-design.md)
pour le raisonnement complet (notamment pourquoi ceci ne réutilise pas
`fork_gate.c`).
```

- [ ] **Step 2: Update `tests/README.md`**

Find the `border_mass` paragraph added by PR #298. Append one sentence:

```markdown
`--forks N` parallélise par forks (coins d'abord, expansion en largeur,
distribution round-robin) — voir la spec liée depuis `docs/tests_et_ci.md`.
```

- [ ] **Step 3: Verify**

Run: `grep -c "forks" docs/tests_et_ci.md tests/README.md`
Expected: at least 1 match in each file.

- [ ] **Step 4: Commit**

```bash
git add docs/tests_et_ci.md tests/README.md
git commit -m "Documente --forks de border_mass"
```

---

## Self-Review Notes

- **Spec coverage:** every spec section maps to a task — `border_corners_first_order` (Task 2), the generalized/resumable DFS (Task 1), the frontier expansion (Task 3), the CLI's fork/pipe/wait orchestration + progress logging + no-signal-handling (Task 4), docs (Task 5). The spec's Non-objectifs (dynamic work-stealing, `fork_gate.c` reuse, interrupt/resume, root generation) have deliberately no task.
- **Type consistency checked:** `border_walk_count_ordered`'s signature (declared Task 1) is used identically in Task 2's and Task 3's tests and in Task 4's driver. `border_partial_cb`/`border_walk_expand_frontier` (declared Task 3) are used identically in Task 4. `border_corners_first_order` (declared Task 2) is used identically in Task 4.
- **No placeholders:** every step carries complete code. Task 4's smoke-test step has a conditional ("if the 16-piece indices file check trips the precondition, fall back to...") — this is a legitimate environment-dependent branch (whether a matching small indices file exists), not a vague instruction, and both branches are fully specified.
