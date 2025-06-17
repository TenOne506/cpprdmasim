#pragma once

#include "rdma_types.h"
#include <cstdint>
#include <memory>
#include <unordered_map>

class RdmaMRCache {
public:
  explicit RdmaMRCache(size_t cache_size) : cache_size_(cache_size) {}
  virtual ~RdmaMRCache() = default;

  bool get(uint32_t lkey, MRValue &info);
  void set(uint32_t lkey, const MRValue &info);
  MRBlock *allocate_block(size_t size, uint32_t flags);
  void free_block(MRBlock *block);

private:
  size_t cache_size_;
  std::unordered_map<uint32_t, MRValue> cache_;
};