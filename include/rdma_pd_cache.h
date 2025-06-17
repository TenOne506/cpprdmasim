#pragma once

#include "rdma_types.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class RdmaPDCache {
public:
  explicit RdmaPDCache(size_t cache_size) : cache_size_(cache_size) {}
  virtual ~RdmaPDCache() = default;

  bool get(uint32_t pd_handle, PDValue &info);
  void set(uint32_t pd_handle, const PDValue &info);
  void add_resource(uint32_t pd_handle, uint32_t resource_id,
                    const std::string &resource_type);
  void remove_resource(uint32_t pd_handle, uint32_t resource_id,
                       const std::string &resource_type);

private:
  size_t cache_size_;
  std::unordered_map<uint32_t, PDValue> cache_;
};