#pragma once

#include "rdma_types.h"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class RdmaCQCache {
public:
  explicit RdmaCQCache(size_t cache_size) : cache_size_(cache_size) {}
  virtual ~RdmaCQCache() = default;

  bool get(uint32_t cq_num, CQValue &info);
  void set(uint32_t cq_num, const CQValue &info);
  void batch_add_completions(uint32_t cq_num,
                             const std::vector<CompletionEntry> &completions);
  std::vector<CompletionEntry> batch_get_completions(uint32_t cq_num,
                                                     uint32_t max_count);

private:
  size_t cache_size_;
  std::unordered_map<uint32_t, CQValue> cache_;
};