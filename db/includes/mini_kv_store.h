#ifndef MINI_LEVELDB_MINI_KV_STORE_H
#define MINI_LEVELDB_MINI_KV_STORE_H

#include <atomic>
#include <condition_variable>
#include <shared_mutex>
#include <fstream>
#include <memory>
#include <deque>
#include <unordered_map>
#include <cstring> // 为后续的内存字节流解码提供 memcpy
#include <list>
#include <mutex>
#include <thread>
#include <exception>

#include "../../mem_table/includes/mem_table.h"

// 数据库接口
// 物理载体：从磁盘提取的标准数据单元
class mini_kv_store
{
public:
    mini_kv_store();
    ~mini_kv_store();
    void close();
    [[nodiscard]] std::string get(std::string_view) const;
    void put(std::string_view, std::string_view);
    void erase(std::string_view);
private:
    mutable std::shared_mutex lifecycle_mutex_;
    bool closed_ = false;
    // --- 活跃内存跳表 ---
    mutable std::shared_mutex rw_mutex; // 内存跳表读写锁

    std::shared_ptr<mem_table> data_ = std::make_shared<mem_table>(); // 活跃内存跳表
    std::atomic<uint64_t> current_seq_ = 0; // 维护全局唯一序列号
    size_t memtable_size_threshold_ = 4096;

    // --- 串行日志追加 ---
    std::ofstream wal_file_;
    mutable std::mutex wal_mutex_;
    void append_to_wal(OperationType, uint64_t, std::string_view, std::string_view);

    // --- 缓存：LRU Block 检索 ---
    struct BlockPos // 数据块在多维矩阵中的唯一三维坐标
    {
        uint64_t sst_id;
        uint64_t offset;
        bool operator==(const BlockPos& other) const // 支持哈希碰撞
        {
            return sst_id == other.sst_id && offset == other.offset;
        }
    };
    struct BlockPosHash // 将二维的物理坐标折叠为一维的哈希定界符
    {
        size_t operator()(const BlockPos& k) const
        {
            return std::hash<uint64_t>()(k.sst_id) ^ (std::hash<uint64_t>()(k.offset) << 1); // 利用位移进行哈希混淆，阻断低阶碰撞
        }
    };
    struct lru_cache
    {
        const size_t capacity_ = 1024; // 物理红线：约缓存 4 MB 的数据块
        mutable std::mutex mtx_; // 绝对独占锁，拦截多线程的并发指针重定向
        struct block_elem
        {
            BlockPos pos;
            std::vector<std::byte> data;
        };
        mutable std::list<std::shared_ptr<block_elem>> lru_queue_; // 物理时间轴：双向链表，头部为 MRU (最新)，尾部为 LRU（最旧）
        mutable std::unordered_map<BlockPos, decltype(lru_queue_)::iterator, BlockPosHash> block_map_; // 块坐标到队列节点的映射
    } search_cache_;

    std::shared_ptr<const std::vector<std::byte>> fetch_block(uint64_t, uint64_t, uint64_t) const; // 磁盘探针升级：不再逐字读取，而是直接暴力抽取整个数据块
    SearchResult search_in_sstable(std::string_view key, uint64_t sst_id) const;

    // --- 单落盘线程后台调度 ---
    mutable std::shared_mutex bg_mutex_; // 落盘队列锁

    std::deque<std::shared_ptr<mem_table>> bg_queue_; // 落盘任务队列
    std::condition_variable_any bg_cv_; // 落盘任务唤醒器
    void background_compaction_routine(); // 落盘调度函数
    std::thread bg_thread_; // 落盘线程
    std::atomic<bool> stop_bg_thread_{false}; // 线程间通信-停机标志位
    std::exception_ptr bg_error_;
    void flush_imm_to_sstable(const std::shared_ptr<mem_table>&); // 落盘函数
    class sst_scanner // sst记录读取器
    {
        std::ifstream file_;
        uint64_t index_offset_;
    public:
        explicit sst_scanner(std::string_view);

        [[nodiscard]] bool has_next();
        [[nodiscard]] ParsedRecord next();
    };
    void compact_level0_to_level1(); // 跨层级压实调度器

    // --- 多级文件矩阵（元数据） ---
    mutable std::shared_mutex manifest_mutex_; // 元数据清单锁

    std::vector<std::vector<uint64_t>> levels_; // 核心物理矩阵：索引代表层级（0为L1, 1为L1，最高L6）
    std::atomic<uint64_t> next_sst_id_ {1}; // 全局唯一物理文件编址
    std::string generate_sst_filename(uint64_t id) const; // sst文件名生成器
    void save_manifest() const; // 元数据固化
    void load_manifest_and_rebuild_cache(); //元数据读取与缓存重建

    // --- 缓存：文件键值稀疏索引 、布隆过滤器 ---
    mutable std::shared_mutex cache_mutex_; // cache_mutex_ 缓存锁

    struct index_record
    {
        std::string key;
        uint64_t offset;
    };
    mutable std::unordered_map<uint64_t, std::vector<index_record>> index_cache_; // 稀疏索引 存储结构：<SST_ID, [<key, offset>]>

    mutable std::unordered_map<uint64_t, std::vector<uint8_t>> bloom_cache_; // 布隆过滤器缓存 存储结构： <SST_ID, 二进制位图>
    static uint32_t bloom_hash(std::string_view, uint32_t); // 底层哈希发生器
    std::vector<uint8_t> generate_bloom_filter(const std::vector<std::string>&) const; // 从key列表生成定长位图
    bool may_exist_in_sst(uint64_t, std::string_view) const;  // 探针前置拦截器
};

#endif //MINI_LEVELDB_MINI_KV_STORE_H
