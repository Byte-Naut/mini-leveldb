// db/bench/mini_kv_bench.cpp
// Engine benchmark: throughput, per-operation latency percentiles (P50/P95/P99).
// Scenarios: write_heavy / mixed / read_heavy / mixed_with_delete
// Usage:
//   mini_kv_bench [--ops N] [--keys N] [--value-size N] [--seed N]
//                 [--read-ratio N] [--delete-ratio N]
//                 [--scenario NAME] [--correctness]

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "../includes/mini_kv_store.h"

using clock_type = std::chrono::steady_clock;
using us         = std::chrono::duration<double, std::micro>;

// Each measured workload gets a fresh database; the caller's working
// directory and any existing database files are preserved.
class benchmark_directory
{
    std::filesystem::path previous_ = std::filesystem::current_path();
    std::filesystem::path path_;
public:
    benchmark_directory()
    {
        const auto seed = clock_type::now().time_since_epoch().count();
        for (int i = 0; i < 100; ++i)
        {
            path_ = std::filesystem::temp_directory_path() /
                ("mini-leveldb-bench-" + std::to_string(seed) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(path_))
            {
                std::filesystem::current_path(path_);
                return;
            }
        }
        throw std::runtime_error("cannot create benchmark directory");
    }
    ~benchmark_directory()
    {
        std::error_code ignored;
        std::filesystem::current_path(previous_, ignored);
        std::filesystem::remove_all(path_, ignored);
    }
};

struct Scenario
{
    std::string name;
    int         total_ops;
    int         read_ratio;
    int         delete_ratio;
    int         key_count;
    int         value_size;
    uint32_t    seed;
};

struct ScenarioResult
{
    double throughput;
    double elapsed_s;
    int    success;
    int    fail;
    int    mismatches;
    double put_p50, put_p95, put_p99;
    double get_p50, get_p95, get_p99;
    double del_p50, del_p95, del_p99;
};

static double percentile(std::vector<double>& v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<size_t>(
        std::min<double>(v.size() - 1, (p / 100.0) * (v.size() - 1)));
    return v[idx];
}

static bool correctness_check()
{
    benchmark_directory directory;
    std::cout << "[correctness] PUT->GET->DEL->GET cycle...\n";
    mini_kv_store store;

    for (int i = 0; i < 200; ++i)
        store.put("ckey_" + std::to_string(i), "cval_" + std::to_string(i));

    int ok = 0, missing = 0;
    for (int i = 0; i < 200; ++i)
    {
        auto v = store.get("ckey_" + std::to_string(i));
        if (v == "cval_" + std::to_string(i)) ++ok; else ++missing;
    }
    std::cout << "[correctness] after PUT: found=" << ok
              << " missing=" << missing << "\n";

    for (int i = 0; i < 200; i += 2)
        store.erase("ckey_" + std::to_string(i));

    int tombstone = 0, alive = 0, wrong = 0;
    for (int i = 0; i < 200; ++i)
    {
        auto v = store.get("ckey_" + std::to_string(i));
        if (i % 2 == 0) { if (v.empty()) ++tombstone; else ++wrong; }
        else            { if (v == "cval_" + std::to_string(i)) ++alive; else ++wrong; }
    }
    std::cout << "[correctness] after DELETE: tombstone=" << tombstone
              << " alive=" << alive << " wrong=" << wrong << "\n";
    std::cout << "[correctness] "
              << (missing == 0 && tombstone == 100 && alive == 100 && wrong == 0 ? "PASS" : "FAIL")
              << "\n";
    store.close();
    return missing == 0 && tombstone == 100 && alive == 100 && wrong == 0;
}

static ScenarioResult run_scenario(const Scenario& s)
{
    benchmark_directory directory;
    mini_kv_store store;
    std::mt19937  rng(s.seed);
    std::uniform_int_distribution<int> key_dist(0, s.key_count - 1);
    std::uniform_int_distribution<int> op_dist(1, 100);

    std::vector<double> put_lat, get_lat, del_lat;
    put_lat.reserve(s.total_ops);
    get_lat.reserve(s.total_ops);
    del_lat.reserve(s.total_ops);
    std::map<std::string, std::string> expected;

    // warm-up: pre-populate keys so reads have something to hit
    for (int i = 0; i < s.key_count; ++i)
    {
        const auto key = "bench_key_" + std::to_string(i);
        const auto value = std::string(s.value_size, static_cast<char>('a' + (i % 26)));
        store.put(key, value);
        expected[key] = value;
    }

    int success = 0, fail = 0, mismatches = 0;
    const auto g0 = clock_type::now();

    for (int i = 0; i < s.total_ops; ++i)
    {
        const int  kid = key_dist(rng);
        std::string key = "bench_key_" + std::to_string(kid);
        const int  roll = op_dist(rng);

        if (roll <= s.read_ratio)
        {
            auto t0 = clock_type::now();
            auto v  = store.get(key);
            auto t1 = clock_type::now();
            get_lat.push_back(static_cast<double>(
                std::chrono::duration_cast<us>(t1 - t0).count()));
            if (v.empty()) ++fail; else ++success;
            const auto found = expected.find(key);
            if (v != (found == expected.end() ? std::string{} : found->second)) ++mismatches;
        }
        else if (s.delete_ratio > 0 && roll <= s.read_ratio + s.delete_ratio)
        {
            auto t0 = clock_type::now();
            store.erase(key);
            auto t1 = clock_type::now();
            del_lat.push_back(static_cast<double>(
                std::chrono::duration_cast<us>(t1 - t0).count()));
            ++success;
            expected.erase(key);
        }
        else
        {
            std::string v(s.value_size, static_cast<char>('a' + (i % 26)));
            auto t0 = clock_type::now();
            store.put(key, v);
            auto t1 = clock_type::now();
            put_lat.push_back(static_cast<double>(
                std::chrono::duration_cast<us>(t1 - t0).count()));
            ++success;
            expected[key] = v;
        }
    }

    const auto g1 = clock_type::now();
    double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(g1 - g0).count();

    ScenarioResult result{
        static_cast<double>(s.total_ops) / elapsed, elapsed, success, fail, mismatches,
        percentile(put_lat, 50), percentile(put_lat, 95), percentile(put_lat, 99),
        percentile(get_lat, 50), percentile(get_lat, 95), percentile(get_lat, 99),
        percentile(del_lat, 50), percentile(del_lat, 95), percentile(del_lat, 99),
    };
    store.close();
    return result;
}

static int parse_int(const char* v)
{
    char* e = nullptr;
    errno = 0;
    long  n = std::strtol(v, &e, 10);
    if (e == v || *e != '\0' || errno == ERANGE || n < 0 || n > std::numeric_limits<int>::max())
        throw std::invalid_argument("invalid nonnegative integer");
    return static_cast<int>(n);
}

int main(int argc, char** argv)
{
    try
    {
    int      ops = 8000, keys = 1000, vsize = 64;
    uint32_t seed = 42;
    bool     single = false;
    std::string sname;
    int rratio = 70, dratio = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--ops"          && i + 1 < argc) ops    = parse_int(argv[++i]);
        else if (a == "--keys"         && i + 1 < argc) keys   = parse_int(argv[++i]);
        else if (a == "--value-size"   && i + 1 < argc) vsize  = parse_int(argv[++i]);
        else if (a == "--seed"         && i + 1 < argc) seed   = static_cast<uint32_t>(parse_int(argv[++i]));
        else if (a == "--read-ratio"   && i + 1 < argc) rratio = parse_int(argv[++i]);
        else if (a == "--delete-ratio" && i + 1 < argc) dratio = parse_int(argv[++i]);
        else if (a == "--scenario"     && i + 1 < argc) { single = true; sname = argv[++i]; }
        else if (a == "--correctness")                  { return correctness_check() ? 0 : 1; }
        else throw std::invalid_argument("unknown option or missing value: " + a);
    }
    if (ops <= 0 || keys <= 0 || vsize <= 0 || rratio > 100 || dratio > 100 || rratio + dratio > 100)
        throw std::invalid_argument("ops/keys/value-size must be positive; read/delete ratios must sum to at most 100");

    std::vector<Scenario> scenarios;
    if (single)
        scenarios.push_back({sname, ops, rratio, dratio, keys, vsize, seed});
    else
        scenarios = {
            {"write_heavy",       ops, 10,  0, keys, vsize, seed},
            {"mixed",             ops, 60, 10, keys, vsize, seed},
            {"read_heavy",        ops, 90,  0, keys, vsize, seed},
            {"mixed_with_delete", ops, 50, 20, keys, vsize, seed},
        };

    std::cout << "scenario,elapsed_s,throughput_ops_s,success,get_misses,mismatches,"
                 "put_p50_us,put_p95_us,put_p99_us,"
                 "get_p50_us,get_p95_us,get_p99_us,"
                 "del_p50_us,del_p95_us,del_p99_us\n";

    bool correct = true;
    for (auto& s : scenarios)
    {
        auto r = run_scenario(s);
        std::cout << s.name << ","
                  << r.elapsed_s    << "," << r.throughput << ","
                  << r.success      << "," << r.fail       << "," << r.mismatches << ","
                  << r.put_p50      << "," << r.put_p95    << "," << r.put_p99 << ","
                  << r.get_p50      << "," << r.get_p95    << "," << r.get_p99 << ","
                  << r.del_p50      << "," << r.del_p95    << "," << r.del_p99 << "\n";
        correct = correct && r.mismatches == 0;
    }
    return correct ? 0 : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << "benchmark: " << error.what() << '\n';
        return 2;
    }
}
