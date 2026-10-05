# Mini-LevelDB

[English](README.md) | 中文

这是一个使用 C++20 实现的教学用嵌入式 LSM-tree 键值引擎，参考 LevelDB 的架构。项目使用自己的 API 和文件格式，不提供 LevelDB API 或文件兼容性。

[![CI](https://github.com/Byte-Naut/Mini-LevelDB/actions/workflows/ci.yml/badge.svg)](https://github.com/Byte-Naut/Mini-LevelDB/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

引擎包含 WAL、跳表 MemTable、SSTable、布隆过滤器、稀疏索引、Block LRU 缓存和后台 L0 到 L1 compaction。CMake 提供独立的 `mini_leveldb` 库、实际使用引擎的 demo、回归测试和 benchmark。

## 构建与测试

需要 CMake 3.20 及以上版本，以及支持 C++20 的编译器，例如 GCC 13 或 Clang 16。支持 Linux 和使用 MinGW GCC 的原生 Windows 环境，Ninja 为可选构建工具。Windows 用户需要将编译器及其运行时 DLL 目录加入 `PATH`，并为下面的可执行程序添加 `.exe` 后缀。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/mini_leveldb_demo ./demo-data
```

demo 会检查 `put`、`get`、`erase`、显式 `close` 和重新打开，并在指定目录保存数据库文件。原有 epoll echo server 已从默认入口删除。

修改后可按目标增量构建，再选择相关测试：

```sh
cmake --build build --target mini_leveldb_tests --parallel 2
ctest --test-dir build -R 'engine.(tombstone|wal_replay|flush_crash_recovery)' --output-on-failure
ctest --test-dir build -L integration --output-on-failure
```

`BUILD_TESTING=OFF` 可以关闭测试目标，`MINI_LEVELDB_BUILD_BENCHMARK=OFF` 可以关闭基准目标；库和 demo 仍可构建。

## 使用库

通过 `add_subdirectory` 引入项目后，链接 `mini_leveldb::mini_leveldb`。也可先安装：

```sh
cmake --install build --prefix ./install
```

安装后的消费者项目使用以下配置，并在配置时将 `CMAKE_PREFIX_PATH` 指向安装目录：

```cmake
find_package(mini_leveldb CONFIG REQUIRED)
add_executable(app app.cpp)
target_link_libraries(app PRIVATE mini_leveldb::mini_leveldb)
```

导出的 target 会提供头文件目录、C++20 要求和线程依赖。

```cpp
#include "db/includes/mini_kv_store.h"

mini_kv_store store;                    // 数据库文件位于当前工作目录
store.put("hello", "world");
std::string value = store.get("hello");
store.erase("hello");
store.close();                         // 让调用方观察刷盘或元数据发布错误
```

并发调用和 `close()` 期间应保持对象存活。`close()` 等待已经进入的操作完成，排空待刷盘的表，等待后台线程退出；关闭后的操作抛出 `std::runtime_error`。成功关闭后可以再次关闭。析构函数会尝试清理；需要获知 I/O 错误时应显式调用 `close()`。

## 存储与恢复

写入先追加并刷新 WAL，再更新活跃内存表。写值和写删除标记都可以触发内存表冻结。后台线程写完 SST 并发布 MANIFEST 后，才从不可变队列移除对应表。

读取依次查询活跃表、不可变表、按发布顺序倒序排列的 L0 文件和 L1。命中 tombstone 后立即停止查询。compaction 合并全部 L0 和 L1 输入，并为相同 key 选择最大的序列号；这些层级中的旧文件全部参与合并，因此可以回收 tombstone。读取期间旧文件仍保持可用，替换完成后再删除。

打开数据库时会恢复 MANIFEST 和缓存，从 SST 恢复序列号，并回放完整 WAL 记录。WAL 尾部存在未完成记录时，先裁剪尾部再接受新写入。后台线程在数据库打开期间保留 WAL；成功关闭会刷完所有已接受的数据，发布元数据，再清空 WAL。这种策略允许长时间打开时 WAL 增长，以简化恢复顺序。

验证范围包含进程崩溃恢复。当前实现使用 C++ 流刷新，没有通过 `fsync` 或 `FlushFileBuffers` 提供断电耐久保证。崩溃测试会让子进程跳过析构并退出，然后重新打开它留下的文件。

## 验证与 CI

CTest 注册了 16 个引擎场景：CRUD、磁盘 tombstone、重开、MemTable flush、compaction、相同 key 多版本、并发关闭和写入、WAL replay、进程崩溃恢复、flush 后崩溃、WAL 不完整尾部、重开后的版本排序、空 key、全删除 compaction、compaction 期间并发读取，以及 I/O 失败后的恢复。7 个场景带有 `integration` 标签。测试使用独立临时目录并执行实际断言，不依赖 GoogleTest 或网络下载。

[GitHub Actions](https://github.com/Byte-Naut/Mini-LevelDB/actions/workflows/ci.yml) 配置了 Linux GCC、Clang 和 Clang ASan/UBSan 三组任务，每组都会构建、运行 CTest、执行 demo 和 benchmark 正确性检查、安装库，并上传测试日志。CI 徽章显示工作流状态，原有静态 build 和 Valgrind 徽章已删除。

在 Linux 下复现 sanitizer 验证：

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DMINI_LEVELDB_SANITIZERS=ON
cmake --build build-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-asan --output-on-failure
```

安装 Valgrind 后可自行运行以下命令。项目没有对未测试版本作持续零泄漏承诺：

```sh
valgrind --leak-check=full --error-exitcode=1 ./build/mini_kv_bench --correctness
```

## 复现 benchmark

源码位于 `db/bench/mini_kv_bench.cpp`，CMake target 为 `mini_kv_bench`：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --target mini_kv_bench --parallel 2
./build-release/mini_kv_bench --correctness
./build-release/mini_kv_bench --ops 8000 --keys 1000 --value-size 64 --seed 42
./build-release/mini_kv_bench --scenario mixed --read-ratio 60 --delete-ratio 10 --ops 8000 --keys 1000 --value-size 64 --seed 42
```

每个场景创建新的临时数据库，关闭后删除；固定种子的操作生成器使负载可复现。CSV 包含吞吐、各操作的 P50/P95/P99 延迟（微秒）、`get_misses` 和 `mismatches`。删除后的预期读缺失与错误返回值分别统计；值不匹配或正确性检查失败会导致非零退出码，数值参数也会验证。

延迟只测量单次引擎调用。吞吐测量操作循环，包含独立的期望值检查，不包含预热和最终关闭。发布结果时应记录源码版本、编译器、构建类型、机器和文件系统。main 中缺少旧性能表和泄漏计数对应的日志及构建输入，因此已移除这些表述。`docs/` 中现有火焰图仅作为历史示意，不能代表当前性能。

## 项目结构与边界

`db/` 包含引擎、序列化头文件和 benchmark；`mem_table/` 包含跳表和 arena 分配；`examples/` 包含 demo；`tests/` 包含 CTest 断言；`cmake/` 包含安装配置；`.github/workflows/` 包含 CI。`server/includes/wire_protocol.h` 是尚未使用的协议草案，项目不构建网络服务。

当前 API 用空字符串表示不存在、删除和已存空值。一个数据库工作目录由一个存活的 store 对象持有；多个进程或多个 store 对象同时使用同一目录不在支持范围内。store 打开期间应保持工作目录固定。目前 compaction 只做 L0 到 L1 的合并；更多层级、打开期间有界 WAL 轮换，以及更完善的文件损坏和断电处理仍待实现。
