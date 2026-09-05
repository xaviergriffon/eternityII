# AGENTS.md

This file is the single source of project guidance for AI coding agents (Claude Code, Codex, …) working with code in this repository. `CLAUDE.md` imports it via `@AGENTS.md`, so edit only this file.

**This file is a compact index, not a changelog.** Full reference documentation of the **implemented** behaviour lives under `docs/` (table below) and is kept current — prefer it over guessing from source or from this file's history. Design proposals **not yet implemented** live in `docs/conception/` (a target, not the code's current behaviour — see [docs/conception/README.md](docs/conception/README.md)). Closed post-mortems on hard-to-reproduce bugs live in `docs/investigations/`. **When you add or change a feature, update the relevant `docs/*.md` file(s) and `README.md`** — not just this file. Only add something here if it's a durable, project-wide convention or invariant that a contributor needs before touching related code.

## Project Overview

eternityII is a C program that attempts to solve the [Eternity II puzzle](https://en.wikipedia.org/wiki/Eternity_II_puzzle) — a 16×16 grid with 256 pieces. It uses a distributed client-server architecture to parallelise the search space across multiple processes or machines.

## Development Workflow

- **Always work on a dedicated branch, never on `master`** — open a PR for review, even for small changes.
- **Commit messages are brief (one line) and never carry a `Co-Authored-By` trailer.**
- **Any feature or behaviour change updates `README.md` and the relevant `docs/*.md` file(s)**, not just this index.

### macOS-specific pitfalls

These bite only on macOS/clang and stay invisible on Linux/CI — `make test-docker` catches them:

- Git tracks `makefile` (lowercase); `git add Makefile` silently stages nothing on macOS's case-insensitive filesystem.
- `_exit()` in a forked child skips the gcov/llvm-cov flush — use `exit()` unless you specifically need to bypass atexit handlers.
- `gcov`'s branch coverage (`-b`) is a no-op on macOS — use clang `-fcoverage-mapping` + `llvm-cov` instead.
- AF_UNIX datagrams over ~2048 bytes fail `sendto` with `EMSGSIZE` (macOS's `net.local.dgram.maxdgram` default) — always check `sendto`'s return value.

## Documentation Map

| Doc | Covers |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Process/thread model, the COW-shared lookup map, parent↔child IPC, source layout |
| [docs/utilisation.md](docs/utilisation.md) | Every CLI mode and option (server/client/pruner/test), RAM cap, disk spillover, startup expansion, generated files |
| [docs/console.md](docs/console.md) | Every interactive console command, help system, ncurses UI |
| [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md) | Wire protocol, control channel, load management, known-clients registry, failure diagnostics |
| [docs/api_http_rest.md](docs/api_http_rest.md) | HTTP admin API: endpoints, auth model, client examples |
| [docs/autosearch_step.md](docs/autosearch_step.md) | Search loop internals: memory flow, forward-check, MRV cell choice |
| [docs/pruner_gpu_cuda.md](docs/pruner_gpu_cuda.md) | GPU pruner (CUDA build) |
| [docs/tests_et_ci.md](docs/tests_et_ci.md) + [tests/README.md](tests/README.md) | Test targets/CI matrix/benchmarks; unit-test conventions and fixtures |
| [docs/compilation.md](docs/compilation.md) | Build targets, debug flags, puzzle-size configuration |
| [docs/conception/](docs/conception/README.md) | Design proposals not yet implemented |
| [docs/investigations/](docs/investigations/README.md) | Closed post-mortems on hard-to-reproduce bugs |

## Source Layout

Sources live under `src/`, split into four domains. Includes are **explicit and domain-qualified** (e.g. `#include "core/part.h"`) and resolve via a single `-Isrc` (Makefile and `target_include_directories(eternityII PRIVATE src)` in CMake).

| Directory | Domain | Modules |
|---|---|---|
| `src/core/` | Puzzle logic & data structures + search engine | `core_static_variables` `part` `readdata` `possibility` `best_board` `lifo` `packed`(h) `etii_search` `datamanager` `stock_spill` `stock_rate` |
| `src/net/`  | TCP protocol & sockets, parent↔child IPC | `etii_protocol` `control_protocol` `client_identity` `tcpclient` `tcpserver` `local_socket` `ipc_protocol`(h) `http_codec` `http_server` |
| `src/ui/`   | Logging, console, command handling | `logger` `logger_ncurses`(c) `console` `command_lines` `command_match` `command_history` `line_edit` |
| `src/app/`  | Entry point, client/server roles, signals, globals, GPU | `main`(c) `etii_client` `etii_server` `etii_control` `control_registry` `known_clients_registry` `client_config` `server_config` `fork_gate` `fork_orchestrator` `app_runtime` `etii_statistic`(h) `app_static_variables` `gpu_pruner`(.cu/.h) |

**Static/global state is split by domain, not bundled in one file.** `src/core/core_static_variables.{h,c}` holds the state that `src/core/` itself needs (puzzle geometry, forward-check counters, the `request`/pause state machine, search/pruner counters) — verified by grep against actual `core/` usage, not reconstituted from memory. `src/app/app_static_variables.{h,c}` holds everything else (CLI options, client identity, HTTP admin, server expansion/rebalance/lease/RAM-cap config, benchmarks). This exists to stop `core/` depending on `app/` for symbols that have nothing applicative about them — a violation the single `static_variables.h` used to force on every file under `core/` that touched puzzle geometry. `core/datamanager.c` and `core/etii_search.c` remain documented exceptions: they read genuinely applicative state directly (protocol `version`, `SERVER_PORT`, `pruner_mode`, `g_client_identity_template`, expansion/lease config) and therefore include both headers — see the note at the top of `core/core_static_variables.h` for the full accounting. **Naming rule: any file holding this kind of global/static state must keep `static_variables` in its name**, prefixed by its domain (`core_`/`app_`) — do not reintroduce a bare `static_variables.{h,c}` or scatter globals under unrelated names.

Other top-level dirs: `data/` (puzzle definitions), `build/` (compilation objects, mirrors `src/`, gitignored), `tests/` (unit tests, mirrors `src/`). **Adding a `.c` means dropping it under the right `src/<domain>/` and adding its `build/<domain>/<name>.o` to the `OBJS` list (and to `add_executable` in `CMakeLists.txt`).** Full diagram and module-by-module responsibility table: [docs/architecture.md](docs/architecture.md).

## Build Commands

```sh
make                          # Release build → ./eternityII
make DEBUG=1                  # Debug build (keeps .o files, adds -g)
make NCURSES=1                # Build with ncurses UI (links -lncurses, replaces logger.c with logger_ncurses.c)
make CUDA=1                   # Build with the GPU pruner (nvcc kernel, see docs/pruner_gpu_cuda.md)
make clean                    # Remove all build artifacts
make test                     # Unit-test suite (tests/) + bench shell tests
make test-integration         # End-to-end client/server scenarios on the 16-piece puzzle
make test-docker               # Replay CI (WERROR, ASan, integration) in 3 parallel Linux/gcc containers
make coverage / coverage-report
```

Darwin auto-links OpenCL with `-framework OpenCL` (currently unused — commented out in the link step). Full target list, `CC`/`CPPFLAGS` overrides, debug flags and puzzle-size configuration (`ETERN_PARTS`, `FORWARD_CHECK_K`): [docs/compilation.md](docs/compilation.md).

## Running the Program

```sh
./eternityII server [nb_threads] [options…] [data/pieces.csv]
./eternityII client [server_host] [nb_threads] [max_stock_per_thread] [options…] [data/pieces.csv]
./eternityII pruner [--gpu] [server_host] [nb_threads] [data/pieces.csv] [batch_size]
./eternityII test [data/pieces.csv]     # self-contained, no server needed
./eternityII --help | help [topic]      # position-independent, case-insensitive topics
```

Full option reference per mode — `--expand-level`, `--stock-max-ram`, `--stock-spill-dir`, `--stock-files`, `--rebalance-budget`, `--tcp-timeout`, `--http-port`, `--http-token-file`, `--name`, `--machine-uid-file`, `--config-file`, `--stop-on-solution`, `--headless` — is in [docs/utilisation.md](docs/utilisation.md).

**CLI help system**: single source of truth is the `cli_topics[]` table in `src/app/app_runtime.c` — it feeds general help, per-topic help, and the invalid-arguments message alike. **Adding a mode or a global option ⇒ add its entry to that table.**

**Pre-fork resolution invariant**: `--stop-on-solution`, `--name`, `--machine-uid-file` and `--config-file` are all parsed/resolved once in `main()`/`handle_client()` **before any `fork()`**, so every forked search worker inherits the same value via copy-on-write. Client config priority is always **CLI > `--config-file` > defaults** (`client_config_apply_to_globals`, `src/app/client_config.c`).

The server has its own config-file mechanism, `src/app/server_config.{h,c}`, covering every server startup option (nb_threads/parts_file plus `--expand-level`, `--expand-max-stock`, `--expand-max-levels`, `--http-port`, `--http-token-file`, `--stock-files`, `--stock-max-ram`, `--stock-spill-dir`, `--rebalance-budget`, `--tcp-timeout`, `--auto-roles`, `--stop-on-solution`, `--headless`) with the same **CLI > `--config-file` > defaults** priority — same `--config-file` flag as the client (only one mode runs per process), but its own default path (`./eternityii-server.conf`, vs. `./eternityii-client.conf`) and key set. Unlike the client, the server has no deferred-start orchestrator: the file is read once, synchronously, in `main()`/`handle_server()` before the server starts — no `configApply` (no staged config to apply hot). `config`/`configSave` DO work server-side: both interpreters branch on `server` and act on `server_config_t` (display/persist the server's own effective config) instead of `client_config_t` — only `config <clé> <valeur>` (staging a change) is rejected there, since the server has nothing to stage it into. `start`/`stopForks`/`configApply` remain client-only (cf. `command_scope_classify`, `src/ui/command_lines.c`). Four keys (`http_port`, `http_token_file`, `stock_files`, `stock_max_ram`) must be applied by `server_config_apply_pre_dispatch` in `main()` **before** `parse_cli_options`'s unconditional post-processing (`http_token_load`, `datamanager_configure_stock_files`/`_configure_ram_limit`) — applying them later (e.g. from inside `handle_server`) would be too late.

## Deferred-start orchestrator

Client/pruner processes don't fork their search workers immediately: a small pure state machine (`orchestrator_step`, `src/app/fork_orchestrator.{h,c}`) either counts down 5s to auto-start (if a config file was found) or waits for a console `start`. Console commands `config`/`configSave`/`start`/`stopForks`/`configApply`, hot vs. restart-requiring config keys, and remote piloting via `clientsCommand --to` are documented in [docs/console.md](docs/console.md) and [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md#pilotage-à-distance-du-cycle-de-vie-des-fils).

**Fork-safety invariants** — each one below was violated once in production and is now load-bearing; see `docs/investigations/` and `docs/echanges_client_serveur.md` for the full diagnoses:

- **No parent thread may run during a `fork()` call**, other than the thread calling it — a thread holding a stdio/logger lock at that instant hands the locked state to a child with no thread able to release it. `src/app/fork_gate.{h,c}` provides cooperative quiescence (`fork_gate_checkpoint`/`_mark_blocked`/`_request_quiesce`) so other parent threads can keep running *between* forks, as long as they park (or declare themselves blocked-in-a-safe-syscall) first.
- **Never call `fork_gate_release_quiesce()` from the child branch** after `fork()` — the child can inherit a torn condvar snapshot mid-transition and hang forever in `pthread_cond_broadcast`. Only the parent releases.
- **Never `flockfile(stdout)`/`flockfile(stderr)` around a `fork()`** — the lock "owner" doesn't survive the fork on macOS; the child deadlocks on its own first log call. Use the plain, non-owner-tracked `logger_lock_output` mutex instead.
- **`fflush(NULL)` can deadlock** if a console thread is mid-`fgetc()` (it holds `stdin`'s stdio lock) — flush only `stdout`/`stderr` explicitly.
- **A forked child inherits the parent's `atexit()` chain**, including ncurses/ANSI terminal teardown — call `status_zone_disown_child()` as the very first statement in a freshly-forked child, or the child's own `exit()` corrupts the shared terminal.
- **A forked child must never delete its parent's Unix socket file.** `build_udp_local_socket` registers each bound path for `atexit` removal (`local_socket_cleanup_owned`, `src/net/local_socket.c`), and a child inherits both that table and the `atexit` chain — only the owner pid recorded at registration keeps it from unlinking `etii_main.<pid>` out from under a live parent. Without the cleanup, every run left an orphan socket in the working directory: invisible to `git status` (git doesn't track special files) and fatal to `make test-docker`'s copy step, which now skips special files rather than trusting a clean directory.
- **Never set `SA_RESTART` on `SIGINT` in a child** (`configure_child_signals`) — blocking calls (`recvfrom`, `connect`) must return `EINTR` so shutdown can interrupt them.

## Server load management

Eight PRs (all shipped) fixing a real production incident: an unbounded lock held for the duration of a multi-GB backup starved every client past its TCP timeout. Full narrative, measurements and two real-incident diagnoses: [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md#gestion-de-charge). User-facing options: [docs/utilisation.md](docs/utilisation.md). Console commands: [docs/console.md](docs/console.md).

- **PR1** — bounded locks: `scroll_from_pool`/`put_to_pool`/`add_possibility_analysed_impl`/`remove_possibility_analysed` give up after a bounded wait instead of spinning forever; `--tcp-timeout <n>` overrides the default 10s work-socket timeout.
- **PR2** — `consistent_backup`: a true point-in-time snapshot via a global freeze then progressive per-file release (deliberately **not** a `fork()`/COW snapshot — too much RAM for a multi-GB stock).
- **PR3** — incremental rebalance (`datamanager_rebalance_step`, `--rebalance-budget <n>`, console `rebalance [n]`): moves packets fullest-file → emptiest-file so PR2's "≤1s per file" holds.
- **PR4** — `--stock-files <n>`: configurable file count via pointer-array storage instead of a fixed static array.
- **PR5** — needless-save avoidance: `should_autobackup` is gated per artefact (stock, best-board, known-clients) instead of one flag governing all three.
- **PR6 / PR8** — round-robin distribution: ADD/GET (PR6) and the analysed-pool ack path (PR8) no longer concentrate all traffic on file 0; PR8 keys the analysed pool by a per-connection stable hint (`compteur % nb_file_possibility`) so acknowledgement stays close to O(1).
- **Two real-incident fixes**, found only by reproducing at the exact reported scale (14M+ possibilities): a double-acknowledgement bug (`get_last_possibility` now reports whether a batch actually came `from_server`, so a locally-recycled possibility is never queued for a spurious ack) and a real heap out-of-bounds in `feed_thread_aposs` (looped over `NB_THREADS`, the *fork count*, indexing a single-element allocation).
- **Epilogue**: `INST_ERROR` from a server busy with its own backup is expected and non-fatal — logged at `log_info`, never `log_error`/board-dump. Any *other* ack value stays a loud `log_error`.

**Coding rule**: `core/` must never depend on `app/`. Where server-only logic (e.g. control-registry liveness) is needed from `core/datamanager.c`, it's injected as a function pointer by the caller — see `owner_alive` in `datamanager_reclaim_expired_leases`. This is the rule the `core_static_variables`/`app_static_variables` split (above) exists to uphold for *global state*; `core/datamanager.c` and `core/etii_search.c` are its known, documented exceptions — they still read applicative state (protocol version, server config) directly rather than through injection, a larger refactor left for later.

## RAM cap & disk spillover

`--stock-max-ram <mo>` bounds the two stock pools (never the analysed pool); `--stock-spill-dir <dir>` gives it a recourse — evict coldest-first to per-file disk segments, reload on demand — instead of refusing growth outright once the cap is hit. Backup/restore is coherent with spilled segments (an incremental snapshot, and a `.spillcount` sidecar that turns an incomplete restore into a loud failure instead of silent data loss). Full behaviour, the 90%/75%/25% hysteresis, and the CLI/console surface: [docs/utilisation.md](docs/utilisation.md#plafond-ram-du-stock---stock-max-ram).

**Key invariants**:
- `core/stock_spill.c` may depend on `core/datamanager.h`; the reverse is forbidden — `put_to_pool`'s hard-cap check has zero awareness spillover exists.
- Reload never destroys disk state until RAM insertion is confirmed ("peek, then commit"); eviction drains RAM first (cheap to undo) before writing to disk.
- `expand_datas_to_level` never drops a possibility on a RAM-cap refusal — it **waits**, bounded only by `REQUEST_STOP`, never by a fixed timeout: a stuck configuration should stall visibly (logged every 5s), not lose data silently.

## HTTP REST admin API

`--http-port <n>` (server-only, loopback-only `127.0.0.1`) starts a minimal, dependency-free HTTP/1.1 admin API (`src/net/http_codec.{h,c}` for parsing/JSON, `src/net/http_server.{h,c}` for the socket shell). `--http-token-file <path>` gates every state-changing command behind a Bearer token — only pure reads (`GET` routes, `clientsWork`) stay open without one. Full endpoint reference, auth model and client examples (curl, Python): [docs/api_http_rest.md](docs/api_http_rest.md).

| Endpoint | Purpose |
|---|---|
| `GET /api/v1/stats` / `/status` | Telemetry, server state |
| `GET /api/v1/clients` / `POST /api/v1/clients/stats` | Per-client stats (cached read / fire-and-forget refresh) |
| `POST /api/v1/command` | Standard + privileged console commands, whitelisted (`control_command_allowed`/`_privileged`, `src/net/control_protocol.c`) |
| `GET /api/v1/best-board` / `/known-clients` / `/stock-distribution` | Best board layout, known-machines registry, per-level stock histogram |
| `GET /api/v1/commands` | List of network-relevant commands with `scope`/`remote_class` classification |

`remote_class` (whether/how a command travels over the network — `control_command_class_t`, `src/net/control_protocol.h`) and `scope` (whether a command makes sense client-side, server-side, or both — `command_scope_classify`, `src/ui/command_lines.c`) are two independent axes, both surfaced by this endpoint.

## Testing

```sh
make test               # unit suites (tests/, greatest framework) + bench shell tests
make test-integration   # end-to-end 16-piece client/server scenarios
make test-docker         # replay CI (WERROR, ASan, integration) in 3 parallel Linux/gcc containers
make test-docker-arm     # compile-check the ARM64 cross-build
make coverage            # gcovr merged summary (256 + 16 piece passes)
make coverage-report     # Cobertura XML + HTML + Markdown
make bench-refutation    # refutation-cost bench, see docs/tests_et_ci.md
```

Suite layout, `fork_assert.h` (for testing `exit()`-calling code without killing the runner), hand-built fixtures, coverage artefacts, and both benchmark harnesses: [docs/tests_et_ci.md](docs/tests_et_ci.md) and [tests/README.md](tests/README.md).

**Reproducing a CI failure that doesn't show up locally**: if a test fails on the GitHub runner (Linux/gcc) but passes on macOS/clang, reproduce it with `make test-docker` **before** investigating further — it replays the exact CI jobs (`WERROR=1` build, unit tests, ASan, integration) in 3 parallel `ubuntu:24.04` containers (one per job, mirroring the CI's separate runners — no single-core bottleneck), and catches classes of bug invisible on macOS (stricter `-Werror` diagnostics, ASan over-reads, glibc vs libSystem). `DOCKER_TEST_CMD="make test ASAN=1"` switches back to a single container replaying just that one job. If the failure is ARM/Raspberry-Pi-specific (e.g. a `-Wformat-truncation` that only fires on aarch64), use `make test-docker-arm` instead — a cross-compiler compile+link check only, not an execution test. Full detail (including why the container runs as root and which two tests are deliberately skipped there): [docs/tests_et_ci.md](docs/tests_et_ci.md#tests-sous-linux-via-docker-make-test-docker).

**Guiding rule: add a unit test for every bug you fix and every behaviour you add**, so a past anomaly can never silently come back. When fixing a bug, first write (or extend) a test that fails on the old behaviour and passes on the fix; when adding a feature, cover its observable contract. If a piece of logic is hard to test, that's usually a sign to extract it into a small pure function rather than skip the test (e.g. `parse_cli_options`, `orchestrator_step`, `bench_should_stop`). This project has a long track record of production bugs caught exactly this way — `git log` and `docs/tests_et_ci.md` carry the running list.

## Architecture

Process/thread model, the copy-on-write shared lookup map, and parent↔child IPC: [docs/architecture.md](docs/architecture.md). Full wire protocol and control channel: [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md). Search-loop internals (memory flow, forward-check, MRV cell choice): [docs/autosearch_step.md](docs/autosearch_step.md).

### Client-Server TCP protocol, quick reference

Fixed-size `packet` structs (`instruction` byte + `possibility_packet`), reassembled via `recv_all`/`send_all` — a raw one-shot `send`/`recv` can desync the whole stream. Bumping the wire format requires bumping `VERSION` (exact-match handshake).

| Constant | Value | Meaning |
|---|---|---|
| `INST_ADD` / `INST_GET` | 1 / 2 | Client↔server possibility exchange (GET since v7: `int32` K + K packets) |
| `INST_SOLUTION` | 3 | Solution found; saved to a unique `solution_<pid>_<seq>` file on both sides |
| `INST_GET_TO_CHECK[_BATCH]` / `INST_POSSIBILITY_ANALYSED[_BATCH]` | 12–14 | Batched pruner exchange |
| `INST_NEED_WORK` | 15 | Hunger probe (v8), enables anticipatory delegation |
| `INST_CONTROL_HELLO` | 16 | Parent-only, opens the control channel (v9) |
| `INST_CLIENT_HELLO` | 17 | Per-fork declared identity (v12) |

### Control channel (v9, extended v10/v12)

A second, independent TCP connection per client **process** (opened only by the parent, never a fork) lets the server pilot a running client — pull live stats, push console commands — without touching search threads. Roles reverse: the server initiates (`CTRL_PING`/`CTRL_GET_STATS`/`CTRL_COMMAND`), the client answers. Only a whitelisted subset of console commands is remotely triggerable (`control_command_allowed`, `src/net/control_protocol.h`), checked **twice**: server-side before broadcast/target resolution, and client-side again (defense-in-depth) before `do_command_line`. Frame format, the known-clients registry, expiration leases on in-progress analyses, and remote lifecycle piloting (`start`/`stopForks`/`configApply`): [docs/echanges_client_serveur.md](docs/echanges_client_serveur.md#canal-de-contrôle-v9).

**Client liveness is judged on TWO signals, never the control channel alone** (`owner_client_alive`, `src/app/etii_server.c`): a control session registered in `control_registry` **or** at least one open work connection (`client_has_open_work_connection` — requires `socket_id != -1` *and* `has_identity`, since `has_identity` is only cleared on slot reuse). The control channel is opened by the parent and closes **before** the work forks have finished flushing, so the single-signal version reclaimed the lease of a possibility whose children the client had already pushed — parent and children both landed in the stock and the parent became the root of its own children, 28,5 % of a production stock. Diagnosis, counts and proof: [docs/investigations/bail_expire_racines_en_stock.md](docs/investigations/bail_expire_racines_en_stock.md). Symmetrically on the client side, `control_channel_keeps_serving` (`src/app/etii_control.c`) keeps an **already-open** control session alive past `REQUEST_STOP` while any work fork is still running (`count_alive_forks`) — the reconnect loop still stops at `REQUEST_STOP`, so no *new* session is opened while shutting down, and `CTRL_COMMAND` is refused during shutdown (a `start` would re-fork a dying client) while `CTRL_PING`/`CTRL_GET_STATS` keep being served. `requeue_last_sent_possibility` deliberately keeps the control-session-only criterion — it is called *by* the closing work connection, which the broader probe could still count as open. Reclaiming a lease also purges the descendants it makes redundant (`datamanager_purge_descendants_of`), keeping the root and dropping the descendant — same arbitrage as the `checkOrigin` console command. Both bulk re-injection paths (`datamanager_reclaim_expired_leases`, `restock_analysed`) go through `put_back_to_stock`, which picks the destination pool **from the packet's `checked` flag** — re-injection does not change the board, so the pruner's verification still holds; `checked` is only cleared on a packet produced by an *expansion*. Forcing the unchecked pool made the packet contradict its own flag until the next `restore` (which routes by the flag) and had it needlessly re-verified.

### Dynamic variable order (MRV)

MRV (most-constrained-first cell choice) is the **sole** search engine, for both real search and the pruner's bounded closure proof (`search_packet_backtracking_mrv`, `src/core/etii_search.c`) — the fixed-order engine it used to compete against (`search_packet_backtracking_core`, selected by the now-removed `mrv_enabled`/`pruner_dfs_mrv` flags) was deleted once the measurement came out favorable in both jobs: [docs/conception/mrv_moteur_unique.md](docs/conception/mrv_moteur_unique.md). The metric that drove the switch was refutation cost at equal CPU time (`make bench-refutation`), not throughput or `max_result` — full measurements and the (now-historical) three-engine ablation: [docs/conception/elagage_recherche.md](docs/conception/elagage_recherche.md) §4.7/§4.10.

**Four caches are maintained incrementally alongside the board and must stay in lockstep with it** — `constraints[][]` (neighbour colours, `bt_propagate_place`/`_undo`), `used[]` (64-bit mirror of the used-piece mask, `mrv_used_set`/`_clear`), `bt_frontier` (the `empty`/`constrained` bitmasks `mrv_choose_cell` enumerates, `bt_frontier_place`/`_undo`), and `cell_mask[]` (the resolved `bucket_id_mask` pointer per cell, `bt_mask_init`/`bt_mask_refresh` — it removes 54,5 index resolutions per node from the MRV scan, measured ×1,22). **Every site that writes `board.grid[cx][cy]` must update all four**, and the `bt_frontier_*` and `bt_mask_refresh` calls sit *next to* the `bt_propagate_*` call rather than inside it, so a new placement site can forget them — `DEBUG_CHECK_POSSIBILITY` builds re-derive `used`, `bt_frontier` and `cell_mask` from the board at every node and log a loud desync precisely to catch that. `bt_mask_cache_stays_in_lockstep_with_constraints` (`tests/core/test_etii_search.c`) locks the fourth one directly, and carries a counter-check — a placement without refresh must make the cache diverge — so it cannot pass if `bt_mask_refresh` ever became a no-op. `bt_frontier.nconstr` (constrained sides per cell) is not bookkeeping for the bitmasks alone: **it is also MRV's tie-break**, and its direction is measured, not derived. At equal MRV score the cell with the MOST constrained sides wins — the *opposite* of the textbook CSP degree heuristic, which loses by +4,4 % of refutation cost where this one gains −6,3 % (§4.12 of [docs/conception/elagage_recherche.md](docs/conception/elagage_recherche.md)). Flip that comparison and two tests fail — and the tie-break now lives inside a **composite key** (`key = (count << MRV_KEY_NC_BITS) | (MRV_KEY_NC_MAX - nconstr)`) whose natural ordering *is* the rule, so the whole choice reduces to one branchless comparison; the field width is locked by a compile-time check (`mrv_key_nc_fits_check`) because an `nconstr` overflowing into `count` would silently corrupt the MRV criterion itself. The frontier's bit order (`pos = x * ETERN_SIZE + y`) remains load-bearing where `nconstr` also ties: it reproduces the `for x { for y }` order of the scan it replaced. `tests/core/test_etii_search.c` locks both with an oracle comparing `mrv_choose_cell` against an independent full scan — one that derives the constrained-side count from `constraints[][]` rather than from `nconstr`, so the oracle checks `nconstr` itself too. **The engine has two code paths, chosen once per search, never per cell** (`bt_masks_complete`, `src/core/etii_search.c`): on a production map (compact index + `bucket_id_mask` with exactly `BT_MASK_WORDS` words) it takes `mrv_choose_cell_fast`/`bt_forward_check_fast`, compiled with a constant mask width and no NULL test — `bt_mask_init`/`bt_mask_refresh` substitute the shared `bt_zero_mask` for an empty bucket's NULL in that mode, while `map_bucket_id_mask`'s own NULL-for-empty contract is untouched; hand-built test maps and out-of-template maps stay on the generic, traversal-fallback versions. Both paths must give identical results — `mrv_choose_cell_fast_matches_generic_on_real_map` and `bt_forward_check_fast_same_verdict_as_generic` lock that, with the fixture's max id forced to `ETERN_PARTS` so the map is « complete » under every compiled puzzle size. `bt_frontier.nc_key` (`((7 - nconstr) << 8) | pos`, the low half of the fast scan's combined choice value) is a fifth lockstep field, written at the same three sites as `nconstr` (`bt_frontier_init`/`_constrain`/`_release`). **The fast scan is issue-bound (IPC ≈ 3,15): each µop per frontier cell costs ~0,25 cycle × 54 cells per node, and two « memory » hypotheses (inline masks, save/restore on undo) were measured and lost** — see the écarté table in [docs/autosearch_step.md](docs/autosearch_step.md) §1.3 quater before re-proposing them. **The hot loop's pruning counters (`fc_attempts`, `fc_pruned`, `fc_cells_studied`, `fc_pruned_at[]`) are written with a relaxed load + relaxed store (`fc_stat_bump`), never `__atomic_fetch_add`**: in any one process a single thread writes them (`run_mono_client` runs the search in the current thread; `NB_THREADS` counts *forks*, i.e. processes, each with its own copy aggregated later over IPC), so the locked read-modify-write was a lock taken against nobody — −5,0 % of cycles. This is not a data race: both accesses stay atomic, only whole-RMW atomicity goes, which a single writer does not need — the same contract `counters[]` has always had. **Cold paths keep `__atomic_fetch_add`** (`bt_materialize_pending`, and `possibility.c` which the console thread can reach through `removeNoNext`). Freshness is unchanged by design, and that is load-bearing: `bench_poll_and_maybe_stop` reads these counters *while* the search runs, and the bench relies on `fc_attempts = nodes + fc_pruned` holding to within 1. A deferred-publication variant (PR #260) breaks that identity by ~199 and was rejected for it — `fc_counters_are_visible_as_soon_as_the_forward_check_returns` guards against reintroducing it.

### Core data structures

- **`struct part`** (`src/core/part.h`): one puzzle piece.
- **`map_big_array`**: 4D lookup table (top/right/bottom/left face colours → matching pieces), three redundant representations — `flat` (5.06 Mo), `packed` (compact index for the forward-check hot loop, 1.27 Mo), `bucket_id_mask` (per-bucket bitmask of piece ids, 0.46 Mo — serves both MRV's `popcount` count and the forward-check's existence test, `map_mask_free_count`/`map_mask_any_free`). Built once pre-fork and shared COW across search workers. Details, and the "reconverting the placement lookup to `packed` regressed, don't repeat it" history: [docs/architecture.md](docs/architecture.md), [docs/autosearch_step.md](docs/autosearch_step.md).
- **`struct possibility_packet`** (`src/core/possibility.h`): full board state on the wire. **Has hidden compiler padding despite explicit field packing — never `memcmp`/hash the raw struct.**
- **`File`** (`src/core/lifo.h`): doubly-linked queue of possibilities. **`big_table`**: flat, dynamically-growing result buffer.

### Key module responsibilities

| File | Responsibility |
|---|---|
| `src/app/main.c` | Entry point; dispatches server/client/test; builds the shared map before forking |
| `src/app/app_runtime.c` | Signal handlers, runtime bootstrap (`init_counters`, `init_childs`, `failed_arg`) |
| `src/core/possibility.c` | Core search logic: generating/checking/stepping board possibilities |
| `src/core/best_board.c` | `best_board_t`: mutex-protected board recorder, reused at fork/client/server scope |
| `src/core/etii_search.c` | `autosearch()` — the search loop; fixed-order and MRV engines share one delegation path |
| `src/app/etii_client.c` | Client orchestration; owns `search_parts_t`/`acquire_search_parts` |
| `src/app/etii_server.c` | Server: TCP accept/distribute; also hosts control-channel sessions |
| `src/app/etii_control.c` | Client-side control channel |
| `src/app/control_registry.c` | Server-side registry of active control sessions |
| `src/app/known_clients_registry.c` | Server-side registry of known machines — cumulative, survives disconnect and restart |
| `src/app/client_config.c` | `--config-file` parsing/loading/saving (client/pruner) |
| `src/app/server_config.c` | `--config-file` parsing/loading/saving (server) |
| `src/app/fork_gate.c` | Cooperative quiescence for forking alongside live parent threads |
| `src/app/fork_orchestrator.c` | Deferred-start state machine; the real per-fork `fork()` |
| `src/net/http_codec.c` / `http_server.c` | HTTP admin API: pure parsing/JSON layer / socket + dispatch shell |
| `src/core/datamanager.c` | Mutex-protected possibility queues; backup/restore; RAM-cap enforcement |
| `src/core/stock_spill.c` | Disk spillover of the stock once `--stock-max-ram` is approached |
| `src/core/stock_rate.c` | ADD/GET stock event-rate counters, rolling 1min/1h/1day windows (console `statistic`, `GET /api/v1/stats`) |
| `src/core/part.c` | Piece rotation, map building, the compact `packed` index |
| `src/core/readdata.c` | Parses `data/pieces.csv` |
| `src/net/etii_protocol.c` | TCP send/recv helpers for work-protocol packets |
| `src/net/client_identity.c` | Declared client identity (v12): `machine_uid`/`client_uid`/label |
| `src/net/control_protocol.c` | Control-channel codec, command whitelists |
| `src/net/tcpclient.c` / `tcpserver.c` | Low-level TCP socket setup |
| `src/net/local_socket.c` | Unix domain UDP sockets for parent↔child IPC |
| `src/core/lifo.c` | `File` queue / `big_table` flat array |
| `src/ui/console.c` / `command_lines.c` | Interactive command parsing/dispatch; Levenshtein typo suggestion |
| `src/ui/command_history.c` | In-session command history (↑/↓ recall) |
| `src/ui/line_edit.c` | I/O-free line-editing core shared by both console frontends |
| `src/ui/logger.c` / `logger_ncurses.c` | Thread-safe logging — ANSI / ncurses variant |
| `src/net/ipc_protocol.h` | Parent↔child Unix socket message structs |
| `src/app/etii_statistic.h` | `client_statistics` struct sent to the parent every second |
| `src/core/core_static_variables.c` | Global state `core/` itself needs: puzzle geometry, forward-check counters, `request`/pause state machine, search/pruner counters |
| `src/app/app_static_variables.c` | Global state that's genuinely applicative: CLI options, pids, socket handles, HTTP/client-identity config |
| `src/app/gpu_pruner.cu` | CUDA batch pruner kernel (`CUDA=1` builds only) |

## Debug Flags & Puzzle Configuration

Debug traces (`DEBUG_SOCKET`, `DEBUG_THREAD`, …) and puzzle size (`ETERN_PARTS`, `FORWARD_CHECK_K`) are `#ifndef`-guarded constants in `src/core/core_static_variables.h`, overridable via `CPPFLAGS` without editing the file (e.g. `make CPPFLAGS="-DETERN_PARTS=16"`). CI compiles every combination with `WERROR=1` so conditionally-compiled code can't rot unnoticed. Full flag list and rationale: [docs/compilation.md](docs/compilation.md).
