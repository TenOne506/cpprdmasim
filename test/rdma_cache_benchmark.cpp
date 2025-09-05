#include "../include/rdma_device.h"
#include "../include/rdma_cq_cache.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using Clock = std::chrono::high_resolution_clock;
using ns = std::chrono::nanoseconds;

static uint64_t bench_loop(RdmaDevice &dev, uint32_t cq, uint32_t qp,
                           const void *data, size_t len, int iters) {
  std::vector<char> buf(len);
  std::memcpy(buf.data(), data, len);

  RdmaWorkRequest wr;
  wr.opcode = RdmaOpcode::SEND;
  wr.local_addr = buf.data();
  wr.length = static_cast<uint32_t>(len);
  wr.signaled = true;

  auto t0 = Clock::now();
  for (int i = 0; i < iters; ++i) {
    wr.wr_id = static_cast<uint64_t>(i + 1);
    if (!dev.post_send(qp, wr)) {
      std::cerr << "post_send failed at iter=" << i << std::endl;
      break;
    }
    std::vector<CompletionEntry> comps;
    while (!dev.poll_cq(cq, comps, 1)) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }
  auto t1 = Clock::now();
  return std::chrono::duration_cast<ns>(t1 - t0).count();
}

int main() {
  const int iters = 200;
  const char *msg = "benchmark-msg";
  const size_t len = std::strlen(msg) + 1;

  // 场景1：设备内存足够（无缓存路径）
  RdmaDevice dev_fast(/*max_connections=*/128, /*max_qps=*/8, /*max_cqs=*/8,
                      /*max_mrs=*/8, /*max_pds=*/4);
  uint32_t cq_fast = dev_fast.create_cq(64);
  uint32_t qp_fast = dev_fast.create_qp(8, 8, cq_fast, cq_fast);
  dev_fast.modify_qp_state(qp_fast, QpState::INIT);
  dev_fast.modify_qp_state(qp_fast, QpState::RTR);
  dev_fast.modify_qp_state(qp_fast, QpState::RTS);
  uint64_t fast_ns = bench_loop(dev_fast, cq_fast, qp_fast, msg, len, iters);
  std::cout << "无缓存路径 总耗时(ns)=" << fast_ns
            << ", 平均每次(ns)=" << (fast_ns / iters) << std::endl;

  // 场景2a：设备内存很小，走主机交换（慢路径，无中间缓存）
  RdmaDevice::set_simulation_mode(false /*disable middle cache*/, 5000 /*host ns*/, 0 /*device ns*/, 0 /*middle ns*/);
  RdmaDevice dev_cached(/*max_connections=*/128, /*max_qps=*/0, /*max_cqs=*/0,
                        /*max_mrs=*/0, /*max_pds=*/0);
  uint32_t cq_cached = dev_cached.create_cq(64);
  uint32_t qp_cached = dev_cached.create_qp(8, 8, cq_cached, cq_cached);
  dev_cached.modify_qp_state(qp_cached, QpState::INIT);
  dev_cached.modify_qp_state(qp_cached, QpState::RTR);
  dev_cached.modify_qp_state(qp_cached, QpState::RTS);
  uint64_t host_ns =
      bench_loop(dev_cached, cq_cached, qp_cached, msg, len, iters);
  std::cout << "主机交换(无中间缓存) 总耗时(ns)=" << host_ns
            << ", 平均每次(ns)=" << (host_ns / iters) << std::endl;

  // 场景2b：设备内存很小，但启用中间缓存（中速）
  RdmaDevice::set_simulation_mode(true /*enable middle cache*/, 5000 /*host ns*/, 0 /*device ns*/, 1000 /*middle ns*/);
  RdmaDevice dev_mid(/*max_connections=*/128, /*max_qps=*/0, /*max_cqs=*/0,
                     /*max_mrs=*/0, /*max_pds=*/0);
  uint32_t cq_mid = dev_mid.create_cq(64);
  uint32_t qp_mid = dev_mid.create_qp(8, 8, cq_mid, cq_mid);
  dev_mid.modify_qp_state(qp_mid, QpState::INIT);
  dev_mid.modify_qp_state(qp_mid, QpState::RTR);
  dev_mid.modify_qp_state(qp_mid, QpState::RTS);
  uint64_t mid_ns = bench_loop(dev_mid, cq_mid, qp_mid, msg, len, iters);
  std::cout << "中间缓存路径 总耗时(ns)=" << mid_ns
            << ", 平均每次(ns)=" << (mid_ns / iters) << std::endl;

  // 简单对比
  if (host_ns > mid_ns && mid_ns > fast_ns) {
    std::cout << "结果：主机交换最慢 > 中间缓存 > 设备内存最快三层关系成立。" << std::endl;
  } else {
    std::cout << "结果：层级不明显，请调整延迟参数或迭代次数。" << std::endl;
  }

  return 0;
}


