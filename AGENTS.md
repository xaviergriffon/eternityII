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
| `src/app/`  | Entry point, client/server roles, globals, GPU | `main`(c) `etii_client` `etii_server` `etii_statistic`(h) `static_variables` `gpu_pruner`(.cu/.h) |

Other top-level dirs: `data/` (puzzle definitions `pieces.csv`, `pieces16.csv`), `build/` (compilation objects, mirrors `src/`, gitignored), `tests/` (unit tests). Adding a `.c` means dropping it under the right `src/<domain>/` and adding its `build/<domain>/<name>.o` to the `OBJS` list (and to `add_executable` in `CMakeLists.txt`).

## Build Commands

```sh
make                          # Release build → ./eternityII
make DEBUG=1                  # Debug build (keeps .o files, adds -g)
make NCURSES=1                # Build with ncurses UI (links -lncurses, replaces logger.c with logger_ncurses.c)
make EXECUTABLE=myBinary      # Custom output name
make clean                    # Remove all build artifacts
make test                     # Build & run the greatest unit-test suite (tests/)
make coverage                 # Run tests under gcov, print per-module line coverage
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

Puzzle definitions live in `data/`: `data/pieces.csv` (256-piece puzzle) and the 16-piece variant `data/pieces16.csv`. The code's built-in default (`parts_files` in `src/app/static_variables.c`) now points at `./data/pieces.csv` (or `./data/pieces16.csv` for the 16-piece build), so running from the repo root works without an explicit path argument.

## Testing

Unit tests live in `tests/` and use [greatest](https://github.com/silentbicycle/greatest) — a single-header C test framework vendored as `tests/greatest.h` (no external dependency). Suites are organised by domain, **mirroring `src/`** (`tests/core/`, `tests/net/`, `tests/ui/`), while the shared harness stays at the `tests/` root (`test_main.c` runner, `greatest.h`, `fork_assert.h`). Test files include production headers in the domain-qualified form (`#include "core/part.h"`, resolved via `-Isrc`) and the harness in short form (`#include "greatest.h"`, resolved via `-Itests`). The coverage report spans the **whole default build** (every `src/**/*.c` except the `NCURSES`/`CUDA` variants), so modules the tests don't exercise (`src/app/main.c`, …) show up at 0 % and the global percentage reflects the entire codebase.

```sh
make test            # compile tests/ + run; non-zero exit on failure (CI-ready)
make coverage        # same, instrumented with --coverage; prints a gcov per-module summary
make coverage-report # gcovr over those .gcda → Cobertura XML + HTML + Markdown summary
```

Conventions to keep in mind when adding or extending tests:

- **No `main.c` in the test binary.** `make test` links only the modules under test plus their transitive link deps (`src/ui/logger.c`, `src/app/static_variables.c`). The `TEST_SRCS` / `TEST_MODULES` Makefile variables (now `src/<domain>/…` paths) control this; each test file exposes a `SUITE` registered in `tests/test_main.c`.
- **Hand-built fixtures, not `pieces.csv` / `rotate_all_parts`.** Tests construct small `part` / `array_part` structs inline, so they stay independent of `ETERN_PARTS` (256 vs 16) and need no data file in the CWD. `rotate_all_parts` indexes by `i + ETERN_PARTS*r` and is only correct when `ETERN_PARTS` matches the real puzzle size — don't build fixtures through it.
- **Error paths that call `exit()` aren't tested** where the code aborts (e.g. a missing CSV in `read_parts`): greatest runs in-process, so an `exit()` would kill the whole runner. Covering those would require forking per test.
- **Coverage artifacts** (`.o/.gcno/.gcda/.gcov`) stay confined to `tests/coverage/` (gitignored, removed by `make clean`). `make coverage` instruments **every default-build module** (`COV_ALL_MODULES`) so each gets a `.gcno`, then links the test binary with only the exercised subset (`COV_LINK_MODULES` = `TEST_MODULES`); modules that are compiled but never linked/run produce no `.gcda` and report 0 %. Drill into `tests/coverage/<module>.c.gcov` (`#####` = never executed).
- **`make coverage-report`** runs [gcovr](https://gcovr.com) (a pip/pipx tool, *not* needed for plain `make coverage`) over those `.gcda`/`.gcno` to emit `tests/coverage/coverage.xml` (Cobertura, for Codecov), an HTML report under `tests/coverage/html/`, and `tests/coverage/coverage.md` (Markdown summary). After gcovr runs, `tests/coverage_by_domain.py` post-processes `coverage.md` to insert a **per-domain section** (`src/core/`, `src/net/`, `src/ui/`, `src/app/` subtotals, from gcovr's `--json-summary`) between the overall and per-file tables. On macOS it auto-passes `--gcov-executable "llvm-cov gcov"`; `COV_FILTER` `--exclude`s `tests/` so only production code is reported. The Codecov upload step pins `disable_search: true` + `plugins: noop` so the action ingests *only* this `coverage.xml` (otherwise its built-in gcov plugin would re-scan the `.gcda` and re-add the `tests/` directory).

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs the release build, `make test`, `make coverage-report`, and a compile-check of the `NCURSES=1` variant on every push and PR. The coverage results are published to **Codecov** (Cobertura `coverage.xml`; private repo → `CODECOV_TOKEN` secret required), as a **PR comment + Job Summary** (from `coverage.md` via `actions/github-script`), and as a downloadable **HTML artifact**. CUDA isn't exercised in CI (no `nvcc` on runners).

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
| `INST_GET` | 2 | Client requests a possibility from the server |
| `INST_SOLUTION` | 3 | Client found a solution |
| `INST_END` | 4 | Session end |
| `INST_CONSIDERED` | 5 | Acknowledge |
| `INST_NULL` | 6 | No possibility available |
| `INST_POSSIBILITY_ANALYSED` | 7 | Possibility already analysed |
| `INST_CHECK_VERSION` / `INST_SUPPORTED_VERSION` / `INST_UNSUPPORTED_VERSION` | 9/10/11 | Version handshake |
| `INST_GET_TO_CHECK` | 12 | Pruner requests one unchecked possibility |
| `INST_GET_TO_CHECK_BATCH` | 13 | Pruner requests up to N unchecked possibilities in one round-trip (`int32` N → `int32` K + K packets) |
| `INST_POSSIBILITY_ANALYSED_BATCH` | 14 | Pruner acks M analysed possibilities in one round-trip (`int32` M + M packets → one `INST_CONSIDERED`) |

A pruner exchanges with the server in batches of `pruner_batch_size` (configurable via the 4th `tcppruner`/`gpupruner` CLI arg, or the `prunerBatch <n>` console command, capped at `PRUNER_BATCH_MAX`), bounding its memory. `recv_all`/`send_all` (etii_protocol.c) handle the partial-transfer of multi-packet blocks. Bumping the wire format requires bumping `VERSION` (exact-match handshake).

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
| `src/app/main.c` | Entry point; dispatches to server/client/test modes; manages fork lifecycle and signals |
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

## Puzzle Configuration

`src/app/static_variables.h` controls the puzzle size:

```c
#define ETERN_PARTS 256   // 256 pieces → 16×16 board
// or
#define ETERN_PARTS 16    // 16 pieces  → 4×4 board (use data/pieces16.csv)
```

Changing `ETERN_PARTS` requires a full rebuild.
