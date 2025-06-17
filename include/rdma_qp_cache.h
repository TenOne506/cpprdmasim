#pragma once

#include "rdma_types.h"
#include <cstdint>
#include <memory>
#include <unordered_map>

class RdmaQPCache {
public:
  explicit RdmaQPCache(size_t cache_size) : cache_size_(cache_size) {}
  virtual ~RdmaQPCache() = default;

  bool get(uint32_t qp_num, QPValue &info);
  void set(uint32_t qp_num, const QPValue &info);

private:
  size_t cache_size_;
  std::unordered_map<uint32_t, QPValue> cache_;
};