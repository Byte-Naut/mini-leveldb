#include <mutex>
#include <filesystem>
#include <complex>
#include <queue>
#include <ranges>
#include <algorithm>
#include <stdexcept>
#include <limits>
#include <array>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "includes/mini_kv_store.h"
#include "../mem_table/includes/mem_table.h"
#include "../db/includes/coding.h"

mini_kv_store::mini_kv_store(): levels_(7)
{
    load_manifest_and_rebuild_cache();
    const std::string wal_path = "wal.log";
    if (std::filesystem::exists(wal_path))
    {
        std::ifstream wal_in(wal_path, std::ios::binary);
        if (!wal_in) throw std::runtime_error("Cannot open WAL for recovery");
        const uint64_t wal_size = std::filesystem::file_size(wal_path);
        uint64_t valid_size = 0;
        while (valid_size < wal_size)
        {
            const uint64_t remaining = wal_size - valid_size;
            constexpr uint64_t fixed_bytes = sizeof(uint8_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
            if (remaining < fixed_bytes) break;
            uint8_t type_raw = 0;
            uint64_t seq = 0;
            uint32_t key_size = 0, value_size = 0;
            if (!coding::read_raw(wal_in, type_raw) ||
                !coding::read_raw(wal_in, seq) ||
                !coding::read_raw(wal_in, key_size)) break;
            uint64_t consumed = sizeof(type_raw) + sizeof(seq) + sizeof(key_size);
            if (key_size > remaining - consumed - sizeof(value_size)) break;
            std::string key = coding::read_slice(wal_in, key_size);
            if (!wal_in || !coding::read_raw(wal_in, value_size)) break;
            consumed += key_size + sizeof(value_size);
            if (value_size > remaining - consumed) break;
            std::string value = coding::read_slice(wal_in, value_size);
            if (!wal_in || (type_raw != static_cast<uint8_t>(OperationType::kValue) &&
                            type_raw != static_cast<uint8_t>(OperationType::kDeletion))) break;
            if (type_raw == static_cast<uint8_t>(OperationType::kValue))
                data_->insert_record(key, seq, value);
            else
                data_->insert_tombstone(key, seq);
            if (seq >= current_seq_.load(std::memory_order_relaxed))
                current_seq_.store(seq + 1, std::memory_order_relaxed);
            valid_size += consumed + value_size;
        }
        wal_in.close();
        if (valid_size != wal_size) std::filesystem::resize_file(wal_path, valid_size);
    }
    wal_file_.open("wal.log", std::ios::app | std::ios::binary);
    if (!wal_file_.is_open())
    {
        throw std::runtime_error("Fatal: 无法建立 WAL 物理管道");
    }

    bg_thread_  = std::thread(&mini_kv_store::background_compaction_routine, this);
}

mini_kv_store::~mini_kv_store()
{
    try { close(); } catch (...) { /* WAL remains available for recovery. */ }
}

void mini_kv_store::close()
{
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (closed_) return;
    if (data_->approximate_memory_usage() != 0)
    {
        std::lock_guard lock(bg_mutex_);
        bg_queue_.push_back(data_);
        data_.reset();
    }
    closed_ = true;
    {
        std::lock_guard lock(bg_mutex_);
        stop_bg_thread_ = true;
    }
    bg_cv_.notify_all();
    if (bg_thread_.joinable()) bg_thread_.join();
    {
        std::lock_guard wal_lock(wal_mutex_);
        if (wal_file_.is_open())
        {
            wal_file_.flush();
            if (!wal_file_) throw std::runtime_error("Cannot flush WAL during close");
            wal_file_.close();
        }
    }
    if (bg_error_) std::rethrow_exception(bg_error_);
    std::ofstream clear_wal("wal.log", std::ios::binary | std::ios::trunc);
    if (!clear_wal) throw std::runtime_error("Cannot truncate committed WAL");
    clear_wal.close();
    if (!clear_wal) throw std::runtime_error("Cannot close committed WAL");
}

std::string mini_kv_store::get(const std::string_view key) const
{
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (closed_) throw std::runtime_error("Store is closed");
    // 穿透层级1：观测最高频的活跃内存表（Active MemTable）
    {
        // 构造期获取共享锁，析构期自动释放
        std::shared_lock lock(rw_mutex);
        auto [status, value] = data_->get(key);
        if (status == SearchStatus::kFoundValue) return value;
        if (status == SearchStatus::kFoundDeletion) return "";
    } // 释放 rw_mutex，避免长时间阻塞外部写入

    // 穿透层级2：观测处于物理流转态的冻结表队列（Immutable MemTables in Queue）
    {
        std::shared_lock lock(bg_mutex_); // 锁定后台队列总线
        // 逆向遍历 deque（从最新冻结的跳表向老跳表扫视）
        for (const auto& it : std::ranges::reverse_view(bg_queue_))
        {
            auto [status, value] = it->get(key);
            if (status == SearchStatus::kFoundValue) return value;
            if (status == SearchStatus::kFoundDeletion) return "";
        }
    } // 释放 bg_mutex_

    // 穿透层级 3：多维物理矩阵级联检索
    // Keep source files alive until the lookup finishes; compaction unlinks them
    // only while holding the exclusive manifest lock.
    std::shared_lock manifest_lock(manifest_mutex_);
    const auto& current_matrix = levels_;

    // 绝对偏序 1：Level 0 具有最高时间权重，需逆向遍历内部文件
    for (const auto id : std::ranges::reverse_view(current_matrix[0]))
    {
        if (!may_exist_in_sst(id, key)) continue; // L0 也可应用布隆拦截
        auto disk_result = search_in_sstable(key, id);
        if (disk_result.status == SearchStatus::FNotOpen) throw std::runtime_error("Cannot read referenced SST");
        if (disk_result.status == SearchStatus::kFoundValue) return disk_result.value;
        if (disk_result.status == SearchStatus::kFoundDeletion) return "";
    }

    // 绝对偏序 2：Level 1 至 Level N，数据绝对互斥且全局有序，执行文件级 O(log N) 二分深潜
    for (size_t lvl = 1; lvl < current_matrix.size(); ++lvl)
    {
        const auto& current_level_files = current_matrix[lvl];
        if (current_level_files.empty()) continue;

        uint64_t target_sst_id = 0;
        bool found_file = false;

        {
            std::shared_lock cache_lock(cache_mutex_);
            // 寻找首个最小 Key 严格大于目标 Key 的 SSTable
            auto it = std::upper_bound(current_level_files.begin(), current_level_files.end(), key,
                [this](const std::string_view k, const uint64_t id)
                {
                    if (index_cache_.at(id).empty()) return false;
                    return k < index_cache_.at(id).front().key;
                });
            // 回退一个身位，锁定唯一可能包含目标数据的物理文件
            if (it != current_level_files.begin())
            {
                target_sst_id = *(it - 1);
                found_file = true;
            }
        }

        // for (const uint64_t sst_id : current_matrix[lvl])
        if (found_file)
        {
            // 命中疑似文件后，再交给布隆断路器与底层探针处理
            // 内存断路器：阻断无效物理磁盘下潜
            if (!may_exist_in_sst(target_sst_id, key)) continue;

            auto disk_result = search_in_sstable(key, target_sst_id);
            if (disk_result.status == SearchStatus::FNotOpen) throw std::runtime_error("Cannot read referenced SST");
            if (disk_result.status == SearchStatus::kFoundValue) return disk_result.value;
            if (disk_result.status == SearchStatus::kFoundDeletion) return "";
        }
    }

    return ""; // 绝对真空，数据不存在
}

void mini_kv_store::put(const std::string_view key, std::string_view value)
{
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (closed_) throw std::runtime_error("Store is closed");
    // 构造期获取排他锁，执行物理清场
    std::unique_lock lock(rw_mutex);

    // 1. 容量探针拦截
    if (data_->approximate_memory_usage() >= memtable_size_threshold_)
    {
        // 1. 将冻结跳表压入异步调度队列
        {
            std::lock_guard bg_lock(bg_mutex_);
            bg_queue_.push_back(std::move(data_));
            // 2. 原址重构活跃跳表
            data_ = std::make_shared<mem_table>();
        }
        bg_cv_.notify_one(); // 向后台线程发送硬件级唤醒脉冲
    }

    const uint64_t seq = current_seq_.fetch_add(1, std::memory_order_relaxed) & 0x00FFFFFFFFFFFFFF;
    // 物理防线：先落盘
    append_to_wal(OperationType::kValue, seq, key, value);
    // 绝对独占状态
    data_->insert_record(key, seq, value);
}

void mini_kv_store::erase(const std::string_view key)
{
    std::shared_lock lifecycle_lock(lifecycle_mutex_);
    if (closed_) throw std::runtime_error("Store is closed");
    // 构造期获取排他锁，执行物理清场
    std::unique_lock lock(rw_mutex);
    if (data_->approximate_memory_usage() >= memtable_size_threshold_)
    {
        auto replacement = std::make_shared<mem_table>();
        {
            std::lock_guard bg_lock(bg_mutex_);
            bg_queue_.push_back(data_);
            data_ = std::move(replacement);
        }
        bg_cv_.notify_one();
    }
    const uint64_t seq = current_seq_.fetch_add(1, std::memory_order_relaxed) & 0x00FFFFFFFFFFFFFF;
    // 物理防线：先落盘，删除操作的 value 设为空
    append_to_wal(OperationType::kDeletion, seq, key, "");
    // 绝对独占状态
    data_->insert_tombstone(key, seq);
}

void mini_kv_store::append_to_wal(OperationType type, uint64_t seq, const std::string_view key, const std::string_view value)
{
    std::lock_guard wal_lock(wal_mutex_);

    const auto t = static_cast<uint8_t>(type);
    coding::write_raw(wal_file_, t);

    coding::write_raw(wal_file_, seq);

    const uint32_t key_size = key.size();
    coding::write_raw(wal_file_, key_size);
    coding::write_slice(wal_file_, key);

    const uint32_t value_size = value.size();
    coding::write_raw(wal_file_, value_size);
    coding::write_slice(wal_file_, value);

    wal_file_.flush();
    if (!wal_file_) throw std::runtime_error("WAL append failed");
}

void mini_kv_store::background_compaction_routine()
{
    while (true)
    {
        std::shared_ptr<mem_table> imm_to_flush;
        {
            // 获取系统调度锁，探测队列状态
            std::unique_lock lock(bg_mutex_);
            bg_cv_.wait(lock, [this]
            {
                return stop_bg_thread_ || !bg_queue_.empty();
            });

            // 若接收到停机信号且队列已物理排空，则终止当前执行流
            if (stop_bg_thread_ && bg_queue_.empty())
                return;

            // 物理抽离队列头部的冻结列表
            imm_to_flush = bg_queue_.front(); // 拷贝 shared_ptr，绝对不弹出队列。此时，前台 Get 接口仍然可以安全遍历 bg_queue 读到该数据。
        } // 离开作用域，瞬间释放 bg_mutex_

        // 绝对算力解耦：在此处执行耗时数百毫秒的磁盘操作.彻底远离主线程 rw_mutex_ 与 bg_mutex_，无阻力落盘
        try
        {
            flush_imm_to_sstable(imm_to_flush);
        }
        catch (...)
        {
            std::lock_guard lock(bg_mutex_);
            bg_error_ = std::current_exception();
            return;
        }

        // 磁盘刻录完成，数据已在SSTable中绝对可见 此时方可获取锁，将跳表从内存观测队列中物理剥离
        {
            std::unique_lock lock(bg_mutex_);
            bg_queue_.pop_front();
        }

        try { compact_level0_to_level1(); }
        catch (...)
        {
            std::lock_guard lock(bg_mutex_);
            bg_error_ = std::current_exception();
            return;
        }
    }
}

void mini_kv_store::flush_imm_to_sstable(const std::shared_ptr<mem_table>& imm_to_flush)
{
    // 1. 物理编制：提取当前可用序列号并原子推进
    uint64_t sst_id = next_sst_id_.fetch_add(1, std::memory_order_relaxed); //?
    std::string sst_path = generate_sst_filename(sst_id);

    std::ofstream sst_file(sst_path, std::ios::binary | std::ios::trunc);
    if (!sst_file.is_open()) throw std::runtime_error("Cannot open SST for flush");

    std::vector<index_record> sparse_index;

    uint64_t current_offset = 0;
    uint64_t last_block_offset = 0;
    std::string previous_key;
    bool have_previous_key = false;

    std::vector<std::string> all_keys_for_bloom;

    // 1. 顺序蚀刻数据块 (Data Block)
    for (auto it = imm_to_flush -> begin(); it != imm_to_flush->end(); ++it)
    {
        auto view = *it;
        all_keys_for_bloom.emplace_back(view.key);

        auto raw_payload = it.raw_payload();

        if (constexpr uint64_t PAGE_SIZE = 4096;
            (current_offset - last_block_offset >= PAGE_SIZE || current_offset == 0) &&
            (!have_previous_key || view.key != previous_key))
        {
            sparse_index.emplace_back(std::string(view.key), current_offset);
            last_block_offset = current_offset;
        }

        coding::write_buffer(sst_file, raw_payload);
        current_offset += raw_payload.size();
        previous_key = view.key;
        have_previous_key = true;
    }

    // 2. 顺序蚀刻索引块（Index Block）
    uint64_t index_block_offset = current_offset;
    for (const auto& [idx_key, idx_offset]: sparse_index)
    {
        uint32_t k_size = idx_key.size();
        coding::write_raw(sst_file, k_size);
        coding::write_slice(sst_file, idx_key);
        coding::write_raw(sst_file, idx_offset);
    }

    // 3. 蚀刻布隆过滤块（Bloom Filter Block）
    uint64_t bloom_block_offset = sst_file.tellp();
    std::vector<uint8_t> bloom_data = generate_bloom_filter(all_keys_for_bloom);
    coding::write_buffer(sst_file, std::span<const uint8_t>{bloom_data});

    // 4. 蚀刻升维后的页脚（Footer：16Bytes）
    // 包含：8 字节索引块便宜 + 8 字节布隆块偏移
    coding::write_raw(sst_file, index_block_offset);
    coding::write_raw(sst_file, bloom_block_offset);

    sst_file.flush();
    if (!sst_file) throw std::runtime_error("Cannot write SST during flush");
    sst_file.close();
    if (!sst_file) throw std::runtime_error("Cannot close SST after flush");

    // 拓扑更新：将新生效的位图直插内存缓存网
    {
        std::unique_lock cache_lock(cache_mutex_);
        bloom_cache_[sst_id] = std::move(bloom_data);
        index_cache_[sst_id] = std::move(sparse_index);
    }

    // 拓扑注册：将刻录完成的物理文件登记至 Level 0 清单
    {
        std::unique_lock manifest_lock(manifest_mutex_);
        levels_[0].push_back(sst_id);
        try { save_manifest(); }
        catch (...) { levels_[0].pop_back(); throw; }
    }
}

mini_kv_store::sst_scanner::sst_scanner(const std::string_view path)
{
    file_.open(path.data(), std::ios::binary);
    if (file_.is_open())
    {
        // 读取 8 字节 Footer, 锁定数据区的物理边界
        // 物理修复：将探针瞬移至倒数 16 字节处（Footer 绝对起点）
        file_.seekg(-static_cast<std::streamoff>(sizeof(uint64_t) * 2), std::ios::end);
        // 提取前 8 字节的 index_offset 作为数据块边界，直接丢弃后 8 字节的 bloom_offset
        coding::read_raw(file_, index_offset_);
        // 将读取探针归位至文件绝对起点，准备顺序榨取数据块
        file_.seekg(0, std::ios::beg);
    }
    else
    {
        index_offset_ = 0;
    }
}

bool mini_kv_store::sst_scanner::has_next()
{
    if (!file_.is_open()) return false;
    const auto position = file_.tellg();
    return position != std::streampos(-1) &&
           static_cast<uint64_t>(static_cast<std::streamoff>(position)) < index_offset_;
}

ParsedRecord mini_kv_store::sst_scanner::next()
{
    ParsedRecord record;
    uint32_t internal_key_size;
    coding::read_raw(file_, internal_key_size);

    const uint32_t real_key_size = internal_key_size - sizeof(uint64_t);
    record.key = coding::read_slice(file_, real_key_size);

    uint64_t pack;
    coding::read_raw(file_, pack);
    record.type = static_cast<OperationType>(pack & 0xFF);
    record.seq = pack >> 8;

    uint32_t value_size;
    coding::read_raw(file_, value_size);
    record.value = coding::read_slice(file_, value_size);

    return record;
}

void mini_kv_store::compact_level0_to_level1()
{
    std::vector<uint64_t> target_l0, target_l1;
    {
        std::unique_lock lock(manifest_mutex_);
        if (levels_[0].size() < 4) return;
        target_l0 = levels_[0];
        target_l1 = levels_[1]; // MVP 阶段：直接全量卷入 L1 执行暴力降噪
    }

    std::vector<uint64_t> all_files = target_l0;
    all_files.insert(all_files.end(), target_l1.begin(), target_l1.end());
    uint64_t new_sst_id = next_sst_id_.fetch_add(1, std::memory_order_relaxed);

    // 1. 构造优先队列（Min-Heap）节点
    struct HeapNode
    {
        ParsedRecord record;
        size_t scanner_idx;
        // 绝对偏序规则：Key 小的优先; Key 相同则 Seq 大的（最新数据）优先
        bool operator>(const HeapNode& other) const
        {
            const int cmp = record.key.compare(other.record.key);
            if (cmp == 0) return record.seq < other.record.seq;
            return cmp > 0;
        }
    };

    // 3. 开启全新物理文件管道承接压缩后的数据
    std::string new_sst_path = generate_sst_filename(new_sst_id);
    std::ofstream new_sst_file(new_sst_path, std::ios::binary | std::ios::trunc);
    if (!new_sst_file) throw std::runtime_error("Cannot open compaction SST");
    std::vector<index_record> sparse_index;

    uint64_t current_offset = 0;
    std::vector<std::string> all_keys_for_bloom;
    {
        std::vector<sst_scanner> scanners;
        // 使用 greater 构建最小堆
        std::priority_queue<HeapNode, std::vector<HeapNode>, std::greater<>> min_heap;

        // 2.初始化 K 个磁盘流扫描探针
        for (size_t i = 0; i < all_files.size(); ++i)
        {
            scanners.emplace_back(generate_sst_filename(all_files[i]));
            if (scanners.back().has_next())
            {
                min_heap.push({scanners.back().next(), i});
            }
        }

        // 4. 执行 K 路流式归并（不消耗额外内存）
        std::string last_key;
        bool have_last_key = false;
        uint64_t last_block_offset = 0;
        while (!min_heap.empty())
        {
            HeapNode top = min_heap.top();
            min_heap.pop();
            // 物理降噪：剔除旧版本数据（同一 Key 仅保留第一次弹出的最新 Seq）
            if (!have_last_key || top.record.key != last_key)
            {
                last_key = top.record.key;
                have_last_key = true;

                // 物理降噪：彻底丢弃物理墓碑，回收磁盘空间
                if (top.record.type != OperationType::kDeletion)
                {
                    // ...稀疏索引与二进制写入逻辑（复用 flush_imm_to_sstable 内部的蚀刻代码）
                    if (constexpr uint64_t PAGE_SIZE = 4096; current_offset - last_block_offset >= PAGE_SIZE || current_offset == 0)
                    {
                        sparse_index.emplace_back(top.record.key, current_offset);
                        last_block_offset = current_offset;
                    }

                    all_keys_for_bloom.push_back(top.record.key);

                    uint32_t internal_key_size = top.record.key.size() + sizeof(uint64_t);
                    coding::write_raw(new_sst_file, internal_key_size);
                    coding::write_slice(new_sst_file, top.record.key);
                    uint64_t pack = (top.record.seq << 8) | static_cast<uint8_t>(top.record.type);
                    coding::write_raw(new_sst_file, pack);
                    uint32_t value_size = top.record.value.size();
                    coding::write_raw(new_sst_file, value_size);
                    coding::write_slice(new_sst_file, top.record.value);

                    current_offset = new_sst_file.tellp();
                }
            }
            // 推进对应通道的物理探针
            if (scanners[top.scanner_idx].has_next())
            {
                min_heap.push({scanners[top.scanner_idx].next(), top.scanner_idx});
            }
        }
    }

    // 5. 蚀刻新文件的索引块
    uint64_t index_block_offset = current_offset;
    for (const auto& [idx_key, idx_offset] : sparse_index) {
        uint32_t k_size = idx_key.size();
        coding::write_raw(new_sst_file, k_size);
        coding::write_slice(new_sst_file, idx_key);
        coding::write_raw(new_sst_file, idx_offset);
    }

    // 6. 蚀刻布隆过滤快
    uint64_t bloom_block_offset = new_sst_file.tellp();
    std::vector<uint8_t> bloom_data = generate_bloom_filter(all_keys_for_bloom);
    coding::write_buffer(new_sst_file, std::span<const uint8_t>{bloom_data});

    // 7. 蚀刻升维后的页脚（Footer: 16 Bytes)
    // 包含：8字节索引块偏移 + 8字节布隆块偏移
    coding::write_raw(new_sst_file, index_block_offset);
    coding::write_raw(new_sst_file, bloom_block_offset);
    new_sst_file.flush();
    if (!new_sst_file) throw std::runtime_error("Cannot write compaction SST");
    new_sst_file.close();
    if (!new_sst_file) throw std::runtime_error("Cannot close compaction SST");
    const bool has_compacted_records = !sparse_index.empty();

    // 拓扑更新：将新生效的位图直插内存缓存网
    {
        std::unique_lock cache_lock(cache_mutex_);
        bloom_cache_[new_sst_id] = std::move(bloom_data);
        index_cache_[new_sst_id] = std::move(sparse_index);
    }

    // 6. 拓扑更新：执行原子替换，抹除旧文件残留
    {
        std::unique_lock manifest_lock{manifest_mutex_};
        // 彻底清空旧防线
        levels_[0].clear();
        levels_[1].clear();
        // 新文件直接沉降至 Level 1 矩阵
        if (has_compacted_records) levels_[1].push_back(new_sst_id);
        try { save_manifest(); }
        catch (...)
        {
            levels_[0] = target_l0;
            levels_[1] = target_l1;
            throw;
        }
        // Readers hold the shared manifest lock across disk access, so old
        // files can be unlinked only while this exclusive lock is held.
        for (uint64_t old_id : all_files)
        {
            if (std::remove(generate_sst_filename(old_id).c_str()) != 0)
                perror("Error deleting file");
        }
    }
    if (!has_compacted_records) std::remove(new_sst_path.c_str());
}

std::string mini_kv_store::generate_sst_filename(uint64_t id) const
{
    std::ostringstream oss;
    // 强制 6 位对齐前导零，保障操作系统层面的字典序即物理时间序
    oss << std::setfill('0') <<std::setw(6) << id << ".sst";
    return oss.str();
}

void mini_kv_store::save_manifest() const
{
    const std::string tmp_manifest = "MANIFEST.tmp";
    const std::string real_manifest = "MANIFEST";

    std::ofstream out(tmp_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) throw std::runtime_error("Cannot open MANIFEST.tmp");

    // 1. 固化全局文件序列号
    const uint64_t current_id = next_sst_id_.load(std::memory_order_relaxed);
    coding::write_raw(out, current_id);

    // 2. 固化多维文件矩阵
    const uint32_t num_levels = levels_.size();
    coding::write_raw(out, num_levels);

    for (const auto& level: levels_)
    {
        uint32_t num_files = level.size();
        coding::write_raw(out, num_files);
        for (uint64_t sst_id : level)
        {
            coding::write_raw(out, sst_id);
        }
    }

    out.flush();
    if (!out) throw std::runtime_error("Cannot write MANIFEST");
    out.close();
    if (!out) throw std::runtime_error("Cannot close MANIFEST");

    // 3. 操作系统级原子替换：强制确保文件系统的绝对一致性
#ifdef _WIN32
    const auto source = std::filesystem::path(tmp_manifest).wstring();
    const auto target = std::filesystem::path(real_manifest).wstring();
    if (!MoveFileExW(source.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace MANIFEST");
#else
    std::filesystem::rename(tmp_manifest, real_manifest);
#endif
}

void mini_kv_store::load_manifest_and_rebuild_cache()
{
    const std::string manifest_path = "MANIFEST";
    if (!std::filesystem::exists(manifest_path)) return;

    std::ifstream in(manifest_path, std::ios::binary);
    if (!in.is_open()) return;

    // 1. 重建全局序列号
    uint64_t loaded_id {1};
    coding::read_raw(in, loaded_id);
    next_sst_id_.store(loaded_id, std::memory_order_relaxed);

    // 2. 重建多维文件矩阵
    uint32_t num_levels = 0;
    coding::read_raw(in, num_levels);

    levels_.resize(num_levels);
    for (uint32_t i{0}; i < num_levels; ++i)
    {
        uint32_t num_files = 0;
        coding::read_raw(in, num_files);
        levels_[i].resize(num_files);
        for (uint32_t j{0}; j < num_files; ++j)
        {
            coding::read_raw(in, levels_[i][j]);
        }
    }
    in.close();

    // 暴力重建布隆过滤器缓存与index索引缓存
    for (const auto& level : levels_)
    {
        for (uint64_t sst_id : level)
        {
            std::string sst_path = generate_sst_filename(sst_id);
            std::ifstream sst_file(sst_path, std::ios::binary | std::ios::ate);
            if (!sst_file.is_open()) throw std::runtime_error("MANIFEST references missing SST");

            uint64_t file_size = sst_file.tellg();
            if (file_size < 16) throw std::runtime_error("MANIFEST references truncated SST");

            // 读取 16 字节 Footer
            sst_file.seekg(-16, std::ios::end);
            uint64_t index_offset, bloom_offset;
            coding::read_raw(sst_file, index_offset);
            coding::read_raw(sst_file, bloom_offset);
            if (!sst_file || index_offset > bloom_offset || bloom_offset > file_size - 16)
                throw std::runtime_error("Invalid SST footer");

            // 计算位图物理体积并抽离至内存
            uint64_t bloom_size = (file_size - 16) - bloom_offset;
            std::vector<uint8_t> bloom_data(bloom_size);

            sst_file.seekg(bloom_offset, std::ios::beg);
            coding::read_buffer(sst_file, std::span{bloom_data});

            bloom_cache_[sst_id] = std::move(bloom_data);

            // 物理提取：将磁盘稀疏索引反序列化至内存缓存
            sst_file.seekg(index_offset, std::ios::beg);
            std::vector<index_record> sparse_index;
            // 索引块的绝对终点即为布隆块的起点（bloom_offset）
            while (true)
            {
                const auto position = sst_file.tellg();
                if (position == std::streampos(-1)) throw std::runtime_error("Invalid SST index");
                if (static_cast<uint64_t>(static_cast<std::streamoff>(position)) >= bloom_offset) break;
                uint32_t k_size;
                if (!coding::read_raw(sst_file, k_size)) throw std::runtime_error("Invalid SST index");
                std::string idx_key = coding::read_slice(sst_file, k_size);

                uint64_t block_offset;
                if (!sst_file || !coding::read_raw(sst_file, block_offset))
                    throw std::runtime_error("Invalid SST index");

                sparse_index.emplace_back(std::move(idx_key), block_offset);
            }
            index_cache_[sst_id] = std::move(sparse_index);
            sst_scanner scanner(sst_path);
            while (scanner.has_next())
            {
                const auto record = scanner.next();
                if (record.seq >= current_seq_.load(std::memory_order_relaxed))
                    current_seq_.store(record.seq + 1, std::memory_order_relaxed);
            }
        }
    }
}

uint32_t mini_kv_store::bloom_hash(std::string_view key, uint32_t seed)
{
    uint32_t h = seed;
    for (const char c : key)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 0x01000193; // 物理常量：FNV prime
    }
    return h;
}

std::vector<uint8_t> mini_kv_store::generate_bloom_filter(const std::vector<std::string>& keys) const
{
    // 物理分配：每个 Key 占用 10 bits，向上取整至字节
    size_t num_bits = std::max(keys.size() * 10, static_cast<size_t>(64));
    size_t num_bytes = (num_bits + 7) / 8;
    std::vector<uint8_t> filter(num_bytes, 0);

    for (const auto& key: keys)
    {
        // 使用 3 个不同的哈希种子进行物理刻录
        for (const uint32_t seed: {0x12345678U, 0x87654321U, 0xABCDEF01U})
        {
            const uint32_t h = bloom_hash(key, seed) % (num_bytes * 8);
            filter[h / 8] |= (1 << (h % 8)); // 物理置位
        }
    }
    return filter;
}

bool mini_kv_store::may_exist_in_sst(uint64_t sst_id, std::string_view key) const
{
    std::shared_lock lock(cache_mutex_);
    auto it = bloom_cache_.find(sst_id);
    if (it == bloom_cache_.end() || it->second.empty())
    {
        return true; // 缓存未命中，强制放行执行物理磁盘校验
    }

    const auto& filter = it -> second;
    size_t num_bits = filter.size() * 8;

    std::vector<uint32_t> seeds{0x12345678, 0x87654321, 0xABCDEF01};
    return std::ranges::none_of(seeds, [&](const uint32_t seed)
    {
        const uint32_t h = bloom_hash(key, seed) % num_bits;
        return (filter[h / 8] & (1 << (h % 8))) == 0; // 绝对物理拦截：只要有一个探测点为 0，数据绝对不存在, 概率放行：可能存在，需下潜磁盘
    });
}

std::shared_ptr<const std::vector<std::byte>> mini_kv_store::fetch_block(const uint64_t sst_id, const uint64_t block_offset,
                                                                   const uint64_t next_offset) const
{
    const BlockPos pos{sst_id, block_offset};

    // --- 尝试缓存拦截 ---
    {
        std::lock_guard lock(search_cache_.mtx_);
        if (const auto it = search_cache_.block_map_.find(pos); it != search_cache_.block_map_.end())
        {
            // 缓存命中：执行物理时间轴重构
            search_cache_.lru_queue_.splice(search_cache_.lru_queue_.begin(), search_cache_.lru_queue_, it->second);             // std::list::splice 能够在不触发任何内存拷贝的前提下，瞬间将节点斩断并接驳至头部
            // 1. 提取当前节点的 shared_ptr<block_elem>
            std::shared_ptr<lru_cache::block_elem> elem_ptr = *(it->second);
            // 2. 物理级重定向
            return {elem_ptr, &(elem_ptr->data)};
        }
    }

    // --- 未命中，从磁盘调取 ---
    const auto sst_path = generate_sst_filename(sst_id);
    std::ifstream sst_file(sst_path, std::ios::binary);
    if (!sst_file.is_open()) return nullptr;
    const uint64_t block_size = next_offset - block_offset;
    std::vector<std::byte> block_data(block_size);
    sst_file.seekg(block_offset, std::ios::beg);
    if (!coding::read_buffer(sst_file, std::span{block_data}))
        throw std::runtime_error("Cannot read SST block");
    sst_file.close();
    // 将新数据块锚定至LRU队列与map
    std::shared_ptr<lru_cache::block_elem> new_elem;
    {
        std::lock_guard lock(search_cache_.mtx_);

        // 若突破容量，则抹除队尾元素
        if (search_cache_.block_map_.size() >= search_cache_.capacity_)
        {
            auto last = search_cache_.lru_queue_.end(); --last;
            search_cache_.block_map_.erase(last->get()->pos);
            search_cache_.lru_queue_.pop_back();
        }

        // 录入块缓存

        new_elem = std::make_shared<lru_cache::block_elem>(lru_cache::block_elem{pos, std::move(block_data)});
        search_cache_.lru_queue_.push_front(new_elem);
        search_cache_.block_map_[pos] = search_cache_.lru_queue_.begin();
    }
    return {new_elem, &(new_elem->data)};
}

SearchResult mini_kv_store::search_in_sstable(const std::string_view key, const uint64_t sst_id) const
{
    // --- 打开 .sst 文件
    const auto sst_path = generate_sst_filename(sst_id);
    std::ifstream sst_file(sst_path, std::ios::binary);
    if (!sst_file.is_open()) return {SearchStatus::FNotOpen, ""};

    // --- 利用稀疏索引缓存查找key所在块 ---
    std::array<std::pair<uint64_t, uint64_t>, 2> candidate_blocks{};
    size_t candidate_count = 0;
    {
        std::shared_lock lock(cache_mutex_);
        const auto& sparse_index = index_cache_.at(sst_id);
        if (sparse_index.empty()) return {SearchStatus::kNotFound, ""};
        auto first_ge = std::lower_bound(sparse_index.begin(), sparse_index.end(), key,
            [](const index_record& elem, const std::string_view k)
            {
                return elem.key < k;
            });
        if (first_ge == sparse_index.begin() && first_ge->key > key)
            return {SearchStatus::kNotFound, ""};
        size_t first_index = first_ge == sparse_index.begin() ? 0 :
                             static_cast<size_t>(first_ge - sparse_index.begin() - 1);
        if (first_ge == sparse_index.end()) first_index = sparse_index.size() - 1;
        const bool check_equal_block = first_ge != sparse_index.end() &&
                                       first_ge != sparse_index.begin() && first_ge->key == key;
        sst_file.seekg(-static_cast<std::streamoff>(sizeof(uint64_t) * 2), std::ios::end);
        uint64_t data_end = 0;
        if (!coding::read_raw(sst_file, data_end)) throw std::runtime_error("Invalid SST footer");
        auto add_block = [&](size_t index)
        {
            const uint64_t end = index + 1 < sparse_index.size() ? sparse_index[index + 1].offset : data_end;
            candidate_blocks[candidate_count++] = {sparse_index[index].offset, end};
        };
        add_block(first_index);
        if (check_equal_block) add_block(first_index + 1);
    }

    for (size_t candidate = 0; candidate < candidate_count; ++candidate)
    {
        const auto [target_block_offset, next_block_offset] = candidate_blocks[candidate];
        auto block_data_ptr = fetch_block(sst_id, target_block_offset, next_block_offset);
        if (!block_data_ptr) return {SearchStatus::FNotOpen, ""};

        size_t offset = 0;
        std::span buffer{*block_data_ptr};
        while (offset < buffer.size())
        {
        const auto internal_key_size = coding::decode_fixed<uint32_t>(buffer.subspan(offset).first<sizeof(uint32_t)>());
        offset += sizeof(internal_key_size);

        const auto real_key_size = internal_key_size - sizeof(uint64_t);
        // no offset!

        const auto key_span = buffer.subspan(offset, real_key_size);
        const std::string_view current_key(reinterpret_cast<const char*>(key_span.data()), real_key_size);
        offset += real_key_size;

        const auto pack = coding::decode_fixed<uint64_t>(buffer.subspan(offset).first<sizeof(uint64_t)>());
        const auto type = static_cast<OperationType>(pack & 0xFF);
        offset += sizeof(pack);

        const auto value_size = coding::decode_fixed<uint32_t>(buffer.subspan(offset).first<sizeof(uint32_t)>());
        offset += sizeof(value_size);

        const auto value_span = buffer.subspan(offset, value_size);
        const std::string_view value(reinterpret_cast<const char*>(value_span.data()), value_size);
        offset += value_size;

        // 执行最终的时空偏序比对
        if (current_key == key)
        {
            if (type == OperationType::kDeletion)
            {
                return {SearchStatus::kFoundDeletion, ""};
            }
            return {SearchStatus::kFoundValue, std::string(value)}; // 缓存脱壳成功
        }

        // 数据块本身是有序的，一旦跨越目标范围即可提前停机
            if (current_key.compare(key) > 0) break;
        }
    }

    return {SearchStatus::kNotFound, ""};
}
