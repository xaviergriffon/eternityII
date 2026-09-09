# border_ring_dp — pool de workers unifié pour la RAM et les fragments — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remplacer, dans `tests/tools/border_ring_dp.c`, le mécanisme actuel (un seul fragment actif à la fois, parallélisme uniquement à l'intérieur d'une transition de niveau) par un coordinateur à deux modes exclusifs qui (a) borne la RAM réelle au budget `--dp-max-ram-mo` au lieu de la dépasser silencieusement, et (b) traite plusieurs fragments indépendants en parallèle une fois qu'ils sont assez nombreux, au lieu de les vider un par un sur un seul cœur.

**Architecture:** Un coordinateur mono-process garde la pile LIFO de fragments et bascule entre mode SOLO (un seul job actif, autorisé à utiliser `bd_transition_parallel` en interne, comme aujourd'hui) et mode POOL (dès que la pile contient au moins `nb_workers` fragments : jusqu'à `nb_workers` process forkés, chacun strictement monoprocessus, communiquant par fichier résultat + code de sortie). Le budget RAM est appliqué différemment selon le mode : marge heuristique documentée pour le mode SOLO, marge EXACTE (dérivée de l'en-tête du fragment sérialisé) pour le mode POOL.

**Tech Stack:** C (C11), `fork`/`waitpid`/fichiers temporaires (pas de threads, pas de nouvelle dépendance), `greatest` pour les tests unitaires.

**Spec:** [docs/superpowers/specs/2026-09-09-border-ring-dp-pool-ram-design.md](../specs/2026-09-09-border-ring-dp-pool-ram-design.md)

## Global Constraints

- Fork uniquement, jamais de threads (spec § Pourquoi fork, pas des threads).
- Mode SOLO et mode POOL ne sont jamais actifs en même temps.
- En mode POOL, un job est strictement monoprocessus : jamais `bd_transition_parallel`, jamais de compaction forkée (`bd_level_to_shards` doit être appelé avec `nb_workers=1` dans ce mode).
- Un job ne garde jamais de fragment pour lui-même après une scission — il repousse TOUS les fragments produits (K, pas K-1) vers la pile du coordinateur.
- La marge de rechargement du mode POOL est un calcul EXACT (`bd_estimate_reload_bytes`, dérivé de l'en-tête du fragment) — jamais un facteur fudge.
- La marge du mode SOLO reste un facteur heuristique documenté comme tel, pas présenté comme un calcul exact.
- `--dp-max-ram-mo` reste le seul paramètre RAM exposé côté CLI (`border_mass.c` inchangé) — toute cette refonte reste interne à `border_ring_dp.c`/`.h`.
- `make test` doit rester vert après chaque tâche, sur les deux tailles de puzzle (`ETERN_PARTS=16` et la config par défaut).
- Un job qui échoue (statut non nul, fichier résultat illisible) arrête bruyamment tous les jobs frères et sort en erreur — jamais un total partiel affiché comme définitif.

---

## File Structure

- **Modify `tests/tools/border_ring_dp.c`** : toute la logique (fonctions ajoutées/réécrites listées par tâche ci-dessous). Aucune nouvelle fonction publique — l'API externe (`border_ring_count_dp`, `border_ring_dp_set_spill_dir`, `border_ring_dp_set_max_ram_mo`) reste inchangée.
- **Modify `tests/tools/border_ring_dp.h`** : commentaire de tête à mettre à jour (Tâche 7) pour décrire le pool de workers au lieu de « un seul repris immédiatement ».
- **Modify `tests/tools/test_border_ring_dp.c`** : nouveaux tests unitaires (un par tâche testable), plus adaptation mineure si un test existant devient redondant.
- **Modify `docs/tests_et_ci.md`** : section `--dp` (Tâche 7).
- **Modify `docs/superpowers/specs/2026-09-06-masse-bordure-design.md`** et **`2026-09-06-border-mass-parallel-design.md`** : renvoi vers la nouvelle spec (Tâche 7).

---

### Task 1: `bd_estimate_reload_bytes` — marge de rechargement exacte

**Files:**
- Modify: `tests/tools/border_ring_dp.c` (ajouter juste après `bd_level_init`, ~ligne 128)
- Test: `tests/tools/test_border_ring_dp.c`

**Interfaces:**
- Produces: `double bd_estimate_reload_bytes(int32_t key_len, uint64_t count)` — non-static (appelée en interne par le coordinateur dans une tâche ultérieure, et testée directement depuis `test_border_ring_dp.c`, jamais déclarée dans `border_ring_dp.h` — même schéma que `border_ring_dp_set_fork_min_states_for_tests`). `int bd_reload_estimate_matches_real_alloc_for_tests(int32_t key_len, uint64_t count)` — non-static, test-only, retourne 1 si `bd_estimate_reload_bytes` égale EXACTEMENT ce qu'un vrai `bd_level_init`+`bd_level_bytes` produirait pour les mêmes paramètres.

- [ ] **Step 1: Write the failing tests**

Dans `tests/tools/test_border_ring_dp.c`, ajouter les déclarations test-only et les tests (avant `SUITE`) :

```c
/* Test-only, jamais déclarées dans border_ring_dp.h — même schéma que
   border_ring_dp_set_fork_min_states_for_tests. */
double bd_estimate_reload_bytes(int32_t key_len, uint64_t count);
int bd_reload_estimate_matches_real_alloc_for_tests(int32_t key_len, uint64_t count);

TEST bd_estimate_reload_bytes_matches_known_capacity_growth(void)
{
    /* key_len=5, count=0 : hint=16, capacity reste 16 (deja >= hint). */
    ASSERT_EQ_FMT(16.0 * (5 + 8) + 2.0, bd_estimate_reload_bytes(5, 0), "%.1f");
    /* key_len=5, count=10 : hint=36, capacite double 16->32->64. */
    ASSERT_EQ_FMT(64.0 * (5 + 8) + 8.0, bd_estimate_reload_bytes(5, 10), "%.1f");
    PASS();
}

TEST bd_estimate_reload_bytes_matches_real_allocation_for_various_sizes(void)
{
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(5, 0));
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(5, 10));
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(20, 1000));
    ASSERT(bd_reload_estimate_matches_real_alloc_for_tests(3, 1000003));
    PASS();
}
```

Ajouter les deux `RUN_TEST(...)` dans `SUITE(border_ring_dp_suite)`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test 2>&1 | grep -A5 bd_estimate_reload_bytes`
Expected: échec de lien (`bd_estimate_reload_bytes`/`bd_reload_estimate_matches_real_alloc_for_tests` non définies).

- [ ] **Step 3: Implement**

Dans `tests/tools/border_ring_dp.c`, juste après `bd_level_init` (avant `bd_level_free`) :

```c
/* Taille RAM EXACTE qu'occupera un fragment sérialisé (en-tête key_len+count,
   format bd_level_write_file) une fois rechargé par bd_level_load_file — sans
   jamais le charger. bd_level_load_file appelle toujours
   bd_level_init(level, key_len, count*2+16) : la capacité finale est donc
   entièrement déterminée par ces deux nombres, lisibles depuis les 12
   premiers octets du fichier. Non-static uniquement pour être testée
   directement depuis test_border_ring_dp.c — jamais déclarée dans
   border_ring_dp.h, appelée uniquement en interne par le coordinateur. */
double bd_estimate_reload_bytes(int32_t key_len, uint64_t count)
{
    size_t hint = (size_t)(count * 2 + 16);
    size_t capacity = 1u << 4;
    while (capacity < hint) {
        capacity <<= 1;
    }
    return (double)capacity * ((size_t)key_len + sizeof(long long)) + (double)((capacity + 7) / 8);
}

/* Test-only : vérifie que l'estimation ci-dessus égale EXACTEMENT ce qu'un
   vrai bd_level_init(key_len, count*2+16) allouerait — verrouille que la
   formule ne diverge jamais silencieusement de l'allocation réelle. */
int bd_reload_estimate_matches_real_alloc_for_tests(int32_t key_len, uint64_t count)
{
    struct bd_level level;
    bd_level_init(&level, key_len, (size_t)count * 2 + 16);
    double real_bytes = bd_level_bytes(&level);
    bd_level_free(&level);
    return bd_estimate_reload_bytes(key_len, count) == real_bytes;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test 2>&1 | grep -i "bd_estimate_reload\|FAILED\|assertions"`
Expected: PASS, aucune régression sur les tests existants de `border_ring_dp_suite`.

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_ring_dp.c tests/tools/test_border_ring_dp.c
git commit -m "border_ring_dp: marge de rechargement exacte d'un fragment (bd_estimate_reload_bytes)"
```

---

### Task 2: `bd_should_run_solo` — prédicat pur de choix de mode

**Files:**
- Modify: `tests/tools/border_ring_dp.c` (ajouter avant `bd_run_opening`)
- Test: `tests/tools/test_border_ring_dp.c`

**Interfaces:**
- Produces: `int bd_should_run_solo(int active, int stack_count, int nb_workers)` — non-static, test-only (comme la Tâche 1), utilisée par le coordinateur (Tâche 5).

- [ ] **Step 1: Write the failing test**

```c
int bd_should_run_solo(int active, int stack_count, int nb_workers);

TEST bd_should_run_solo_picks_mode_from_queue_depth(void)
{
    /* Aucun job actif, peu de fragments en attente (< nb_workers) : solo. */
    ASSERT(bd_should_run_solo(0, 0, 4));
    ASSERT(bd_should_run_solo(0, 3, 4));
    /* Assez de fragments pour remplir tous les workers : pool. */
    ASSERT_FALSE(bd_should_run_solo(0, 4, 4));
    ASSERT_FALSE(bd_should_run_solo(0, 10, 4));
    /* Un job deja actif (pool en cours) : jamais solo tant qu'il tourne,
       meme si la pile s'est videe entre-temps — on laisse le pool en cours
       se terminer avant de rebasculer. */
    ASSERT_FALSE(bd_should_run_solo(1, 0, 4));
    PASS();
}
```

Ajouter le `RUN_TEST(...)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | grep bd_should_run_solo`
Expected: échec de lien.

- [ ] **Step 3: Implement**

```c
/* Mode SOLO (un seul job, autorise a utiliser bd_transition_parallel) tant
   qu'aucun pool n'est deja en cours ET que la pile ne contient pas encore de
   quoi remplir tous les workers ; MODE POOL sinon. Jamais les deux modes
   actifs en meme temps (cf. spec) : un pool deja lance va jusqu'au bout de
   ses jobs actifs avant qu'on reevalue. Non-static, test-only — utilisee par
   le coordinateur (bd_run_opening). */
int bd_should_run_solo(int active, int stack_count, int nb_workers)
{
    return active == 0 && stack_count < nb_workers;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test 2>&1 | grep -i "bd_should_run_solo\|FAILED\|assertions"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_ring_dp.c tests/tools/test_border_ring_dp.c
git commit -m "border_ring_dp: predicat pur de choix de mode (bd_should_run_solo)"
```

---

### Task 3: extraire `bd_run_fragment_job` (refactor sans changement de comportement)

**Files:**
- Modify: `tests/tools/border_ring_dp.c` (ajouter `struct bd_job_result` + `bd_run_fragment_job`, réécrire le corps de `bd_run_opening` pour l'utiliser)
- Test: aucun nouveau test — les tests EXISTANTS de `border_ring_dp_suite` doivent tous passer sans modification, c'est la preuve de non-régression de cette tâche.

**Interfaces:**
- Consumes : `bd_transition_parallel`, `bd_transition_range`, `bd_level_init/_free/_bytes`, `bd_finalize_range`, `bd_level_to_shards`, `bd_pick_nb_shards`, `bd_fork_min_states`, `bd_split_threshold_bytes` (tous déjà existants, inchangés).
- Produces : `struct bd_job_result` (champs `closed`, `total`, `shard_dir[128]`, `nb_shards`, `resume_pos`) et `static struct bd_job_result bd_run_fragment_job(const struct bd_ctx *ctx, int8_t closure_target, struct bd_level *level, int start_pos, int allow_parallel, int nb_workers, double effective_budget_bytes)` — **prend possession de `*level` et le libère avant de retourner**, que ce soit en fermant l'anneau ou en scindant. Consommée par la Tâche 4 (fork) et réutilisée dans cette même tâche par `bd_run_opening`.

- [ ] **Step 1: Ajouter `struct bd_job_result` et `bd_run_fragment_job`**

Juste avant `bd_run_opening` (~ligne 953) :

```c
/* Resultat d'un job : soit il a ferme l'anneau (closed=1, total valide),
   soit il a du se scinder (closed=0, K fragments deposes dans shard_dir,
   tous a reprendre depuis resume_pos) — un job ne garde JAMAIS un fragment
   pour lui-meme apres une scission, cf. spec § Comportement uniforme d'un
   job. */
struct bd_job_result {
    long long total;
    int closed;
    char shard_dir[128];
    int nb_shards;
    int resume_pos;
};

/* Coeur d'un job, commun aux modes SOLO et POOL — prend possession de
   `*level` (le libere avant de retourner, dans tous les cas). `allow_parallel`
   n'autorise bd_transition_parallel ET la compaction forkee de
   bd_level_to_shards QUE si vrai (mode SOLO) — en mode POOL (allow_parallel=0)
   un job reste strictement monoprocessus, cf. spec § Pourquoi fork / Non-objectifs
   (pas de fork imbrique). */
static struct bd_job_result bd_run_fragment_job(const struct bd_ctx *ctx, int8_t closure_target,
                                                 struct bd_level *level, int start_pos, int allow_parallel,
                                                 int nb_workers, double effective_budget_bytes)
{
    struct bd_job_result result;
    memset(&result, 0, sizeof result);

    struct bd_level cur = *level;
    int pos = start_pos;

    while (pos < BORDER_RING_LEN - 1) {
        int want_corner = ctx->is_corner_at[pos];
        struct bd_level next;
        if (allow_parallel && nb_workers > 1 && cur.used >= bd_fork_min_states) {
            bd_transition_parallel(ctx, want_corner, &cur, nb_workers, &next);
        } else {
            bd_level_init(&next, 1 + ctx->nb_classes, cur.used / 2 + 16);
            bd_transition_range(ctx, want_corner, &cur, 0, cur.capacity, &next);
        }
        bd_level_free(&cur);
        cur = next;
        pos++;

        double bytes = bd_level_bytes(&cur);
        fprintf(stderr, "border_ring_count_dp : position %d/%d, %zu etats (%.2f Go)\n", pos,
                BORDER_RING_LEN - 1, cur.used, bytes / (1024.0 * 1024.0 * 1024.0));

        /* cur.used > 1 : cf. la garde documentee dans border_ring_dp.h contre
           une scission degeneree d'un niveau a 0 ou 1 entree. */
        if (bytes >= effective_budget_bytes && cur.used > 1) {
            int nb_shards = bd_pick_nb_shards(bytes, nb_workers);
            char dir[128];
            snprintf(dir, sizeof dir, "%s/etii_bd_%d_p%d", bd_spill_dir, (int)getpid(), pos);
            struct bd_shard_set shards;
            int compact_workers = allow_parallel ? nb_workers : 1;
            bd_level_to_shards(&cur, dir, nb_shards, compact_workers, &shards);
            bd_level_free(&cur);

            fprintf(stderr,
                    "border_ring_count_dp : position %d/%d, scission en %d fragments "
                    "(%.2f Go, %zu etats au total)\n",
                    pos, BORDER_RING_LEN - 1, shards.nb_shards,
                    shards.total_bytes / (1024.0 * 1024.0 * 1024.0), shards.total_used);

            result.closed = 0;
            snprintf(result.shard_dir, sizeof result.shard_dir, "%s", dir);
            result.nb_shards = nb_shards;
            result.resume_pos = pos;
            return result;
        }
    }

    long long total = bd_finalize_range(ctx, ctx->is_corner_at[BORDER_RING_LEN - 1], closure_target, &cur, 0,
                                         cur.capacity);
    bd_level_free(&cur);
    result.closed = 1;
    result.total = total;
    return result;
}

/* Repousse le resultat d'un job vers la pile partagee : accumule le total
   s'il a ferme l'anneau, ou empile les K fragments produits sinon — jamais
   les deux. */
static void bd_apply_job_result(struct bd_pending_stack *stack, long long *total, const struct bd_job_result *r)
{
    if (r->closed) {
        *total += r->total;
        return;
    }
    for (int d = r->nb_shards - 1; d >= 0; d--) {
        char path[512];
        snprintf(path, sizeof path, "%s/shard_%d.bin", r->shard_dir, d);
        bd_pending_push(stack, path, r->shard_dir, r->resume_pos);
    }
}
```

- [ ] **Step 2: Réécrire `bd_run_opening` pour utiliser `bd_run_fragment_job` (encore mono-process, pas de fork de job)**

Remplacer l'intégralité du corps actuel de `bd_run_opening` par :

```c
static long long bd_run_opening(const struct bd_ctx *ctx, int8_t initial_required,
                                 const int8_t *initial_counts, int8_t closure_target, int nb_workers)
{
    struct bd_pending_stack stack;
    memset(&stack, 0, sizeof stack);
    long long total = 0;

    struct bd_level cur;
    bd_level_init(&cur, 1 + ctx->nb_classes, 1 << 10);
    uint8_t key0[1 + BD_MAX_CLASSES];
    key0[0] = (uint8_t)initial_required;
    memcpy(key0 + 1, initial_counts, (size_t)ctx->nb_classes);
    bd_level_add(&cur, key0, 1);
    int start_pos = 1;

    for (;;) {
        struct bd_job_result r = bd_run_fragment_job(ctx, closure_target, &cur, start_pos,
                                                      /*allow_parallel=*/1, nb_workers, bd_split_threshold_bytes);
        bd_apply_job_result(&stack, &total, &r);

        if (stack.count == 0) {
            break;
        }
        struct bd_pending_slice slice = stack.items[--stack.count];
        bd_level_load_file(&cur, slice.shard_path);
        unlink(slice.shard_path);
        rmdir(slice.shard_dir); /* echoue silencieusement si non vide : des tranches soeurs y restent */
        start_pos = slice.resume_pos;
    }

    free(stack.items);
    return total;
}
```

Cette version reste mono-process (identique en substance au comportement actuel : un seul job actif, la pile locale absorbe les fragments non traités immédiatement) — la Tâche 5 la remplacera par le vrai coordinateur à deux modes.

- [ ] **Step 3: Run the full existing suite to verify no regression**

Run: `make test 2>&1 | tail -40`
Expected: `border_ring_dp_suite` (et le reste) toujours entièrement au vert — même total sur chaque fixture qu'avant cette tâche (`border_ring_count_dp_matches_brute_force_when_forked`, `..._when_sharded_to_disk`, les deux variantes `pieces16` sous `#if ETERN_PARTS == 16`).

- [ ] **Step 4: Commit**

```bash
git add tests/tools/border_ring_dp.c
git commit -m "border_ring_dp: extrait bd_run_fragment_job (refactor sans changement de comportement)"
```

---

### Task 4: sérialisation d'un `bd_job_result` (fichier)

**Files:**
- Modify: `tests/tools/border_ring_dp.c` (ajouter après `struct bd_job_result`)
- Test: `tests/tools/test_border_ring_dp.c`

**Interfaces:**
- Produces: `void bd_job_result_write_or_die(const struct bd_job_result *r, const char *path)`, `int bd_job_result_read(struct bd_job_result *r, const char *path)` (0 si succès, -1 si fichier absent/tronqué) — non-static, utilisées par le coordinateur (Tâche 5) pour communiquer entre un job forké et le parent.

- [ ] **Step 1: Write the failing test**

```c
void bd_job_result_write_or_die(const struct bd_job_result *r, const char *path);
int bd_job_result_read(struct bd_job_result *r, const char *path);

TEST bd_job_result_round_trips_through_a_file_when_closed(void)
{
    char path[] = "/tmp/etii_brd_result_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct bd_job_result written;
    memset(&written, 0, sizeof written);
    written.closed = 1;
    written.total = 4242;

    bd_job_result_write_or_die(&written, path);

    struct bd_job_result read_back;
    memset(&read_back, 0, sizeof read_back);
    ASSERT_EQ(0, bd_job_result_read(&read_back, path));
    ASSERT_EQ(1, read_back.closed);
    ASSERT_EQ_FMT(4242LL, read_back.total, "%lld");

    unlink(path);
    PASS();
}

TEST bd_job_result_round_trips_through_a_file_when_split(void)
{
    char path[] = "/tmp/etii_brd_result_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);

    struct bd_job_result written;
    memset(&written, 0, sizeof written);
    written.closed = 0;
    snprintf(written.shard_dir, sizeof written.shard_dir, "/tmp/etii_bd_test_dir");
    written.nb_shards = 7;
    written.resume_pos = 21;

    bd_job_result_write_or_die(&written, path);

    struct bd_job_result read_back;
    memset(&read_back, 0, sizeof read_back);
    ASSERT_EQ(0, bd_job_result_read(&read_back, path));
    ASSERT_EQ(0, read_back.closed);
    ASSERT_STR_EQ("/tmp/etii_bd_test_dir", read_back.shard_dir);
    ASSERT_EQ(7, read_back.nb_shards);
    ASSERT_EQ(21, read_back.resume_pos);

    unlink(path);
    PASS();
}

TEST bd_job_result_read_reports_failure_on_missing_file(void)
{
    ASSERT_EQ(-1, bd_job_result_read(&(struct bd_job_result){0}, "/tmp/etii_brd_does_not_exist"));
    PASS();
}
```

Ajouter les trois `RUN_TEST(...)`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test 2>&1 | grep bd_job_result`
Expected: échec de lien.

- [ ] **Step 3: Implement**

```c
/* Communication entre un job (potentiellement forke, cf. Tache 5) et le
   coordinateur : struct de taille fixe, ecrite/lue en un seul bloc — pas de
   variable-length, un job ne produit jamais qu'UNE scission avant de sortir
   (il ne garde jamais de fragment pour lui-meme, cf. bd_apply_job_result). */
void bd_job_result_write_or_die(const struct bd_job_result *r, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : ecriture du resultat '%s' impossible — arret\n", path);
        exit(1);
    }
    bd_write_or_die(fp, r, sizeof *r, path);
    bd_close_or_die(fp, path);
}

int bd_job_result_read(struct bd_job_result *r, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    int ok = (fread(r, sizeof *r, 1, fp) == 1);
    fclose(fp);
    return ok ? 0 : -1;
}
```

`bd_job_result_write_or_die` doit être définie APRÈS `bd_write_or_die`/`bd_close_or_die` (déjà présentes plus haut dans le fichier, ~lignes 416-435) — la placer juste après `struct bd_job_result` (avant `bd_run_fragment_job`) fonctionne puisque ces deux helpers sont définis plus haut dans le fichier.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test 2>&1 | grep -i "bd_job_result\|FAILED\|assertions"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_ring_dp.c tests/tools/test_border_ring_dp.c
git commit -m "border_ring_dp: serialisation fichier d'un bd_job_result"
```

---

### Task 5: coordinateur — dispatch à deux modes (SOLO / POOL)

**Files:**
- Modify: `tests/tools/border_ring_dp.c` (nouvelles fonctions + réécriture finale de `bd_run_opening`)
- Test: `tests/tools/test_border_ring_dp.c`

**Interfaces:**
- Consumes : `bd_run_fragment_job`, `bd_apply_job_result`, `bd_job_result_write_or_die`/`_read`, `bd_estimate_reload_bytes`, `bd_should_run_solo`, `bd_pending_push`/`struct bd_pending_stack`/`struct bd_pending_slice` (déjà existants), `bd_abort_workers` (déjà existante).
- Produces : `struct bd_active_job { pid_t pid; char result_path[300]; double estimated_bytes; }` ; réécriture finale de `static long long bd_run_opening(...)` (signature inchangée, consommée par `bd_count_openings`, déjà existant).

- [ ] **Step 1: Write the failing test — force le mode POOL et vérifie le total**

Dans `test_border_ring_dp.c`, ajouter un test qui force au moins `nb_workers` fragments en attente simultanément (mêmes hooks `_for_tests` que `border_ring_count_dp_matches_brute_force_when_sharded_to_disk`, avec `nb_workers` assez grand pour que le nombre de fragments produits dépasse ce nombre) :

```c
/* Force plusieurs scissions consecutives avec un seuil de fragment minuscule
   ET un nombre de workers assez petit pour que la pile depasse nb_workers
   fragments en attente en cours de route — exerce reellement le mode POOL
   (jobs forkes concurrents), pas seulement le mode SOLO deja verrouille par
   border_ring_count_dp_matches_brute_force_when_sharded_to_disk. */
TEST border_ring_count_dp_matches_brute_force_when_pool_mode_engages(void)
{
    struct array_part *all = brd_make_rotate_parts(2);
    ASSERT(all != NULL);
    map_big_array *map = prepare_map_part(all);
    ASSERT(map != NULL);

    long long brute = border_walk_count(map, all, NULL, NULL);

    border_ring_dp_set_disk_mode_min_bytes_for_tests(1.0);
    border_ring_dp_set_shard_target_bytes_for_tests(32.0);
    long long dp = border_ring_count_dp(map, all, 2);
    border_ring_dp_set_disk_mode_min_bytes_for_tests(2.0 * 1024.0 * 1024.0 * 1024.0);
    border_ring_dp_set_shard_target_bytes_for_tests(768.0 * 1024.0 * 1024.0);

    ASSERT_EQ_FMT(12LL, brute, "%lld");
    ASSERT_EQ_FMT(brute, dp, "%lld");

    free_bigarray(map);
    free_array_part(all);
    PASS();
}
```

`nb_workers=2` avec un seuil de scission à 1 octet garantit que dès la première scission (plusieurs fragments produits), la pile dépasse 2 éléments → `bd_should_run_solo` bascule en mode POOL dès la deuxième itération du coordinateur.

Ajouter le `RUN_TEST(...)`.

- [ ] **Step 2: Run test to verify it still passes with the Task 3/4 (still solo-only) coordinator**

Run: `make test 2>&1 | grep -i "pool_mode_engages\|FAILED"`
Expected: PASS déjà à ce stade (le total est correct même en mode SOLO pur, puisque Task 3/4 n'ont rien changé au résultat) — ce test ne DEVIENT un test de non-régression du mode POOL qu'une fois le Step 3 de cette tâche appliqué. Le vérifier maintenant établit la ligne de base avant de toucher au dispatch.

- [ ] **Step 3: Implement the two-mode coordinator**

Ajouter, juste avant `bd_run_opening` (après `bd_apply_job_result` de la Tâche 3) :

```c
struct bd_active_job {
    pid_t pid;
    char result_path[300];
    double estimated_bytes;
};

static double bd_active_bytes_sum(const struct bd_active_job *jobs, int active)
{
    double sum = 0.0;
    for (int i = 0; i < active; i++) {
        sum += jobs[i].estimated_bytes;
    }
    return sum;
}

static int bd_find_slot(const struct bd_active_job *jobs, int active, pid_t pid)
{
    for (int i = 0; i < active; i++) {
        if (jobs[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

static void bd_remove_slot(struct bd_active_job *jobs, int *active, int slot)
{
    jobs[slot] = jobs[(*active) - 1];
    (*active)--;
}

/* Lit l'en-tete (12 octets) d'un fragment DEJA sur disque pour estimer sa
   taille de rechargement, sans jamais le charger — cf. bd_estimate_reload_bytes
   (Tache 1). */
static double bd_pending_fragment_estimate_bytes(const char *shard_path)
{
    FILE *fp = fopen(shard_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "border_ring_count_dp : lecture de '%s' impossible pour estimation — arret\n",
                shard_path);
        exit(1);
    }
    int32_t key_len = 0;
    uint64_t count = 0;
    if (fread(&key_len, sizeof key_len, 1, fp) != 1 || fread(&count, sizeof count, 1, fp) != 1) {
        fprintf(stderr, "border_ring_count_dp : en-tete de '%s' illisible — arret\n", shard_path);
        exit(1);
    }
    fclose(fp);
    return bd_estimate_reload_bytes(key_len, count);
}

/* Fork un job strictement monoprocessus (allow_parallel=0, cf. spec) pour le
   fragment `slice` — le charge, l'efface du disque, avance jusqu'a fermeture
   ou nouvelle scission, ecrit son resultat dans result_path. Retourne le pid
   de l'enfant. */
static pid_t bd_fork_pool_job(const struct bd_ctx *ctx, int8_t closure_target,
                               const struct bd_pending_slice *slice, int nb_workers,
                               double effective_budget_bytes, char *result_path, size_t result_path_size)
{
    static int job_seq = 0;
    snprintf(result_path, result_path_size, "%s/etii_bd_job_%d_%d.bin", bd_spill_dir, (int)getpid(),
             job_seq++);

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "border_ring_count_dp : fork() a echoue pour un job du pool — arret\n");
        exit(1);
    }
    if (pid == 0) {
        struct bd_level cur;
        bd_level_load_file(&cur, slice->shard_path);
        unlink(slice->shard_path);
        rmdir(slice->shard_dir);
        struct bd_job_result r =
            bd_run_fragment_job(ctx, closure_target, &cur, slice->resume_pos, /*allow_parallel=*/0, nb_workers,
                                 effective_budget_bytes);
        bd_job_result_write_or_die(&r, result_path);
        exit(0);
    }
    return pid;
}
```

Puis remplacer le corps de `bd_run_opening` (celui écrit en Tâche 3) par :

```c
static long long bd_run_opening(const struct bd_ctx *ctx, int8_t initial_required,
                                 const int8_t *initial_counts, int8_t closure_target, int nb_workers)
{
    int nb_workers_eff = nb_workers < 1 ? 1 : nb_workers;
    double budget_solo = bd_effective_solo_budget_bytes();
    double budget_pool_total = bd_split_threshold_bytes; /* somme visee sur TOUS les slots actifs */

    struct bd_pending_stack stack;
    memset(&stack, 0, sizeof stack);
    long long total = 0;

    /* Job d'amorcage (l'ouverture) : toujours SOLO, rien d'autre a
       repartir a cet instant. */
    struct bd_level seed;
    bd_level_init(&seed, 1 + ctx->nb_classes, 1 << 10);
    uint8_t key0[1 + BD_MAX_CLASSES];
    key0[0] = (uint8_t)initial_required;
    memcpy(key0 + 1, initial_counts, (size_t)ctx->nb_classes);
    bd_level_add(&seed, key0, 1);
    struct bd_job_result r0 =
        bd_run_fragment_job(ctx, closure_target, &seed, 1, /*allow_parallel=*/1, nb_workers_eff, budget_solo);
    bd_apply_job_result(&stack, &total, &r0);

    struct bd_active_job *jobs = malloc((size_t)nb_workers_eff * sizeof *jobs);
    int active = 0;

    while (stack.count > 0 || active > 0) {
        if (bd_should_run_solo(active, stack.count, nb_workers_eff)) {
            struct bd_pending_slice slice = stack.items[--stack.count];
            struct bd_level cur;
            bd_level_load_file(&cur, slice.shard_path);
            unlink(slice.shard_path);
            rmdir(slice.shard_dir);
            struct bd_job_result r = bd_run_fragment_job(ctx, closure_target, &cur, slice.resume_pos,
                                                          /*allow_parallel=*/1, nb_workers_eff, budget_solo);
            bd_apply_job_result(&stack, &total, &r);
            continue;
        }

        /* Mode POOL : ne consulte que le sommet de la pile (pas de recherche
           plus profonde pour un fragment plus petit qui tiendrait mieux —
           simplicite assumee, cf. spec § Risques connus). */
        while (active < nb_workers_eff && stack.count > 0) {
            double est = bd_pending_fragment_estimate_bytes(stack.items[stack.count - 1].shard_path);
            if (bd_active_bytes_sum(jobs, active) + est > budget_pool_total) {
                break;
            }
            struct bd_pending_slice slice = stack.items[--stack.count];
            jobs[active].estimated_bytes = est;
            jobs[active].pid = bd_fork_pool_job(ctx, closure_target, &slice, nb_workers_eff,
                                                 budget_pool_total / (double)nb_workers_eff,
                                                 jobs[active].result_path, sizeof jobs[active].result_path);
            active++;
        }

        if (active == 0) {
            fprintf(stderr,
                    "border_ring_count_dp : --dp-max-ram-mo trop bas pour traiter ne serait-ce qu'un "
                    "fragment en attente (meme reduit a sa part par worker) — arret\n");
            exit(1);
        }

        int status;
        pid_t done = waitpid(-1, &status, 0);
        int slot = bd_find_slot(jobs, active, done);
        if (slot < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "border_ring_count_dp : un job du pool a echoue (pid=%d, status=%d) — arret\n",
                    (int)done, status);
            for (int i = 0; i < active; i++) {
                if (jobs[i].pid != done) {
                    kill(jobs[i].pid, SIGTERM);
                    waitpid(jobs[i].pid, NULL, 0);
                }
            }
            exit(1);
        }

        struct bd_job_result r;
        if (bd_job_result_read(&r, jobs[slot].result_path) != 0) {
            fprintf(stderr, "border_ring_count_dp : resultat du job pid=%d illisible ('%s') — arret\n",
                    (int)done, jobs[slot].result_path);
            exit(1);
        }
        unlink(jobs[slot].result_path);
        bd_apply_job_result(&stack, &total, &r);
        bd_remove_slot(jobs, &active, slot);
    }

    free(jobs);
    free(stack.items);
    return total;
}
```

Note : `bd_effective_solo_budget_bytes()` est introduite à la Tâche 6 — pour que CETTE tâche compile avant, ajouter provisoirement juste avant `bd_run_opening` :

```c
static double bd_effective_solo_budget_bytes(void)
{
    return bd_split_threshold_bytes; /* remplace par une vraie marge a la Tache 6 */
}
```

(la Tâche 6 remplace ce corps, ne le laisse pas en l'état).

- [ ] **Step 4: Run all tests, including the new one**

Run: `make test 2>&1 | tail -60`
Expected: tout au vert — en particulier `border_ring_count_dp_matches_brute_force_when_pool_mode_engages`, `..._when_forked`, `..._when_sharded_to_disk`, et les deux variantes `pieces16`.

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_ring_dp.c tests/tools/test_border_ring_dp.c
git commit -m "border_ring_dp: coordinateur a deux modes (SOLO/POOL) pour paralleliser entre fragments"
```

---

### Task 6: marge heuristique du budget SOLO

**Files:**
- Modify: `tests/tools/border_ring_dp.c` (`border_ring_dp_set_max_ram_mo`, `bd_effective_solo_budget_bytes`)
- Test: `tests/tools/test_border_ring_dp.c`

**Interfaces:**
- Produces: `double border_ring_dp_get_solo_budget_bytes_for_tests(void)` — non-static, test-only.
- Modifies: `void border_ring_dp_set_max_ram_mo(long mo, int nb_workers)` (signature publique inchangée, déjà dans `border_ring_dp.h`).

- [ ] **Step 1: Write the failing test**

```c
double border_ring_dp_get_solo_budget_bytes_for_tests(void);

TEST border_ring_dp_set_max_ram_mo_derives_a_conservative_solo_budget(void)
{
    border_ring_dp_set_max_ram_mo(4096, 10);
    double solo_budget = border_ring_dp_get_solo_budget_bytes_for_tests();
    double raw_budget = 4096.0 * 1024.0 * 1024.0;

    /* La marge doit reserver une fraction reelle du budget brut : ni egale
       (aucune marge), ni degenere (proche de 0). */
    ASSERT(solo_budget < raw_budget);
    ASSERT(solo_budget > raw_budget / 10.0);

    border_ring_dp_set_max_ram_mo(2048, 4);
    PASS();
}
```

Ajouter le `RUN_TEST(...)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test 2>&1 | grep derives_a_conservative`
Expected: échec de lien (`border_ring_dp_get_solo_budget_bytes_for_tests` non définie), ou échec d'assertion si le stub temporaire de la Tâche 5 (`bd_split_threshold_bytes` sans marge) est encore en place.

- [ ] **Step 3: Implement**

Remplacer le corps temporaire de `bd_effective_solo_budget_bytes` (ajouté en Tâche 5) et étendre `border_ring_dp_set_max_ram_mo` :

```c
/* Marge heuristique du mode SOLO, PAS un calcul exact (contrairement a
   bd_estimate_reload_bytes) : /2 pour la co-residence ancien+nouveau niveau
   pendant toute la duree d'une transition, /2 supplementaire pour la
   non-deduplication entre les nb_workers tables locales de
   bd_transition_parallel — valeur a ajuster empiriquement une fois mesuree
   sur un run reel, meme demarche que le /3 de bd_shard_target_bytes
   ci-dessous. Cf. spec § Comptabilite RAM. */
static double bd_solo_budget_bytes = 2.0 * 1024.0 * 1024.0 * 1024.0 / 4.0;

static double bd_effective_solo_budget_bytes(void)
{
    return bd_solo_budget_bytes;
}

double border_ring_dp_get_solo_budget_bytes_for_tests(void)
{
    return bd_solo_budget_bytes;
}
```

Dans `border_ring_dp_set_max_ram_mo` (définition existante, ~ligne 652), ajouter la dernière ligne :

```c
void border_ring_dp_set_max_ram_mo(long mo, int nb_workers)
{
    double ram_bytes = (double)mo * 1024.0 * 1024.0;
    bd_split_threshold_bytes = ram_bytes;

    int workers = nb_workers < 1 ? 1 : nb_workers;
    double target = ram_bytes / ((double)workers * 3.0);
    bd_shard_target_bytes = target < BD_SHARD_TARGET_BYTES_FLOOR ? BD_SHARD_TARGET_BYTES_FLOOR : target;

    bd_solo_budget_bytes = ram_bytes / 4.0;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test 2>&1 | grep -i "derives_a_conservative\|FAILED\|assertions"`
Expected: PASS, et l'ensemble de `make test` reste vert (le budget SOLO par défaut, `2 Go / 4 = 512 Mo`, reste largement au-dessus des tailles de fixtures de test — pas de régression sur les scissions forcées par les hooks `_for_tests`, qui court-circuitent `bd_split_threshold_bytes`/`bd_shard_target_bytes` directement, jamais `bd_solo_budget_bytes`).

- [ ] **Step 5: Commit**

```bash
git add tests/tools/border_ring_dp.c tests/tools/test_border_ring_dp.c
git commit -m "border_ring_dp: marge heuristique documentee pour le budget RAM du mode solo"
```

---

### Task 7: documentation

**Files:**
- Modify: `docs/tests_et_ci.md` (section `--dp`, § « Scission par pile LIFO », lignes ~380-463)
- Modify: `tests/tools/border_ring_dp.h` (commentaire de tête)
- Modify: `docs/superpowers/specs/2026-09-06-masse-bordure-design.md` et `2026-09-06-border-mass-parallel-design.md` (renvoi)

**Interfaces:** aucune — tâche documentaire pure.

- [ ] **Step 1: Mettre à jour `docs/tests_et_ci.md`**

Remplacer le paragraphe « Scission par pile LIFO... » (§ commençant ligne ~380) par une description du coordinateur à deux modes : mode SOLO (comportement historique, marge heuristique `/4` sur le budget), mode POOL (dès `nb_workers` fragments en attente, jobs forkés concurrents strictement monoprocessus, marge de rechargement EXACTE via `bd_estimate_reload_bytes`). Renvoyer vers
`docs/superpowers/specs/2026-09-09-border-ring-dp-pool-ram-design.md` pour le raisonnement complet, à la manière du renvoi existant vers `2026-09-06-border-mass-parallel-design.md`.

- [ ] **Step 2: Mettre à jour le commentaire de tête de `border_ring_dp.h`**

Remplacer « un seul repris IMMÉDIATEMENT en mémoire... les K-1 autres empilés » par une description du pool de workers (K fragments TOUS repoussés vers la pile partagée, redistribués par le coordinateur entre mode SOLO et mode POOL selon la profondeur de la pile).

- [ ] **Step 3: Ajouter les renvois croisés dans les deux specs existantes**

Dans `docs/superpowers/specs/2026-09-06-masse-bordure-design.md` et `2026-09-06-border-mass-parallel-design.md`, ajouter une ligne dans leur section pertinente (`--dp`) renvoyant vers `2026-09-09-border-ring-dp-pool-ram-design.md` pour la gestion RAM/fragments à jour.

- [ ] **Step 4: Vérifier qu'aucune référence obsolète ne subsiste**

Run: `grep -rn "un seul.*repris immédiatement\|K-1 autres" docs/ tests/tools/border_ring_dp.h`
Expected: plus aucune occurrence décrivant l'ancien mécanisme comme le comportement actuel (les specs historiques, elles, restent inchangées — c'est de l'archive, pas de la doc de comportement courant).

- [ ] **Step 5: Commit**

```bash
git add docs/tests_et_ci.md tests/tools/border_ring_dp.h docs/superpowers/specs/2026-09-06-masse-bordure-design.md docs/superpowers/specs/2026-09-06-border-mass-parallel-design.md
git commit -m "docs: decrit le pool de workers a deux modes de border_ring_dp"
```

---

## Self-Review

**1. Couverture de la spec** :
- § Deux modes exclusifs → Tâches 2, 5.
- § Comptabilité RAM (marge exacte mode POOL) → Tâche 1, utilisée en Tâche 5.
- § Comptabilité RAM (marge heuristique mode SOLO) → Tâche 6.
- § Comportement uniforme d'un job (jamais garder un fragment) → Tâche 3 (`bd_apply_job_result`), confirmé en Tâche 5.
- § Ordonnancement (pile LIFO partagée, `waitpid(-1,...)`) → Tâche 5.
- § Échecs et interruption → Tâche 5 (abandon des jobs frères sur échec).
- § Pourquoi fork, pas des threads → décision déjà actée dans la spec, reflétée par le choix d'implémentation (fork partout, jamais de thread) — aucune tâche dédiée nécessaire, c'est une contrainte de conception suivie tout du long.
- § Documentation à mettre à jour → Tâche 7.

**2. Scan de placeholders** : aucun "TBD"/"TODO" dans les étapes ; le seul renvoi différé explicite (le stub temporaire de `bd_effective_solo_budget_bytes` en Tâche 5, remplacé en Tâche 6) est signalé clairement comme tel avec la tâche qui le résorbe, pas laissé en l'air.

**3. Cohérence des types/signatures** : `struct bd_job_result` (Tâche 3) est utilisée identiquement en Tâches 4 et 5 ; `bd_should_run_solo(int active, int stack_count, int nb_workers)` (Tâche 2) est appelée avec exactement ces trois arguments dans la boucle de la Tâche 5 ; `bd_estimate_reload_bytes(int32_t, uint64_t)` (Tâche 1) est appelée avec les mêmes types lus depuis l'en-tête du fragment dans `bd_pending_fragment_estimate_bytes` (Tâche 5).

---

Plan complete and saved to `docs/superpowers/plans/2026-09-09-border-ring-dp-pool-ram.md`. Deux options d'exécution :

**1. Subagent-Driven (recommandé)** — je dispatche un subagent frais par tâche, avec revue entre chaque tâche, itération rapide.

**2. Exécution en ligne** — j'exécute les tâches dans cette session via `executing-plans`, par lots avec points de contrôle pour revue.

Laquelle préférez-vous ?
