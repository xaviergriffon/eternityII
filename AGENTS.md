# AGENTS.md

This file is the single source of project guidance for AI coding agents (Claude Code, Codex, …) working with code in this repository. `CLAUDE.md` imports it via `@AGENTS.md`, so edit only this file.

## Project Overview

eternityII is a C program that attempts to solve the [Eternity II puzzle](https://en.wikipedia.org/wiki/Eternity_II_puzzle) — a 16×16 grid with 256 pieces. It uses a distributed client-server architecture to parallelise the search space across multiple processes or machines.

## Source Layout

Sources live under `src/`, split into four domains. Includes are **explicit and domain-qualified** (e.g. `#include "core/part.h"`) and resolve via a single `-Isrc` (passed by both the Makefile and `target_include_directories(eternityII PRIVATE src)` in CMake).

| Directory | Domain | Modules |
|---|---|---|
| `src/core/` | Puzzle logic & data structures + search engine | `part` `readdata` `possibility` `best_board` `lifo` `packed`(h) `etii_search` `datamanager` |
| `src/net/`  | TCP protocol & sockets, parent↔child IPC | `etii_protocol` `control_protocol` `tcpclient` `tcpserver` `local_socket` `ipc_protocol`(h) |
| `src/ui/`   | Logging, console, command handling | `logger` `logger_ncurses`(c) `console` `command_lines` `command_match` `command_history` |
| `src/app/`  | Entry point, client/server roles, signals, globals, GPU | `main`(c) `etii_client` `etii_server` `etii_control` `control_registry` `app_runtime` `etii_statistic`(h) `static_variables` `gpu_pruner`(.cu/.h) |

Other top-level dirs: `data/` (puzzle definitions `pieces.csv`, `pieces16.csv`), `build/` (compilation objects, mirrors `src/`, gitignored), `tests/` (unit tests). Adding a `.c` means dropping it under the right `src/<domain>/` and adding its `build/<domain>/<name>.o` to the `OBJS` list (and to `add_executable` in `CMakeLists.txt`).

## Build Commands

```sh
make                          # Release build → ./eternityII
make DEBUG=1                  # Debug build (keeps .o files, adds -g)
make NCURSES=1                # Build with ncurses UI (links -lncurses, replaces logger.c with logger_ncurses.c)
make EXECUTABLE=myBinary      # Custom output name
make clean                    # Remove all build artifacts
make test                     # Build & run the greatest unit-test suite (tests/)
make test-integration         # End-to-end client/server scenarios on the 16-piece puzzle (solution round-trip + control channel)
make test-docker              # Replay the CI test jobs (WERROR build, tests, ASan, integration) in a Linux/gcc container (requires Docker)
make coverage                 # Both passes (256+16); gcovr merged text summary (requires gcovr)
make coverage-256             # 256-piece pass only; gcov per-module summary
make coverage-report          # gcovr reports: Cobertura XML + HTML + Markdown summary
```

The Makefile auto-detects Darwin and links OpenCL with `-framework OpenCL` instead of `-lOpenCL` (OpenCL support is currently commented out in the link step).

## Running the Program

```sh
# Start the server (distributes possibilities to clients)
./eternityII server [nb_threads] [data/pieces.csv]
# …with startup stock expansion (anti-starvation): pre-expand to cursor level 4
./eternityII server [nb_threads] --expand-level 4 [data/pieces.csv]
# …with the HTTP REST admin API enabled on 127.0.0.1:8080
./eternityII server [nb_threads] --http-port 8080 [data/pieces.csv]

# Start a client (does the search)
./eternityII client [server_host] [nb_threads] [max_stock_per_thread] [data/pieces.csv]

# Start a pruner client (validates unchecked possibilities, batched exchange)
./eternityII pruner [server_host] [nb_threads] [data/pieces.csv] [batch_size]
# GPU pruner (CUDA build only): same args, batch checked on the GPU
./eternityII pruner --gpu [server_host] [nb_threads] [data/pieces.csv] [batch_size]

# Self-contained test/auto mode (no server needed)
./eternityII test [data/pieces.csv]

# Built-in CLI help: general help, or per-topic detail (mode or option)
./eternityII --help          # position-independent, also -h; exits with success
./eternityII help server  # topic names are case-insensitive; leading dashes optional
```

**CLI help system** (`--help`/`-h` anywhere in argv, or the `help [topic]` mode): the single source of truth is the `cli_topics[]` table in `src/app/app_runtime.c` (mirroring the console's `commands[]` design) — general help (`format_cli_help`), per-topic help (`format_cli_help_topic`), and the invalid-arguments message (`failed_arg`) all derive from it. **Adding a mode or a global option ⇒ add its entry to that table.** An unknown `help <topic>` prints an error plus the general help and exits with failure (so a typo is never a silent success); the `--gpu` option is always listed (with a "CUDA=1 build only" note) even in non-CUDA builds, so users can discover GPU pruning exists (a non-CUDA binary given `--gpu` fails with an explicit error instead of silently falling back to CPU).

**`--stop-on-solution`** (optional, accepted in any position by any mode, stripped from argv before the positional parse): stop at the **first** solution. A search process that finds one exits; a server that receives one backs up its queues and stops. **Default (flag absent): keep going** — the search process backtracks to look for more solutions and the server stays in service so clients keep exploring. Read in `main()` *before* any fork (global `stop_on_solution`), so forked search children inherit it. Each solution is saved to a **unique** file (`./solution_<pid>_<seq>` client-side, `./solution_server_<pid>_<seq>` server-side) — multiple solutions never overwrite one another.

**`--expand-level <n>`** (optional, position-independent valued option, stripped with its value from argv before the positional parse; server-only). At startup, after seeding the genesis possibility, the server **expands its own stock** — placing a candidate piece on the next cell of each possibility (via `search_possiblity_light`) — until every possibility's cursor `alloc` reaches level `n`. This turns the lone genesis packet into thousands of distributable possibilities, curing the **startup starvation** where the first client to connect grabs the whole tree and the server has nothing to hand the others (the complement, from the client side, is the v8 `INST_NEED_WORK` anticipatory delegation). It is a pure server-side computation done before any client connects, so **client-side impact is nil**. Bounded on two axes (`src/app/static_variables.h`): `EXPAND_MAX_LEVELS` (4) caps the number of passes regardless of `n` — a *depth* guard so the server doesn't work too long — and `EXPAND_MAX_STOCK` (100000) caps the *count* between passes, the real safeguard since the branching factor is unknown and one pass can explode. Measured branching on the 256-puzzle is ≈11×/level (level 2 → ~45 possibilities, level 3 → ~495, level 4 → ~5300, level 5 → ~56000); **level 3–4 is the practical sweet spot** — enough to fill every client's local stock with reserve, in well under a second. The same routine (`expand_datas_to_level`, `src/core/datamanager.c`) is also reachable at runtime via the **`expand <n>` console command** (rebuilds the map like `removeNoNext`), useful when the distributable stock has run low mid-run.

### HTTP REST admin API

**`--http-port <n>`** (optional, position-independent valued option, stripped with its value from argv before the positional parse; server-only, `n` in `[1, 65535]`). Starts a minimal HTTP/1.1 admin API on **`127.0.0.1:<n>`** — loopback only, never `INADDR_ANY`, so it is never reachable off the machine by default (use an SSH tunnel or a reverse proxy for remote access). **Absent by default** (`HTTP_PORT` global defaults to 0): no extra socket is opened unless explicitly requested. Lets an external HTTP application (written in any language) read server telemetry and drive a few whitelisted admin actions without speaking the binary `packet`/`control_protocol` wire formats.

Implementation is hand-rolled and dependency-free by design (`src/net/http_codec.{h,c}` for the pure parsing/JSON layer, `src/net/http_server.{h,c}` for the socket/thread shell) — no HTTP or JSON library is vendored, matching the project's "compilable everywhere with minimal dependencies" goal. One connection is served at a time (sequential `accept()` loop on a single detached thread, `Connection: close`, 5s I/O timeout, 8 KiB request cap → `413` beyond that) — this is an occasional admin API, not a production web server. **No bump of `VERSION`**: the HTTP port is a completely separate listening socket from `SERVER_PORT`, so the existing binary protocol (packet/control_protocol handshake) is untouched.

Endpoints (all under `/api/v1`, JSON in/out):

| Method | Path | Body | Response |
|---|---|---|---|
| GET | `/api/v1/stats` | — | `{"shots_per_second","possibility_stock","checked_stock","analysed_stock","max_result","active_threads","pruner_checked","pruner_removed","queues":[{"file","unchecked","checked","analysed"}, …]}` |
| GET | `/api/v1/status` | — | `{"state","uptime_seconds","version","limit","max_stock_by_thread","pruner_batch"}` (`state` ∈ `running`/`admin_pause`/`regulation_pause`/`stopping`) |
| POST | `/api/v1/command` | `{"command":"limit 1000"}` | `{"result":"ok"}` (200), or an error body with 400 (missing/invalid args), 403 (not whitelisted), 404 (unknown path), or 405 (wrong method) |
| GET | `/api/v1/clients` | — | `{"clients":[{"pid","forks","mode","last_activity","stats"}, …]}` — one entry per active control-channel session (`control_registry_snapshot`, same source as the console `clients` command); `mode` ∈ `search`/`pruner`/`gpu_pruner`/`unknown`, `last_activity` is a Unix epoch (seconds); empty array if no client is connected. `stats` is `null` until a `CTRL_GET_STATS` round-trip has completed at least once for that session, otherwise `{"shots_per_second","possibility_stock","analysed_stock","max_result","pruner_checked","pruner_removed","pruner_cells_per_second","stats_time"}` (cached snapshot, `stats_time` = Unix epoch of that reply — can be stale if the client hasn't been re-polled). |
| POST | `/api/v1/clients/stats` | — | `{"result":"ok","requested":N}` — HTTP equivalent of the console `clientsStats` command: broadcasts `CTRL_GET_STATS` to the `N` active control sessions and returns immediately (fire-and-forget, like its console counterpart). Replies land asynchronously on each session's own control thread and are cached in `control_registry` (`control_registry_record_stats`); poll `GET /api/v1/clients` shortly after (sessions wake immediately on the posted command, so the round-trip is typically sub-second) to read the refreshed `stats`. |
| GET | `/api/v1/best-board` | — | `{"has_board","alloc","grid"}` — full representation (not just the piece count) of the best board known to the server (`g_server_best_board`, `src/core/best_board.h`). `has_board` is `false` until at least one board has been recorded (fresh start, no `restore`); otherwise `alloc` (piece count) and `grid` (`grid[x][y]`, `null` for an empty cell, otherwise `{"id","rotation","top","right","bottom","left"}` — the actual piece placed, its rotation, and its 4 border colours, decoded via `g_server_rotate_parts`, never the raw internal index) are populated. A **dedicated** request, deliberately absent from `/api/v1/stats` — the 256-cell grid is an order of magnitude bigger than a counter, a consumer that only cares about throughput shouldn't pay for it on every poll. Synchronous local read (no network round-trip to clients), like `/api/v1/stats`. |

`POST /api/v1/command` only accepts the same whitelist as the binary control channel's `CTRL_COMMAND` (`control_command_allowed`, `src/net/control_protocol.c`): `pause`, `resume`, `limit <n>`, `maxStockByThread <n>`, `prunerBatch <n>` — never `exit`/`restore`/`import`. Execution goes through **`admin_apply_remote_command`** (`src/ui/command_lines.c`), a `strtok_r`-based reentrant sibling of `do_command_line` — deliberately *not* `do_command_line` itself, since that function tokenizes via the process-global (non-reentrant) `strtok`, which a concurrent caller (HTTP thread, console thread, control-channel thread) could corrupt mid-parse. Like their console counterparts, the `pause`/`resume` branches also call `control_registry_broadcast_command` (not `strtok`-based, safe to call from this reentrant path) — otherwise a `pause` issued over HTTP would only flip the server's own (unused) `request` and never reach connected clients.

**`GET /api/v1/clients` and `POST /api/v1/clients/stats`** are the HTTP counterparts of the console `clients`/`clientsStats` commands (see *Control Channel* below), split across two endpoints because of an inherent async/sync mismatch: the HTTP server serves one connection at a time on a single thread and must answer within its 5s I/O timeout, but a `CTRL_GET_STATS` reply lands asynchronously on that session's own dedicated control thread — there is no way to block the HTTP thread on "wait for these N sessions to answer" without risking it hanging past its timeout if a client is slow or gone. So the read path (`GET /api/v1/clients`, via `http_clients_collect` → `control_registry_snapshot`) is a synchronous, non-blocking read of whatever `control_registry_record_stats` last cached, and the refresh path (`POST /api/v1/clients/stats`, via `control_registry_broadcast_get_stats`) is fire-and-forget, exactly like the console command it mirrors — it returns as soon as the request is queued, not once every client has replied. In practice the two are used as a pair: `POST` to trigger a refresh, then `GET` shortly after (each session's control thread wakes immediately on the posted command via its `pthread_cond_t`, so the round-trip is typically sub-second, but nothing enforces that — a slow or stalled client just leaves its cached `stats` stale rather than blocking the poller).

```sh
./eternityII server 4 --http-port 8080 data/pieces.csv
curl http://127.0.0.1:8080/api/v1/stats
curl http://127.0.0.1:8080/api/v1/status
curl http://127.0.0.1:8080/api/v1/clients
curl -X POST http://127.0.0.1:8080/api/v1/clients/stats   # -> {"result":"ok","requested":N}, then GET /clients for fresh "stats"
curl -X POST -d '{"command":"pause"}' http://127.0.0.1:8080/api/v1/command
curl -X POST -d '{"command":"exit"}'  http://127.0.0.1:8080/api/v1/command   # -> 403, refused
curl http://127.0.0.1:8080/api/v1/best-board
```

Puzzle definitions live in `data/`: `data/pieces.csv` (256-piece puzzle) and the 16-piece variant `data/pieces16.csv`. The code's built-in default (`parts_files` in `src/app/static_variables.c`) now points at `./data/pieces.csv` (or `./data/pieces16.csv` for the 16-piece build), so running from the repo root works without an explicit path argument.

## Testing

Unit tests live in `tests/` and use [greatest](https://github.com/silentbicycle/greatest) — a single-header C test framework vendored as `tests/greatest.h` (no external dependency). Suites are organised by domain, **mirroring `src/`** (`tests/core/`, `tests/net/`, `tests/ui/`), while the shared harness stays at the `tests/` root (`test_main.c` runner, `greatest.h`, `fork_assert.h`). Test files include production headers in the domain-qualified form (`#include "core/part.h"`, resolved via `-Isrc`) and the harness in short form (`#include "greatest.h"`, resolved via `-Itests`). The coverage report spans the **whole default build** (every `src/**/*.c` except the `NCURSES`/`CUDA` variants), so modules the tests don't exercise (`src/app/main.c`, …) show up at 0 % and the global percentage reflects the entire codebase.

```sh
make test            # compile tests/ + run; non-zero exit on failure (CI-ready)
make coverage        # both passes (256 + 16) + gcovr merged text summary (requires gcovr)
make coverage-256    # 256-piece pass only; prints a gcov per-module summary
make coverage-report # gcovr over those .gcda/gcno → Cobertura XML + HTML + Markdown summary
```

Beyond the unit suites, `make test-integration` compiles **one** `ETERN_PARTS=16` binary and runs it through **two** end-to-end scripts in sequence (both must pass):

- **`tests/integration/run_solution_16.sh`** exercises the real client/server work protocol: launches a server + a client **both with `--stop-on-solution`**, and checks that **both sides** observe the solution. The client solves the 4×4, reports it via `INST_SOLUTION`; the server displays it, backs up its queues (`./eternityII.back`, `./eternityII-in_analyse.back`) and **stops** — that clean termination is what makes the test deterministic. It asserts: server exited cleanly, both logs carry the solution, both `solution_*` files and the `.back` backups exist.
- **`tests/integration/run_control_channel.sh`** exercises the control channel (v9, see *Control Channel* below): launches a server + a client **without** `--stop-on-solution` (both processes stay alive regardless of solve speed — the 4×4 solves near-instantly, which would otherwise race any attempt to drive commands mid-search). It drives the SERVER's own console through a named pipe (`clientsStats`, `pause`, `resume`) and asserts the full round-trip in **both** logs: the control session registers, `clientsStats` yields an aggregated-stats line server-side, `pause`/`resume` each get acknowledged server-side (`commande distante "…" exécutée (code retour 0)`) **and** take effect client-side (`pause administrative demandée` / `levée`). It then stops both processes deterministically via their own `exit` console command (not a signal) before the safety-net `kill` trap.

Both scripts run in an isolated `mktemp -d` working directory (nothing lands in the repo) and enforce a bounded timeout (`INTEGRATION_TIMEOUT`, default 60 s **per script**) so neither can hang; `run_control_channel.sh` additionally polls for log patterns in short slices (0.2 s ticks) rather than sleeping a fixed delay, so it finishes as soon as each round-trip completes instead of always waiting out the timeout.

**Why `run_solution_16.sh`'s server needs 2 threads, not 1**: the client's PARENT process now opens, in addition to its search fork's work connection, its own control-channel connection (`INST_CONTROL_HELLO`) — and that session occupies a slot in the SAME `NB_THREADS` pool as work connections (see *Control Channel* below). With only 1 server thread, the control session and the search fork raced for the single slot; whichever lost was starved forever (`request unfulfilled: all threads busy`), and the test hung until timeout. This is a real operational implication, not just a test quirk: **any deployment must size the server's `NB_THREADS` for (concurrent work connections) + (concurrent client processes)**, not just the former — the default of 80 leaves ample headroom for normal fleets, but a server pinned to a small thread count needs to account for one extra slot per connected client machine.

**Reproducing CI failures locally: `make test-docker`.** Tests that pass on macOS/clang sometimes fail on the Linux/gcc CI (stricter `-Werror` diagnostics, ASan catching over-reads invisible on macOS, glibc vs libSystem, gcov vs llvm-cov). `make test-docker` replays the CI test jobs in a container built from `tests/docker/Dockerfile` — pinned to `ubuntu:24.04` with the same toolchain as the runner (gcc/make/gcov preinstalled equivalent via `build-essential`, gcovr via pipx, `procps` for the integration script's `pkill`). The repo is mounted **read-only** on `/src` and copied to `/work` inside the container before building, so Linux artifacts (ELF `.o`, binaries, `.gcda`) never mix with the macOS ones in the host working directory. The default command chain mirrors the CI's `test`, `test-asan` and `integration-test` jobs (`make WERROR=1 && make test && make test ASAN=1 && make test-integration`, with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` like CI); override it with `DOCKER_TEST_CMD="…"` to replay a single step (e.g. `make test-docker DOCKER_TEST_CMD="make test ASAN=1"`). This target is local-only tooling — CI itself already runs on Linux and does not use the image. When GitHub migrates `ubuntu-latest` to a newer release, bump the `FROM` line accordingly.

**Guiding rule: always try to add a unit test for every bug you fix and every behaviour you add**, so a past anomaly can never silently come back. When fixing a bug, first write (or extend) a test that fails on the old behaviour and passes on the fix; when adding a feature, cover its observable contract. If a piece of logic is hard to test, that is usually a sign to extract it into a small pure function (as was done for `parse_cli_options` in `src/app/static_variables.c`, tested in `tests/app/test_static_variables.c`; and for the signal/bootstrap helpers moved out of the unlinkable `main.c` into `src/app/app_runtime.c`, tested in `tests/app/test_app_runtime.c`) rather than to skip the test. The bugs already locked in this way: the server being told about solutions (`send_solution` local-mode guard, `tests/core/test_datamanager.c`), unique solution filenames so two solutions never overwrite (`log_solution`, `tests/core/test_possibility.c`), the `--stop-on-solution` argv parsing, the full client/server solution round-trip (`make test-integration` → `run_solution_16.sh`), the control-channel round-trip (`run_control_channel.sh`), the server thread-pool starvation caused by the control channel's extra connection under a small `NB_THREADS` (fixed by sizing `run_solution_16.sh`'s server at 2 threads instead of 1 — see the *Testing* section above for the full story), and the parent↔fork IPC datagrams silently dropped on macOS when they exceed `net.local.dgram.maxdgram` (2048 bytes — e.g. `IPC_MSG_STATS` once `FC_STAT_MAX_K` is raised to follow a high `FORWARD_CHECK_K`, which starved the client parent of all stats and made it look completely idle; fixed by sizing `SO_SNDBUF`/`SO_RCVBUF` from `ipc_max_datagram()` in `build_udp_local_socket`, locked by `build_udp_local_socket_allows_max_ipc_datagram` in `tests/net/test_local_socket.c`).

Conventions to keep in mind when adding or extending tests:

- **No `main.c` in the test binary.** `make test` links only the modules under test plus their transitive link deps (`src/ui/logger.c`, `src/app/static_variables.c`). The `TEST_SRCS` / `TEST_MODULES` Makefile variables (now `src/<domain>/…` paths) control this; each test file exposes a `SUITE` registered in `tests/test_main.c`. A brand-new test file must be added to `TEST_SRCS` **and** registered (`SUITE_EXTERN` + `RUN_SUITE`) in `tests/test_main.c` — see `tests/app/test_static_variables.c`.
- **Hand-built fixtures, not `pieces.csv` / `rotate_all_parts`.** Tests construct small `part` / `array_part` structs inline, so they stay independent of `ETERN_PARTS` (256 vs 16) and need no data file in the CWD. `rotate_all_parts` indexes by `i + ETERN_PARTS*r` and is only correct when `ETERN_PARTS` matches the real puzzle size — don't build fixtures through it.
- **Code paths that call `exit()` ARE testable via `tests/fork_assert.h`.** greatest runs in-process, so a direct `exit()` would kill the whole runner; `run_in_fork(fn, &pid)` runs `fn` in a forked child (stdout/stderr to `/dev/null`, context passed through file-static globals copied by `fork()`) and returns its exit code to assert on. Used for `save_possibility` aborting on an unwritable path and `checkIfResultFound`/`log_solution` on a complete board (`tests/core/test_possibility.c`). Genuinely unreachable abort paths (e.g. a missing CSV deep in `read_parts`) can still be left uncovered, but prefer a fork test over skipping.
- **Coverage artifacts** (`.o/.gcno/.gcda/.gcov`) are split across two directories: `tests/coverage/` (256-piece pass, produced by `make coverage-256`) and `tests/coverage-16/` (16-piece pass, produced by `make coverage-16`). Both are gitignored and removed by `make clean`. `make coverage-256` instruments **every default-build module** (`COV_ALL_MODULES`) so each gets a `.gcno`, then links the test binary with only the exercised subset (`COV_LINK_MODULES` = `TEST_MODULES`); modules that are compiled but never linked/run produce no `.gcda` and report 0 %. Drill into `tests/coverage/<module>.c.gcov` (`#####` = never executed).
- **`make coverage`** (= `coverage-256` + `coverage-16`) runs [gcovr](https://gcovr.com) (pip/pipx) with `--txt` over both directories, printing a single merged text summary — the union of lines covered by both puzzle sizes. **`make coverage-report`** goes further and emits `tests/coverage/coverage.xml` (Cobertura, for Codecov), an HTML report under `tests/coverage/html/`, and `tests/coverage/coverage.md` (Markdown summary). After gcovr runs, `tests/coverage_by_domain.py` post-processes `coverage.md` to insert a **per-domain section** (`src/core/`, `src/net/`, `src/ui/`, `src/app/` subtotals, from gcovr's `--json-summary`) between the overall and per-file tables, **and** — reading the Cobertura `coverage.xml` passed as its 3rd arg — a short note under the *Overall coverage* table explaining why **Codecov reports a lower percentage**. gcovr counts a line as covered as soon as it is executed at least once (partially-taken branch lines included); Codecov files those partial-branch lines in a separate *partial* bucket that it does **not** count as covered. The note prints the actual partial-line count and the "hits-only" equivalent (`hit / total`) — whose numerator matches Codecov's "X of Y lines covered" headline (the denominators differ slightly because Codecov applies its own Cobertura normalization on ingest). This is expected, not a misconfiguration: both tools read the same `coverage.xml`, they just classify partially-covered lines differently. On macOS it auto-passes `--gcov-executable "llvm-cov gcov"`; `COV_FILTER` `--exclude`s `tests/` so only production code is reported. The Codecov upload step pins `disable_search: true` + `plugins: noop` so the action ingests *only* this `coverage.xml` (otherwise its built-in gcov plugin would re-scan the `.gcda` and re-add the `tests/` directory).

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs the release build (`make WERROR=1`), `make test`, `make coverage-report`, and the `integration-test` job (`make test-integration`, both 16-piece end-to-end client/server scripts) on every push and PR. **Guiding rule: CI compiles every build combination of the code, each with `WERROR=1` (any warning fails the build)** — so no conditionally-compiled path can rot unnoticed. Beyond the release build, dedicated compile-check jobs cover the `NCURSES=1` variant, the `CUDA=1` variant (plus `CUDA=1 VERIFY=1`, the `-DGPU_PRUNER_VERIFY` cross-check path), a build that enables **all** the `DEBUG_*` flags of `src/app/static_variables.h` at once, and the alternative puzzle/algorithm configs `ETERN_PARTS=16` (4×4 board) and `FORWARD_CHECK_K=0` (forward-checking compiled out). All of these are driven via `CPPFLAGS` (`-D…`), so the source stays untouched — which is why `ETERN_PARTS` and `FORWARD_CHECK_K` are `#ifndef`-guarded in the header (overridable, default `256`/`6`). The CUDA toolkit is installed on the runner (`Jimver/cuda-toolkit`, network method) for **compilation only**: GitHub runners have no NVIDIA GPU, so the CUDA binary is never executed (functional validation happens on Jetson) — likewise every variant job is a compile/link check, not a run. For the CUDA jobs `WERROR=1` is enforced on both sides: the gcc-compiled C under `WITH_CUDA` (`-Werror`) **and** the `nvcc`-compiled `.cu` kernel (the Makefile adds `-Werror all-warnings` to `NVCCFLAGS`). The coverage results are published to **Codecov** (Cobertura `coverage.xml`; private repo → `CODECOV_TOKEN` secret required), as a **PR comment + Job Summary** (from `coverage.md` via `actions/github-script`), and as a downloadable **HTML artifact**.

## Architecture

### Process/Thread Model

- **Client mode**: the parent process forks `NB_THREADS` child processes. Each child runs `run_mono_client()` independently, connecting to the server over TCP.
- **Communication between parent and children**: Unix domain UDP sockets (`etii_main.<pid>` and `etii_fork.<pid>`). Children send `client_statistics` structs back to the parent every second via these sockets.
- **Statistics thread** (`run_checker`): a detached thread in each process that monitors search rates (shots/second) and reports back to the parent.
- **Console thread** (`run_console`): a detached thread reading stdin commands and dispatching to `do_command_line()`.

### Client-Server TCP Protocol (`etii_protocol`)

The protocol uses fixed-size `packet` structs containing an `instruction` byte and a `possibility_packet`. Instructions:

| Constant | Value | Meaning |
|---|---|---|
| `INST_ADD` | 1 | Client sends a possibility to the server |
| `INST_GET` | 2 | Client requests a possibility from the server (response since v7: `int32` K + K packets, K ∈ {0, 1}) |
| `INST_SOLUTION` | 3 | Client found a solution: sends the full board; the search child also saves a unique `./solution_<pid>_<seq>`. The server displays it and saves a unique `./solution_server_<pid>_<seq>`. With `--stop-on-solution` the search child exits and the server backs up its queues and **stops**; by default both keep running to look for more solutions. |
| `INST_END` | 4 | Session end |
| `INST_CONSIDERED` | 5 | Acknowledge |
| `INST_NULL` | 6 | No possibility available (legacy: no longer emitted since v7 — GET responses carry an explicit `int32` count instead) |
| `INST_POSSIBILITY_ANALYSED` | 7 | Possibility already analysed |
| `INST_CHECK_VERSION` / `INST_SUPPORTED_VERSION` / `INST_UNSUPPORTED_VERSION` | 9/10/11 | Version handshake |
| `INST_GET_TO_CHECK` | 12 | Pruner requests one unchecked possibility (response since v7: `int32` K + K packets, like `INST_GET`) |
| `INST_GET_TO_CHECK_BATCH` | 13 | Pruner requests up to N unchecked possibilities in one round-trip (`int32` N → `int32` K + K packets) |
| `INST_POSSIBILITY_ANALYSED_BATCH` | 14 | Pruner acks M analysed possibilities in one round-trip (`int32` M + M packets → one `INST_CONSIDERED`) |
| `INST_NEED_WORK` | 15 | Hunger probe (since v8): client asks how many possibilities the server would like to receive (response: `int32` N ≥ 0, `compute_server_hunger`). Sent by the client's feed thread in place of the keepalive; a positive N enables *anticipatory delegation* — busy search threads cede up to half their implicit stock (`bt_delegation_quota`) even below `max_stock_by_thread`, fixing the startup starvation where one client holds the whole tree while the server has nothing to serve. |
| `INST_CONTROL_HELLO` | 16 | Control-channel announcement (since v9): the client's PARENT process (never a search fork) sends this on a SEPARATE TCP connection, immediately after the version handshake, to switch that connection into control-channel mode — see *Control Channel* below. |

A pruner exchanges with the server in batches of `pruner_batch_size` (configurable via the 4th `pruner` CLI arg, or the `prunerBatch <n>` console command, capped at `PRUNER_BATCH_MAX`), bounding its memory. **Every** `possibility_packet` transfer (unit GET/ADD/ANALYSED paths included, since v7) goes through `recv_all`/`send_all` (etii_protocol.c), which reassemble partial TCP transfers — a raw one-shot `send()`/`recv()` of a ~520-byte packet can transfer only part of it and desynchronise the whole connection stream. Bumping the wire format requires bumping `VERSION` (exact-match handshake).

### Control Channel (v9, extended in v10)

Beyond the work protocol above (client-initiated: GET/ADD/ANALYSED/…), a **second, independent TCP connection per client process** lets the SERVER pilot a running client — request its live statistics, or push a console command (`pause`, `resume`, `limit`, …) — without touching the search threads at all.

**Who opens it, and why it costs nothing to the search.** Only the client's **PARENT** process (the one that forks the search workers, never a fork itself) opens this connection — one per client *process*, not per fork. It runs on its own detached thread (`run_control_channel`, `src/app/etii_control.c`), entirely separate from the search threads (`etii_search.c`), the work-protocol feed thread (`feed_thread_aposs`, `etii_client.c`), and the stats/console threads. The only thing the search hot loop pays is the pre-existing per-node read of the global `request` (see *Administrative pause* below) — nothing new was added to that loop for the control channel itself.

**Role reversal.** On this connection, after the version handshake, the client sends `INST_CONTROL_HELLO` (pid, fork count, mode — `control_hello_t`, `src/net/control_protocol.h`) and the roles flip: the **server becomes the initiator**. The server's session thread for that connection stops reading work-protocol instructions (`INST_GET`/`INST_ADD`/…) and instead runs `run_control_session`/`control_session_step` (`src/app/etii_server.c`), which either forwards a pending console command or sends a `CTRL_PING` keepalive, and the client answers.

**Frame format.** A small, independent, heterogeneous-payload codec (`src/net/control_protocol.{h,c}`) — deliberately NOT reusing `packet`/`possibility_packet`, which only ever carries one payload shape (and whose `packed` struct still has hidden compiler padding — never memcmp/hash it raw, never put it on the wire without going through explicit fields). Every frame is `uint8_t cmd` + `int32_t len` + `len` bytes of payload, always through `send_all`/`recv_all` (never raw `send`/`recv`, which can transfer a frame partially and desync the whole stream — the same rule as the work protocol). `ctrl_recv_frame` rejects an out-of-range `len` (`< 0` or `> CTRL_PAYLOAD_MAX` = 4000) without ever attempting to allocate an attacker/corruption-controlled size.

| `CTRL_*` command | Value | Meaning |
|---|---|---|
| `CTRL_PING` / `CTRL_ACK` | 1 / 2 | Keepalive, sent by the server when no command is pending (bounded by `tcp_timeout`, same `SO_RCVTIMEO` as the work protocol). |
| `CTRL_GET_STATS` / `CTRL_STATS` | 3 / 4 | Server asks for the client's aggregated statistics; `control_stats_t` (shots/s, possibility/analysed stock, `max_result`, pruner checked/removed, pruner cells/s — summed across `fork_statistics[]`, the same source `build_thread_queues_table` uses for the console report) comes back. `pruner_cells_per_second` is the pruner's throughput analog to `shots_per_second` — the rate of cells studied by pruning (rmnonext/forward-check), 0 outside pruner mode. |
| `CTRL_COMMAND` / `CTRL_RESULT` | 5 / 6 | Server pushes a console command line (text payload); the client executes it via `do_command_line()` (which already propagates to search forks via the existing `send_command_to_childs` IPC for commands flagged `send_to_childs = 1`) and returns an `int32` result code. |
| `CTRL_GET_BEST_BOARD` / `CTRL_BEST_BOARD` | 7 / 8 | *(v10)* Server asks for the full representation (not just the count) of the best board known to the client (its forks' aggregate, `g_client_aggregate_best_board`); reply payload = `uint8_t valid` then, if `valid`, `sizeof(struct possibility_packet)` raw bytes (same convention as the work protocol's GET/ADD: struct copied as-is on the wire, valid for a same-build round-trip). Sent by `control_session_step` right after decoding a `CTRL_STATS` reply whose `max_result` exceeds the server's own best-known record — never unconditionally. |

**Best board known (`CTRL_GET_BEST_BOARD`/`CTRL_BEST_BOARD`, v10, `src/core/best_board.h`).** Statistics (`max_result`/`control_stats_t.max_result`) only ever exposed the piece *count* at the record — never the board layout that produced it, since the backtracking board keeps mutating right after. `best_board_t` (mutex-protected, "only the first strictly-greater record wins", never overwritten by a tie) is reused at three independent scopes, none aware of the others: a search fork (`g_search_best_board`, updated in `etii_search.c`'s hot loop alongside `max_result`), the client PARENT process (`g_client_aggregate_best_board`, populated by `IPC_MSG_BEST_BOARD` — sent by a fork over the same Unix-domain socket as `IPC_MSG_STATS`, but only on a genuine local record, not every second), and the server (`g_server_best_board`, populated by its own genesis and by clients — pulled via `CTRL_GET_BEST_BOARD` the moment `control_session_step` sees a `CTRL_STATS.max_result` beat it, on the SAME connection, before returning to the ping loop). The server persists its aggregate alongside the rest of the stock (`best_board_save`/`best_board_load`, `./eternityII-best_board.back` / `./temp-best_board.back`, hooked into the same autobackup/stop-on-solution/`restore` call sites as `backup()`/`backup_analysed()`) and exposes it over `GET /api/v1/best-board` — a dedicated HTTP route, never folded into `/api/v1/stats`. That route decodes each `possibility_packet.grid[x][y]` raw index (`id + ETERN_PARTS*rotation`) into the actual piece placed — id, rotation, and its 4 border colours — via `g_server_rotate_parts` (`src/app/etii_server.c`, the same rotation table `runserver` already builds and shares with every `client_t.rotate_parts` for CSV solution export), so a consumer never has to reverse-engineer the internal indexing itself.

**Two independent whitelist checks, not one.** Only a short list of console commands can be triggered remotely (`control_command_allowed`, `src/net/control_protocol.h`): `pause`, `resume`, `limit`, `maxStockByThread`, `prunerBatch` — never `exit`, `restore`, `import`, or anything destructive. This is checked **twice**, independently: server-side in the `clientsCommand` console interpreter (refuses to even broadcast a disallowed line) and, defense-in-depth, client-side again in `control_channel_handle_frame` (`src/app/etii_control.c`) before calling `do_command_line` — the client never trusts a `CTRL_COMMAND` payload just because it arrived on this socket.

**Server-side session bookkeeping (`src/app/control_registry.c`).** A control session shares its slot in the *same* `client_t[NB_THREADS]` pool as a normal work connection (see the capacity note in *Testing* above) — there is no separate pool of sockets. What IS separate is `control_registry`: a small bounded table (`MAX_CONTROL_SESSIONS`, 64) of session state — hello info, a per-session mutex + `pthread_cond_t` guarding a bounded queue (`CONTROL_SESSION_QUEUE_CAP`, 16) of pending commands. Console commands post into this queue and signal the condvar; `control_session_step` wakes immediately instead of waiting out the next ping interval.

**Cached `CTRL_STATS` replies (`control_registry_record_stats`).** The registry also caches the last `control_stats_t` decoded for each session (plus the Unix timestamp it arrived at), reset to "no stats yet" on `register`/`unregister` so a slot reused by a different pid never leaks a stale reading. `control_session_step` writes into this cache immediately after a successful `CTRL_STATS` decode, right before the existing `log_info` line — the log line and the cache are two independent consumers of the same decoded reply, neither depends on the other. This exists purely so a synchronous reader with no way to wait on a specific session's condvar (the HTTP admin API's single accepter thread, see below) can read a client's last-known throughput without blocking on a live round-trip; `control_registry_snapshot` returns it alongside the existing hello/last-activity fields (`has_stats` flags whether it's populated).

**Console commands** (`src/ui/command_lines.c`), server-only in practical effect (`send_to_childs = 0` for the `clients*` ones — they act on the registry, not on a client's own forks; `pause`/`resume` are shared with the client role, see below):

| Command | Effect |
|---|---|
| `clients` | Lists active control sessions (pid, fork count, mode, last activity) via `control_registry_snapshot`. |
| `clientsStats` | Broadcasts `CTRL_GET_STATS` to every active session; each reply is logged (`stats client : coups/s=… stock=… …`) **and** cached in `control_registry` (`control_registry_record_stats`) — the same cache the HTTP admin API's `GET /api/v1/clients` reads, so a console `clientsStats` and an HTTP `POST /api/v1/clients/stats` are interchangeable triggers for the same underlying refresh. |
| `clientsCommand <line>` (alias `clientsCmd`) | Broadcasts `CTRL_COMMAND <line>` to every active session, **after** checking `control_command_allowed` on the first word — refused lines are never sent. |
| `pause` / `resume` | Sets/clears the LOCAL `REQUEST_ADMIN_PAUSE` **and** broadcasts `CTRL_COMMAND "pause"/"resume"` to every active control session (see below). |

**`pause`/`resume` double as the remote broadcast (former `clientsPause`/`clientsResume`, now merged in).** On a **client**, the server never opens a control session against it in the "wrong" direction, but the local admin-pause transition is what actually matters (it pauses that process's own search). On the **server**, the local transition is a no-op — `request` is only ever consulted by `autosearch()` (`src/core/etii_search.c`), which the server process never runs — so without the broadcast, `pause` on the server console would do nothing observable. `pause_interpreter`/`resume_interpreter` (`src/ui/command_lines.c`) therefore call `control_registry_broadcast_command` unconditionally, on both roles: on a client, `control_registry` is always empty (only the server populates it, via `INST_CONTROL_HELLO`), so the call is a silent no-op (`n == 0`); on a server, it actually reaches every connected client. This also benefits from **persisted desired state** (`control_registry_desired_pause_state`, `src/app/control_registry.c`): a server-side `pause` sets a registry-global "desired state" flag, and any client that connects *afterward* has `CTRL_COMMAND "pause"` pre-queued at `control_registry_register` time — so a fleet that scales up mid-pause doesn't need the command replayed. `clientsCommand pause`/`clientsCommand resume` remain available too (same whitelist, same effect on the desired state) for scripting that wants the generic path.

**Administrative pause (`REQUEST_ADMIN_PAUSE`, `src/app/static_variables.h`).** The pre-existing `REQUEST_PAUSE` belongs to the throughput regulator (`control_step`, `etii_client.c`): it's auto-lifted the instant a thread goes idle or the measured rate drops back under `max_search_by_sec`. Reusing it for a remotely-triggered pause would make it evaporate on the very next regulation tick. `REQUEST_ADMIN_PAUSE` is a distinct value that only the `pause`/`resume` console commands (locally, or remotely via a server-side `pause`/`resume` → `CTRL_COMMAND`, see above) ever set or clear — `control_step`'s strict `== REQUEST_PAUSE` comparisons never touch it. The search hot loop's existing per-node check (`request != REQUEST_CONTINUE`) already covers it for free; two small predicates, `request_is_pause()` (`REQUEST_PAUSE` or `REQUEST_ADMIN_PAUSE`) and `request_keeps_running()` (anything but `REQUEST_STOP`), keep the call sites readable without adding any new global state to watch.

### Core Data Structures

- **`struct part`** (`src/core/part.h`): one puzzle piece — `id`, `top/right/bottom/left` face colours, `rotation`.
- **`struct array_part`**: flat array of parts.
- **`map_big_array`** / **`big_array`**: 4-dimensional array indexed by `(top, right, bottom, left)` face values, used as a fast lookup map from required edge colours → matching pieces.
- **`struct possibility_packet`** (`src/core/possibility.h`): the full board state passed between client and server — current position `(x, y)`, the 16×16 grid of placed piece IDs, bitmask of used pieces (`b_faceused`), and an `alloc` counter.
- **`File`** (`src/core/lifo.h`): doubly-linked list used as a queue of `possibility_packet` objects.
- **`big_table`** (`src/core/lifo.h`): dynamically-growing flat array used as a high-performance result buffer.
- **`client_possibility_t`** (`src/app/etii_client.h`): per-search-thread context holding its queue, map, socket, and counters.

### Key Module Responsibilities

| File | Responsibility |
|---|---|
| `src/app/main.c` | Entry point; dispatches to server/client/test modes; manages fork lifecycle (signal handlers & runtime bootstrap live in `app_runtime.c`) |
| `src/app/app_runtime.c` | Process plumbing extracted from `main.c` to be unit-testable: signal handlers/installers (`signal_end_handler`, `sigchld_handler`, `init_signals`, `configure_child_signals`, `wait_child`, …) and runtime bootstrap (`init_counters`, `init_childs`, `failed_arg`) |
| `src/core/possibility.c` | Core search logic: generating, checking, and stepping through board possibilities |
| `src/core/best_board.c` | `best_board_t`: mutex-protected "first strictly-greater record wins" board recorder, reused at three scopes (fork-local, client-parent aggregate, server aggregate) — see *Control Channel* below and `GET /api/v1/best-board` |
| `src/core/etii_search.c` | `autosearch()` — the inner search loop run by each thread |
| `src/app/etii_client.c` | Client orchestration: spawns search threads, manages their lifecycle |
| `src/app/etii_server.c` | Server: accepts TCP connections, distributes/collects possibilities; also hosts `run_control_session`/`control_session_step` (control-channel sessions, see *Control Channel*) |
| `src/app/etii_control.c` | Client-side control channel: `run_control_channel` (parent-only thread, reconnect/back-off, hello, service loop) and `control_channel_handle_frame` (testable per-frame handler, defense-in-depth whitelist check) |
| `src/app/control_registry.c` | Server-side registry of active control sessions: hello info + per-session bounded command queue (mutex + `pthread_cond_t`) + cached last `CTRL_STATS` reply (`control_registry_record_stats`), independent of the `client_t` socket pool |
| `src/net/http_codec.c` | Pure HTTP/1.1 admin API layer: request parsing, route resolution, response/JSON formatting (`http_json_format_stats/status/clients/best_board`) — no socket, no allocation |
| `src/net/http_server.c` | Socket/thread shell of the admin API: `accept()` loop, per-request dispatch (`handle_http_connection`), and the `http_*_collect` functions that pull live server/registry state into the `http_codec.h` view structs |
| `src/core/datamanager.c` | 10 mutex-protected possibility queues; backup/restore to `.back` files |
| `src/core/part.c` | Piece rotation, map building (`prepare_map_part`), face lookups |
| `src/core/readdata.c` | Parses `data/pieces.csv` into `array_part` |
| `src/net/etii_protocol.c` | TCP send/recv helpers for the work-protocol `packet` structs |
| `src/net/control_protocol.c` | Codec for the control channel: `CTRL_*` framed messages (`ctrl_send_frame`/`ctrl_recv_frame`), `control_hello_t`/`control_stats_t` (de)serialisation, `control_command_allowed` whitelist |
| `src/net/tcpclient.c` / `src/net/tcpserver.c` | Low-level TCP socket setup |
| `src/net/local_socket.c` | Unix domain UDP sockets for parent↔child IPC |
| `src/core/lifo.c` | Queue (`File`) and flat array (`big_table`) data structures |
| `src/ui/console.c` / `src/ui/command_lines.c` | Interactive command parsing from stdin; Levenshtein-based typo suggestion for unknown commands. The `commands[]` table carries help metadata (category, usage, summary, details, aliases) — single source of truth for the categorized `help` / `help <topic>` output and the automatic usage recall (`CMD_ERR_USAGE`); command names are case-insensitive |
| `src/ui/command_history.c` | In-session command history (↑/↓ recall, 100-entry ring, dedup) |
| `src/ui/logger.c` | Thread-safe `log_info/log_debug/log_error/log_console/log_event/log_status` — ANSI build |
| `src/ui/logger_ncurses.c` | Ncurses variant of logger (compiled instead of `src/ui/logger.c` when `NCURSES=1`); 4-pane layout: output pad, stats banner, events, input |
| `src/net/ipc_protocol.h` | Structs for parent↔child Unix socket messages (stats, log forwarding, best-board-on-record) |
| `src/app/etii_statistic.h` | `client_statistics` struct sent by child processes to parent every second |
| `src/app/static_variables.c` | All global state (counters, flags, pids, socket handles) |
| `src/app/gpu_pruner.cu` | CUDA batch pruner kernel (compiled only with `CUDA=1`) |

## Debug Flags

Defined (and commented out) in `src/app/static_variables.h`. Uncomment before building to enable:

```c
#define DEBUG_IN_MONO_PROCESS  // forces single-process (no fork) — essential for debugger
#define DEBUG_SOCKET           // TCP connection/disconnection traces
#define DEBUG_SIGNAL           // signal handler traces
#define DEBUG_LOCAL_SOCKET     // Unix domain socket traces
#define DEBUG_THREAD           // thread creation traces
#define DEBUG_COMMANDS         // command parsing traces
#define DEBUG_CHECK_POSSIBILITY // validates possibility packets
#define DEBUG_RM_NO_NEXT       // traces rmnonext pruning
```

Locally you uncomment them; CI instead enables **all** of them at once via `CPPFLAGS` (the `debug-build` job, `make WERROR=1 CPPFLAGS="-DDEBUG_… …"`) and fails on any warning — so this normally-dead trace code can't rot when surrounding symbols change. Add a new `DEBUG_*` flag → add its `-D` to that job.

## Puzzle Configuration

`src/app/static_variables.h` controls the puzzle size:

```c
#define ETERN_PARTS 256   // 256 pieces → 16×16 board
// or
#define ETERN_PARTS 16    // 16 pieces  → 4×4 board (use data/pieces16.csv)
```

The `#define` is `#ifndef`-guarded, so you can also override it without editing the file: `make CPPFLAGS="-DETERN_PARTS=16"` (this is how CI compile-checks the 4×4 build). Same pattern for `FORWARD_CHECK_K`. Changing `ETERN_PARTS` requires a full rebuild.
