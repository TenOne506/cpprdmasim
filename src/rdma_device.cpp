#include "../include/rdma_device.h"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>
RdmaDevice::RdmaDevice(size_t max_connections, size_t max_qps, size_t max_cqs,
                       size_t max_mrs, size_t max_pds)
    : max_qps_(max_qps), max_cqs_(max_cqs), max_mrs_(max_mrs),
      max_pds_(max_pds), next_qp_num_(1), next_cq_num_(1), next_mr_lkey_(1),
      next_pd_handle_(1), should_stop_(false),
      max_connections_(max_connections) {

  // 初始化缓存系统 - 缓存大小设置为设备资源限制的2倍，作为溢出缓存
  qp_cache_ = std::make_unique<RdmaQPCache>(max_qps * 2);
  cq_cache_ = std::make_unique<RdmaCQCache>(max_cqs * 2);
  mr_cache_ = std::make_unique<RdmaMRCache>(max_mrs * 2);
  pd_cache_ = std::make_unique<RdmaPDCache>(max_pds * 2);

  // 启动网络处理线程
  network_thread_ =
      std::make_unique<std::thread>(&RdmaDevice::network_thread_func, this);
}

// 模拟配置（静态）
std::atomic<bool> RdmaDevice::enable_middle_cache_{true};
std::atomic<uint32_t> RdmaDevice::host_swap_delay_ns_{0};
std::atomic<uint32_t> RdmaDevice::device_delay_ns_{0};
std::atomic<uint32_t> RdmaDevice::middle_delay_ns_{0};

void RdmaDevice::set_simulation_mode(bool enable_middle_cache,
                                     uint32_t host_swap_delay_ns,
                                     uint32_t device_delay_ns,
                                     uint32_t middle_delay_ns) {
  enable_middle_cache_.store(enable_middle_cache, std::memory_order_relaxed);
  host_swap_delay_ns_.store(host_swap_delay_ns, std::memory_order_relaxed);
  device_delay_ns_.store(device_delay_ns, std::memory_order_relaxed);
  middle_delay_ns_.store(middle_delay_ns, std::memory_order_relaxed);
}

static inline void maybe_sleep_ns(uint32_t ns) {
  if (ns > 0) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
  }
}

RdmaDevice::~RdmaDevice() {
  // 停止网络处理线程
  should_stop_ = true;
  if (network_thread_ && network_thread_->joinable()) {
    network_thread_->join();
  }

  // 清理资源
  cleanup_resources();
}

uint32_t RdmaDevice::create_qp(uint32_t /*max_send_wr*/,
                               uint32_t /*max_recv_wr*/, uint32_t send_cq,
                               uint32_t recv_cq) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 验证CQ是否存在
  {
    std::lock_guard<std::mutex> cq_lock(cq_mutex_);
    CQValue send_cq_value;
    CQValue recv_cq_value;
    bool send_cq_exists = cqs_.find(send_cq) != cqs_.end();
    bool recv_cq_exists = cqs_.find(recv_cq) != cqs_.end();
    if (!send_cq_exists || !recv_cq_exists) {
      if (enable_middle_cache_.load(std::memory_order_relaxed)) {
        send_cq_exists = send_cq_exists || cq_cache_->get(send_cq, send_cq_value);
        recv_cq_exists = recv_cq_exists || cq_cache_->get(recv_cq, recv_cq_value);
      } else {
        send_cq_exists = send_cq_exists || (cqs_host_.find(send_cq) != cqs_host_.end());
        recv_cq_exists = recv_cq_exists || (cqs_host_.find(recv_cq) != cqs_host_.end());
      }
    }

    if (!send_cq_exists || !recv_cq_exists) {
      return 0; // 返回0表示创建失败
    }
  }

  uint32_t qp_num = next_qp_num_++;

  // 检查设备资源是否已满
  if (qps_.size() < max_qps_) {
    maybe_sleep_ns(device_delay_ns_.load(std::memory_order_relaxed));
    // 创建新的QP
    QPValue qp_value{};
    qp_value.qp_num = qp_num;
    qp_value.state = QpState::RESET; // RESET state
    qp_value.send_cq = send_cq;      // 设置发送CQ
    qp_value.recv_cq = recv_cq;      // 设置接收CQ
    qp_value.created_time = std::chrono::steady_clock::now();

    // 存储在设备的资源中
    qps_[qp_num] = qp_value;
    return qp_num;
  }

  // 如果设备资源已满
  QPValue qp_value{};
  qp_value.qp_num = qp_num;
  qp_value.state = QpState::RESET; // RESET state
  qp_value.send_cq = send_cq;      // 设置发送CQ
  qp_value.recv_cq = recv_cq;      // 设置接收CQ
  qp_value.created_time = std::chrono::steady_clock::now();

  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    maybe_sleep_ns(middle_delay_ns_.load(std::memory_order_relaxed));
    qp_cache_->set(qp_num, qp_value);
  } else {
    maybe_sleep_ns(host_swap_delay_ns_.load(std::memory_order_relaxed));
    qps_host_[qp_num] = qp_value;
  }
  return qp_num;
}

uint32_t RdmaDevice::create_cq(uint32_t max_cqe) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  uint32_t cq_num = next_cq_num_++;

  // 检查设备资源是否已满
  if (cqs_.size() < max_cqs_) {
    maybe_sleep_ns(device_delay_ns_.load(std::memory_order_relaxed));
    // 创建新的CQ
    CQValue cq_value{};
    cq_value.cq_num = cq_num;
    cq_value.cqe = max_cqe;

    cqs_[cq_num] = cq_value;
    return cq_num;
  }

  // 如果设备资源已满
  CQValue cq_value{};
  cq_value.cq_num = cq_num;
  cq_value.cqe = max_cqe;
  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    maybe_sleep_ns(middle_delay_ns_.load(std::memory_order_relaxed));
    cq_cache_->set(cq_num, cq_value);
  } else {
    maybe_sleep_ns(host_swap_delay_ns_.load(std::memory_order_relaxed));
    cqs_host_[cq_num] = cq_value;
  }
  return cq_num;
}

uint32_t RdmaDevice::register_mr(void *addr, size_t length,
                                 uint32_t access_flags) {
  std::lock_guard<std::mutex> lock(mr_mutex_);

  uint32_t lkey = next_mr_lkey_++;

  // 检查设备资源是否已满
  if (mrs_.size() < max_mrs_) {
    // 创建新的MR
    MRValue mr_value{};
    mr_value.lkey = lkey;
    mr_value.addr = addr;
    mr_value.length = length;
    mr_value.access_flags = access_flags;

    // 存储在设备的资源中
    mrs_[lkey] = mr_value;
    return lkey;
  }

  // 如果设备资源已满，尝试使用缓存
  MRValue mr_value{};
  mr_value.lkey = lkey;
  mr_value.addr = addr;
  mr_value.length = length;
  mr_value.access_flags = access_flags;

  mr_cache_->set(lkey, mr_value);
  return lkey;
}

uint32_t RdmaDevice::create_pd() {
  std::lock_guard<std::mutex> lock(pd_mutex_);

  uint32_t pd_handle = next_pd_handle_++;

  // 检查设备资源是否已满
  if (pds_.size() < max_pds_) {
    // 创建新的PD
    PDValue pd_value{};
    pd_value.pd_handle = pd_handle;

    // 存储在设备的资源中
    pds_[pd_handle] = pd_value;
    return pd_handle;
  }

  // 如果设备资源已满，尝试使用缓存
  PDValue pd_value{};
  pd_value.pd_handle = pd_handle;

  pd_cache_->set(pd_handle, pd_value);
  return pd_handle;
}

bool RdmaDevice::get_qp_info(uint32_t qp_num, QPValue &info) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    info = it->second;
    return true;
  }

  // 如果在设备资源中找不到
  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    return qp_cache_->get(qp_num, info);
  } else {
    auto hit = qps_host_.find(qp_num);
    if (hit != qps_host_.end()) {
      info = hit->second;
      return true;
    }
    return false;
  }
}

bool RdmaDevice::get_cq_info(uint32_t cq_num, CQValue &info) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  // 首先在设备资源中查找
  auto it = cqs_.find(cq_num);
  if (it != cqs_.end()) {
    info = it->second;
    return true;
  }

  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    return cq_cache_->get(cq_num, info);
  } else {
    auto hit = cqs_host_.find(cq_num);
    if (hit != cqs_host_.end()) {
      maybe_sleep_ns(host_swap_delay_ns_.load(std::memory_order_relaxed));
      info = hit->second;
      return true;
    }
    return false;
  }
}

bool RdmaDevice::get_mr_info(uint32_t lkey, MRValue &info) {
  std::lock_guard<std::mutex> lock(mr_mutex_);

  // 首先在设备资源中查找
  auto it = mrs_.find(lkey);
  if (it != mrs_.end()) {
    info = it->second;
    return true;
  }

  // 如果在设备资源中找不到，尝试从缓存中获取
  return mr_cache_->get(lkey, info);
}

void RdmaDevice::cleanup_resources() {
  std::lock_guard<std::mutex> qp_lock(qp_mutex_);
  std::lock_guard<std::mutex> cq_lock(cq_mutex_);
  std::lock_guard<std::mutex> mr_lock(mr_mutex_);
  std::lock_guard<std::mutex> pd_lock(pd_mutex_);

  // 清理设备资源
  qps_.clear();
  cqs_.clear();
  mrs_.clear();
  pds_.clear();

  // 清理缓存
  qp_cache_.reset();
  cq_cache_.reset();
  mr_cache_.reset();
  pd_cache_.reset();
}

void RdmaDevice::network_thread_func() {
  while (!should_stop_) {
    // TODO: 实现网络处理逻辑
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// 资源释放函数
void RdmaDevice::destroy_qp(uint32_t qp_num) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 首先尝试从设备资源中删除
  if (qps_.erase(qp_num) > 0) {
    return;
  }

  // 如果不在设备资源中，尝试从缓存中删除
  QPValue dummy;
  if (qp_cache_->get(qp_num, dummy)) {
    qp_cache_->set(qp_num, QPValue{}); // 使用空值覆盖缓存中的条目
  }
}

void RdmaDevice::destroy_cq(uint32_t cq_num) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  // 首先尝试从设备资源中删除
  if (cqs_.erase(cq_num) > 0) {
    return;
  }

  // 如果不在设备资源中，尝试从缓存中删除
  CQValue dummy;
  if (cq_cache_->get(cq_num, dummy)) {
    cq_cache_->set(cq_num, CQValue{}); // 使用空值覆盖缓存中的条目
  }
}

void RdmaDevice::deregister_mr(uint32_t lkey) {
  std::lock_guard<std::mutex> lock(mr_mutex_);

  // 首先尝试从设备资源中删除
  if (mrs_.erase(lkey) > 0) {
    return;
  }

  // 如果不在设备资源中，尝试从缓存中删除
  MRValue dummy;
  if (mr_cache_->get(lkey, dummy)) {
    mr_cache_->set(lkey, MRValue{}); // 使用空值覆盖缓存中的条目
  }
}

void RdmaDevice::destroy_pd(uint32_t pd_handle) {
  std::lock_guard<std::mutex> lock(pd_mutex_);

  // 首先尝试从设备资源中删除
  if (pds_.erase(pd_handle) > 0) {
    return;
  }

  // 如果不在设备资源中，尝试从缓存中删除
  PDValue dummy;
  if (pd_cache_->get(pd_handle, dummy)) {
    pd_cache_->set(pd_handle, PDValue{}); // 使用空值覆盖缓存中的条目
  }
}

// QP操作函数
bool RdmaDevice::modify_qp_state(uint32_t qp_num, QpState new_state) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    if (!validate_qp_transition(it->second.state, new_state)) {
      return false;
    }
    it->second.state = new_state;
    return true;
  }

  // 如果不在设备资源中
  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    QPValue qp_info;
    if (!qp_cache_->get(qp_num, qp_info)) {
      return false;
    }
    if (!validate_qp_transition(qp_info.state, new_state)) {
      return false;
    }
    qp_info.state = new_state;
    maybe_sleep_ns(middle_delay_ns_.load(std::memory_order_relaxed));
    qp_cache_->set(qp_num, qp_info);
    return true;
  } else {
    auto it_host = qps_host_.find(qp_num);
    if (it_host == qps_host_.end()) return false;
    if (!validate_qp_transition(it_host->second.state, new_state)) {
      return false;
    }
    it_host->second.state = new_state;
    return true;
  }
}

bool RdmaDevice::connect_qp(uint32_t qp_num, const QPValue &remote_info) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    it->second.dest_qp_num = remote_info.qp_num;
    it->second.remote_lid = remote_info.lid;
    it->second.remote_psn = remote_info.psn;
    it->second.remote_gid = remote_info.gid;
    return true;
  }

  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    QPValue qp_info;
    if (!qp_cache_->get(qp_num, qp_info)) {
      return false;
    }
    qp_info.dest_qp_num = remote_info.qp_num;
    qp_info.remote_lid = remote_info.lid;
    qp_info.remote_psn = remote_info.psn;
    qp_info.remote_gid = remote_info.gid;
    qp_cache_->set(qp_num, qp_info);
    return true;
  } else {
    auto it_host = qps_host_.find(qp_num);
    if (it_host == qps_host_.end()) return false;
    it_host->second.dest_qp_num = remote_info.qp_num;
    it_host->second.remote_lid = remote_info.lid;
    it_host->second.remote_psn = remote_info.psn;
    it_host->second.remote_gid = remote_info.gid;
    return true;
  }
}

// 静态映射表，用于存储所有设备中的QP信息
// 键是QP编号，值是指向QPValue的指针和指向RdmaDevice的指针
static std::unordered_map<uint32_t, std::pair<QPValue *, RdmaDevice *>>
    global_qp_map;
static std::mutex global_qp_mutex;

bool RdmaDevice::post_send(uint32_t qp_num, const RdmaWorkRequest &wr) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 获取QP信息
  QPValue qp_info;
  bool found = false;

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    qp_info = it->second;
    found = true;

    // 将QP信息添加到全局映射表
    std::lock_guard<std::mutex> global_lock(global_qp_mutex);
    global_qp_map[qp_num] = std::make_pair(&(it->second), this);
  } else {
    if (enable_middle_cache_.load(std::memory_order_relaxed)) {
      if (qp_cache_->get(qp_num, qp_info)) {
        found = true;
      }
    } else {
      auto it_host = qps_host_.find(qp_num);
      if (it_host != qps_host_.end()) {
        qp_info = it_host->second;
        found = true;
        // 加入全局映射，指向 host 表中的对象
        std::lock_guard<std::mutex> global_lock(global_qp_mutex);
        global_qp_map[qp_num] = std::make_pair(&(it_host->second), this);
      }
    }
  }

  if (!found) {
    return false;
  }

  // 检查QP状态是否为RTS
  if (qp_info.state != QpState::RTS) {
    return false;
  }

  // 创建完成事件
  if (wr.signaled) {
    CompletionEntry completion;
    completion.wr_id = wr.wr_id;
    completion.status = 0; // 成功状态
    completion.opcode = wr.opcode;
    completion.length = wr.length;

    // 将完成事件添加到CQ
    std::lock_guard<std::mutex> cq_lock(cq_mutex_);
    auto cq_it = cqs_.find(qp_info.send_cq);
    if (cq_it != cqs_.end()) {
      cq_it->second.completions.push_back(completion);
      std::cout << "Added completion to device CQ " << qp_info.send_cq
                << std::endl;
    } else {
      CQValue cq_info;
      if (cq_cache_->get(qp_info.send_cq, cq_info)) {
        maybe_sleep_ns(middle_delay_ns_.load(std::memory_order_relaxed));
        cq_info.completions.push_back(completion);
        cq_cache_->set(qp_info.send_cq, cq_info);
        std::cout << "Added completion to cached CQ " << qp_info.send_cq
                  << std::endl;
      } else {
        // 尝试主机交换路径
        auto host_it = cqs_host_.find(qp_info.send_cq);
        if (host_it != cqs_host_.end()) {
          maybe_sleep_ns(host_swap_delay_ns_.load(std::memory_order_relaxed));
          host_it->second.completions.push_back(completion);
          std::cout << "Added completion to host CQ " << qp_info.send_cq
                    << std::endl;
        } else {
          std::cerr << "Failed to find CQ " << qp_info.send_cq
                    << " for completion" << std::endl;
        }
      }
    }
  }

  // 模拟数据传输 - 在实际场景中，这里会通过网络发送数据
  if (wr.opcode == RdmaOpcode::RDMA_WRITE || wr.opcode == RdmaOpcode::SEND) {
    uint32_t dest_qp_num = qp_info.dest_qp_num;

    // 在全局映射表中查找目标QP
    QPValue *dest_qp_ptr = nullptr;
    RdmaDevice *dest_device = nullptr;

    {
      std::lock_guard<std::mutex> global_lock(global_qp_mutex);
      auto dest_it = global_qp_map.find(dest_qp_num);
      if (dest_it != global_qp_map.end()) {
        dest_qp_ptr = dest_it->second.first;
        dest_device = dest_it->second.second;
      }
    }

    // 如果找到目标QP，执行数据传输
    if (dest_qp_ptr && dest_device) {
      // 执行数据复制，即使目标QP当前没有接收缓冲区也要保存数据
      if (dest_qp_ptr->recv_addr != nullptr) {
        size_t copy_size = std::min(wr.length, dest_qp_ptr->recv_length);
        memcpy(dest_qp_ptr->recv_addr, wr.local_addr, copy_size);

        // 创建接收完成事件
        CompletionEntry recv_completion;
        recv_completion.wr_id = 0;  // 简化处理
        recv_completion.status = 0; // 成功状态
        recv_completion.length = copy_size;
        recv_completion.opcode = RdmaOpcode::RECV;

        // 将完成事件添加到接收CQ
        std::lock_guard<std::mutex> dest_cq_lock(dest_device->cq_mutex_);
        auto dest_cq_it = dest_device->cqs_.find(dest_qp_ptr->recv_cq);
        if (dest_cq_it != dest_device->cqs_.end()) {
          dest_cq_it->second.completions.push_back(recv_completion);
          std::cout << "Added receive completion to device CQ "
                    << dest_qp_ptr->recv_cq << std::endl;
        } else {
          CQValue dest_cq_info;
          if (dest_device->cq_cache_->get(dest_qp_ptr->recv_cq, dest_cq_info)) {
            maybe_sleep_ns(middle_delay_ns_.load(std::memory_order_relaxed));
            dest_cq_info.completions.push_back(recv_completion);
            dest_device->cq_cache_->set(dest_qp_ptr->recv_cq, dest_cq_info);
            std::cout << "Added receive completion to cached CQ "
                      << dest_qp_ptr->recv_cq << std::endl;
          } else {
            // 主机交换路径
            auto host_it = dest_device->cqs_host_.find(dest_qp_ptr->recv_cq);
            if (host_it != dest_device->cqs_host_.end()) {
              maybe_sleep_ns(host_swap_delay_ns_.load(std::memory_order_relaxed));
              host_it->second.completions.push_back(recv_completion);
              std::cout << "Added receive completion to host CQ "
                        << dest_qp_ptr->recv_cq << std::endl;
            } else {
              std::cerr << "Failed to find receive CQ " << dest_qp_ptr->recv_cq
                        << " for completion" << std::endl;
            }
          }
        }

        // 清除接收缓冲区信息，表示已经使用过了
        dest_qp_ptr->recv_addr = nullptr;
        dest_qp_ptr->recv_length = 0;
      } else {
        // 如果目标QP没有接收缓冲区，先保存数据
        dest_qp_ptr->pending_data.assign(
            static_cast<const char *>(wr.local_addr),
            static_cast<const char *>(wr.local_addr) + wr.length);
      }
    }
  }

  return true;
}

bool RdmaDevice::post_recv(uint32_t qp_num, const RdmaWorkRequest &wr) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 获取QP信息
  QPValue qp_info;
  bool found = false;
  bool is_cached = false;

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    qp_info = it->second;
    found = true;
    std::cout << "Found QP " << qp_num << " in device resources" << std::endl;
  } else {
    if (enable_middle_cache_.load(std::memory_order_relaxed)) {
      if (qp_cache_->get(qp_num, qp_info)) {
        found = true;
        is_cached = true;
        std::cout << "Found QP " << qp_num << " in cache" << std::endl;
      }
    } else {
      auto it_host = qps_host_.find(qp_num);
      if (it_host != qps_host_.end()) {
        qp_info = it_host->second;
        found = true;
        std::cout << "Found QP " << qp_num << " in host table" << std::endl;
      }
    }
  }

  if (!found) {
    std::cerr << "QP " << qp_num << " not found for post_recv" << std::endl;
    return false;
  }

  // 检查QP状态是否至少为RTR
  if (qp_info.state != QpState::RTR && qp_info.state != QpState::RTS) {
    return false;
  }

  // 保存接收缓冲区信息
  qp_info.recv_addr = wr.local_addr;
  qp_info.recv_length = wr.length;

  // 检查是否有待处理的数据
  if (!qp_info.pending_data.empty()) {
    // 有待处理数据，复制到接收缓冲区
    size_t copy_size =
        std::min(qp_info.pending_data.size(), (unsigned long)wr.length);
    memcpy(wr.local_addr, qp_info.pending_data.data(), copy_size);

    // 创建接收完成事件
    CompletionEntry recv_completion;
    recv_completion.wr_id = wr.wr_id;
    recv_completion.status = 0; // 成功状态
    recv_completion.length = copy_size;
    recv_completion.opcode = RdmaOpcode::RECV;

    // 将完成事件添加到CQ
    std::lock_guard<std::mutex> cq_lock(cq_mutex_);
    auto cq_it = cqs_.find(qp_info.recv_cq);
    if (cq_it != cqs_.end()) {
      cq_it->second.completions.push_back(recv_completion);
    } else {
      CQValue cq_info;
      if (cq_cache_->get(qp_info.recv_cq, cq_info)) {
        maybe_sleep_ns(middle_delay_ns_.load(std::memory_order_relaxed));
        cq_info.completions.push_back(recv_completion);
        cq_cache_->set(qp_info.recv_cq, cq_info);
      }
    }

    // 清除待处理数据和接收缓冲区信息
    qp_info.pending_data.clear();
    qp_info.recv_addr = nullptr;
    qp_info.recv_length = 0;
  }

  // 更新QP信息
  if (it != qps_.end()) {
    it->second = qp_info;
    std::cout << "Updated QP " << qp_num
              << " receive buffer: addr=" << qp_info.recv_addr
              << ", length=" << qp_info.recv_length << std::endl;

    // 将更新后的QP信息添加到全局映射表
    std::lock_guard<std::mutex> global_lock(global_qp_mutex);
    global_qp_map[qp_num] = std::make_pair(&(it->second), this);
    std::cout << "Updated global QP map for QP " << qp_num << std::endl;
  } else {
    if (enable_middle_cache_.load(std::memory_order_relaxed)) {
      qp_cache_->set(qp_num, qp_info);
      std::cout << "Updated cached QP " << qp_num << " receive buffer"
                << std::endl;
    } else {
      qps_host_[qp_num] = qp_info;
      std::cout << "Updated host QP " << qp_num << " receive buffer"
                << std::endl;
    }
  }

  // 检查是否有待处理数据可以立即处理
  if (!qp_info.pending_data.empty() && qp_info.recv_addr) {
    std::cout << "Processing pending data for QP " << qp_num
              << ", size=" << qp_info.pending_data.size() << std::endl;
  }

  return true;
}

// CQ操作函数
bool RdmaDevice::poll_cq(uint32_t cq_num,
                         std::vector<CompletionEntry> &completions,
                         uint32_t max_entries) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  // 首先在设备资源中查找
  auto it = cqs_.find(cq_num);
  if (it != cqs_.end()) {
    if (!it->second.completions.empty()) {
      size_t num_entries = std::min(static_cast<size_t>(max_entries),
                                    it->second.completions.size());
      completions.insert(completions.end(), it->second.completions.begin(),
                         it->second.completions.begin() + num_entries);
      it->second.completions.erase(it->second.completions.begin(),
                                   it->second.completions.begin() +
                                       num_entries);
      return true;
    }
  }

  // 如果不在设备资源中或设备资源中没有完成事件，尝试从缓存/主机获取
  if (enable_middle_cache_.load(std::memory_order_relaxed)) {
    auto cached_completions =
        cq_cache_->batch_get_completions(cq_num, max_entries);
    if (!cached_completions.empty()) {
      completions.insert(completions.end(), cached_completions.begin(),
                         cached_completions.end());
      return true;
    }
  } else {
    auto hit = cqs_host_.find(cq_num);
    if (hit != cqs_host_.end() && !hit->second.completions.empty()) {
      maybe_sleep_ns(host_swap_delay_ns_.load(std::memory_order_relaxed));
      size_t num_entries = std::min(static_cast<size_t>(max_entries),
                                    hit->second.completions.size());
      completions.insert(completions.end(), hit->second.completions.begin(),
                         hit->second.completions.begin() + num_entries);
      hit->second.completions.erase(hit->second.completions.begin(),
                                    hit->second.completions.begin() + num_entries);
      return true;
    }
  }

  return false;
}

bool RdmaDevice::req_notify_cq(uint32_t cq_num, bool /*solicited_only*/) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  // 首先在设备资源中查找
  auto it = cqs_.find(cq_num);
  if (it != cqs_.end()) {
    // TODO: 实现CQ通知机制
    return true;
  }

  // 如果不在设备资源中，检查缓存
  CQValue cq_info;
  if (!cq_cache_->get(cq_num, cq_info)) {
    return false;
  }

  // TODO: 实现CQ通知机制
  return true;
}

// MR操作函数
MRBlock *RdmaDevice::allocate_mr(size_t size, uint32_t access_flags) {
  std::lock_guard<std::mutex> lock(mr_mutex_);

  // TODO: 实现内存分配逻辑
  return nullptr;
}

void RdmaDevice::free_mr(MRBlock *block) {
  if (!block) {
    return;
  }

  std::lock_guard<std::mutex> lock(mr_mutex_);

  // TODO: 实现内存释放逻辑
}

bool RdmaDevice::validate_qp_transition(QpState current_state,
                                        QpState new_state) {
  // TODO: 实现QP状态转换验证逻辑
  return true;
}