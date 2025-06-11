#ifndef RDMA_CACHE_BASE_H
#define RDMA_CACHE_BASE_H

#include <unordered_map>
#include <list>
#include <mutex>
#include <cstdint>
#include <chrono>

template<typename KeyType, typename ValueType>
class RdmaCacheBase {
public:
    enum class CachePolicy {
        LRU,    // Least Recently Used
        MRU,    // Most Recently Used
        FIFO    // First In First Out
    };

    struct CacheMetrics {
        struct HitRate {
            uint64_t hits = 0;
            uint64_t misses = 0;
            float get_rate() const { 
                return (hits + misses) > 0 ? static_cast<float>(hits) / (hits + misses) : 0.0f; 
            }
        } hit_rate;

        struct MemoryUsage {
            size_t original_size = 0;
            size_t compressed_size = 0;
            float get_compression_ratio() const { 
                return original_size > 0 ? static_cast<float>(compressed_size) / original_size : 0.0f; 
            }
        } memory_usage;

        struct AccessTime {
            std::chrono::nanoseconds total_time{0};
            uint64_t access_count = 0;
            float get_average_time() const { 
                return access_count > 0 ? 
                    static_cast<float>(total_time.count()) / access_count : 0.0f; 
            }
        } access_time;
    };

    RdmaCacheBase(size_t max_size, CachePolicy policy = CachePolicy::LRU)
        : max_size_(max_size), current_size_(0), policy_(policy) {}

    virtual ~RdmaCacheBase() = default;

    // 基本缓存操作
    virtual bool contains(const KeyType& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_map_.find(key) != cache_map_.end();
    }

    virtual bool get(const KeyType& key, ValueType& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(key);
        if (it == cache_map_.end()) {
            metrics_.hit_rate.misses++;
            return false;
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        value = it->second;
        auto end_time = std::chrono::high_resolution_clock::now();
        
        metrics_.access_time.total_time += (end_time - start_time);
        metrics_.access_time.access_count++;
        metrics_.hit_rate.hits++;
        
        update_access_order(key);
        return true;
    }

    virtual void put(const KeyType& key, const ValueType& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t value_size = get_value_size(value);
        
        // 检查是否需要驱逐
        while (current_size_ + value_size > max_size_) {
            evict();
        }

        // 更新或插入新值
        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            current_size_ -= get_value_size(it->second);
        }
        
        cache_map_[key] = value;
        current_size_ += value_size;
        update_access_order(key);
    }

    virtual void remove(const KeyType& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            current_size_ -= get_value_size(it->second);
            cache_map_.erase(it);
            access_order_.remove(key);
        }
    }

    virtual void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_map_.clear();
        access_order_.clear();
        current_size_ = 0;
        metrics_ = CacheMetrics();
    }

    virtual void resize(size_t new_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        max_size_ = new_size;
        while (current_size_ > max_size_) {
            evict();
        }
    }

    // 获取缓存指标
    virtual CacheMetrics get_metrics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }

protected:
    // 获取值的大小（子类需要实现）
    virtual size_t get_value_size(const ValueType& value) const = 0;

    // 更新访问顺序
    void update_access_order(const KeyType& key) {
        if (policy_ == CachePolicy::FIFO) return;
        
        access_order_.remove(key);
        if (policy_ == CachePolicy::LRU) {
            access_order_.push_back(key);
        } else if (policy_ == CachePolicy::MRU) {
            access_order_.push_front(key);
        }
    }

    // 驱逐策略
    void evict() {
        if (access_order_.empty()) return;
        
        KeyType key_to_evict;
        if (policy_ == CachePolicy::LRU || policy_ == CachePolicy::FIFO) {
            key_to_evict = access_order_.front();
            access_order_.pop_front();
        } else { // MRU
            key_to_evict = access_order_.back();
            access_order_.pop_back();
        }
        
        auto it = cache_map_.find(key_to_evict);
        if (it != cache_map_.end()) {
            current_size_ -= get_value_size(it->second);
            cache_map_.erase(it);
        }
    }

    size_t max_size_;
    size_t current_size_;
    CachePolicy policy_;
    mutable std::mutex mutex_;
    std::unordered_map<KeyType, ValueType> cache_map_;
    std::list<KeyType> access_order_;
    CacheMetrics metrics_;
};

#endif // RDMA_CACHE_BASE_H 