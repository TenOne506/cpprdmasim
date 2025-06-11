#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

struct QPCacheValue {
    uint32_t qp_num;
    uint8_t state;
    uint8_t access_flags;
    uint8_t port_num;
    uint32_t qp_access_flags;
};

class RdmaQPCache {
public:
    explicit RdmaQPCache(size_t cache_size) : cache_size_(cache_size) {}
    virtual ~RdmaQPCache() = default;

    bool get(uint32_t qp_num, QPCacheValue& info);
    void set(uint32_t qp_num, const QPCacheValue& info);

private:
    size_t cache_size_;
    std::unordered_map<uint32_t, QPCacheValue> cache_;
}; 