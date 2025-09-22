#include "../include/rdma_device.h"
#include "../include/rdma_types.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>

using Clock = std::chrono::high_resolution_clock;
using ns = std::chrono::nanoseconds;

struct Stat {
  uint64_t total_ns{0};
  uint64_t avg_ns{0};
  uint64_t p50_ns{0};
  uint64_t p95_ns{0};
  uint64_t p99_ns{0};
  size_t ops{0};
};

static Stat summarize(std::vector<uint64_t> &lat) {
  Stat s{};
  if (lat.empty()) return s;
  std::sort(lat.begin(), lat.end());
  s.ops = lat.size();
  s.total_ns = std::accumulate(lat.begin(), lat.end(), 0ULL);
  s.avg_ns = s.total_ns / s.ops;
  s.p50_ns = lat[(size_t)(s.ops * 0.5)];
  s.p95_ns = lat[(size_t)std::max<size_t>(0, (long)(s.ops * 0.95) - 1)];
  s.p99_ns = lat[(size_t)std::max<size_t>(0, (long)(s.ops * 0.99) - 1)];
  return s;
}

// 生成Zipf分布索引（s>1越偏斜）
static std::vector<size_t> gen_zipf_indices(size_t N, size_t count, double s) {
  std::vector<double> w(N);
  for (size_t i = 1; i <= N; ++i) w[i - 1] = 1.0 / std::pow((double)i, s);
  double sum = std::accumulate(w.begin(), w.end(), 0.0);
  for (double &x : w) x /= sum;
  std::discrete_distribution<size_t> dist(w.begin(), w.end());
  std::mt19937_64 rng{12345};
  std::vector<size_t> idx(count);
  for (size_t i = 0; i < count; ++i) idx[i] = dist(rng);
  return idx;
}

// 创建一组CQ/QP，记录其所属设备指针；前hot_count个钉扎在设备，其余强制溢出
struct CQPair { RdmaDevice* dev; uint32_t cq; uint32_t qp; };
static std::vector<CQPair> create_cqs_qps(RdmaDevice &dev_device, RdmaDevice &dev_overflow,
                                          size_t total, size_t hot_count) {
  std::vector<CQPair> res; res.reserve(total);
  for (size_t i = 0; i < total; ++i) {
    bool hot = (i < hot_count);
    RdmaDevice &d = hot ? dev_device : dev_overflow;
    uint32_t cq = d.create_cq(128);
    uint32_t qp = d.create_qp(32, 32, cq, cq);
    if (cq == 0 || qp == 0) continue;
    d.modify_qp_state(qp, QpState::INIT);
    d.modify_qp_state(qp, QpState::RTR);
    d.modify_qp_state(qp, QpState::RTS);
    res.push_back({&d, cq, qp});
  }
  return res;
}

// 单条轮询 vs 批量轮询
static uint64_t do_send_and_poll(RdmaDevice &dev, uint32_t cq, uint32_t qp,
                                 const void *data, size_t len,
                                 uint32_t batch) {
  RdmaWorkRequest wr{};
  wr.opcode = RdmaOpcode::SEND;
  wr.local_addr = const_cast<void*>(data);
  wr.length = (uint32_t)len;
  wr.signaled = true;
  wr.wr_id = 1;

  auto t0 = Clock::now();
  if (!dev.post_send(qp, wr)) return UINT64_MAX;
  std::vector<CompletionEntry> comps;
  comps.reserve(batch);
  // 批量轮询：一次poll最多取batch个；单条时batch=1
  while (!dev.poll_cq(cq, comps, batch)) {
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }
  auto t1 = Clock::now();
  return std::chrono::duration_cast<ns>(t1 - t0).count();
}

int main() {
  std::cout << "RDMA缓存创新对比测试" << std::endl;
  const int iters = 2000;
  const size_t total_cq = 64;      // 总CQ数量
  const size_t hot_cq = 8;         // 热门CQ数量（钉扎）
  const size_t msg_size = 256;     // 消息大小
  const double zipf_s = 1.2;       // 偏斜程度

  std::string payload(msg_size, 'Z');

  // 配置延迟模型：设备最快；中间缓存中速(1us)；主机最慢(5us)
  // 我们通过两个设备实例来区分热点(设备资源充足)和溢出(资源为0)
  RdmaDevice::set_simulation_mode(true, /*host*/5000, /*device*/0, /*middle*/1000);
  RdmaDevice dev_hot(/*conn*/512, /*qps*/128, /*cqs*/128, /*mrs*/64, /*pds*/32);
  RdmaDevice dev_cold(/*conn*/512, /*qps*/0, /*cqs*/0, /*mrs*/0, /*pds*/0);

  // 构建连接集合：前hot_cq钉扎在设备，其他走缓存/主机
  auto pairs = create_cqs_qps(dev_hot, dev_cold, total_cq, hot_cq);
  if (pairs.size() < total_cq) {
    std::cerr << "资源创建不足: " << pairs.size() << "/" << total_cq << std::endl;
  }

  // 访问序列：Zipf偏斜，更多命中热门CQ
  auto access_idx = gen_zipf_indices(pairs.size(), iters, zipf_s);

  // A) 单条轮询（baseline）
  std::vector<uint64_t> lat_single; lat_single.reserve(iters);
  for (int i = 0; i < iters; ++i) {
    auto &p = pairs[access_idx[i]];
    auto ns = do_send_and_poll(*p.dev, p.cq, p.qp, payload.data(), payload.size(), 1);
    if (ns != UINT64_MAX) lat_single.push_back(ns);
  }
  auto stat_single = summarize(lat_single);
  std::cout << "单条轮询: avg(ns)=" << stat_single.avg_ns
            << ", p50=" << stat_single.p50_ns
            << ", p95=" << stat_single.p95_ns
            << ", p99=" << stat_single.p99_ns
            << ", ops=" << stat_single.ops << std::endl;

  // B) 批量轮询（优化）
  std::vector<uint64_t> lat_batch; lat_batch.reserve(iters);
  const uint32_t batch = 8;
  for (int i = 0; i < iters; ++i) {
    auto &p = pairs[access_idx[i]];
    auto ns = do_send_and_poll(*p.dev, p.cq, p.qp, payload.data(), payload.size(), batch);
    if (ns != UINT64_MAX) lat_batch.push_back(ns);
  }
  auto stat_batch = summarize(lat_batch);
  std::cout << "批量轮询(batch=" << batch << "): avg(ns)=" << stat_batch.avg_ns
            << ", p50=" << stat_batch.p50_ns
            << ", p95=" << stat_batch.p95_ns
            << ", p99=" << stat_batch.p99_ns
            << ", ops=" << stat_batch.ops << std::endl;

  // C) 无钉扎对比：全部走缓存/主机
  RdmaDevice dev_all_cold(/*conn*/512, /*qps*/0, /*cqs*/0, /*mrs*/0, /*pds*/0);
  auto all_cold_pairs = create_cqs_qps(dev_all_cold, dev_all_cold, total_cq, 0);
  auto access_idx2 = gen_zipf_indices(all_cold_pairs.size(), iters, zipf_s);

  std::vector<uint64_t> lat_nohot; lat_nohot.reserve(iters);
  for (int i = 0; i < iters; ++i) {
    auto &p = all_cold_pairs[access_idx2[i]];
    auto ns = do_send_and_poll(*p.dev, p.cq, p.qp, payload.data(), payload.size(), 1);
    if (ns != UINT64_MAX) lat_nohot.push_back(ns);
  }
  auto stat_nohot = summarize(lat_nohot);
  std::cout << "无钉扎(全溢出): avg(ns)=" << stat_nohot.avg_ns
            << ", p50=" << stat_nohot.p50_ns
            << ", p95=" << stat_nohot.p95_ns
            << ", p99=" << stat_nohot.p99_ns
            << ", ops=" << stat_nohot.ops << std::endl;

  // 总结：展示三种策略的收益
  std::cout << "\n=== 策略收益概览 ===" << std::endl;
  std::cout << "单条轮询 vs 批量轮询: 提升倍数="
            << std::fixed << std::setprecision(2)
            << (stat_single.avg_ns > 0 ? (double)stat_single.avg_ns / (double)stat_batch.avg_ns : 0.0)
            << "x (avg延迟降低)" << std::endl;
  std::cout << "钉扎热点 vs 全溢出: 提升倍数="
            << std::fixed << std::setprecision(2)
            << (stat_nohot.avg_ns > 0 ? (double)stat_nohot.avg_ns / (double)stat_single.avg_ns : 0.0)
            << "x (avg延迟降低)" << std::endl;

  return 0;
}
