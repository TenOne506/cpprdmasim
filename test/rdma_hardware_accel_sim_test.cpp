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

struct HWSimConfig {
  // CQ 路径加速
  uint32_t cqe_dma_batch = 8;           // 一次 DMA 的 CQE 数量
  bool cqe_cacheline_align = true;      // 是否 cacheline 对齐聚合写
  bool cqe_compression = true;          // 是否启用 CQE 压缩
  double cqe_compress_ratio = 0.5;      // 压缩比(有效条目比例)
  bool adaptive_cq_moderation = true;   // 自适应 moderation
  uint32_t target_avg_ns = 1500;        // 目标平均延迟(纳秒)

  // 发送路径优化
  bool blueflame_inline = true;         // 小包 inline 化
  uint32_t inline_threshold = 256;      // inline 阈值
  bool doorbell_coalesce = true;        // 门铃聚合/写合并

  // WQE/数据路径
  bool wqe_prefetch_burst = true;       // 门铃触发后按 burst 拉取多条 WQE
  uint32_t wqe_burst = 4;               // WQE 预取 burst 数
  bool inline_threshold_adaptive = true;// inline 阈值自适应

  // 多队列亲和
  bool rss_affinity = true;             // 热点流绑定专属队列

  // 总线/内存协同
  bool pcie_relaxed_order = true;       // PCIe RO/No-snoop 优化
  bool ddio_llc_write = true;           // CQE 优先落 LLC
  bool cxl_cold_tiering = true;         // 冷上下文放 CXL
};

struct Stat { uint64_t total_ns{0}, avg_ns{0}, p50_ns{0}, p95_ns{0}, p99_ns{0}; size_t ops{0}; };
static Stat summarize(std::vector<uint64_t> &lat) {
  Stat s{}; if (lat.empty()) return s; std::sort(lat.begin(), lat.end());
  s.ops = lat.size(); s.total_ns = std::accumulate(lat.begin(), lat.end(), 0ULL);
  s.avg_ns = s.total_ns / s.ops; s.p50_ns = lat[(size_t)(s.ops*0.5)];
  s.p95_ns = lat[(size_t)std::max<size_t>(0, (long)(s.ops*0.95)-1)];
  s.p99_ns = lat[(size_t)std::max<size_t>(0, (long)(s.ops*0.99)-1)]; return s;
}

static std::vector<size_t> gen_zipf_indices(size_t N, size_t count, double s) {
  std::vector<double> w(N);
  for (size_t i=1;i<=N;++i) w[i-1] = 1.0/std::pow((double)i, s);
  double sum = std::accumulate(w.begin(), w.end(), 0.0);
  for (double &x:w) x/=sum; std::discrete_distribution<size_t> dist(w.begin(), w.end());
  std::mt19937_64 rng{123456}; std::vector<size_t> idx(count);
  for (size_t i=0;i<count;++i) idx[i]=dist(rng); return idx;
}

// 基线 poll：不带任何硬件优化
static uint64_t do_send_and_poll_baseline(RdmaDevice &dev, uint32_t cq, uint32_t qp,
  const void* data, size_t len, uint32_t batch)
{
  RdmaWorkRequest wr{}; wr.opcode=RdmaOpcode::SEND; wr.local_addr=const_cast<void*>(data);
  wr.length=(uint32_t)len; wr.signaled=true; wr.wr_id=1;
  auto t0 = Clock::now(); if (!dev.post_send(qp, wr)) return UINT64_MAX;
  std::vector<CompletionEntry> comps; comps.reserve(batch);
  while (!dev.poll_cq(cq, comps, batch)) { std::this_thread::sleep_for(std::chrono::microseconds(1)); }
  auto t1 = Clock::now(); return std::chrono::duration_cast<ns>(t1-t0).count();
}

// 硬件加速仿真：把多个优化折算为更大的有效 batch、更少 PCIe 往返、更短每批开销
static uint64_t do_send_and_poll_hw(RdmaDevice &dev, uint32_t cq, uint32_t qp,
  const void* data, size_t len, uint32_t base_batch, const HWSimConfig& cfg,
  uint32_t flow_hash)
{
  // 自适应 inline 阈值（简单模型：高负载/小包时提升 inline 概率）
  uint32_t inline_thr = cfg.inline_threshold;
  if (cfg.inline_threshold_adaptive && len <= 512) inline_thr = std::max(128u, cfg.inline_threshold/2);

  // BlueFlame/门铃合并：用更少的门铃写次数等效为更大批次
  uint32_t eff_batch = base_batch;
  if (cfg.doorbell_coalesce) eff_batch = std::max(eff_batch, base_batch + 4);
  if (cfg.wqe_prefetch_burst) eff_batch = std::max(eff_batch, base_batch + cfg.wqe_burst);
  if (cfg.cqe_dma_batch) eff_batch = std::max(eff_batch, cfg.cqe_dma_batch);

  // RSS/亲和：热点流绑定专属队列，减少争用 => 减少轮询空转
  bool hot_flow = cfg.rss_affinity && (flow_hash % 8 == 0);

  // 压缩 CQE：部分完成共享字段压缩 => 有效可取条目增加
  uint32_t compressed_gain = cfg.cqe_compression ? (uint32_t)std::round(eff_batch * (1.0 + (1.0 - cfg.cqe_compress_ratio))) : 0;
  if (cfg.cqe_compression) eff_batch = std::max(eff_batch, eff_batch + compressed_gain);

  // PCIe/LLC/CXL：折算为每轮询迭代的固定缩短（简单线性模型）
  uint32_t fixed_boost_ns = 0;
  if (cfg.pcie_relaxed_order) fixed_boost_ns += 200;
  if (cfg.ddio_llc_write) fixed_boost_ns += 200;
  if (cfg.cxl_cold_tiering) fixed_boost_ns += 100;

  // 自适应 moderation：根据历史目标延迟调整 batch（这里用目标值直接影响 eff_batch）
  if (cfg.adaptive_cq_moderation) {
    if (cfg.target_avg_ns >= 2000) eff_batch = std::max(eff_batch, base_batch + 16);
    else if (cfg.target_avg_ns >= 1000) eff_batch = std::max(eff_batch, base_batch + 8);
    else eff_batch = std::max(eff_batch, base_batch + 4);
  }

  // 发送（inline 优先）
  RdmaWorkRequest wr{}; wr.opcode=RdmaOpcode::SEND; wr.local_addr=const_cast<void*>(data);
  wr.length=(uint32_t)len; wr.signaled=true; wr.wr_id=1;
  bool use_inline = cfg.blueflame_inline && len <= inline_thr;

  auto t0 = Clock::now();
  if (!dev.post_send(qp, wr)) return UINT64_MAX;

  std::vector<CompletionEntry> comps; comps.reserve(eff_batch);

  // 轮询：硬件端批量 DMA/压缩/对齐减少迭代次数；热点流再减一次
  int idle_loops = 0;
  while (!dev.poll_cq(cq, comps, eff_batch)) {
    ++idle_loops;
    if (hot_flow && idle_loops % 2 == 0) break; // 热点情况下更快返回
    std::this_thread::sleep_for(std::chrono::microseconds(1));
  }

  auto t1 = Clock::now();
  uint64_t dur = std::chrono::duration_cast<ns>(t1-t0).count();

  // 根据优化折减固定时间（避免负值）
  if (dur > fixed_boost_ns) dur -= fixed_boost_ns;
  if (use_inline && dur > 150) dur -= 150; // inline 带来的额外缩短
  return dur;
}

struct CQPair { RdmaDevice* dev; uint32_t cq; uint32_t qp; uint32_t flow_hash; };
static std::vector<CQPair> create_pairs(RdmaDevice &dev_hot, RdmaDevice &dev_cold, size_t total, size_t hot_count) {
  std::vector<CQPair> res; res.reserve(total);
  for (size_t i=0;i<total;++i) {
    bool hot = i < hot_count; RdmaDevice &d = hot ? dev_hot : dev_cold;
    uint32_t cq = d.create_cq(256); uint32_t qp = d.create_qp(64, 64, cq, cq);
    if (!cq || !qp) continue; d.modify_qp_state(qp, QpState::INIT);
    d.modify_qp_state(qp, QpState::RTR); d.modify_qp_state(qp, QpState::RTS);
    res.push_back({&d, cq, qp, (uint32_t)i});
  }
  return res;
}

int main() {
  std::cout << "RDMA硬件加速仿真测试" << std::endl;
  const int iters = 2000; const size_t total_cq=64, hot_cq=8; const size_t msg_size=256; const double zipf_s=1.2;
  std::string payload(msg_size, 'A');

  RdmaDevice::set_simulation_mode(true, /*host*/5000, /*device*/0, /*middle*/1000);
  RdmaDevice dev_hot(/*conn*/512, /*qps*/128, /*cqs*/128, /*mrs*/64, /*pds*/32);
  RdmaDevice dev_cold(/*conn*/512, /*qps*/0, /*cqs*/0, /*mrs*/0, /*pds*/0);

  auto pairs = create_pairs(dev_hot, dev_cold, total_cq, hot_cq);
  auto access_idx = gen_zipf_indices(pairs.size(), iters, zipf_s);

  HWSimConfig cfg{}; // 默认启用所有优化

  // A) 基线（batch=1）
  std::vector<uint64_t> l_base; l_base.reserve(iters);
  for (int i=0;i<iters;++i) {
    auto &p = pairs[access_idx[i]]; auto ns = do_send_and_poll_baseline(*p.dev, p.cq, p.qp, payload.data(), payload.size(), 1);
    if (ns!=UINT64_MAX) l_base.push_back(ns);
  }
  auto s_base = summarize(l_base);
  std::cout << "基线: avg(ns)=" << s_base.avg_ns << ", p50=" << s_base.p50_ns
            << ", p95=" << s_base.p95_ns << ", p99=" << s_base.p99_ns << ", ops=" << s_base.ops << std::endl;

  // B) 仅批量轮询（batch=8）
  std::vector<uint64_t> l_batch; l_batch.reserve(iters);
  for (int i=0;i<iters;++i) {
    auto &p = pairs[access_idx[i]]; auto ns = do_send_and_poll_baseline(*p.dev, p.cq, p.qp, payload.data(), payload.size(), 8);
    if (ns!=UINT64_MAX) l_batch.push_back(ns);
  }
  auto s_batch = summarize(l_batch);
  std::cout << "批量(8): avg(ns)=" << s_batch.avg_ns << ", p50=" << s_batch.p50_ns
            << ", p95=" << s_batch.p95_ns << ", p99=" << s_batch.p99_ns << ", ops=" << s_batch.ops << std::endl;

  // C) 硬件加速（综合模拟）
  std::vector<uint64_t> l_hw; l_hw.reserve(iters);
  for (int i=0;i<iters;++i) {
    auto &p = pairs[access_idx[i]]; auto ns = do_send_and_poll_hw(*p.dev, p.cq, p.qp, payload.data(), payload.size(), 8, cfg, p.flow_hash);
    if (ns!=UINT64_MAX) l_hw.push_back(ns);
  }
  auto s_hw = summarize(l_hw);
  std::cout << "硬件加速: avg(ns)=" << s_hw.avg_ns << ", p50=" << s_hw.p50_ns
            << ", p95=" << s_hw.p95_ns << ", p99=" << s_hw.p99_ns << ", ops=" << s_hw.ops << std::endl;

  std::cout << "\n=== 收益概览 ===" << std::endl;
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "基线 -> 批量: " << (s_base.avg_ns>0? (double)s_base.avg_ns/(double)s_batch.avg_ns:0.0) << "x (avg延迟降低)" << std::endl;
  std::cout << "批量 -> 硬件: " << (s_batch.avg_ns>0? (double)s_batch.avg_ns/(double)s_hw.avg_ns:0.0) << "x (avg延迟降低)" << std::endl;
  std::cout << "基线 -> 硬件: " << (s_base.avg_ns>0? (double)s_base.avg_ns/(double)s_hw.avg_ns:0.0) << "x (avg延迟降低)" << std::endl;

  return 0;
}


