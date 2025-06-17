#include "../include/rdma_qp_cache.h"
#include <mutex>

// 可选：用于多线程安全
static std::mutex cache_mutex;

bool RdmaQPCache::get(uint32_t qp_num, QPValue &info) {
  std::lock_guard<std::mutex> lock(cache_mutex);

  auto it = cache_.find(qp_num);
  if (it != cache_.end()) {
    info = it->second;
    return true;
  }
  return false;
}

void RdmaQPCache::set(uint32_t qp_num, const QPValue &info) {
  std::lock_guard<std::mutex> lock(cache_mutex);

  // 如果超过大小，则简单地移除一个（更好的策略是 LRU）
  if (cache_.size() >= cache_size_) {
    // 简单策略：移除第一个
    auto it = cache_.begin();
    if (it != cache_.end()) {
      cache_.erase(it);
    }
  }

  cache_[qp_num] = info;
}
