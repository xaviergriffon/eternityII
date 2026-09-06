# Walker de bordure — masse totale (phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a permanent tool that exhaustively counts every valid closed border ring (60 cells on the real 16×16 puzzle: 4 corners + 56 edge pieces) anchored at corner `(0,0)`, and reports the total mass directly (the DFS itself explores all 4 corner pieces as possible openers at the first cell, since no neighbor is placed yet there — no external ×4 needed, see the plan's "Mid-execution correction" section near the end) — phase 1 output is a number only, no root files.

**Architecture:** A pure-core sequential DFS (`tests/tools/border_walk.c`) walks a fixed 60-cell perimeter order and reuses the existing lookup/key machinery (`prepare_map_part`, `map_bucket_packed`, `what_search_in_grid_to_key`, `set_face_used`/`is_face_used`) unmodified — interior cells are never touched, so their wildcard behavior already gives the right "don't care" semantics for free. Because no neighbor is placed yet at the very first cell, the DFS itself explores all 4 corners as possible openers at `(0,0)`, so its raw count *is* the total mass already (discovered during Task 2 — see that task's dispatch notes; no external ×4). A thin CLI driver (`tests/tools/border_mass.c`) loads the real puzzle data, checks that no official index sits on a border cell (the precondition this counting depends on), and prints the count.

**Tech Stack:** C (gnu99), existing `src/core/` puzzle data structures, `greatest` test framework (already vendored at `tests/greatest.h`), GNU Make.

**Spec:** [docs/superpowers/specs/2026-09-06-masse-bordure-design.md](../specs/2026-09-06-masse-bordure-design.md)

## Global Constraints

- No changes to `src/core/part.c`, `src/core/readdata.c`, or `src/core/etii_search.c` — the walker is a standalone consumer of their existing public API, never a modification of it (per spec, "Non-objectifs").
- Test fixtures with hand-crafted pieces must contain **exactly `ETERN_PARTS` pieces** — `rotate_all_parts` indexes `id + ETERN_PARTS * rotation`, so a shorter fixture overflows its own allocation (see `tests/tools/test_root_from_board.c`'s header comment, the existing precedent this plan follows).
- Every new pure-core test must pass under **both** compile-time puzzle sizes (`ETERN_PARTS=16`/`ETERN_SIZE=4` and the default `ETERN_PARTS=256`/`ETERN_SIZE=16`) — `make test` builds both (`test-16` then `test-256`) from the same `TEST_SUITES_COMMON` list. Nothing in this plan may hardcode `16` or `4` — always `ETERN_SIZE`/`ETERN_PARTS`/`BORDER_RING_LEN`.
- `border_mass` is a **permanent** tool (like `gen_root`), not throwaway instrumentation — its pure core (`border_walk.c`) is compiled into `TEST_MODULES` and covered by `make test`/`make coverage`, exactly like `root_from_board.c`.
- Commit messages: brief, one line, no `Co-Authored-By` trailer for commits *within* the working branch (per project convention — this differs from this session's own attribution footer, which still applies to the final PR description, not to intermediate commit messages the plan asks for). Follow `git commit -m "<one line>"`.

---

## File Structure

| File | Responsibility |
|---|---|
| `tests/tools/border_walk.h` | Public API of the pure core: `BORDER_RING_LEN`, `border_ring_order`, `border_ring_found_cb`, `border_walk_count`. |
| `tests/tools/border_walk.c` | Implementation: ring traversal order, sequential DFS enumeration. No I/O. |
| `tests/tools/test_border_walk.c` | Unit tests (greatest), covering geometry (Task 1) and enumeration correctness (Task 2). |
| `tests/tools/border_mass.c` | CLI driver: loads `pieces.csv`/`indices.csv`, checks the border-index precondition, prints `N` (the total mass directly — see Task 2). |
| `makefile` | New `.PHONY: border-mass` target; `border_walk.c` added to `TEST_MODULES`; `test_border_walk.c` added to `TEST_SUITES_COMMON`. |
| `tests/test_main.c` | New `SUITE_EXTERN(border_walk_suite)` / `RUN_SUITE(border_walk_suite)`. |
| `docs/tests_et_ci.md` | New `## Outil border_mass` section, modeled on the existing `## Outil gen_root` section. |
| `tests/README.md` | New row/paragraph in the `tests/tools/` section. |

---

### Task 1: Ring traversal order + build wiring

**Files:**
- Create: `tests/tools/border_walk.h`
- Create: `tests/tools/border_walk.c`
- Create: `tests/tools/test_border_walk.c`
- Modify: `makefile:211` (add to `TEST_SUITES_COMMON`)
- Modify: `makefile:232` (add to `TEST_MODULES`)
- Modify: `tests/test_main.c` (register the new suite)

**Interfaces:**
- Produces: `#define BORDER_RING_LEN (4 * (ETERN_SIZE - 1))` and `void border_ring_order(int8_t ring[BORDER_RING_LEN][2]);`, both in `tests/tools/border_walk.h`. Later tasks (and the CLI driver) rely on this exact macro name and function signature.

- [ ] **Step 1: Create the header with just the ring-order API**

Create `tests/tools/border_walk.h`:

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

#endif /* eternityII_border_walk_h */
```

- [ ] **Step 2: Implement `border_ring_order`**

Create `tests/tools/border_walk.c`:

```c
#include "tools/border_walk.h"

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
```

- [ ] **Step 3: Write the failing tests**

Create `tests/tools/test_border_walk.c`:

```c
/*
 * Tests de tests/tools/border_walk.c — cœur pur du walker de bordure.
 *
 * Voir docs/superpowers/specs/2026-09-06-masse-bordure-design.md pour le
 * raisonnement complet (pourquoi aucune modification à part.c/readdata.c,
 * pourquoi la fermeture du cycle se vérifie gratuitement, etc.).
 */
#include "greatest.h"

#include "tools/border_walk.h"
#include "core/core_static_variables.h"

#include <string.h>

TEST border_ring_order_has_the_right_length_and_starts_at_origin(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    ASSERT_EQ_FMT(0, (int)ring[0][0], "%d");
    ASSERT_EQ_FMT(0, (int)ring[0][1], "%d");
    PASS();
}

TEST border_ring_order_visits_every_perimeter_cell_exactly_once(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    int seen[ETERN_SIZE][ETERN_SIZE];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int x = ring[i][0];
        int y = ring[i][1];
        ASSERT(x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1);
        ASSERT_EQ_FMT(0, seen[x][y], "%d");
        seen[x][y] = 1;
    }

    int total = 0;
    for (int x = 0; x < ETERN_SIZE; x++) {
        for (int y = 0; y < ETERN_SIZE; y++) {
            if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1) {
                ASSERT_EQ_FMT(1, seen[x][y], "%d");
                total++;
            }
        }
    }
    ASSERT_EQ_FMT(BORDER_RING_LEN, total, "%d");
    PASS();
}

TEST border_ring_order_is_a_closed_walk_of_adjacent_cells(void)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        int j = (i + 1) % BORDER_RING_LEN;
        int dx = ring[j][0] - ring[i][0];
        int dy = ring[j][1] - ring[i][1];
        int manhattan = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        ASSERT_EQ_FMT(1, manhattan, "%d");
    }
    PASS();
}

SUITE(border_walk_suite)
{
    RUN_TEST(border_ring_order_has_the_right_length_and_starts_at_origin);
    RUN_TEST(border_ring_order_visits_every_perimeter_cell_exactly_once);
    RUN_TEST(border_ring_order_is_a_closed_walk_of_adjacent_cells);
}
```

- [ ] **Step 4: Wire the new files into the build**

In `makefile`, `TEST_SUITES_COMMON` (line 211), add `tests/tools/test_border_walk.c` after `tests/tools/test_root_from_board.c`:

```
                tests/tools/test_root_from_board.c tests/tools/test_border_walk.c
```

In `makefile`, `TEST_MODULES` (line 232), add `tests/tools/border_walk.c` after `tests/tools/root_from_board.c`:

```
... tests/tools/root_from_board.c tests/tools/border_walk.c
```

In `tests/test_main.c`, add after `SUITE_EXTERN(fork_orchestrator_suite);` (line 47, before the `#if ETERN_PARTS == 16` block):

```c
SUITE_EXTERN(border_walk_suite);
```

And after `RUN_SUITE(fork_orchestrator_suite);` (line 94, before the `#if ETERN_PARTS == 16` block):

```c
    RUN_SUITE(border_walk_suite);
```

- [ ] **Step 5: Run the tests to verify they compile and pass**

Run: `make test 2>&1 | tail -60`
Expected: both `test-16` and `test-256` build and run; the three new `border_ring_order_*` assertions pass (look for `border_walk_suite` in the greatest output, 0 failures).

- [ ] **Step 6: Commit**

```bash
git add tests/tools/border_walk.h tests/tools/border_walk.c tests/tools/test_border_walk.c makefile tests/test_main.c
git commit -m "Ajoute l'ordre de parcours de l'anneau de bordure (walker, étape 1)"
```

---

### Task 2: DFS enumeration (`border_walk_count`)

**Files:**
- Modify: `tests/tools/border_walk.h` (add `border_ring_found_cb`, `border_walk_count`)
- Modify: `tests/tools/border_walk.c` (implement the DFS)
- Modify: `tests/tools/test_border_walk.c` (add fixtures + 2 tests, extend the `SUITE`)

**Interfaces:**
- Consumes: `BORDER_RING_LEN`, `border_ring_order` (Task 1). `prepare_map_part`, `map_bucket_packed`, `what_search_in_grid_to_key`, `id_for_rotated_part`, `set_face_used`, `is_face_used` (existing `core/part.h`/`core/possibility.h` — no signature changes).
- Produces: `typedef void (*border_ring_found_cb)(const struct possibility_packet *ring_state, void *ctx);` and `long long border_walk_count(map_big_array *map, struct array_part *all_rotate_parts, border_ring_found_cb on_found, void *ctx);`, both in `tests/tools/border_walk.h`. `border_mass.c` (Task 3) calls `border_walk_count` with `on_found = NULL`.

- [ ] **Step 1: Extend the header**

In `tests/tools/border_walk.h`, add before the closing `#endif`:

```c
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
```

- [ ] **Step 2: Write the failing tests**

In `tests/tools/test_border_walk.c`, add `#include "core/readdata.h"`, `#include <stdio.h>`, `#include <stdlib.h>`, `#include <unistd.h>` to the includes, then add before `SUITE(border_walk_suite)`:

```c
/* Bases choisies pour rester à l'intérieur de int8_t (les faces de `struct
   part` sont des int8_t, cf. src/core/part.h) tout en restant juste après la
   plage des leurres ([1,10]) : maxFace = BW_EDGE_BASE + BORDER_RING_LEN - 1
   ≈ 71 sur le jeu 256 pièces (contre 22 pour data/pieces.csv), gardant la
   table de lookup de la fixture proche de l'échelle de production. */
#define BW_EDGE_BASE 12
#define BW_INTERIOR_PLACEHOLDER 11

/* Couleur requise sur la face de `ring[i]` tournée vers `(nx,ny)` : 0 hors
   grille, une couleur unique par arête de l'anneau si `(nx,ny)` est le
   voisin précédent/suivant dans l'anneau, un joker constant sinon (case
   intérieure — jamais vérifiée par le DFS, la valeur exacte importe peu). */
static int bw_required_face(const int8_t ring[BORDER_RING_LEN][2], int i, int nx, int ny)
{
    if (nx < 0 || nx >= ETERN_SIZE || ny < 0 || ny >= ETERN_SIZE) {
        return 0;
    }
    int prev = (i - 1 + BORDER_RING_LEN) % BORDER_RING_LEN;
    int next = (i + 1) % BORDER_RING_LEN;
    if (ring[prev][0] == nx && ring[prev][1] == ny) {
        return BW_EDGE_BASE + prev;
    }
    if (ring[next][0] == nx && ring[next][1] == ny) {
        return BW_EDGE_BASE + i;
    }
    return BW_INTERIOR_PLACEHOLDER;
}

/* Écrit un jeu de ETERN_PARTS pièces (fichier temporaire, format read_parts)
   et retourne rotate_all_parts() dessus. Si `with_unique_ring` est vrai, les
   BORDER_RING_LEN premières pièces (id 1..BORDER_RING_LEN) forment l'UNIQUE
   anneau de bordure valide possible : chaque arête de l'anneau porte une
   couleur qui n'apparaît que sur les deux pièces qui la partagent, donc
   aucune pièce ne peut se substituer à une autre position. Toutes les autres
   pièces (et la totalité si `with_unique_ring` est faux) sont des leurres
   sans face à 0 — jamais candidats sur le bord, qui exige toujours au moins
   un 0. */
static struct array_part *bw_make_rotate_parts(int with_unique_ring)
{
    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);

    char path[] = "/tmp/etii_bw_pieces_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "w");
    if (fp == NULL) { close(fd); return NULL; }
    fprintf(fp, "ntiles: %d\n", ETERN_PARTS);

    for (int id = 1; id <= ETERN_PARTS; id++) {
        int top, right, bottom, left;
        if (with_unique_ring && id <= BORDER_RING_LEN) {
            int i = id - 1;
            int x = ring[i][0];
            int y = ring[i][1];
            top = bw_required_face(ring, i, x, y - 1);
            right = bw_required_face(ring, i, x + 1, y);
            bottom = bw_required_face(ring, i, x, y + 1);
            left = bw_required_face(ring, i, x - 1, y);
        } else {
            int base = id % 7;
            top = base + 1;
            left = base + 2;
            bottom = base + 3;
            right = base + 4;
        }
        /* read_parts attend : id top left bottom right */
        fprintf(fp, "%d %d %d %d %d\n", id, top, left, bottom, right);
    }
    fclose(fp);

    struct array_part *apart = read_parts(path);
    unlink(path);
    if (apart == NULL) return NULL;
    struct array_part *rot = rotate_all_parts(apart);
    free_array_part(apart);
    return rot;
}

struct bw_found_record {
    int calls;
    struct possibility_packet last;
};

static void bw_on_found(const struct possibility_packet *ring_state, void *ctx)
{
    struct bw_found_record *rec = (struct bw_found_record *)ctx;
    rec->calls++;
    rec->last = *ring_state;
}

TEST border_walk_count_returns_zero_without_any_border_shaped_piece(void)
{
    struct array_part *all = bw_make_rotate_parts(0);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long n = border_walk_count(map, all, NULL, NULL);
    ASSERT_EQ_FMT(0LL, n, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}

/* Une pièce crée un anneau valide UNIQUE (en tant qu'objet cyclique abstrait)
   quand `with_unique_ring` est vrai — et pourtant border_walk_count doit
   retourner 4, pas 1 : le DFS ne fixe aucune pièce à l'ouverture en (0,0), et
   à cette toute première étape aucun voisin n'est encore posé, donc n'importe
   lequel des 4 coins de l'anneau peut servir de point d'ouverture. Chacun
   produit un placement complet différent sur la grille absolue (le même
   anneau, mais tourné) — c'est exactement le mécanisme dont dépend la
   masse totale (`N` EST la masse, pas `4×N`, cf. la spec). Vérifié
   empiriquement (12 et 60 cases) avant d'écrire cette assertion — ne pas la
   « corriger » vers 1 sans revérifier. */
TEST border_walk_count_finds_the_crafted_ring_once_per_opening_corner(void)
{
    struct array_part *all = bw_make_rotate_parts(1);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    struct bw_found_record rec;
    memset(&rec, 0, sizeof rec);
    long long n = border_walk_count(map, all, bw_on_found, &rec);

    ASSERT_EQ_FMT(4LL, n, "%lld");
    ASSERT_EQ_FMT(4, rec.calls, "%d");
    ASSERT_EQ_FMT(BORDER_RING_LEN, possibility_placed_count(&rec.last), "%d");

    int8_t ring[BORDER_RING_LEN][2];
    border_ring_order(ring);
    for (int i = 0; i < BORDER_RING_LEN; i++) {
        ASSERT(rec.last.grid[ring[i][0]][ring[i][1]] != -2);
    }
    /* Contre-épreuve : le centre du plateau (jamais un bord) doit être resté
       vide — le walker ne pose jamais l'intérieur. */
    ASSERT_EQ_FMT((int16_t)-2, rec.last.grid[ETERN_SIZE / 2][ETERN_SIZE / 2], "%d");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
```

Then update the `SUITE` block to:

```c
SUITE(border_walk_suite)
{
    RUN_TEST(border_ring_order_has_the_right_length_and_starts_at_origin);
    RUN_TEST(border_ring_order_visits_every_perimeter_cell_exactly_once);
    RUN_TEST(border_ring_order_is_a_closed_walk_of_adjacent_cells);
    RUN_TEST(border_walk_count_returns_zero_without_any_border_shaped_piece);
    RUN_TEST(border_walk_count_finds_the_crafted_ring_once_per_opening_corner);
}
```

- [ ] **Step 3: Run the tests to verify the new ones fail**

Run: `make test 2>&1 | tail -40`
Expected: FAIL to link — `undefined reference to 'border_walk_count'` (the header declares it, `border_walk.c` doesn't define it yet).

- [ ] **Step 4: Implement the DFS**

In `tests/tools/border_walk.c`, add `#include <string.h>` at the top and append:

```c
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
        uint16_t face_idx = (uint16_t)(cand->id - 1);
        if (is_face_used(ctx->state.b_faceused, face_idx)) {
            continue;
        }

        ctx->state.grid[x][y] = (int16_t)id_for_rotated_part((uint16_t)cand->id, (uint8_t)cand->rotation);
        set_face_used(ctx->state.b_faceused, face_idx, 1);

        bw_dfs(ctx, i + 1);

        set_face_used(ctx->state.b_faceused, face_idx, 0);
        ctx->state.grid[x][y] = -2;
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

- [ ] **Step 5: Run the tests to verify they now pass**

Run: `make test 2>&1 | tail -40`
Expected: PASS — both `border_walk_count_*` tests green, in both `test-16` and `test-256`.

- [ ] **Step 6: Commit**

```bash
git add tests/tools/border_walk.h tests/tools/border_walk.c tests/tools/test_border_walk.c
git commit -m "Ajoute l'énumération DFS des anneaux de bordure (border_walk_count)"
```

---

### Task 3: CLI driver (`border_mass`)

**Files:**
- Create: `tests/tools/border_mass.c`
- Modify: `makefile` (new `.PHONY: border-mass` target, placed after the existing `gen-root` target)

**Interfaces:**
- Consumes: `border_walk_count` (Task 2), `read_parts`/`read_indices`/`free_array_index` (`core/readdata.h`), `rotate_all_parts`/`prepare_map_part` (`core/part.h`).

- [ ] **Step 1: Write the driver**

Create `tests/tools/border_mass.c`:

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
        if (x == 0 || x == ETERN_SIZE - 1 || y == 0 || y == ETERN_SIZE - 1) {
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

- [ ] **Step 2: Add the Makefile target**

In `makefile`, immediately after the existing `gen-root` target block (after line 319), add:

```make
# Outil border_mass (tests/tools/) : mesure la masse totale des anneaux de
# bordure valides — voir docs/superpowers/specs/2026-09-06-masse-bordure-design.md
# et docs/tests_et_ci.md. Réutilise $(TEST_MODULES) tel quel : border_walk.c
# appelle what_search_in_grid_to_key (core/possibility.c), qui entraîne au
# link tout le graphe déjà nécessaire à `make test` (datamanager.c compris) —
# plus simple et plus sûr que reconstituer un sous-ensemble minimal à la main.
BORDER_MASS_BIN := tests/tools/border_mass

.PHONY: border-mass
border-mass:
	gcc -Wall -Wextra -std=gnu99 -O2 -Isrc -Itests $(CPPFLAGS) -Werror -pthread -o $(BORDER_MASS_BIN) \
	    tests/tools/border_mass.c tests/tools/border_walk.c $(TEST_MODULES) -lm
```

- [ ] **Step 3: Build and smoke-test on the real puzzle data**

Run: `make border-mass`
Expected: compiles cleanly, produces `tests/tools/border_mass`.

Run: `tests/tools/border_mass data/pieces.csv data/indices.csv`
Expected: one line printed, `masse totale des anneaux de bordure valides : <N>`, process exits 0. This is the phase-1 deliverable number — record it wherever the spec's "Critères de succès" asks (this plan does not prescribe where to publish it; report it back once obtained). If the run does not terminate in an acceptable time, interrupt it and report that instead — per the spec, non-termination is itself a valid phase-1 finding.

Run: `make border-mass CPPFLAGS="-DETERN_PARTS=16"` then `tests/tools/border_mass data/pieces16.csv data/indices.csv` if a 16-piece dataset with matching indices exists under `data/`; otherwise skip this smaller-scale run (check `ls data/` for what's actually available before assuming a specific filename).

- [ ] **Step 4: Commit**

```bash
git add tests/tools/border_mass.c makefile
git commit -m "Ajoute l'outil CLI border_mass (mesure de la masse totale)"
```

---

### Task 4: Documentation

**Files:**
- Modify: `docs/tests_et_ci.md` (new section, modeled on the existing `## Outil gen_root (make gen-root)` section around line 215)
- Modify: `tests/README.md` (new paragraph in the `## Outils (tests/tools/)` section, after the `gen_root` paragraph around line 78)

**Interfaces:** None — documentation only, no code.

- [ ] **Step 1: Add the `docs/tests_et_ci.md` section**

In `docs/tests_et_ci.md`, immediately after the existing `## Outil gen_root (make gen-root)` section (ends around line 240, right before `## Banc de mesure du débit de recherche`), insert:

```markdown
## Outil `border_mass` (`make border-mass`)

Mesure la **masse totale** des anneaux de bordure valides (les
`BORDER_RING_LEN` = `4×(ETERN_SIZE-1)` cases du pourtour du plateau — 60 sur
le puzzle 256 pièces) : une recherche exhaustive ancrée au coin `(0,0)`
trouve toute la population d'anneaux, et le nombre brut trouvé **est** déjà
la masse totale — aucun voisin n'est encore posé à la toute première case,
donc le DFS explore de lui-même les 4 coins possibles comme point
d'ouverture, retrouvant chaque anneau abstrait une fois par coin (constaté
empiriquement pendant l'implémentation). Cela suppose qu'aucun indice
officiel ne touche une case de bord (vérifié au démarrage par l'outil
lui-même) — sinon un seul coin serait valide comme ouverture, pas 4. Voir
[docs/superpowers/specs/2026-09-06-masse-bordure-design.md](superpowers/specs/2026-09-06-masse-bordure-design.md)
pour le raisonnement complet.

```sh
make border-mass
tests/tools/border_mass data/pieces.csv data/indices.csv
```

Contrairement à `gen_root`, cet outil ne produit aucune racine de stock —
c'est la **phase 1** d'un projet en deux temps : seul un chiffre est
rapporté (la masse totale `N`). La génération de racines `.back` à partir des
anneaux trouvés est un sous-projet distinct, non implémenté.

Le cœur pur (`border_walk.c`) est compilé avec les autres modules et couvert
par `test_border_walk.c`, comme `root_from_board.c` pour `gen_root`. Il
réutilise sans modification `prepare_map_part`/`map_bucket_packed`/
`what_search_in_grid_to_key` : le côté intérieur d'une pièce de bord n'est
jamais posé donc toujours traité comme joker par ces fonctions existantes,
exactement le comportement voulu.
```

- [ ] **Step 2: Add the `tests/README.md` paragraph**

In `tests/README.md`, immediately after the `gen_root` section's two-bullet "pièges à l'usage" list (ends around line 77, right before `## Tests d'intégration bout-en-bout`), insert:

```markdown
`border_mass` mesure la masse totale des anneaux de bordure valides (voir
[docs/tests_et_ci.md](../docs/tests_et_ci.md#outil-border_mass-make-border-mass)
et [docs/superpowers/specs/2026-09-06-masse-bordure-design.md](../docs/superpowers/specs/2026-09-06-masse-bordure-design.md)).
Son cœur pur (`border_walk.c`) est, comme celui de `gen_root`, compilé avec
les autres modules et couvert par `test_border_walk.c`.

```sh
make border-mass
tests/tools/border_mass data/pieces.csv data/indices.csv
```
```

Also update the table row at line 39 (`tests/tools/` entry) from:

```
| `tests/tools/` | Outils autonomes + leur cœur testé (`gen_root` / `root_from_board` — conversion d'un plateau externe en racine de stock). |
```

to:

```
| `tests/tools/` | Outils autonomes + leur cœur testé (`gen_root` / `root_from_board` — conversion d'un plateau externe en racine de stock ; `border_mass` / `border_walk` — masse totale des anneaux de bordure). |
```

- [ ] **Step 3: Verify the docs build/render sensibly**

Run: `grep -c "border_mass" docs/tests_et_ci.md tests/README.md`
Expected: at least 1 match in each file (sanity check the edits landed).

- [ ] **Step 4: Commit**

```bash
git add docs/tests_et_ci.md tests/README.md
git commit -m "Documente l'outil border_mass"
```

---

## Self-Review Notes

- **Spec coverage:** every phase-1 requirement in the spec has a task — ring order + wiring (Task 1), enumeration + precondition-dependent correctness (Task 2), CLI + precondition check itself (Task 3), doc updates (Task 4). The spec's explicit non-objectives (root generation, persistence, meet-in-the-middle) have deliberately no task here.
- **Type consistency checked:** `BORDER_RING_LEN`, `border_ring_order`, `border_ring_found_cb`, `border_walk_count` are declared once in Task 1/2's header edits and used with identical signatures in `border_walk.c`, `test_border_walk.c`, and `border_mass.c`.
- **No placeholders:** every step carries real, complete code; the one adaptive step (Task 3, Step 3's "if it doesn't terminate, report that instead") is a legitimate measurement-outcome branch explicitly anticipated by the spec's "Risque connu, assumé" section, not a vague instruction.

### Mid-execution correction (during Task 2, 2026-09-06)

The original Task 2 text (and the spec) claimed `border_walk_count` returns `N` and the caller multiplies by 4 for the total mass. This was wrong, discovered when the Task 2 implementer's crafted "exactly one ring" fixture returned 4 instead of 1. Root cause verified independently with a standalone diagnostic (empirically, at both `BORDER_RING_LEN=12` and `=60`): the DFS places no constraint on either non-outward side of the very first cell (no neighbor is placed yet), so any of a ring's 4 corner pieces can open the search at `(0,0)`, and each produces a distinct, valid, complete grid placement (the same ring, rotated). `N` therefore already **is** the total mass — the external `×4` in the original plan/spec text would have double-counted it as `×16`. This document has been corrected throughout (Task 2's test now expects 4 for one crafted ring, Task 3's driver prints `N` directly, the spec's "Pourquoi la symétrie de rotation tient" section carries the corrected reasoning) before Task 2 was completed, so no task built on the wrong formula.

A secondary, independently-measured finding from the same diagnostic: the crafted-ring test fixture's per-edge-unique-color scheme pushes `maxFace` to ~71 for the 256-piece build (vs. 22 for real puzzle data), costing ~8.7s and ~866MB peak per `make test` run for that one test. `BW_EDGE_BASE` was tightened from an original (buggy, `int8_t`-overflowing) `2000` down to `12` — starting right after the fixture's decoy-color range instead of an arbitrary offset — but the underlying `O(BORDER_RING_LEN)`-many-unique-colors cost remains. Accepted as a known, documented phase-1 cost (see spec's Tests section) rather than pursued further (a smaller palette needs pair-uniqueness, e.g. a de Bruijn-style construction, judged disproportionate complexity for a test fixture right now).
