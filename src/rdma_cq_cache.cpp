#include "../include/rdma_cq_cache.h"
#include <algorithm>
#include <mutex>

namespace {
std::mutex cq_mutex; // 可选：线程安全（如不需要并发，可去掉）
}

bool RdmaCQCache::get(uint32_t cq_num, CQValue &info) {
  std::lock_guard<std::mutex> lock(cq_mutex);

  auto it = cache_.find(cq_num);
  if (it != cache_.end()) {
    info = it->second;
    return true;
  }
  return false;
}

void RdmaCQCache::set(uint32_t cq_num, const CQValue &info) {
  std::lock_guard<std::mutex> lock(cq_mutex);

  // 简单容量限制策略（非LRU）
  if (cache_.size() >= cache_size_) {
    auto it = cache_.begin();
    if (it != cache_.end()) {
      cache_.erase(it);
    }
  }

  cache_[cq_num] = info;
}

void RdmaCQCache::batch_add_completions(
    uint32_t cq_num, const std::vector<CompletionEntry> &completions) {
  std::lock_guard<std::mutex> lock(cq_mutex);

  auto it = cache_.find(cq_num);
  if (it != cache_.end()) {
    // 添加到已有 CQ 的 completions 列表末尾
    it->second.completions.insert(it->second.completions.end(),
                                  completions.begin(), completions.end());
  } else {
    // 不存在，创建一个新的 CQValue
    CQValue cq;
    cq.cq_num = cq_num;
    cq.completions = completions;
    cache_[cq_num] = std::move(cq);
  }
}

std::vector<CompletionEntry>
RdmaCQCache::batch_get_completions(uint32_t cq_num, uint32_t max_count) {
  std::lock_guard<std::mutex> lock(cq_mutex);

  std::vector<CompletionEntry> result;

  auto it = cache_.find(cq_num);
  if (it != cache_.end()) {
    auto &comps = it->second.completions;

    uint32_t count = std::min<uint32_t>(max_count, comps.size());
    result.insert(result.end(), comps.begin(), comps.begin() + count);

    // 移除这些已消费的完成事件
    comps.erase(comps.begin(), comps.begin() + count);
  }

  return result;
}
