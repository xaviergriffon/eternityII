# AGENTS.md

This file is the single source of project guidance for AI coding agents (Claude Code, Codex, …) working with code in this repository. `CLAUDE.md` imports it via `@AGENTS.md`, so edit only this file.

## Project Overview

eternityII is a C program that attempts to solve the [Eternity II puzzle](https://en.wikipedia.org/wiki/Eternity_II_puzzle) — a 16×16 grid with 256 pieces. It uses a distributed client-server architecture to parallelise the search space across multiple processes or machines.

## Source Layout

Sources live under `src/`, split into four domains. Includes are **explicit and domain-qualified** (e.g. `#include "core/part.h"`) and resolve via a single `-Isrc` (passed by both the Makefile and `target_include_directories(eternityII PRIVATE src)` in CMake).

| Directory | Domain | Modules |
|---|---|---|
| `src/core/` | Puzzle logic & data structures + search engine | `part` `readdata` `possibility` `lifo` `packed`(h) `etii_search` `datamanager` |
| `src/net/`  | TCP protocol & sockets, parent↔child IPC | `etii_protocol` `tcpclient` `tcpserver` `local_socket` `ipc_protocol`(h) |
| `src/ui/`   | Logging, console, command handling | `logger` `logger_ncurses`(c) `console` `command_lines` `command_match` `command_history` |
| `src/app/`  | Entry point, client/server roles, signals, globals, GPU | `main`(c) `etii_client` `etii_server` `app_runtime` `etii_statistic`(h) `static_variables` `gpu_pruner`(.cu/.h) |

Other top-level dirs: `data/` (puzzle definitions `pieces.csv`, `pieces16.csv`), `build/` (compilation objects, mirrors `src/`, gitignored), `tests/` (unit tests). Adding a `.c` means dropping it under the right `src/<domain>/` and adding its `build/<domain>/<name>.o` to the `OBJS` list (and to `add_executable` in `CMakeLists.txt`).

## Build Commands

```sh
make                          # Release build → ./eternityII
make DEBUG=1                  # Debug build (keeps .o files, adds -g)
make NCURSES=1                # Build with ncurses UI (links -lncurses, replaces logger.c with logger_ncurses.c)
make EXECUTABLE=myBinary      # Custom output name
make clean                    # Remove all build artifacts
make test                     # Build & run the greatest unit-test suite (tests/)
make test-integration         # End-to-end client/server scenario on the 16-piece puzzle
make test-docker              # Replay the CI test jobs (WERROR build, tests, ASan, integration) in a Linux/gcc container (requires Docker)
make coverage                 # Both passes (256+16); gcovr merged text summary (requires gcovr)
make coverage-256             # 256-piece pass only; gcov per-module summary
make coverage-report          # gcovr reports: Cobertura XML + HTML + Markdown summary
```

The Makefile auto-detects Darwin and links OpenCL with `-framework OpenCL` instead of `-lOpenCL` (OpenCL support is currently commented out in the link step).

## Running the Program

```sh
# Start the server (distributes possibilities to clients)
./eternityII tcpserver [nb_threads] [data/pieces.csv]

# Start a client (does the search)
./eternityII tcpclient [server_host] [nb_threads] [max_stock_per_thread] [data/pieces.csv]

# Start a pruner client (validates unchecked possibilities, batched exchange)
./eternityII tcppruner [server_host] [nb_threads] [data/pieces.csv] [batch_size]
# GPU pruner (CUDA build only): same args, batch checked on the GPU
./eternityII gpupruner [server_host] [nb_threads] [data/pieces.csv] [batch_size]

# Self-contained test/auto mode (no server needed)
./eternityII test [data/pieces.csv]
```

**`--stop-on-solution`** (optional, accepted in any position by any mode, stripped from argv before the positional parse): stop at the **first** solution. A search process that finds one exits; a server that receives one backs up its queues and stops. **Default (flag absent): keep going** — the search process backtracks to look for more solutions and the server stays in service so clients keep exploring. Read in `main()` *before* any fork (global `stop_on_solution`), so forked search children inherit it. Each solution is saved to a **unique** file (`./solution_<pid>_<seq>` client-side, `./solution_server_<pid>_<seq>` server-side) — multiple solutions never overwrite one another.

Puzzle definitions live in `data/`: `data/pieces.csv` (256-piece puzzle) and the 16-piece variant `data/pieces16.csv`. The code's built-in default (`parts_files` in `src/app/static_variables.c`) now points at `./data/pieces.csv` (or `./data/pieces16.csv` for the 16-piece build), so running from the repo root works without an explicit path argument.

## Testing

Unit tests live in `tests/` and use [greatest](https://github.com/silentbicycle/greatest) — a single-header C test framework vendored as `tests/greatest.h` (no external dependency). Suites are organised by domain, **mirroring `src/`** (`tests/core/`, `tests/net/`, `tests/ui/`), while the shared harness stays at the `tests/` root (`test_main.c` runner, `greatest.h`, `fork_assert.h`). Test files include production headers in the domain-qualified form (`#include "core/part.h"`, resolved via `-Isrc`) and the harness in short form (`#include "greatest.h"`, resolved via `-Itests`). The coverage report spans the **whole default build** (every `src/**/*.c` except the `NCURSES`/`CUDA` variants), so modules the tests don't exercise (`src/app/main.c`, …) show up at 0 % and the global percentage reflects the entire codebase.

```sh
make test            # compile tests/ + run; non-zero exit on failure (CI-ready)
make coverage        # both passes (256 + 16) + gcovr merged text summary (requires gcovr)
make coverage-256    # 256-piece pass only; prints a gcov per-module summary
make coverage-report # gcovr over those .gcda/gcno → Cobertura XML + HTML + Markdown summary
```

Beyond the unit suites, an **end-to-end integration test** (`tests/integration/run_solution_16.sh`, driven by `make test-integration`) exercises the real client/server protocol: it compiles an `ETERN_PARTS=16` binary, launches a server + a client **both with `--stop-on-solution`**, and checks that **both sides** observe the solution. The client solves the 4×4, reports it via `INST_SOLUTION`; the server displays it, backs up its queues (`./eternityII.back`, `./eternityII-in_analyse.back`) and **stops** — that clean termination is what makes the test deterministic. The script runs in an isolated `mktemp -d` working dir (no `.back`/`solution_*` ever land in the repo) and enforces a bounded timeout (`INTEGRATION_TIMEOUT`, default 60 s) so it can never hang. It asserts: server exited cleanly, both logs carry the solution, both `solution_*` files and the `.back` backups exist.

**Reproducing CI failures locally: `make test-docker`.** Tests that pass on macOS/clang sometimes fail on the Linux/gcc CI (stricter `-Werror` diagnostics, ASan catching over-reads invisible on macOS, glibc vs libSystem, gcov vs llvm-cov). `make test-docker` replays the CI test jobs in a container built from `tests/docker/Dockerfile` — pinned to `ubuntu:24.04` with the same toolchain as the runner (gcc/make/gcov preinstalled equivalent via `build-essential`, gcovr via pipx, `procps` for the integration script's `pkill`). The repo is mounted **read-only** on `/src` and copied to `/work` inside the container before building, so Linux artifacts (ELF `.o`, binaries, `.gcda`) never mix with the macOS ones in the host working directory. The default command chain mirrors the CI's `test`, `test-asan` and `integration-test` jobs (`make WERROR=1 && make test && make test ASAN=1 && make test-integration`, with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` like CI); override it with `DOCKER_TEST_CMD="…"` to replay a single step (e.g. `make test-docker DOCKER_TEST_CMD="make test ASAN=1"`). This target is local-only tooling — CI itself already runs on Linux and does not use the image. When GitHub migrates `ubuntu-latest` to a newer release, bump the `FROM` line accordingly.

**Guiding rule: always try to add a unit test for every bug you fix and every behaviour you add**, so a past anomaly can never silently come back. When fixing a bug, first write (or extend) a test that fails on the old behaviour and passes on the fix; when adding a feature, cover its observable contract. If a piece of logic is hard to test, that is usually a sign to extract it into a small pure function (as was done for `parse_cli_options` in `src/app/static_variables.c`, tested in `tests/app/test_static_variables.c`; and for the signal/bootstrap helpers moved out of the unlinkable `main.c` into `src/app/app_runtime.c`, tested in `tests/app/test_app_runtime.c`) rather than to skip the test. The bugs already locked in this way: the server being told about solutions (`send_solution` local-mode guard, `tests/core/test_datamanager.c`), unique solution filenames so two solutions never overwrite (`log_solution`, `tests/core/test_possibility.c`), the `--stop-on-solution` argv parsing, and — end-to-end — the full client/server solution round-trip (`make test-integration`).

Conventions to keep in mind when adding or extending tests:

- **No `main.c` in the test binary.** `make test` links only the modules under test plus their transitive link deps (`src/ui/logger.c`, `src/app/static_variables.c`). The `TEST_SRCS` / `TEST_MODULES` Makefile variables (now `src/<domain>/…` paths) control this; each test file exposes a `SUITE` registered in `tests/test_main.c`. A brand-new test file must be added to `TEST_SRCS` **and** registered (`SUITE_EXTERN` + `RUN_SUITE`) in `tests/test_main.c` — see `tests/app/test_static_variables.c`.
- **Hand-built fixtures, not `pieces.csv` / `rotate_all_parts`.** Tests construct small `part` / `array_part` structs inline, so they stay independent of `ETERN_PARTS` (256 vs 16) and need no data file in the CWD. `rotate_all_parts` indexes by `i + ETERN_PARTS*r` and is only correct when `ETERN_PARTS` matches the real puzzle size — don't build fixtures through it.
- **Code paths that call `exit()` ARE testable via `tests/fork_assert.h`.** greatest runs in-process, so a direct `exit()` would kill the whole runner; `run_in_fork(fn, &pid)` runs `fn` in a forked child (stdout/stderr to `/dev/null`, context passed through file-static globals copied by `fork()`) and returns its exit code to assert on. Used for `save_possibility` aborting on an unwritable path and `checkIfResultFound`/`log_solution` on a complete board (`tests/core/test_possibility.c`). Genuinely unreachable abort paths (e.g. a missing CSV deep in `read_parts`) can still be left uncovered, but prefer a fork test over skipping.
- **Coverage artifacts** (`.o/.gcno/.gcda/.gcov`) are split across two directories: `tests/coverage/` (256-piece pass, produced by `make coverage-256`) and `tests/coverage-16/` (16-piece pass, produced by `make coverage-16`). Both are gitignored and removed by `make clean`. `make coverage-256` instruments **every default-build module** (`COV_ALL_MODULES`) so each gets a `.gcno`, then links the test binary with only the exercised subset (`COV_LINK_MODULES` = `TEST_MODULES`); modules that are compiled but never linked/run produce no `.gcda` and report 0 %. Drill into `tests/coverage/<module>.c.gcov` (`#####` = never executed).
- **`make coverage`** (= `coverage-256` + `coverage-16`) runs [gcovr](https://gcovr.com) (pip/pipx) with `--txt` over both directories, printing a single merged text summary — the union of lines covered by both puzzle sizes. **`make coverage-report`** goes further and emits `tests/coverage/coverage.xml` (Cobertura, for Codecov), an HTML report under `tests/coverage/html/`, and `tests/coverage/coverage.md` (Markdown summary). After gcovr runs, `tests/coverage_by_domain.py` post-processes `coverage.md` to insert a **per-domain section** (`src/core/`, `src/net/`, `src/ui/`, `src/app/` subtotals, from gcovr's `--json-summary`) between the overall and per-file tables, **and** — reading the Cobertura `coverage.xml` passed as its 3rd arg — a short note under the *Overall coverage* table explaining why **Codecov reports a lower percentage**. gcovr counts a line as covered as soon as it is executed at least once (partially-taken branch lines included); Codecov files those partial-branch lines in a separate *partial* bucket that it does **not** count as covered. The note prints the actual partial-line count and the "hits-only" equivalent (`hit / total`) — whose numerator matches Codecov's "X of Y lines covered" headline (the denominators differ slightly because Codecov applies its own Cobertura normalization on ingest). This is expected, not a misconfiguration: both tools read the same `coverage.xml`, they just classify partially-covered lines differently. On macOS it auto-passes `--gcov-executable "llvm-cov gcov"`; `COV_FILTER` `--exclude`s `tests/` so only production code is reported. The Codecov upload step pins `disable_search: true` + `plugins: noop` so the action ingests *only* this `coverage.xml` (otherwise its built-in gcov plugin would re-scan the `.gcda` and re-add the `tests/` directory).

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs the release build (`make WERROR=1`), `make test`, `make coverage-report`, and the `integration-test` job (`make test-integration`, the 16-piece end-to-end client/server run) on every push and PR. **Guiding rule: CI compiles every build combination of the code, each with `WERROR=1` (any warning fails the build)** — so no conditionally-compiled path can rot unnoticed. Beyond the release build, dedicated compile-check jobs cover the `NCURSES=1` variant, the `CUDA=1` variant (plus `CUDA=1 VERIFY=1`, the `-DGPU_PRUNER_VERIFY` cross-check path), a build that enables **all** the `DEBUG_*` flags of `src/app/static_variables.h` at once, and the alternative puzzle/algorithm configs `ETERN_PARTS=16` (4×4 board) and `FORWARD_CHECK_K=0` (forward-checking compiled out). All of these are driven via `CPPFLAGS` (`-D…`), so the source stays untouched — which is why `ETERN_PARTS` and `FORWARD_CHECK_K` are `#ifndef`-guarded in the header (overridable, default `256`/`6`). The CUDA toolkit is installed on the runner (`Jimver/cuda-toolkit`, network method) for **compilation only**: GitHub runners have no NVIDIA GPU, so the CUDA binary is never executed (functional validation happens on Jetson) — likewise every variant job is a compile/link check, not a run. For the CUDA jobs `WERROR=1` is enforced on both sides: the gcc-compiled C under `WITH_CUDA` (`-Werror`) **and** the `nvcc`-compiled `.cu` kernel (the Makefile adds `-Werror all-warnings` to `NVCCFLAGS`). The coverage results are published to **Codecov** (Cobertura `coverage.xml`; private repo → `CODECOV_TOKEN` secret required), as a **PR comment + Job Summary** (from `coverage.md` via `actions/github-script`), and as a downloadable **HTML artifact**.

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

A pruner exchanges with the server in batches of `pruner_batch_size` (configurable via the 4th `tcppruner`/`gpupruner` CLI arg, or the `prunerBatch <n>` console command, capped at `PRUNER_BATCH_MAX`), bounding its memory. **Every** `possibility_packet` transfer (unit GET/ADD/ANALYSED paths included, since v7) goes through `recv_all`/`send_all` (etii_protocol.c), which reassemble partial TCP transfers — a raw one-shot `send()`/`recv()` of a ~520-byte packet can transfer only part of it and desynchronise the whole connection stream. Bumping the wire format requires bumping `VERSION` (exact-match handshake).

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
| `src/core/etii_search.c` | `autosearch()` — the inner search loop run by each thread |
| `src/app/etii_client.c` | Client orchestration: spawns search threads, manages their lifecycle |
| `src/app/etii_server.c` | Server: accepts TCP connections, distributes/collects possibilities |
| `src/core/datamanager.c` | 10 mutex-protected possibility queues; backup/restore to `.back` files |
| `src/core/part.c` | Piece rotation, map building (`prepare_map_part`), face lookups |
| `src/core/readdata.c` | Parses `data/pieces.csv` into `array_part` |
| `src/net/etii_protocol.c` | TCP send/recv helpers for `packet` structs |
| `src/net/tcpclient.c` / `src/net/tcpserver.c` | Low-level TCP socket setup |
| `src/net/local_socket.c` | Unix domain UDP sockets for parent↔child IPC |
| `src/core/lifo.c` | Queue (`File`) and flat array (`big_table`) data structures |
| `src/ui/console.c` / `src/ui/command_lines.c` | Interactive command parsing from stdin; Levenshtein-based typo suggestion for unknown commands |
| `src/ui/command_history.c` | In-session command history (↑/↓ recall, 100-entry ring, dedup) |
| `src/ui/logger.c` | Thread-safe `log_info/log_debug/log_error/log_console/log_event/log_status` — ANSI build |
| `src/ui/logger_ncurses.c` | Ncurses variant of logger (compiled instead of `src/ui/logger.c` when `NCURSES=1`); 4-pane layout: output pad, stats banner, events, input |
| `src/net/ipc_protocol.h` | Structs for parent↔child Unix socket messages (stats, log forwarding) |
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
