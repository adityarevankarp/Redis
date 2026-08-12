# CrazyRedis

A small, single-file Redis-compatible server written in modern C++. It speaks
the real RESP wire protocol, keeps data in a thread-safe, bounded LRU store
with key expiry, and supports basic leader/replica replication — built as a
from-scratch learning project, not a production Redis replacement.

## Features

- **Real RESP parsing.** Commands are decoded with a proper RESP
  array/bulk-string parser that handles partial reads and pipelined
  commands, instead of splitting on fixed string offsets.
- **Thread-safe storage.** Every client is handled on its own thread; the
  key/value store is guarded by a mutex so concurrent `GET`/`SET`/`DEL`
  calls can't race.
- **Bounded LRU cache.** The store holds up to 10,000 keys by default. Once
  full, the least-recently-used key is evicted to make room for a new one.
- **Key expiry.** `SET ... PX <ms>` / `EX <sec>` attaches a TTL to a key.
  Expiry is checked lazily on access and swept periodically by a background
  reaper — no per-key timer threads.
- **Basic replication.** Start a server with `--replicaof "<host> <port>"`
  and it will perform the `PING` → `REPLCONF` → `PSYNC` handshake against a
  master and receive propagated writes.
- **Cross-platform.** Builds on Linux, macOS, and Windows (MSVC or
  MinGW-w64) from the same source, via a small POSIX/Winsock2 shim.

## Supported commands

| Command | Description |
|---|---|
| `PING` | Liveness check. |
| `ECHO <msg>` | Echoes the message back. |
| `SET <key> <value> [PX ms \| EX sec]` | Sets a key, with optional expiry. |
| `GET <key>` | Returns a key's value, or nil if missing/expired. |
| `DEL <key> [key ...]` | Deletes one or more keys. |
| `KEYS *` | Lists all non-expired keys. |
| `CONFIG GET <dir\|dbfilename>` | Returns a runtime config value. |
| `INFO replication` | Reports role, replication ID, and offset. |
| `REPLCONF`, `PSYNC` | Replication handshake commands. |

## Building

### Linux / macOS / WSL

```bash
g++ -std=c++17 -O2 -pthread -o crazyredis server.cpp
```

or with CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Windows

Using [MSYS2](https://www.msys2.org/) with the `mingw-w64-ucrt-x86_64-gcc`
toolchain:

```bash
g++ -std=c++17 -O2 -o crazyredis.exe server.cpp -lws2_32
```

Or with CMake + MSVC / Ninja:

```powershell
cmake -B build
cmake --build build --config Release
```

## Running

```bash
./crazyredis --port 6379
```

Common flags:

| Flag | Purpose |
|---|---|
| `--port <n>` | Port to listen on (default `6379`). |
| `--dir <path>`, `--dbfilename <name>` | Reported via `CONFIG GET`; checked for an existing RDB file at startup. |
| `--replicaof "<host> <port>"` | Start as a replica of the given master. |

Then, from another terminal:

```bash
redis-cli -p 6379 set foo bar
redis-cli -p 6379 get foo
```

## Design notes & known limitations

- **RDB loading is intentionally minimal.** The server checks for an
  existing dump file at startup but does not decode the RDB binary format —
  a full parser is a project of its own. It starts with an empty dataset
  rather than guessing at contents.
- **Replication propagates `SET`/`DEL` only**, and a replica does not yet
  apply the initial RDB snapshot it receives from `PSYNC`.
- **The LRU capacity (10,000 keys) is a compile-time default** rather than
  a runtime flag; bump `LruStore store{...}` in `server.cpp` if you need a
  different ceiling.

These are the honest boundaries of what this project promises — the parser,
concurrency, and expiry logic above it are all real and verified against
`redis-cli`.
