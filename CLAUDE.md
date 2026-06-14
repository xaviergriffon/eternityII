# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

eternityII is a C program that attempts to solve the [Eternity II puzzle](https://en.wikipedia.org/wiki/Eternity_II_puzzle) — a 16×16 grid with 256 pieces. It uses a distributed client-server architecture to parallelise the search space across multiple processes or machines.

## Build Commands

```sh
make                          # Release build → ./eternityII
make DEBUG=1                  # Debug build (keeps .o files, adds -g)
make NCURSES=1                # Build with ncurses UI (links -lncurses, replaces logger.c with logger_ncurses.c)
make EXECUTABLE=myBinary      # Custom output name
make clean                    # Remove all build artifacts
```

The Makefile auto-detects Darwin and links OpenCL with `-framework OpenCL` instead of `-lOpenCL` (OpenCL support is currently commented out in the link step).

## Running the Program

```sh
# Start the server (distributes possibilities to clients)
./eternityII tcpserver [nb_threads] [pieces.csv]

# Start a client (does the search)
./eternityII tcpclient [server_host] [nb_threads] [max_stock_per_thread] [pieces.csv]

# Self-contained test/auto mode (no server needed)
./eternityII test [pieces.csv]
```

Default piece file: `pieces.csv` (256-piece puzzle). A smaller 16-piece variant is `pieces16.csv`.

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

### Core Data Structures

- **`struct part`** (`part.h`): one puzzle piece — `id`, `top/right/bottom/left` face colours, `rotation`.
- **`struct array_part`**: flat array of parts.
- **`map_big_array`** / **`big_array`**: 4-dimensional array indexed by `(top, right, bottom, left)` face values, used as a fast lookup map from required edge colours → matching pieces.
- **`struct possibility_packet`** (`possibility.h`): the full board state passed between client and server — current position `(x, y)`, the 16×16 grid of placed piece IDs, bitmask of used pieces (`b_faceused`), and an `alloc` counter.
- **`File`** (`lifo.h`): doubly-linked list used as a queue of `possibility_packet` objects.
- **`big_table`** (`lifo.h`): dynamically-growing flat array used as a high-performance result buffer.
- **`client_possibility_t`** (`etii_client.h`): per-search-thread context holding its queue, map, socket, and counters.

### Key Module Responsibilities

| File | Responsibility |
|---|---|
| `main.c` | Entry point; dispatches to server/client/test modes; manages fork lifecycle and signals |
| `possibility.c` | Core search logic: generating, checking, and stepping through board possibilities |
| `etii_search.c` | `autosearch()` — the inner search loop run by each thread |
| `etii_client.c` | Client orchestration: spawns search threads, manages their lifecycle |
| `etii_server.c` | Server: accepts TCP connections, distributes/collects possibilities |
| `datamanager.c` | 10 mutex-protected possibility queues; backup/restore to `.back` files |
| `part.c` | Piece rotation, map building (`prepare_map_part`), face lookups |
| `readdata.c` | Parses `pieces.csv` into `array_part` |
| `etii_protocol.c` | TCP send/recv helpers for `packet` structs |
| `tcpclient.c` / `tcpserver.c` | Low-level TCP socket setup |
| `local_socket.c` | Unix domain UDP sockets for parent↔child IPC |
| `lifo.c` | Queue (`File`) and flat array (`big_table`) data structures |
| `console.c` / `command_lines.c` | Interactive command parsing from stdin; Levenshtein-based typo suggestion for unknown commands |
| `command_history.c` | In-session command history (↑/↓ recall, 100-entry ring, dedup) |
| `logger.c` | Thread-safe `log_info/log_debug/log_error/log_console/log_event/log_status` — ANSI build |
| `logger_ncurses.c` | Ncurses variant of logger (compiled instead of `logger.c` when `NCURSES=1`); 4-pane layout: output pad, stats banner, events, input |
| `ipc_protocol.h` | Structs for parent↔child Unix socket messages (stats, log forwarding) |
| `etii_statistic.h` | `client_statistics` struct sent by child processes to parent every second |
| `static_variables.c` | All global state (counters, flags, pids, socket handles) |

## Debug Flags

Defined (and commented out) in `static_variables.h`. Uncomment before building to enable:

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

`static_variables.h` controls the puzzle size:

```c
#define ETERN_PARTS 256   // 256 pieces → 16×16 board
// or
#define ETERN_PARTS 16    // 16 pieces  → 4×4 board (use pieces16.csv)
```

Changing `ETERN_PARTS` requires a full rebuild.
