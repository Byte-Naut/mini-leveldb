#ifndef MINI_LEVELDB_ORDERED_MAP_H
#define MINI_LEVELDB_ORDERED_MAP_H

#include <vector>
#include <random>
#include <iterator>
#include <memory_resource>

#include <cstddef>
#include <span>
#include <algorithm>

#include "../../db/includes/db_format.h"

// --- 内存跳表 ---
class mem_table
{
    struct Node;
public:
    mem_table();
    ~mem_table();

    // --- 屏蔽拷贝 & 移动语义 ---
    mem_table(const mem_table&) = delete;
    mem_table& operator=(const mem_table&) = delete;
    mem_table(mem_table&&) = delete;
    mem_table& operator=(mem_table&&) = delete;

    // --- 迭代器 ---
    class skip_list_iterator
    {
        friend class mem_table;
        explicit skip_list_iterator (Node*);
    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = ParsedRecordView;
        using reference = value_type;

        [[nodiscard]] reference operator*() const;
        [[nodiscard]] std::span<const std::byte> raw_payload() const;
        skip_list_iterator& operator++();
        skip_list_iterator operator++(int);
        bool operator==(const skip_list_iterator&) const;
    private:
        Node* current_;
    };
    using const_iterator = skip_list_iterator;
    [[nodiscard]] skip_list_iterator begin() const;
    [[nodiscard]] skip_list_iterator end() const;
    [[nodiscard]] const_iterator cbegin() const;

    // --- CURD ---
    [[nodiscard]] SearchResult get (std::string_view) const;
    void insert_record(std::string_view, uint64_t, std::string_view);
    void insert_tombstone(std::string_view, uint64_t);

    // --- 空间占用 ---
    [[nodiscard]] size_t approximate_memory_usage() const;

private:
    // --- 节点结构 ---
    struct Node
    {
        Node(int, std::string_view, uint64_t, OperationType, std::string_view, std::pmr::polymorphic_allocator<>);
        std::pmr::vector<std::byte> payload_; // 堆上字节载荷
        std::pmr::vector<Node*> forward_; // 后继数组
        [[nodiscard]] ParsedRecordView decode() const; // 返回载荷内容
    };

    // --- 跳表结构 ---
    Node* head_;
    int max_level_ = 15; // 层级上限（16层）
    float probability_ = 0.25; // 提升概率
    int current_level_ = 0; // 当前最高层级
    void internal_put(std::string_view, uint64_t, OperationType, std::string_view); // 插入节点

    // --- 随机数生成 ---
    std::mt19937 eng_{std::random_device{}()}; // 每个内存表独立的随机数引擎
    std::uniform_int_distribution<> dist_{0, 10};
    double get_random();

    // --- 内存管理 ---
    std::pmr::monotonic_buffer_resource arena_ {1024 * 1024}; // 内存池
    std::pmr::polymorphic_allocator<> alloc_ {&arena_}; // 内存分配器
    size_t usage_ = 0; // 当前内存占用
};

#endif //MINI_LEVELDB_ORDERED_MAP_H
