# Mini-LevelDB

English | [中文](README_ZH.md)

An educational embedded LSM-tree key-value engine in C++20, inspired by LevelDB's architecture. This project has its own API and file formats; it does not provide LevelDB API or file compatibility.

[![CI](https://github.com/Byte-Naut/Mini-LevelDB/actions/workflows/ci.yml/badge.svg)](https://github.com/Byte-Naut/Mini-LevelDB/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

The engine combines a write-ahead log (WAL), a skip-list MemTable, SSTables, Bloom filters, sparse indexes, a block LRU cache, and background L0-to-L1 compaction. CMake builds the `mini_leveldb` library, an engine demo, regression tests, and a benchmark.

## Build and test

Use CMake 3.20 or newer and a C++20 compiler, such as GCC 13 or Clang 16. Linux and native Windows with MinGW GCC are supported. Ninja is optional. On Windows, put the compiler and its runtime DLL directory on `PATH` and use the `.exe` suffix for programs below.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/mini_leveldb_demo ./demo-data
```

The demo checks `put`, `get`, `erase`, explicit `close`, and reopening the same database. It stores files in the supplied directory. The former epoll echo executable has been removed from the default entry point.

For incremental verification, rebuild the affected target and select the relevant tests:

```sh
cmake --build build --target mini_leveldb_tests --parallel 2
ctest --test-dir build -R 'engine.(tombstone|wal_replay|flush_crash_recovery)' --output-on-failure
ctest --test-dir build -L integration --output-on-failure
```

`BUILD_TESTING=OFF` disables the test executable. `MINI_LEVELDB_BUILD_BENCHMARK=OFF` disables the benchmark. The library and demo remain available.

## Use the library

Link to `mini_leveldb::mini_leveldb` after `add_subdirectory`, or install the package:

```sh
cmake --install build --prefix ./install
```

An installed consumer can use:

```cmake
find_package(mini_leveldb CONFIG REQUIRED)
add_executable(app app.cpp)
target_link_libraries(app PRIVATE mini_leveldb::mini_leveldb)
```

Set `CMAKE_PREFIX_PATH` to the installation directory when configuring that consumer. The target carries its include paths, C++20 requirement, and thread dependency.

```cpp
#include "db/includes/mini_kv_store.h"

mini_kv_store store;                    // database files in the working directory
store.put("hello", "world");
std::string value = store.get("hello");
store.erase("hello");
store.close();                         // report flush or publication failures
```

Keep the object alive while concurrent calls and `close()` execute. `close()` waits for admitted calls, drains pending tables, joins the background worker, and rejects subsequent operations with `std::runtime_error`. Repeated successful closes are harmless. The destructor attempts cleanup; use explicit `close()` when the caller must observe an I/O error.

## Storage and recovery

Writes append and flush a WAL record before updating the active MemTable. Both puts and deletions can rotate a full table into the immutable queue. The worker writes an SST and publishes its manifest entry before removing that table from the queue.

Reads search the active table, immutable tables, L0 files in reverse publication order, and L1. A tombstone stops lookup. Compaction selects the highest sequence number for each key and merges all L0 and L1 inputs; it can discard tombstones because all older files in those levels participate. Readers retain access to the old files until replacement completes.

Opening a database rebuilds the manifest and caches, restores sequence numbers from SST records, and replays complete WAL records. Recovery trims an incomplete WAL tail before accepting more writes. The worker retains the WAL throughout the open session. A successful close flushes all admitted data, publishes metadata, and then clears the WAL. This policy trades WAL growth during a long session for straightforward recovery.

These guarantees cover the tested process-crash scenarios. The implementation uses C++ stream flushing and does not promise power-loss durability through `fsync` or `FlushFileBuffers`. The tests terminate child writers without running destructors, then reopen their files.

## Design notes

These notes explain why the code is shaped the way it is, so the source is easier to follow.

**Skip list for the MemTable.** A skip list sorts records by key and supports forward iteration, which is exactly what the SST flush needs: scan every record in order and write it out. A red-black tree would also work, but skip lists require less implementation machinery — no rotations, and insertion only needs to update a flat array of forward pointers. The level cap is 16 with a 0.25 promotion probability (`mem_table.h:73-74`), which keeps average path length at about log₄(n). Nodes are allocated from a PMR monotonic arena rather than the heap, so the entire MemTable is freed in one call when the table is discarded after a flush.

**Internal key format.** Every record stored in the skip list or an SST carries an internal key: the user key followed by a 64-bit pack value where the upper 56 bits hold the sequence number and the lower 8 bits hold the operation type (`mem_table.cpp:155`). Packing them together means a single integer comparison orders records correctly: same user key sorts newer sequence numbers first, and a put and a delete for the same key at the same sequence are distinct values. The sequence number is capped at 56 bits (`mini_kv_store.cpp:213`), giving 72 quadrillion operations before rollover.

**WAL lifecycle.** The WAL stays open for the entire database session and is never rotated mid-session. On a clean close, the engine flushes all admitted data to SSTs, publishes the manifest, and then truncates the WAL to zero. If the process crashes, recovery reads the WAL, replays every complete record into the active MemTable, and trims any incomplete tail before opening for writes (`mini_kv_store.cpp:26-61`). The trade-off is that a very long session accumulates a large WAL, but the recovery path stays simple: there is only one WAL and it either replays cleanly or gets trimmed.

**Flush threshold and SST layout.** The MemTable flushes at 4 KB (`mini_kv_store.h:38`), which is intentionally small so that small test datasets still exercise the flush and compaction paths. Each SST is written in three sequential sections: data blocks (4 KB pages, one sparse index entry per page boundary), an index block listing the first key and offset of each page, and a Bloom filter block. A 16-byte footer closes the file with the byte offsets of the index and filter blocks. The read path therefore only needs to seek twice before reaching the right data block: once to read the footer, once to jump to the target page.

**L0 scan versus L1 binary search.** L0 files can overlap — each flush produces a new file without regard for key ranges already in L0 — so a read must check every L0 file in reverse publication order (newest first) and stop at the first match or tombstone. L1 files are produced by compaction, which merges all L0 and L1 inputs and sorts the output, so no two L1 files share a key range. A read into L1 therefore uses `upper_bound` on the per-file first keys to find the one candidate file in O(log n) (`mini_kv_store.cpp:162-173`).

**Tombstones and compaction scope.** Deleting a key writes a tombstone record rather than removing existing data. The tombstone propagates from the MemTable to L0 and then to L1. It is only safe to discard a tombstone during a compaction that includes every file that could hold an older version of the same key. Because the current engine runs full L0-to-L1 compaction — all L0 files and all L1 files participate — it can always discard tombstones at that point. Compaction beyond L1 is not implemented.

**What is left out deliberately.** The engine omits checksums, `fsync` durability, snapshots, range iterators, configurable options, and bounded WAL rotation. These are the features that make a production storage engine substantially more complex. Leaving them out keeps the core LSM data flow — write to WAL, buffer in skip list, flush to SST, compact across levels — legible without the surrounding safety and tunability machinery.

## Verification and CI

CTest registers 16 engine cases: CRUD, disk tombstones, reopen, MemTable flush, compaction, multiple versions, concurrent close/write, WAL replay, process-crash recovery, crash after flush, incomplete WAL tails, versions after reopen, empty keys, all-deleted compaction, concurrent reads during compaction, and I/O failure recovery. Seven cases carry the `integration` label. Tests use independent temporary directories and require successful assertions; they work without GoogleTest or network downloads.

[GitHub Actions](https://github.com/Byte-Naut/Mini-LevelDB/actions/workflows/ci.yml) defines GCC, Clang, and Clang ASan/UBSan jobs on Linux. Each job builds, runs CTest, exercises the demo and benchmark correctness check, installs the library, and uploads test logs. The CI badge reflects workflow state. Historical static build and Valgrind badges have been removed.

To reproduce sanitizer checks on Linux:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DMINI_LEVELDB_SANITIZERS=ON
cmake --build build-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

Optional Valgrind checks require a local installation. No continuous zero-leak claim is made for untested versions:

```sh
valgrind --leak-check=full --error-exitcode=1 ./build/mini_kv_bench --correctness
```

## Reproduce benchmarks

The source is `db/bench/mini_kv_bench.cpp`, and its CMake target is `mini_kv_bench`:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --target mini_kv_bench --parallel 2
./build-release/mini_kv_bench --correctness
./build-release/mini_kv_bench --ops 8000 --keys 1000 --value-size 64 --seed 42
./build-release/mini_kv_bench --scenario mixed --read-ratio 60 --delete-ratio 10 --ops 8000 --keys 1000 --value-size 64 --seed 42
```

Every scenario starts with a fresh temporary database and removes it after closing. A seeded operation generator makes the workload reproducible. CSV output includes throughput, per-operation P50/P95/P99 latency in microseconds, `get_misses`, and `mismatches`. Expected misses after deletion are separate from incorrect values; a mismatch or failed correctness check causes a nonzero exit. Numeric arguments are validated.

Latency measures individual engine calls. Throughput measures the operation loop, including the independent expected-value checks, and excludes warm-up and final close. Record the source revision, compiler, build type, machine, and filesystem with any published result. The old performance table and unsourced leak counts have been removed because their logs and build inputs were unavailable on main. Existing flamegraph files in `docs/` are historical illustrations, not current measurements.

## Project layout and limits

`db/` contains the engine, serialization headers, and benchmark. `mem_table/` contains the skip list and arena allocation. `examples/` contains the demo; `tests/` contains the CTest assertions; `cmake/` contains the installed package configuration; `.github/workflows/` contains CI. The unused `server/includes/wire_protocol.h` remains a protocol sketch. No network service is built.

The current API returns an empty string for a missing key, deletion, or stored empty value. One live store owns a database working directory; concurrent processes or multiple store objects sharing that directory are outside the supported contract. The caller must keep that working directory fixed while a store is open. Compaction currently merges L0 into L1; additional levels, bounded WAL rotation during an open session, and stronger corruption and power-loss handling remain future work.
