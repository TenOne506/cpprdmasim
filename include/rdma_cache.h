#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <chrono>
#include <deque>
#include "rdma_qp_cache.h"
#include "rdma_cq_cache.h"
#include "rdma_mr_cache.h"
#include "rdma_pd_cache.h"

// 定义组件类型枚举
enum class ComponentType {
    QP,  // Queue Pair
    CQ,  // Completion Queue
    MR,  // Memory Region
    PD   // Protection Domain
};

// 定义缓存策略枚举
enum class CachePolicy {
    LRU,  // Least Recently Used
    MRU,  // Most Recently Used
    FIFO  // First In First Out
};

// 前向声明
struct QPInfo;
struct CQInfo;
struct MRInfo;
struct PDInfo;
struct QPCacheValue;
struct CQCacheValue;
struct MRCacheValue;
struct PDCacheValue;
struct MRBlock;
struct CompletionEntry;

// 定义压缩的QP状态结构
// struct CompressedQPState {
//     uint8_t state : 4;        // QP状态
//     uint8_t access_flags : 4; // 访问标志
//     uint8_t port_num : 4;     // 端口号
// };

// // 定义完成条目结构
// struct CompletionEntry {
//     uint64_t wr_id;
//     uint32_t status;
//     uint32_t opcode;
//     uint32_t byte_len;
//     uint32_t imm_data;
// };

// 定义QP信息结构
struct QPInfo {
    uint32_t qp_num;
    uint8_t state;
    uint8_t access_flags;
    uint8_t port_num;
    uint32_t qp_access_flags;
};

// 定义CQ信息结构
struct CQInfo {
    uint32_t cq_num;
    uint32_t cqe;
    uint32_t comp_vector;
};

// 定义MR信息结构
struct MRInfo {
    uint32_t lkey;
    uint32_t access_flags;
    uint64_t length;
};

// 定义PD信息结构
struct PDInfo {
    uint32_t pd_handle;
    std::unordered_map<std::string, std::vector<uint32_t>> resources;
};

// 定义统计信息结构
struct Stats {
    size_t hits;
    size_t misses;
    size_t evictions;
};

// 定义缓存指标结构
struct CacheMetrics {
    struct {
        size_t hits;
        size_t misses;
    } hit_rate;
    
    struct {
        std::chrono::nanoseconds total_time;
        size_t access_count;
    } access_time;
    
    struct {
        size_t original_size;
        size_t compressed_size;
    } memory_usage;
};

// 定义缓存条目结构
struct CacheEntry {
    std::vector<uint8_t> data;
    size_t size;
    ComponentType type;
    uint32_t id;
    struct ComponentInfo {
        QPInfo qp_info;
        CQInfo cq_info;
        MRInfo mr_info;
        PDInfo pd_info;
        
        ComponentInfo() : qp_info{}, cq_info{}, mr_info{}, pd_info{} {}
    } component_info;
    
    CacheEntry() : size(0), type(ComponentType::QP), id(0) {}
    CacheEntry(const CacheEntry& other) = default;
    CacheEntry& operator=(const CacheEntry& other) = default;
};

// RDMA缓存类
class RdmaCache {
public:
    explicit RdmaCache(size_t total_cache_size) {
        // 分配缓存大小
        size_t qp_cache_size = total_cache_size * 0.5;  // 50%
        size_t cq_cache_size = total_cache_size * 0.2;  // 20%
        size_t mr_cache_size = total_cache_size * 0.2;  // 30%
        size_t pd_cache_size = total_cache_size * 0.1;  // 20%
        
        // 创建组件缓存
        qp_cache_ = std::make_unique<RdmaQPCache>(qp_cache_size);
        cq_cache_ = std::make_unique<RdmaCQCache>(cq_cache_size);
        mr_cache_ = std::make_unique<RdmaMRCache>(mr_cache_size);
        pd_cache_ = std::make_unique<RdmaPDCache>(pd_cache_size);
    }
    
    virtual ~RdmaCache() = default;

    // 基本缓存操作
    bool contains(ComponentType type, uint32_t id) const;
    bool get(ComponentType type, uint32_t id, void* buffer, size_t size);
    void put(ComponentType type, uint32_t id, const void* data, size_t size);
    
    // QP缓存操作
    bool get_qp_info(uint32_t qp_num, QPCacheValue& info);
    void set_qp_info(uint32_t qp_num, const QPCacheValue& info);
    
    // CQ缓存操作
    bool get_cq_info(uint32_t cq_num, CQCacheValue& info);
    void set_cq_info(uint32_t cq_num, const CQCacheValue& info);
    void batch_add_completions(uint32_t cq_num, const std::vector<CompletionEntry>& completions);
    std::vector<CompletionEntry> batch_get_completions(uint32_t cq_num, uint32_t max_count);
    
    // MR缓存操作
    bool get_mr_info(uint32_t lkey, MRCacheValue& info);
    void set_mr_info(uint32_t lkey, const MRCacheValue& info);
    MRBlock* allocate_block(size_t size, uint32_t flags);
    void free_block(MRBlock* block);
    
    // PD缓存操作
    bool get_pd_info(uint32_t pd_handle, PDCacheValue& info);
    void set_pd_info(uint32_t pd_handle, const PDCacheValue& info);
    void add_resource(uint32_t pd_handle, uint32_t resource_id, const std::string& resource_type);
    void remove_resource(uint32_t pd_handle, uint32_t resource_id, const std::string& resource_type);
    
    // 缓存管理
    void resize(size_t new_size);
    void clear();
    CacheMetrics get_metrics() const;
    Stats get_stats() const;

private:
    void evict(ComponentType type);
    void update_access_order(ComponentType type, uint32_t id);

    std::unique_ptr<RdmaQPCache> qp_cache_;
    std::unique_ptr<RdmaCQCache> cq_cache_;
    std::unique_ptr<RdmaMRCache> mr_cache_;
    std::unique_ptr<RdmaPDCache> pd_cache_;
    mutable std::mutex mutex_;
};