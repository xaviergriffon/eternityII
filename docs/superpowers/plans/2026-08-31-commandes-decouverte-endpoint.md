# Endpoint de découverte des commandes (`GET /api/v1/commands`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the `scope` (`common`/`client_only`/`server_only`) and `remote_class` (`read_only`/`write_relayable`/`write_server_only`) classification of every network-reachable console command as consultable data, via a new unauthenticated `GET /api/v1/commands` endpoint — so a third-party consumer (`eternityII_web`) stops hand-mirroring this taxonomy across two repos.

**Architecture:** Consolidate the existing (correct but scattered) classification logic into two small, enumerable, single-source-of-truth functions — `control_command_classify`/`control_command_enumerate` for `remote_class` (`src/net/control_protocol.c`), `command_scope_classify` for `scope` (`src/ui/command_lines.c`, replacing two duplicated hand-written functions) — then a thin collection/formatting layer (`http_server.c`/`http_codec.c`, same pattern as every other `GET` route) turns that into JSON.

**Tech Stack:** C (C11), greatest.h unit test framework, existing HTTP/1.1 admin API (`src/net/http_codec.c`/`http_server.c`), no new dependencies.

**Spec:** [docs/conception/decouverte_commandes_scope_remote_class.md](../../conception/decouverte_commandes_scope_remote_class.md)

## Global Constraints

- No new struct field on `command_description` (`src/ui/command_lines.c`) — that table has ~50 positionally-initialized entries; a new field would force touching every one or trigger `-Wmissing-field-initializers` (documented prior decision, do not revisit).
- `GET /api/v1/commands` requires **no authentication** — same posture as every other `GET` route (pure static metadata, nothing sensitive).
- The endpoint lists **only** the ~27 commands known to `control_command_classify` (the network-relevant subset) — not the full ~50-entry console table.
- `requires_token` is an **explicit** field on the wire, computed server-side as `!control_command_read_only(name)` — never left for a consumer to derive from `remote_class`.
- `summary`/`usage` are embedded in the JSON **without escaping** — they are compile-time string literals from `command_description`, never runtime/attacker-controlled data (same distinction the codebase already draws for `json_escape_label`, reserved for declared/unverified fields like `label`).
- Every behavior change gets a unit test in the same task (TDD: failing test first). `make test` must pass after every task.
- Indentation matches the file being edited: `src/net/control_protocol.c` uses **tabs**; `src/ui/command_lines.c`, `src/net/http_server.c`, `src/net/http_codec.c` use **4 spaces**.
- Commit messages are one line, no `Co-Authored-By` trailer (project convention).

---

## Task 1: Consolidate `control_command_classify` into one enumerable table

**Files:**
- Modify: `src/net/control_protocol.h`
- Modify: `src/net/control_protocol.c:183-229` (replaces the body of `control_command_classify`, keeps `command_first_word_matches` unused code removed)
- Test: `tests/net/test_control_protocol.c`

**Interfaces:**
- Produces: `int control_command_enumerate(const char *out_names[], control_command_class_t out_classes[], int max);` — fills up to `max` `(name, class)` pairs, returns the count written. Declared in `control_protocol.h`, consumed by Task 4.
- Produces: `#define CONTROL_COMMAND_TABLE_MAX 40` in `control_protocol.h` — sizes caller-side arrays.
- `control_command_classify`/`control_command_allowed`/`control_command_privileged`/`control_command_read_only` keep their existing signatures and behavior unchanged (verified by the full existing test suite in `test_control_protocol.c`, which must still pass with zero edits).

- [ ] **Step 1: Write the failing test for `control_command_enumerate`**

Add to `tests/net/test_control_protocol.c`, right after `control_command_read_only_handles_null` (before the `SUITE` block):

```c
/* control_command_enumerate : énumère exactement la table interne, sans
   doublon, et control_command_classify sur chaque nom retourné doit
   redonner la classe rapportée -- garde-fou contre une table qui
   divergerait de la fonction de classification qu'elle alimente. */
TEST control_command_enumerate_lists_all_known_commands(void)
{
    const char *names[CONTROL_COMMAND_TABLE_MAX];
    control_command_class_t classes[CONTROL_COMMAND_TABLE_MAX];

    int n = control_command_enumerate(names, classes, CONTROL_COMMAND_TABLE_MAX);

    ASSERT_EQ_FMT(27, n, "%d");
    for (int i = 0; i < n; i++) {
        ASSERT_EQ_FMT((int)classes[i], (int)control_command_classify(names[i]), "%d");
        for (int j = 0; j < n; j++) {
            if (i != j) {
                ASSERT(strcmp(names[i], names[j]) != 0);
            }
        }
    }
    PASS();
}

TEST control_command_enumerate_respects_max(void)
{
    const char *names[3];
    control_command_class_t classes[3];

    int n = control_command_enumerate(names, classes, 3);

    ASSERT_EQ_FMT(3, n, "%d");
    PASS();
}

TEST control_command_enumerate_covers_all_three_classes(void)
{
    const char *names[CONTROL_COMMAND_TABLE_MAX];
    control_command_class_t classes[CONTROL_COMMAND_TABLE_MAX];
    int n = control_command_enumerate(names, classes, CONTROL_COMMAND_TABLE_MAX);

    int seen_read_only = 0, seen_relayable = 0, seen_server_only = 0;
    for (int i = 0; i < n; i++) {
        if (classes[i] == CTRL_CMD_READ_ONLY) seen_read_only = 1;
        if (classes[i] == CTRL_CMD_WRITE_RELAYABLE) seen_relayable = 1;
        if (classes[i] == CTRL_CMD_WRITE_SERVER_ONLY) seen_server_only = 1;
    }
    ASSERT_EQ_FMT(1, seen_read_only, "%d");
    ASSERT_EQ_FMT(1, seen_relayable, "%d");
    ASSERT_EQ_FMT(1, seen_server_only, "%d");
    PASS();
}
```

Register the three in `SUITE(control_protocol_suite)`, right after `RUN_TEST(control_command_read_only_handles_null);`:

```c
    RUN_TEST(control_command_enumerate_lists_all_known_commands);
    RUN_TEST(control_command_enumerate_respects_max);
    RUN_TEST(control_command_enumerate_covers_all_three_classes);
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test 2>&1 | grep -A5 "control_command_enumerate"`
Expected: build error — `control_command_enumerate`/`CONTROL_COMMAND_TABLE_MAX` undeclared.

- [ ] **Step 3: Add the table, `CONTROL_COMMAND_TABLE_MAX`, and `control_command_enumerate` declarations to `control_protocol.h`**

In `src/net/control_protocol.h`, right after the `control_command_class_t` enum's closing `} control_command_class_t;` (currently ending the block that starts at line 207) and before `control_command_classify`'s declaration, insert:

```c
/// Borne haute du nombre de commandes réseau-pertinentes (les trois classes
/// de control_command_class_t confondues, hors CTRL_CMD_UNKNOWN) que
/// control_command_enumerate peut rapporter. 27 aujourd'hui ; marge incluse
/// pour ne pas avoir à retoucher les appelants (GET /api/v1/commands) à
/// chaque commande ajoutée.
#define CONTROL_COMMAND_TABLE_MAX 40
```

Then, right after the declaration of `control_command_read_only` (end of the block documenting the four `control_command_*` functions), add:

```c
/**
 * @brief Énumère toutes les commandes connues de control_command_classify
 *        (les trois classes confondues), pour construire une réponse
 *        GET /api/v1/commands (src/net/http_server.c) sans dupliquer la
 *        table interne à cette fonction.
 *
 * @param out_names   Tableau de sortie de pointeurs vers des noms de
 *                     commande (littéraux statiques, jamais à libérer),
 *                     capacité au moins `max`.
 * @param out_classes Tableau de sortie parallèle à `out_names`, même capacité.
 * @param max         Capacité des deux tableaux de sortie.
 * @return             Nombre d'entrées écrites (borné par `max`).
 */
int control_command_enumerate(const char *out_names[], control_command_class_t out_classes[], int max);
```

- [ ] **Step 4: Replace the three static arrays in `control_command_classify` with one table, and implement `control_command_enumerate`**

In `src/net/control_protocol.c`, replace the entire body of `control_command_classify` (from `control_command_class_t control_command_classify(const char *command_name)` through its closing `}`, currently lines 183-229) with:

```c
typedef struct {
	const char *name;
	control_command_class_t cls;
} command_table_entry_t;

static const command_table_entry_t g_command_table[] = {
	{ "clientsWork", CTRL_CMD_READ_ONLY },
	{ "pause", CTRL_CMD_WRITE_RELAYABLE },
	{ "resume", CTRL_CMD_WRITE_RELAYABLE },
	{ "limit", CTRL_CMD_WRITE_RELAYABLE },
	{ "maxStockByThread", CTRL_CMD_WRITE_RELAYABLE },
	{ "prunerBatch", CTRL_CMD_WRITE_RELAYABLE },
	{ "prunerDfsBudget", CTRL_CMD_WRITE_RELAYABLE },
	{ "clientsCommand", CTRL_CMD_WRITE_RELAYABLE },
	{ "clientsCmd", CTRL_CMD_WRITE_RELAYABLE },
	{ "clientsRoles", CTRL_CMD_WRITE_RELAYABLE },
	{ "start", CTRL_CMD_WRITE_RELAYABLE },
	{ "stopForks", CTRL_CMD_WRITE_RELAYABLE },
	{ "configApply", CTRL_CMD_WRITE_RELAYABLE },
	{ "config", CTRL_CMD_WRITE_RELAYABLE },
	{ "configSave", CTRL_CMD_WRITE_RELAYABLE },
	{ "restore", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "backup", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "sortAsc", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "sortAscFiles", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "sortDesc", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "sortDescFiles", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "sortDescMulti", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "split", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "regroup", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "rebalance", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "stockMaxRam", CTRL_CMD_WRITE_SERVER_ONLY },
	{ "spill", CTRL_CMD_WRITE_SERVER_ONLY },
};
#define COMMAND_TABLE_SIZE (sizeof(g_command_table) / sizeof(g_command_table[0]))

control_command_class_t control_command_classify(const char *command_name)
{
	if (command_name == NULL) {
		return CTRL_CMD_UNKNOWN;
	}

	/* Longueur du premier mot (avant un espace éventuel). */
	size_t word_len = 0;
	while (command_name[word_len] != '\0' && command_name[word_len] != ' ') {
		word_len++;
	}
	if (word_len == 0) {
		return CTRL_CMD_UNKNOWN;
	}

	for (size_t i = 0; i < COMMAND_TABLE_SIZE; i++) {
		if (strlen(g_command_table[i].name) == word_len
		    && strncmp(g_command_table[i].name, command_name, word_len) == 0) {
			return g_command_table[i].cls;
		}
	}
	return CTRL_CMD_UNKNOWN;
}

int control_command_enumerate(const char *out_names[], control_command_class_t out_classes[], int max)
{
	int n = 0;
	for (size_t i = 0; i < COMMAND_TABLE_SIZE && n < max; i++) {
		out_names[n] = g_command_table[i].name;
		out_classes[n] = g_command_table[i].cls;
		n++;
	}
	return n;
}
```

This removes the now-unused `command_first_word_matches` helper (its only callers were the three arrays being replaced) — delete its definition too (the `static int command_first_word_matches(...)` function immediately above the old `control_command_classify`).

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make test 2>&1 | tail -40`
Expected: PASS for all `control_protocol_suite` tests, including the three new ones, and the full suite (no regression elsewhere — `control_command_allowed`/`_privileged`/`_read_only` behavior is byte-identical, they call the same classify function).

- [ ] **Step 6: Commit**

```bash
git add src/net/control_protocol.h src/net/control_protocol.c tests/net/test_control_protocol.c
git commit -m "net: consolidate control_command_classify into one enumerable table"
```

---

## Task 2: `command_scope_classify` — dedupe the two `client_only` functions

**Files:**
- Modify: `src/ui/command_lines.h`
- Modify: `src/ui/command_lines.c:1793-1797` (removes `admin_remote_command_is_client_only`), `:1815` (call site), `:2450-2454` (removes `command_is_client_only`), `:2469`, `:2561`, `:2610`, `:2702` (call sites)
- Test: `tests/ui/test_command_lines.c`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `typedef enum { CMD_SCOPE_COMMON = 0, CMD_SCOPE_CLIENT_ONLY, CMD_SCOPE_SERVER_ONLY } command_scope_t;` and `command_scope_t command_scope_classify(const char *command_name);` — declared in `command_lines.h`, consumed by Task 4 (`http_commands_collect`).
- Produces: `int command_lookup_help_text(const char *command_name, const char **out_summary, const char **out_usage);` — declared in `command_lines.h`, consumed by Task 4.

- [ ] **Step 1: Write the failing tests**

Add to `tests/ui/test_command_lines.c`, near the top of the file (after the existing `#include`s, before the first `TEST`) — check the file does not already `#include <string.h>`; it does (used throughout), so no new include needed. Add this block right before `SUITE(command_lines_suite)`:

```c
/* ---------- command_scope_classify ----------------------------------------- */

TEST command_scope_classify_client_only(void)
{
    ASSERT_EQ_FMT((int)CMD_SCOPE_CLIENT_ONLY, (int)command_scope_classify("start"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_CLIENT_ONLY, (int)command_scope_classify("stopForks"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_CLIENT_ONLY, (int)command_scope_classify("configApply"), "%d");
    PASS();
}

TEST command_scope_classify_server_only(void)
{
    ASSERT_EQ_FMT((int)CMD_SCOPE_SERVER_ONLY, (int)command_scope_classify("clientsWork"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_SERVER_ONLY, (int)command_scope_classify("stockMaxRam"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_SERVER_ONLY, (int)command_scope_classify("clientsCommand"), "%d");
    PASS();
}

TEST command_scope_classify_common(void)
{
    ASSERT_EQ_FMT((int)CMD_SCOPE_COMMON, (int)command_scope_classify("pause"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_COMMON, (int)command_scope_classify("restore"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_COMMON, (int)command_scope_classify("config"), "%d");
    PASS();
}

TEST command_scope_classify_unknown_defaults_to_common(void)
{
    ASSERT_EQ_FMT((int)CMD_SCOPE_COMMON, (int)command_scope_classify("notACommand"), "%d");
    ASSERT_EQ_FMT((int)CMD_SCOPE_COMMON, (int)command_scope_classify(NULL), "%d");
    PASS();
}

/* ---------- command_lookup_help_text ---------------------------------------- */

TEST command_lookup_help_text_found_with_usage(void)
{
    const char *summary = NULL;
    const char *usage = NULL;
    ASSERT_EQ_FMT(1, command_lookup_help_text("pause", &summary, &usage), "%d");
    ASSERT(summary != NULL);
    /* "pause" ne prend pas d'argument -> usage NULL dans command_description. */
    ASSERT(usage == NULL);
    PASS();
}

TEST command_lookup_help_text_found_without_usage(void)
{
    const char *summary = NULL;
    const char *usage = NULL;
    ASSERT_EQ_FMT(1, command_lookup_help_text("limit", &summary, &usage), "%d");
    ASSERT(summary != NULL);
    ASSERT(usage != NULL);
    PASS();
}

TEST command_lookup_help_text_not_found(void)
{
    const char *summary = (const char *)0x1; /* sentinelle : ne doit pas être touchée */
    const char *usage = (const char *)0x1;
    ASSERT_EQ_FMT(0, command_lookup_help_text("notACommand", &summary, &usage), "%d");
    ASSERT(summary == (const char *)0x1);
    ASSERT(usage == (const char *)0x1);
    PASS();
}
```

Register in `SUITE(command_lines_suite)`:

```c
    RUN_TEST(command_scope_classify_client_only);
    RUN_TEST(command_scope_classify_server_only);
    RUN_TEST(command_scope_classify_common);
    RUN_TEST(command_scope_classify_unknown_defaults_to_common);
    RUN_TEST(command_lookup_help_text_found_with_usage);
    RUN_TEST(command_lookup_help_text_found_without_usage);
    RUN_TEST(command_lookup_help_text_not_found);
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test 2>&1 | grep -A5 "command_scope_classify\|command_lookup_help_text"`
Expected: build error — both symbols undeclared.

- [ ] **Step 3: Declare the new enum and functions in `command_lines.h`**

In `src/ui/command_lines.h`, right after `const char *command_canonical_name(const char *name);` and before the `help_format_general` declaration, insert:

```c
/**
 * @brief Axe "où cette commande a-t-elle un sens en local" -- orthogonal à
 *        control_command_class_t (src/net/control_protocol.h), qui répond à
 *        "comment/si elle voyage sur le réseau". Voir
 *        docs/conception/decouverte_commandes_scope_remote_class.md.
 */
typedef enum {
    /// Exécutable des deux côtés (la majorité des commandes).
    CMD_SCOPE_COMMON = 0,
    /// N'a de sens que côté CLIENT (pilotage du cycle de vie des fils de
    /// recherche) -- masquée côté serveur (do_command_line, help_format_*,
    /// admin_apply_remote_command).
    CMD_SCOPE_CLIENT_ONLY,
    /// N'a de sens que côté SERVEUR (champ command_description.server_only).
    CMD_SCOPE_SERVER_ONLY,
} command_scope_t;

/**
 * @brief Classifie une commande sur l'axe `scope` (fonction pure) -- source
 *        unique de vérité, remplace les anciennes command_is_client_only/
 *        admin_remote_command_is_client_only (dupliquées, même littéral
 *        "start"/"stopForks"/"configApply" porté deux fois).
 *
 * Contrat : `command_name` est un nom SEUL, sans arguments -- contrairement à
 * control_command_classify, cette fonction ne tokenise pas une ligne
 * complète (aucun appelant actuel ou prévu n'a jamais qu'un verbe déjà
 * isolé). NULL ou un nom inconnu retourne CMD_SCOPE_COMMON.
 *
 * @param command_name Nom de commande (ex. "pause", "start").
 * @return              CMD_SCOPE_COMMON / CMD_SCOPE_CLIENT_ONLY / CMD_SCOPE_SERVER_ONLY.
 */
command_scope_t command_scope_classify(const char *command_name);

/**
 * @brief Cherche le résumé/usage d'aide d'une commande, pour construire une
 *        réponse GET /api/v1/commands (src/net/http_server.c) sans exposer
 *        command_description/find_command hors de ce fichier.
 *
 * @param command_name Nom de commande (alias résolus, comme find_command).
 * @param out_summary  Reçoit un pointeur vers le résumé (littéral statique),
 *                      inchangé si la commande est introuvable.
 * @param out_usage    Reçoit un pointeur vers la syntaxe (littéral statique),
 *                      NULL si la commande ne prend pas d'argument, inchangé
 *                      si la commande est introuvable.
 * @return              1 si trouvée, 0 sinon.
 */
int command_lookup_help_text(const char *command_name, const char **out_summary, const char **out_usage);

```

- [ ] **Step 4: Implement `command_scope_classify` and `command_lookup_help_text`, remove the two duplicated functions, update call sites**

In `src/ui/command_lines.c`:

4a. Delete `admin_remote_command_is_client_only` entirely (currently lines 1782-1797, the Doxygen comment plus the function body):

```c
static int admin_remote_command_is_client_only(const char *word) {
    return strcmp(word, "start") == 0 ||
           strcmp(word, "stopForks") == 0 ||
           strcmp(word, "configApply") == 0;
}
```

4b. At its call site (currently `if (server && admin_remote_command_is_client_only(word)) {` inside `admin_apply_remote_command`), replace with:

```c
        if (server && command_scope_classify(word) == CMD_SCOPE_CLIENT_ONLY) {
```

4c. Delete `command_is_client_only` entirely (currently lines 2432-2454, the Doxygen comment plus the function body):

```c
static int command_is_client_only(const command_description *command) {
    return strcmp(command->command, "start") == 0 ||
           strcmp(command->command, "stopForks") == 0 ||
           strcmp(command->command, "configApply") == 0;
}
```

4d. Update its four call sites, replacing `command_is_client_only(command)` / `command_is_client_only(command_desc)` with `command_scope_classify(command->command) == CMD_SCOPE_CLIENT_ONLY` / `command_scope_classify(command_desc->command) == CMD_SCOPE_CLIENT_ONLY` (same variable each site already uses). Each site also carries an inline or Doxygen comment naming `command_is_client_only` by name — update those references too, so none point at a deleted symbol:

- in `visible_command_names`'s Doxygen comment (was line 2459): `` (masquage server-side de `config`/`configSave`, voir `command_is_client_only`) `` → `` (masquage server-side de `config`/`configSave`, voir `command_scope_classify`) ``
- in `visible_command_names` (was line 2469): `if (server && command_is_client_only(&commands[c])) {` → `if (server && command_scope_classify(commands[c].command) == CMD_SCOPE_CLIENT_ONLY) {`
- in `help_append_category` (was line 2561-2562): `if (server && command_is_client_only(command)) { /* Masquée côté serveur, cf. command_is_client_only. */` → `if (server && command_scope_classify(command->command) == CMD_SCOPE_CLIENT_ONLY) { /* Masquée côté serveur, cf. command_scope_classify. */`
- in `help_format_topic` (was line 2610-2612): `if (command != NULL && server && command_is_client_only(command)) { /* Masquée côté serveur, cf. command_is_client_only : traitée comme un sujet inconnu plutôt que d'en détailler l'usage. */` → `if (command != NULL && server && command_scope_classify(command->command) == CMD_SCOPE_CLIENT_ONLY) { /* Masquée côté serveur, cf. command_scope_classify : traitée comme un sujet inconnu plutôt que d'en détailler l'usage. */`
- in `do_command_line` (was line 2702-2706): `if (command_desc != NULL && server && command_is_client_only(command_desc)) { /* Masquée côté serveur, cf. command_is_client_only : traitée comme une commande inconnue plutôt que d'être exécutée (elle agirait sur les globales du serveur, sans rapport avec la configuration client qu'elle est censée afficher/écrire). */` → `if (command_desc != NULL && server && command_scope_classify(command_desc->command) == CMD_SCOPE_CLIENT_ONLY) { /* Masquée côté serveur, cf. command_scope_classify : traitée comme une commande inconnue plutôt que d'être exécutée (elle agirait sur les globales du serveur, sans rapport avec la configuration client qu'elle est censée afficher/écrire). */`

Also update the two other doc comments that name `command_is_client_only` elsewhere in the file (both currently cross-reference the function this task deletes):
- The comment above `admin_remote_command_is_client_only`'s old call site inside `admin_apply_remote_command` no longer applies (that function is gone — nothing to edit there beyond Step 4b above).
- `tests/ui/test_command_lines.c` also names `command_is_client_only` in prose comments near `help_shows_config_commands_in_both_roles` and `admin_apply_remote_command_lifecycle_forbidden_on_server` (Task 2's own test file) — update `` (voir `command_is_client_only`, ...) `` and `` -- même raisonnement que `command_is_client_only` pour la console. `` to say `command_scope_classify` instead, in the same edit as Step 1's new tests.

4e. Add the new functions. Place them right after `find_command` (end of file region shown earlier, after its closing `}`):

```c
command_scope_t command_scope_classify(const char *command_name)
{
    static const char *const client_only[] = { "start", "stopForks", "configApply" };

    if (command_name == NULL) {
        return CMD_SCOPE_COMMON;
    }
    for (size_t i = 0; i < sizeof(client_only) / sizeof(client_only[0]); i++) {
        if (strcmp(client_only[i], command_name) == 0) {
            return CMD_SCOPE_CLIENT_ONLY;
        }
    }

    const command_description *command = find_command(command_name);
    if (command != NULL && command->server_only) {
        return CMD_SCOPE_SERVER_ONLY;
    }
    return CMD_SCOPE_COMMON;
}

int command_lookup_help_text(const char *command_name, const char **out_summary, const char **out_usage)
{
    const command_description *command = find_command(command_name);
    if (command == NULL) {
        return 0;
    }
    *out_summary = command->summary;
    *out_usage = command->usage;
    return 1;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make test 2>&1 | tail -60`
Expected: PASS for all `command_lines_suite` tests, including the 7 new ones, AND the pre-existing masking tests unaffected: `help_shows_config_commands_in_both_roles` (still hides `start`/`stopForks`/`configApply` when `server = 1`), `admin_apply_remote_command_lifecycle_forbidden_on_server` (still `ADMIN_CMD_FORBIDDEN` for those 3 plus `config nb_forks 2` — the latter comes from unrelated `admin_remote_config` logic, untouched by this task).

- [ ] **Step 6: Commit**

```bash
git add src/ui/command_lines.h src/ui/command_lines.c tests/ui/test_command_lines.c
git commit -m "ui: replace duplicated client-only checks with command_scope_classify"
```

---

## Task 3: `http_command_info_t` and `http_json_format_commands`

**Files:**
- Modify: `src/net/http_codec.h`
- Modify: `src/net/http_codec.c`
- Test: `tests/net/test_http_codec.c`

**Interfaces:**
- Consumes: `command_scope_t` (Task 2, `ui/command_lines.h`), `control_command_class_t` (Task 1, `net/control_protocol.h`).
- Produces: `typedef struct { char name[HTTP_COMMAND_NAME_MAX]; command_scope_t scope; control_command_class_t remote_class; int requires_token; const char *summary; const char *usage; } http_command_info_t;` and `int http_json_format_commands(char *buf, size_t size, const http_command_info_t *infos, int count);` — consumed by Task 4.

- [ ] **Step 1: Write the failing test**

Add to `tests/net/test_http_codec.c`, near the other `http_json_format_*` golden tests (same style as `http_json_format_known_clients_golden`), before `SUITE(http_codec_suite)`:

```c
TEST http_json_format_commands_golden(void)
{
    http_command_info_t infos[2];
    memset(&infos, 0, sizeof(infos));

    strncpy(infos[0].name, "pause", sizeof(infos[0].name) - 1);
    infos[0].scope = CMD_SCOPE_COMMON;
    infos[0].remote_class = CTRL_CMD_WRITE_RELAYABLE;
    infos[0].requires_token = 1;
    infos[0].summary = "pose une pause administrative";
    infos[0].usage = NULL;

    strncpy(infos[1].name, "clientsWork", sizeof(infos[1].name) - 1);
    infos[1].scope = CMD_SCOPE_SERVER_ONLY;
    infos[1].remote_class = CTRL_CMD_READ_ONLY;
    infos[1].requires_token = 0;
    infos[1].summary = "consultation en lecture seule d'une session";
    infos[1].usage = "clientsWork <session_no|client_uid|label>";

    char buf[2048];
    int written = http_json_format_commands(buf, sizeof(buf), infos, 2);

    ASSERT(written > 0);
    ASSERT(strstr(buf, "{\"commands\":[") == buf);
    ASSERT(strstr(buf, "\"name\":\"pause\"") != NULL);
    ASSERT(strstr(buf, "\"scope\":\"common\"") != NULL);
    ASSERT(strstr(buf, "\"remote_class\":\"write_relayable\"") != NULL);
    ASSERT(strstr(buf, "\"requires_token\":true") != NULL);
    ASSERT(strstr(buf, "\"summary\":\"pose une pause administrative\"") != NULL);
    ASSERT(strstr(buf, "\"usage\":null") != NULL);
    ASSERT(strstr(buf, "\"name\":\"clientsWork\"") != NULL);
    ASSERT(strstr(buf, "\"scope\":\"server_only\"") != NULL);
    ASSERT(strstr(buf, "\"remote_class\":\"read_only\"") != NULL);
    ASSERT(strstr(buf, "\"requires_token\":false") != NULL);
    ASSERT(strstr(buf, "\"usage\":\"clientsWork <session_no|client_uid|label>\"") != NULL);
    PASS();
}

TEST http_json_format_commands_empty(void)
{
    char buf[64];
    int written = http_json_format_commands(buf, sizeof(buf), NULL, 0);
    ASSERT(written > 0);
    ASSERT_STR_EQ("{\"commands\":[]}", buf);
    PASS();
}

TEST http_json_format_commands_rejects_bad_args(void)
{
    http_command_info_t infos[1];
    memset(&infos, 0, sizeof(infos));
    char buf[64];
    ASSERT_EQ_FMT(-1, http_json_format_commands(NULL, sizeof(buf), infos, 1), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_commands(buf, 0, infos, 1), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_commands(buf, sizeof(buf), NULL, 1), "%d");
    ASSERT_EQ_FMT(-1, http_json_format_commands(buf, sizeof(buf), infos, -1), "%d");
    PASS();
}
```

Register in `SUITE(http_codec_suite)`:

```c
    RUN_TEST(http_json_format_commands_golden);
    RUN_TEST(http_json_format_commands_empty);
    RUN_TEST(http_json_format_commands_rejects_bad_args);
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make test 2>&1 | grep -A5 "http_json_format_commands\|http_command_info_t"`
Expected: build error — both symbols undeclared.

- [ ] **Step 3: Add `http_command_info_t` and the `HTTP_COMMAND_NAME_MAX` constant to `http_codec.h`**

In `src/net/http_codec.h`, add the two new includes right after the existing `#include "core/datamanager.h"`:

```c
#include "net/control_protocol.h"
#include "ui/command_lines.h"
```

Add the size constant near the other `HTTP_*_MAX` constants (after `HTTP_CLIENT_IP_MAX`):

```c
/// Longueur maximale (avec terminateur) d'un nom de commande dans
/// http_command_info_t.name -- la plus longue actuelle est
/// "maxStockByThread" (16 caractères), marge incluse.
#define HTTP_COMMAND_NAME_MAX 32
```

Add the struct and function declaration after `http_json_format_known_clients`'s declaration (end of that struct's block, before `http_best_board_cell_t`):

```c
/**
 * @brief Vue en lecture d'une commande whitelistée, pour
 *        GET /api/v1/commands. Remplie par http_commands_collect
 *        (src/net/http_server.h) à partir de control_command_enumerate
 *        (net/control_protocol.h) et command_scope_classify/
 *        command_lookup_help_text (ui/command_lines.h) -- voir
 *        docs/conception/decouverte_commandes_scope_remote_class.md.
 */
typedef struct {
    /// Nom de la commande (ex. "pause", "restore").
    char name[HTTP_COMMAND_NAME_MAX];
    /// Où cette commande a un sens en local (command_scope_t).
    command_scope_t scope;
    /// Comment/si elle voyage sur le réseau (control_command_class_t).
    control_command_class_t remote_class;
    /// 1 si POST /api/v1/command exige un jeton Bearer pour cette commande
    /// (toujours `!control_command_read_only(name)` -- champ explicite pour
    /// qu'un consommateur n'ait pas à redériver la règle d'auth lui-même).
    int requires_token;
    /// Résumé d'aide (littéral statique de command_description, jamais NULL
    /// pour une commande de control_command_enumerate).
    const char *summary;
    /// Syntaxe avec arguments (littéral statique), NULL si la commande n'en
    /// prend pas.
    const char *usage;
} http_command_info_t;

/**
 * @brief Sérialise un tableau de commandes en JSON dans `buf` (cf. schéma
 *        documenté dans docs/api_http_rest.md).
 *
 * @param buf    Tampon destination.
 * @param size   Taille de `buf`.
 * @param infos  Tableau de commandes (peut être vide/NULL si `count == 0`).
 * @param count  Nombre d'entrées valides dans `infos`.
 * @return       Longueur écrite (hors NUL final), ou -1 si `buf` est trop
 *               petit ou les arguments incohérents.
 */
int http_json_format_commands(char *buf, size_t size, const http_command_info_t *infos, int count);
```

- [ ] **Step 4: Implement `http_json_format_commands` in `http_codec.c`**

Add two label helpers right after `client_mode_label` (same file, same style), then the formatter after `http_json_format_known_clients`:

```c
static const char *command_scope_label(command_scope_t scope)
{
    switch (scope) {
        case CMD_SCOPE_CLIENT_ONLY: return "client_only";
        case CMD_SCOPE_SERVER_ONLY: return "server_only";
        case CMD_SCOPE_COMMON:
        default: return "common";
    }
}

static const char *command_remote_class_label(control_command_class_t cls)
{
    switch (cls) {
        case CTRL_CMD_READ_ONLY: return "read_only";
        case CTRL_CMD_WRITE_SERVER_ONLY: return "write_server_only";
        case CTRL_CMD_WRITE_RELAYABLE:
        default: return "write_relayable";
    }
}

int http_json_format_commands(char *buf, size_t size, const http_command_info_t *infos, int count)
{
    if (buf == NULL || size == 0 || (infos == NULL && count > 0) || count < 0) {
        return -1;
    }

    size_t offset = 0;
    int written = snprintf(buf + offset, size - offset, "{\"commands\":[");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    for (int i = 0; i < count; i++) {
        written = snprintf(buf + offset, size - offset,
            "%s{\"name\":\"%s\",\"scope\":\"%s\",\"remote_class\":\"%s\","
            "\"requires_token\":%s,\"summary\":\"%s\",\"usage\":",
            (i == 0) ? "" : ",",
            infos[i].name,
            command_scope_label(infos[i].scope),
            command_remote_class_label(infos[i].remote_class),
            infos[i].requires_token ? "true" : "false",
            infos[i].summary != NULL ? infos[i].summary : "");
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;

        if (infos[i].usage != NULL) {
            written = snprintf(buf + offset, size - offset, "\"%s\"}", infos[i].usage);
        } else {
            written = snprintf(buf + offset, size - offset, "null}");
        }
        if (written < 0 || (size_t)written >= size - offset) {
            return -1;
        }
        offset += (size_t)written;
    }

    written = snprintf(buf + offset, size - offset, "]}");
    if (written < 0 || (size_t)written >= size - offset) {
        return -1;
    }
    offset += (size_t)written;

    return (int)offset;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make test 2>&1 | tail -40`
Expected: PASS for all `http_codec_suite` tests, including the 3 new ones.

- [ ] **Step 6: Commit**

```bash
git add src/net/http_codec.h src/net/http_codec.c tests/net/test_http_codec.c
git commit -m "net: add http_command_info_t and http_json_format_commands"
```

---

## Task 4: Wire `GET /api/v1/commands`

**Files:**
- Modify: `src/net/http_codec.h` (route enum), `src/net/http_codec.c` (`http_route_resolve`)
- Modify: `src/net/http_server.h`, `src/net/http_server.c`
- Test: `tests/net/test_http_codec.c`, `tests/net/test_http_server.c`

**Interfaces:**
- Consumes: `control_command_enumerate`/`CONTROL_COMMAND_TABLE_MAX` (Task 1), `command_scope_classify`/`command_lookup_help_text` (Task 2), `http_command_info_t`/`http_json_format_commands` (Task 3).
- Produces: `int http_commands_collect(http_command_info_t *out, int max);` — no further consumers in this plan.

- [ ] **Step 1: Write the failing route-resolution test**

Add to `tests/net/test_http_codec.c`, inside/near `http_route_resolve_known_routes` (extend the existing test rather than adding a new one — same pattern as the other routes already listed there):

```c
    ASSERT_EQ_FMT(HTTP_ROUTE_COMMANDS, http_route_resolve("GET", "/api/v1/commands"), "%d");
```

Add this line to `http_route_resolve_known_routes`, and add to `http_route_resolve_bad_method`:

```c
    ASSERT_EQ_FMT(HTTP_ROUTE_BAD_METHOD, http_route_resolve("POST", "/api/v1/commands"), "%d");
```

- [ ] **Step 2: Write the failing integration test**

Add to `tests/net/test_http_server.c`, near the other route tests (same style as `http_server_get_stock_distribution_returns_200_empty`), before `SUITE(http_server_suite)`:

```c
TEST http_server_get_commands_returns_200(void)
{
    int sv[2];
    MAKE_PAIR(sv);

    const char req[] = "GET /api/v1/commands HTTP/1.1\r\nHost: x\r\n\r\n";
    ASSERT_EQ_FMT(0, send_all_test(sv[0], req, strlen(req)), "%d");
    ASSERT_EQ_FMT(0, handle_http_connection(sv[1]), "%d");

    char resp[HTTP_RESPONSE_MAX];
    ssize_t n = read_response(sv[0], resp, sizeof(resp));
    ASSERT(n > 0);
    ASSERT(strstr(resp, "HTTP/1.1 200 OK") == resp);
    /* Aucune authentification -- pas de "Authorization" dans la requête ci-dessus,
       et pourtant 200 : confirme la posture "sans jeton" de cette route. */
    ASSERT(strstr(resp, "\"commands\":[") != NULL);
    ASSERT(strstr(resp, "\"name\":\"pause\"") != NULL);
    ASSERT(strstr(resp, "\"name\":\"restore\"") != NULL);
    ASSERT(strstr(resp, "\"scope\":\"client_only\"") != NULL); /* "start" */
    ASSERT(strstr(resp, "\"remote_class\":\"write_server_only\"") != NULL);

    close(sv[0]); close(sv[1]);
    PASS();
}
```

Register both new/extended tests: no new `RUN_TEST` needed for the extended `http_route_resolve_known_routes`/`_bad_method` (already registered), and add:

```c
    RUN_TEST(http_server_get_commands_returns_200);
```

to `SUITE(http_server_suite)` in `test_http_server.c`.

- [ ] **Step 3: Run the tests to verify they fail**

Run: `make test 2>&1 | grep -A5 "HTTP_ROUTE_COMMANDS\|http_commands_collect\|http_server_get_commands"`
Expected: build error — `HTTP_ROUTE_COMMANDS`/`http_commands_collect` undeclared.

- [ ] **Step 4: Add `HTTP_ROUTE_COMMANDS` to the route enum and `http_route_resolve`**

In `src/net/http_codec.h`, add to the `http_route_t` enum, right after `HTTP_ROUTE_COMMAND,       ///< POST /api/v1/command`:

```c
    HTTP_ROUTE_COMMANDS,      ///< GET /api/v1/commands
```

In `src/net/http_codec.c`, in `http_route_resolve`, add right after the `/api/v1/command` block:

```c
    if (strcmp(path, "/api/v1/commands") == 0) {
        return (strcmp(method, "GET") == 0) ? HTTP_ROUTE_COMMANDS : HTTP_ROUTE_BAD_METHOD;
    }
```

- [ ] **Step 5: Declare and implement `http_commands_collect`**

In `src/net/http_server.h`, add right after `int http_known_clients_collect(http_known_client_info_t *out, int max);`:

```c
/**
 * @brief Construit la liste des commandes réseau-pertinentes (celles de
 *        control_command_enumerate) avec leur scope/remote_class/
 *        requires_token/summary/usage, pour GET /api/v1/commands. Fonction
 *        pure : aucun état vivant à lire, tout est dérivé de tables
 *        statiques (contrairement à http_clients_collect).
 *
 * @param out Tableau de sortie, capacité `max`.
 * @param max Capacité de `out`.
 * @return    Nombre d'entrées écrites (borné par `max` ET par
 *            CONTROL_COMMAND_TABLE_MAX).
 */
int http_commands_collect(http_command_info_t *out, int max);
```

In `src/net/http_server.c`, add right after `http_known_clients_collect`'s definition:

```c
int http_commands_collect(http_command_info_t *out, int max)
{
    const char *names[CONTROL_COMMAND_TABLE_MAX];
    control_command_class_t classes[CONTROL_COMMAND_TABLE_MAX];
    int n = control_command_enumerate(names, classes, CONTROL_COMMAND_TABLE_MAX);
    if (n > max) {
        n = max;
    }

    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].name, names[i], sizeof(out[i].name) - 1);
        out[i].remote_class = classes[i];
        out[i].scope = command_scope_classify(names[i]);
        out[i].requires_token = !control_command_read_only(names[i]);
        const char *summary = NULL;
        const char *usage = NULL;
        command_lookup_help_text(names[i], &summary, &usage);
        out[i].summary = summary;
        out[i].usage = usage;
    }
    return n;
}
```

- [ ] **Step 6: Wire the route in the dispatch `switch`**

In `src/net/http_server.c`, in the request-handling function's `switch (route)` block, add a new `case` right after `case HTTP_ROUTE_COMMAND: handle_command_route(socket_id, &req); break;`:

```c
        case HTTP_ROUTE_COMMANDS: {
            http_command_info_t infos[CONTROL_COMMAND_TABLE_MAX];
            int n = http_commands_collect(infos, CONTROL_COMMAND_TABLE_MAX);
            if (http_json_format_commands(json, sizeof(json), infos, n) > 0) {
                send_response(socket_id, 200, json);
            } else {
                send_response(socket_id, 500, "{\"error\":\"internal\"}");
            }
            break;
        }
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make test 2>&1 | tail -60`
Expected: PASS for all of `http_codec_suite` and `http_server_suite`, including the new/extended tests.

- [ ] **Step 8: Commit**

```bash
git add src/net/http_codec.h src/net/http_codec.c src/net/http_server.h src/net/http_server.c tests/net/test_http_codec.c tests/net/test_http_server.c
git commit -m "net: wire GET /api/v1/commands"
```

---

## Task 5: Docs, full suite, conception doc status

**Files:**
- Modify: `docs/api_http_rest.md`
- Modify: `docs/conception/decouverte_commandes_scope_remote_class.md`
- Modify: `docs/conception/README.md`

**Interfaces:**
- Consumes: nothing new — pure documentation, no code.

- [ ] **Step 1: Add the endpoint section to `docs/api_http_rest.md`**

Insert a new `### GET /api/v1/commands` section, right after `### GET /api/v1/stock-distribution` and before `## Séquences typiques`, following the exact style of the other endpoint sections (JSON example, field table, prose notes):

```markdown
### GET /api/v1/commands

Liste les commandes réseau-pertinentes (celles de `control_command_classify`,
[src/net/control_protocol.c](../src/net/control_protocol.c)) avec leur
classification sur les deux axes orthogonaux `scope` et `remote_class` — pour
qu'un consommateur tiers (ex. le dashboard `eternityII_web`) cesse de
recopier cette taxonomie à la main à chaque commande ajoutée ou retirée.
Aucune authentification requise (pure métadonnée statique, comme toutes les
autres routes `GET`).

```json
{
  "commands": [
    {
      "name": "pause",
      "scope": "common",
      "remote_class": "write_relayable",
      "requires_token": true,
      "summary": "pose une pause administrative",
      "usage": null
    },
    {
      "name": "clientsWork",
      "scope": "server_only",
      "remote_class": "read_only",
      "requires_token": false,
      "summary": "consultation en lecture seule d'une session",
      "usage": "clientsWork <session_no|client_uid|label>"
    }
  ]
}
```

| Champ | Type | Sens |
|---|---|---|
| `commands` | tableau | Une entrée par commande de `control_command_classify` (27 aujourd'hui) |
| `name` | chaîne | Nom de la commande, tel qu'accepté par `POST /api/v1/command` |
| `scope` | chaîne | Où la commande a un sens en LOCAL : `common` (les deux rôles), `client_only` (pilotage du cycle de vie des fils, masqué côté serveur), `server_only` (n'a de sens que sur un serveur) |
| `remote_class` | chaîne | Comment/si elle voyage sur le réseau : `read_only` (relayable, sans jeton), `write_relayable` (relayable, jeton requis), `write_server_only` (jamais relayable, jeton requis) — voir `control_command_class_t`, [src/net/control_protocol.h](../src/net/control_protocol.h) |
| `requires_token` | booléen | `true` sauf pour `clientsWork` — équivalent à `remote_class != "read_only"`, exposé explicitement pour qu'un consommateur n'ait pas à redériver la règle d'authentification lui-même |
| `summary` | chaîne | Résumé d'aide d'une ligne (même texte que la console `help <commande>`) |
| `usage` | chaîne ou `null` | Syntaxe avec arguments, `null` si la commande n'en prend pas |

`scope` et `remote_class` sont **orthogonaux** : `restore` est `common` ×
`write_server_only` (exécutable en local sur un client, jamais relayable) ;
`clientsWork` est `server_only` × `read_only` (n'a de sens que sur un
serveur, relayable et sans jeton). Voir
[docs/conception/decouverte_commandes_scope_remote_class.md](conception/decouverte_commandes_scope_remote_class.md)
pour le raisonnement complet.
```

Also add this file to the intro list at the top of `api_http_rest.md` (the bullet list of source files under "Le code correspondant vit dans :"), noting that `http_commands_collect`/`http_json_format_commands` share the same files already listed for the other endpoints (`http_server.c`/`http_codec.c`) — no new bullet needed, only extend the existing `control_protocol.c` bullet to mention `control_command_enumerate` alongside `control_command_classify`, and the `command_lines.c` bullet (if any) to mention `command_scope_classify`. Read the current intro list first and add these two function names to the relevant existing bullets rather than duplicating file references.

- [ ] **Step 2: Flip the conception doc's status to "implémenté"**

In `docs/conception/decouverte_commandes_scope_remote_class.md`, change the status line:

```markdown
**Statut : proposition** — non implémenté. Décrit une cible, pas le comportement actuel.
```

to:

```markdown
**Statut : implémenté** — voir [api_http_rest.md](../api_http_rest.md#get-apiv1commands) pour le comportement de référence à jour. Ce document garde le raisonnement, les approches écartées (A/C) et les points laissés ouverts.
```

- [ ] **Step 3: Update the `docs/conception/README.md` table entry**

Change the status column for `decouverte_commandes_scope_remote_class.md` from `proposition` to `implémenté`, and shorten the description to point at the reference doc (matching how other `implémenté` rows in that table read):

```markdown
| [decouverte_commandes_scope_remote_class.md](decouverte_commandes_scope_remote_class.md) | implémenté | Nouvel endpoint `GET /api/v1/commands` (voir [api_http_rest.md](../api_http_rest.md#get-apiv1commands)) exposant `scope`/`remote_class` comme donnée consultable. Préalable livré côté `eternityII` : `control_command_enumerate` (table unique, `control_protocol.c`) et `command_scope_classify` (déduplique `command_is_client_only`/`admin_remote_command_is_client_only`, `command_lines.c`). La consommation côté `eternityII_web` reste un second sous-projet, non traité ici. |
```

- [ ] **Step 4: Run the full test suite one last time**

Run: `make test 2>&1 | tail -10`
Expected: all suites PASS, zero failures.

Run: `make test-docker 2>&1 | tail -40`
Expected: the three CI jobs (WERROR build, unit tests, ASan) pass — this catches anything invisible on macOS/clang (stricter `-Werror`, ASan over-reads).

- [ ] **Step 5: Commit**

```bash
git add docs/api_http_rest.md docs/conception/decouverte_commandes_scope_remote_class.md docs/conception/README.md
git commit -m "docs: document GET /api/v1/commands, mark conception doc implemented"
```

---

## Self-Review Notes

- **Spec coverage:** every element of the spec's "Cible" section maps to a task — table consolidation (Task 1), `command_scope_classify` dedup (Task 2), struct/formatter (Task 3), route wiring (Task 4), docs (Task 5). The spec's "points laissés ouverts" (buffer sizing, entry ordering) are resolved during Task 3/4 (buffer size = existing `HTTP_RESPONSE_MAX`, already generously sized for the larger `best-board` route; entry order = table declaration order, verified by the golden test's exact-match assertions).
- **Type consistency:** `http_command_info_t.scope`/`remote_class` (Task 3) match `command_scope_t`/`control_command_class_t` (Tasks 1-2) exactly; `http_commands_collect`'s signature (Task 4) matches its declaration; `CONTROL_COMMAND_TABLE_MAX` is defined once (Task 1) and reused verbatim in Tasks 3-4.
- **No placeholders:** every step carries complete, compilable code — no "similar to above", no TBD.
