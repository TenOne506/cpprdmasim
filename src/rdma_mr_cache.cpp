#include "../include/rdma_mr_cache.h"

bool RdmaMRCache::get(uint32_t mr_handle, MRValue &info) {
  auto it = cache_.find(mr_handle);
  if (it != cache_.end()) {
    info = it->second;
    return true;
  }
  return false;
}

void RdmaMRCache::set(uint32_t mr_handle, const MRValue &info) {
  // 如果缓存已满，移除最旧的条目
  if (cache_.size() >= cache_size_ && cache_.find(mr_handle) == cache_.end()) {
    if (!cache_.empty()) {
      cache_.erase(cache_.begin());
    }
  }
  cache_[mr_handle] = info;
}