#include "../include/rdma_pd_cache.h"

bool RdmaPDCache::get(uint32_t pd_handle, PDValue &info) {
  auto it = cache_.find(pd_handle);
  if (it != cache_.end()) {
    info = it->second;
    return true;
  }
  return false;
}

void RdmaPDCache::set(uint32_t pd_handle, const PDValue &info) {
  // 如果缓存已满，移除最旧的条目
  if (cache_.size() >= cache_size_ && cache_.find(pd_handle) == cache_.end()) {
    if (!cache_.empty()) {
      cache_.erase(cache_.begin());
    }
  }
  cache_[pd_handle] = info;
}