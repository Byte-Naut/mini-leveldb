#include "db/includes/mini_kv_store.h"
#include "db/includes/coding.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
static int assertions = 0;
static fs::path executable;

static void require(bool condition, const std::string& message)
{
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

class temporary_directory
{
    fs::path previous_ = fs::current_path();
    fs::path path_;
public:
    temporary_directory()
    {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int i = 0; i < 100; ++i)
        {
            path_ = fs::temp_directory_path() / ("mini-leveldb-test-" + std::to_string(seed) + "-" + std::to_string(i));
            if (fs::create_directory(path_)) { fs::current_path(path_); return; }
        }
        throw std::runtime_error("cannot create isolated test directory");
    }
    ~temporary_directory()
    {
        std::error_code ignored;
        fs::current_path(previous_, ignored);
        fs::remove_all(path_, ignored);
    }
};

template<class Predicate>
static void wait_for(Predicate predicate, const std::string& message)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(2ms);
    }
    require(true, message);
}

struct manifest_view
{
    uint64_t next = 0;
    std::vector<std::vector<uint64_t>> levels;
};

static manifest_view read_manifest()
{
    std::ifstream in("MANIFEST", std::ios::binary);
    manifest_view result;
    uint32_t count = 0;
    if (!coding::read_raw(in, result.next) || !coding::read_raw(in, count) || count != 7) return {};
    result.levels.resize(count);
    for (auto& level : result.levels)
    {
        uint32_t files = 0;
        if (!coding::read_raw(in, files) || files > 10000) return {};
        level.resize(files);
        for (auto& id : level) if (!coding::read_raw(in, id)) return {};
    }
    return result;
}

static void force_flush(mini_kv_store& store, int round)
{
    const auto before = read_manifest().next;
    store.put("padding:" + std::to_string(round), std::string(5000, 'p'));
    store.put("marker:" + std::to_string(round), "accepted");
    wait_for([&] { return read_manifest().next > before; }, "MemTable must publish an SST");
}

static std::string wal_record(OperationType type, uint64_t seq, std::string_view key, std::string_view value)
{
    std::ostringstream out(std::ios::binary);
    coding::write_raw(out, static_cast<uint8_t>(type));
    coding::write_raw(out, seq);
    coding::write_raw(out, static_cast<uint32_t>(key.size()));
    coding::write_slice(out, key);
    coding::write_raw(out, static_cast<uint32_t>(value.size()));
    coding::write_slice(out, value);
    return out.str();
}

static void write_bytes(const std::string& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    require(static_cast<bool>(out), "write fixture " + path);
}

static void crud()
{
    mini_kv_store store;
    require(store.get("missing").empty(), "missing key");
    store.put("hello", "world");
    require(store.get("hello") == "world", "put/get");
    store.put("hello", "updated");
    require(store.get("hello") == "updated", "update");
    const std::string binary_key("k\0x", 3), binary_value("v\0z", 3);
    store.put(binary_key, binary_value);
    require(store.get(binary_key) == binary_value, "binary key/value");
    store.erase("hello");
    require(store.get("hello").empty(), "erase");
    store.erase("absent");
    require(store.get("absent").empty(), "erase missing");
}

static void tombstone()
{
    {
        mini_kv_store store;
        store.put("victim", "old");
        force_flush(store, 0);
        store.erase("victim");
        force_flush(store, 1);
        require(store.get("victim").empty(), "disk tombstone masks older SST");
        store.close();
    }
    mini_kv_store reopened;
    require(reopened.get("victim").empty(), "reopened deletion");
    reopened.put("victim", "reborn");
    require(reopened.get("victim") == "reborn", "put after deletion");
}

static void reopen()
{
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        mini_kv_store store;
        if (cycle) require(store.get("persist") == "v" + std::to_string(cycle - 1), "reopen value");
        store.put("persist", "v" + std::to_string(cycle));
        store.put("gone", "delete me");
        store.erase("gone");
        store.close();
        store.close();
        require(fs::file_size("wal.log") == 0, "successful close checkpoints WAL");
    }
    mini_kv_store store;
    require(store.get("gone").empty(), "reopen tombstone");
}

static void memtable_flush()
{
    mini_kv_store store;
    store.put("keep", "through flush");
    force_flush(store, 0);
    require(store.get("keep") == "through flush", "read flushed key");
    const auto manifest = read_manifest();
    require(!manifest.levels.empty() && !manifest.levels[0].empty(), "manifest registers L0");
}

static void compaction()
{
    {
        mini_kv_store store;
        for (int i = 0; i < 8; ++i)
        {
            store.put("key:" + std::to_string(i), "value:" + std::to_string(i));
            force_flush(store, i);
        }
        wait_for([] { auto m = read_manifest(); return m.levels.size() > 1 && !m.levels[1].empty(); }, "compaction must publish L1");
        for (int i = 0; i < 8; ++i) require(store.get("key:" + std::to_string(i)) == "value:" + std::to_string(i), "compacted value");
        store.close();
    }
    mini_kv_store store;
    for (int i = 0; i < 8; ++i) require(store.get("key:" + std::to_string(i)) == "value:" + std::to_string(i), "reopen compacted value");
}

static void legacy_duplicate_index()
{
    // Independently encode a legacy SST whose repeated key crosses blocks.
    std::ofstream out("000001.sst", std::ios::binary);
    const std::string value_new(5000, 'n'), value_old(5000, 'o');
    std::vector<uint64_t> offsets;
    for (const auto& [seq, value] : std::vector<std::pair<uint64_t, std::string>>{{2, value_new}, {1, value_old}})
    {
        offsets.push_back(static_cast<uint64_t>(out.tellp()));
        coding::write_raw(out, uint32_t{9});
        coding::write_slice(out, "k");
        coding::write_raw(out, (seq << 8) | 1);
        coding::write_raw(out, static_cast<uint32_t>(value.size()));
        coding::write_slice(out, value);
    }
    const auto index = static_cast<uint64_t>(out.tellp());
    for (auto offset : offsets)
    {
        coding::write_raw(out, uint32_t{1}); coding::write_slice(out, "k"); coding::write_raw(out, offset);
    }
    const auto bloom = static_cast<uint64_t>(out.tellp());
    coding::write_slice(out, std::string(8, static_cast<char>(0xff)));
    coding::write_raw(out, index); coding::write_raw(out, bloom); out.close();
    std::ofstream manifest("MANIFEST", std::ios::binary);
    coding::write_raw(manifest, uint64_t{2}); coding::write_raw(manifest, uint32_t{7});
    for (int i = 0; i < 7; ++i)
    {
        coding::write_raw(manifest, uint32_t(i == 0));
        if (i == 0) coding::write_raw(manifest, uint64_t{1});
    }
    manifest.close();
    mini_kv_store store;
    require(store.get("k") == value_new, "legacy duplicate sparse key chooses newest block");
}

static void multiple_versions()
{
    {
        mini_kv_store store;
        for (int i = 0; i < 30; ++i) store.put("same", "v" + std::to_string(i));
        require(store.get("same") == "v29", "active newest version");
        store.close();
    }
    {
        mini_kv_store store;
        require(store.get("same") == "v29", "SST newest version");
    }
    fs::create_directory("legacy"); fs::current_path("legacy");
    legacy_duplicate_index();
}

static void concurrent_close()
{
    std::map<std::string, std::string> admitted;
    {
        mini_kv_store store;
        std::atomic<int> accepted{0};
        std::atomic<bool> stopped{false};
        std::thread writer([&] {
            for (int i = 0; i < 10000; ++i)
            {
                try { store.put("write:" + std::to_string(i), "value"); }
                catch (const std::runtime_error&) { stopped = true; break; }
                admitted.emplace("write:" + std::to_string(i), "value");
                ++accepted;
                std::this_thread::yield();
            }
        });
        wait_for([&] { return accepted.load() >= 10; }, "writer starts before close");
        store.close();
        writer.join();
        require(accepted.load() > 0, "writes admitted before close");
        bool rejected = false;
        try { store.put("late", "value"); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "writes rejected after close");
        rejected = false;
        try { static_cast<void>(store.get("late")); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "reads rejected after close");
    }
    mini_kv_store reopened;
    for (const auto& [key, value] : admitted) require(reopened.get(key) == value, "every accepted write survives close");
}

static void wal_replay()
{
    write_bytes("wal.log", wal_record(OperationType::kValue, 8, "live", "v") +
                          wal_record(OperationType::kValue, 9, "deleted", "old") +
                          wal_record(OperationType::kDeletion, 10, "deleted", ""));
    mini_kv_store store;
    require(store.get("live") == "v", "WAL put replay");
    require(store.get("deleted").empty(), "WAL deletion replay");
    store.put("live", "new");
    require(store.get("live") == "new", "sequence advances after replay");
}

static void child(const std::string& mode)
{
    const std::string command = "\"" + executable.string() + "\" " + mode;
    require(std::system(command.c_str()) == 0, "crash writer completes accepted operations");
}

static void crash_writer(bool after_flush)
{
    // Deliberately terminate without destructors. This models process death,
    // while keeping the completed write set deterministic for the parent.
    mini_kv_store store;
    store.put("crash:value", "accepted");
    store.put("crash:deleted", "old");
    store.erase("crash:deleted");
    if (after_flush)
    {
        store.put("padding", std::string(5000, 'p'));
        store.put("active:new", "newer-than-SST");
        wait_for([] { return !read_manifest().levels.empty(); }, "child flush publishes");
        // The old implementation truncated WAL just after publication.
        std::this_thread::sleep_for(100ms);
    }
    std::_Exit(0);
}

static void crash_recovery(bool after_flush)
{
    child(after_flush ? "_crash_flush" : "_crash");
    mini_kv_store store;
    require(store.get("crash:value") == "accepted", "recover accepted crash write");
    require(store.get("crash:deleted").empty(), "recover crash deletion");
    if (after_flush) require(store.get("active:new") == "newer-than-SST", "flush cannot erase active WAL writes");
}

static void truncated_wal()
{
    const auto complete = wal_record(OperationType::kValue, 2, "complete", "kept");
    const auto tail = wal_record(OperationType::kValue, 3, "tail", "partial");
    for (size_t prefix = 1; prefix < tail.size(); ++prefix)
    {
        const auto previous = fs::current_path();
        fs::create_directory(std::to_string(prefix)); fs::current_path(std::to_string(prefix));
        write_bytes("wal.log", complete + tail.substr(0, prefix));
        {
            mini_kv_store store;
            require(store.get("complete") == "kept", "retain complete WAL prefix");
            require(store.get("tail").empty(), "ignore incomplete record");
            store.put("after", "valid");
            const auto expected = complete.size() + wal_record(OperationType::kValue, 3, "after", "valid").size();
            require(fs::file_size("wal.log") == expected, "trim partial bytes before append");
            store.close();
        }
        mini_kv_store reopened;
        require(reopened.get("after") == "valid", "appended value survives reopen");
        reopened.close();
        fs::current_path(previous);
    }
}

static void reopen_versions()
{
    {
        mini_kv_store store;
        for (int i = 0; i < 100; ++i) store.put("version", "old:" + std::to_string(i));
        store.close();
    }
    {
        mini_kv_store store;
        store.put("version", "new-after-reopen");
        for (int i = 0; i < 6; ++i) force_flush(store, i);
        wait_for([] { auto m = read_manifest(); return m.levels.size() > 1 && !m.levels[1].empty(); }, "reopen compaction path");
        require(store.get("version") == "new-after-reopen", "new seq outranks historical SST");
        store.close();
    }
    mini_kv_store store;
    require(store.get("version") == "new-after-reopen", "reopen compacted newest version");
}

static void empty_key()
{
    {
        mini_kv_store store;
        store.put("", "empty-key-value");
        for (int i = 0; i < 5; ++i) force_flush(store, i);
        require(store.get("") == "empty-key-value", "empty key survives compaction");
        store.close();
    }
    mini_kv_store store;
    require(store.get("") == "empty-key-value", "empty key reopens");
}

static void all_deleted()
{
    {
        mini_kv_store store;
        const std::string key(5000, 'k');
        for (int i = 0; i < 5; ++i) store.erase(key);
        store.close();
        const auto manifest = read_manifest();
        require(manifest.levels.size() == 7 && manifest.levels[1].empty(), "all-tombstone compaction omits empty L1");
    }
    mini_kv_store store;
    require(store.get(std::string(5000, 'k')).empty(), "reopen all deleted");
    require(store.get("unrelated").empty(), "empty-index lookup remains safe");
}

static void concurrent_compaction()
{
    mini_kv_store store;
    for (int i = 0; i < 10; ++i) store.put("stable:" + std::to_string(i), "value:" + std::to_string(i));
    force_flush(store, 0);
    std::atomic<bool> done{false}, wrong{false};
    std::atomic<int> reads{0};
    std::thread reader([&] {
        try
        {
            while (!done)
            {
                for (int i = 0; i < 10; ++i)
                {
                    if (store.get("stable:" + std::to_string(i)) != "value:" + std::to_string(i)) wrong = true;
                    ++reads;
                }
            }
        }
        catch (...) { wrong = true; }
    });
    for (int i = 1; i < 10; ++i) force_flush(store, i);
    done = true;
    reader.join();
    require(reads > 0, "reader overlaps compaction");
    require(!wrong, "no stable value disappears during compaction");
}

static void io_failure()
{
    {
        mini_kv_store store;
        store.put("recoverable", "in-WAL");
        fs::create_directory("MANIFEST.tmp");
        bool failed = false;
        try { store.close(); } catch (const std::runtime_error&) { failed = true; }
        require(failed, "manifest I/O failure propagates through close");
        require(fs::file_size("wal.log") > 0, "failed publication retains WAL");
    }
    fs::remove("MANIFEST.tmp");
    mini_kv_store recovered;
    require(recovered.get("recoverable") == "in-WAL", "recover after failed publication");
}

int main(int argc, char** argv)
{
    executable = fs::absolute(argv[0]);
    if (argc == 2 && std::string(argv[1]) == "_crash") { crash_writer(false); return 1; }
    if (argc == 2 && std::string(argv[1]) == "_crash_flush") { crash_writer(true); return 1; }
    const std::map<std::string, std::function<void()>> cases = {
        {"crud", crud}, {"tombstone", tombstone}, {"reopen", reopen}, {"memtable_flush", memtable_flush},
        {"compaction", compaction}, {"multiple_versions", multiple_versions}, {"concurrent_close", concurrent_close},
        {"wal_replay", wal_replay}, {"crash_recovery", [] { crash_recovery(false); }},
        {"flush_crash_recovery", [] { crash_recovery(true); }}, {"truncated_wal", truncated_wal},
        {"reopen_versions", reopen_versions}, {"empty_key", empty_key}, {"all_deleted", all_deleted},
        {"concurrent_compaction", concurrent_compaction}, {"io_failure", io_failure}};
    try
    {
        if (argc != 2 || !cases.contains(argv[1])) throw std::runtime_error("provide a registered test case");
        temporary_directory directory;
        cases.at(argv[1])();
        std::cout << "ASSERTIONS=" << assertions << "\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << " (assertions=" << assertions << ")\n";
        return 1;
    }
}
