#ifndef RDMA_DEVICE_H
#define RDMA_DEVICE_H

#include "rdma_cache.h"
#include "rdma_control_channel.h"
#include "rdma_cq_cache.h"
#include "rdma_mr_cache.h"
#include "rdma_pd_cache.h"
#include "rdma_qp_cache.h"
#include "rdma_types.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Forward declarations
struct RdmaWorkRequest;
struct RdmaCompletion;
struct QPValue;
struct CQValue;
struct RdmaQPInfo;

/**
 * @brief RDMA设备类，模拟RDMA网卡(RNIC)的功能
 *
 * 该类负责管理RDMA资源，包括队列对(QP)、完成队列(CQ)和内存区域(MR)。
 * 同时提供网络处理线程和缓存系统，用于优化RDMA操作性能。
 */
class RdmaDevice {
public:
  /**
   * @brief 构造函数，初始化RDMA设备
   * @param max_connections 最大连接数
   * @param max_qps 设备支持的最大QP数量
   * @param max_cqs 设备支持的最大CQ数量
   * @param max_mrs 设备支持的最大MR数量
   * @param max_pds 设备支持的最大PD数量
   */
  RdmaDevice(size_t max_connections = 1024, size_t max_qps = 256,
             size_t max_cqs = 256, size_t max_mrs = 1024, size_t max_pds = 64);

  /**
   * @brief 析构函数，清理RDMA设备资源
   */
  ~RdmaDevice();

  // 基本资源管理函数
  uint32_t create_qp(uint32_t max_send_wr, uint32_t max_recv_wr,
                     uint32_t send_cq, uint32_t recv_cq);
  uint32_t create_cq(uint32_t max_cqe);
  uint32_t register_mr(void *addr, size_t length, uint32_t access_flags);
  uint32_t create_pd();

  // QP操作函数
  bool modify_qp_state(uint32_t qp_num, QpState new_state);
  bool connect_qp(uint32_t qp_num, const QPValue &remote_info);
  bool post_send(uint32_t qp_num, const RdmaWorkRequest &wr);
  bool post_recv(uint32_t qp_num, const RdmaWorkRequest &wr);

  // CQ操作函数
  bool poll_cq(uint32_t cq_num, std::vector<CompletionEntry> &completions,
               uint32_t max_entries);
  bool req_notify_cq(uint32_t cq_num, bool solicited_only);

  // MR操作函数
  MRBlock *allocate_mr(size_t size, uint32_t access_flags);
  void free_mr(MRBlock *block);

  // 资源释放函数
  void destroy_qp(uint32_t qp_num);
  void destroy_cq(uint32_t cq_num);
  void deregister_mr(uint32_t lkey);
  void destroy_pd(uint32_t pd_handle);

  // 监控接口
  bool get_qp_info(uint32_t qp_num, QPValue &info);
  bool get_cq_info(uint32_t cq_num, CQValue &info);
  bool get_mr_info(uint32_t lkey, MRValue &info);

private:
  // 设备自己的资源
  std::unordered_map<uint32_t, QPValue> qps_;
  std::unordered_map<uint32_t, CQValue> cqs_;
  std::unordered_map<uint32_t, MRValue> mrs_;
  std::unordered_map<uint32_t, PDValue> pds_;

  // 资源容量限制
  size_t max_qps_;
  size_t max_cqs_;
  size_t max_mrs_;
  size_t max_pds_;

  // 缓存系统 - 当设备自己的资源不足时使用
  std::unique_ptr<RdmaQPCache> qp_cache_;
  std::unique_ptr<RdmaCQCache> cq_cache_;
  std::unique_ptr<RdmaMRCache> mr_cache_;
  std::unique_ptr<RdmaPDCache> pd_cache_;

  // 资源计数器
  std::atomic<uint32_t> next_qp_num_;
  std::atomic<uint32_t> next_cq_num_;
  std::atomic<uint32_t> next_mr_lkey_;
  std::atomic<uint32_t> next_pd_handle_;

  // 互斥锁
  std::mutex qp_mutex_;
  std::mutex cq_mutex_;
  std::mutex mr_mutex_;
  std::mutex pd_mutex_;

  // 网络处理线程
  std::unique_ptr<std::thread> network_thread_;
  std::atomic<bool> should_stop_;

  // 设备配置
  size_t max_connections_;

  // 内部辅助函数
  void network_thread_func();
  bool validate_qp_transition(QpState current_state, QpState new_state);
  void cleanup_resources();
};

#endif // RDMA_DEVICE_H