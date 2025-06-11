#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

struct MRBlock {
    void* addr;
    size_t size;
    uint32_t lkey;
    uint32_t rkey;
    uint32_t access_flags;
};

struct MRCacheValue {
    uint32_t lkey;
    uint32_t access_flags;
    uint64_t length;
    void* addr;
};

class RdmaMRCache {
public:
    explicit RdmaMRCache(size_t cache_size) : cache_size_(cache_size) {}
    virtual ~RdmaMRCache() = default;

    bool get(uint32_t lkey, MRCacheValue& info);
    void set(uint32_t lkey, const MRCacheValue& info);
    MRBlock* allocate_block(size_t size, uint32_t flags);
    void free_block(MRBlock* block);

private:
    size_t cache_size_;
    std::unordered_map<uint32_t, MRCacheValue> cache_;
}; 