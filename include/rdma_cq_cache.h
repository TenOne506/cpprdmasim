#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct CompletionEntry {
    uint64_t wr_id;
    uint32_t status;
    uint32_t opcode;
    uint32_t byte_len;
    uint32_t imm_data;
};

struct CQCacheValue {
    uint32_t cq_num;
    uint32_t cqe;
    uint32_t comp_vector;
    std::vector<CompletionEntry> completions;
};

class RdmaCQCache {
public:
    explicit RdmaCQCache(size_t cache_size) : cache_size_(cache_size) {}
    virtual ~RdmaCQCache() = default;

    bool get(uint32_t cq_num, CQCacheValue& info);
    void set(uint32_t cq_num, const CQCacheValue& info);
    void batch_add_completions(uint32_t cq_num, const std::vector<CompletionEntry>& completions);
    std::vector<CompletionEntry> batch_get_completions(uint32_t cq_num, uint32_t max_count);

private:
    size_t cache_size_;
    std::unordered_map<uint32_t, CQCacheValue> cache_;
}; 