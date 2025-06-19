#include "../include/rdma_device.h"
#include <stdexcept>

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
                               uint32_t /*max_recv_wr*/) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  uint32_t qp_num = next_qp_num_++;

  // 检查设备资源是否已满
  if (qps_.size() < max_qps_) {
    // 创建新的QP
    QPValue qp_value{};
    qp_value.qp_num = qp_num;
    qp_value.state = QpState::RESET; // RESET state
    qp_value.created_time = std::chrono::steady_clock::now();

    // 存储在设备的资源中
    qps_[qp_num] = qp_value;
    return qp_num;
  }

  // 如果设备资源已满，尝试使用缓存
  QPValue qp_value{};
  qp_value.qp_num = qp_num;
  qp_value.state = QpState::RESET; // RESET state
  qp_value.created_time = std::chrono::steady_clock::now();

  qp_cache_->set(qp_num, qp_value);
  return qp_num;
}

uint32_t RdmaDevice::create_cq(uint32_t max_cqe) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  uint32_t cq_num = next_cq_num_++;

  // 检查设备资源是否已满
  if (cqs_.size() < max_cqs_) {
    // 创建新的CQ
    CQValue cq_value{};
    cq_value.cq_num = cq_num;
    cq_value.cqe = max_cqe;

    cqs_[cq_num] = cq_value;
    return cq_num;
  }

  // 如果设备资源已满，尝试使用缓存
  CQValue cq_value{};
  cq_value.cq_num = cq_num;
  cq_value.cqe = max_cqe;

  cq_cache_->set(cq_num, cq_value);
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

  // 如果在设备资源中找不到，尝试从缓存中获取
  return qp_cache_->get(qp_num, info);
}

bool RdmaDevice::get_cq_info(uint32_t cq_num, CQValue &info) {
  std::lock_guard<std::mutex> lock(cq_mutex_);

  // 首先在设备资源中查找
  auto it = cqs_.find(cq_num);
  if (it != cqs_.end()) {
    info = it->second;
    return true;
  }

  // 如果在设备资源中找不到，尝试从缓存中获取
  return cq_cache_->get(cq_num, info);
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

  // 如果不在设备资源中，尝试在缓存中修改
  QPValue qp_info;
  if (!qp_cache_->get(qp_num, qp_info)) {
    return false;
  }

  if (!validate_qp_transition(qp_info.state, new_state)) {
    return false;
  }

  qp_info.state = new_state;
  qp_cache_->set(qp_num, qp_info);
  return true;
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

  // 如果不在设备资源中，尝试在缓存中修改
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
}

bool RdmaDevice::post_send(uint32_t qp_num, const RdmaWorkRequest & /*wr*/) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    // TODO: 实现发送逻辑
    return true;
  }

  // 如果不在设备资源中，尝试在缓存中处理
  QPValue qp_info;
  if (!qp_cache_->get(qp_num, qp_info)) {
    return false;
  }

  // TODO: 实现发送逻辑
  return true;
}

bool RdmaDevice::post_recv(uint32_t qp_num, const RdmaWorkRequest & /*wr*/) {
  std::lock_guard<std::mutex> lock(qp_mutex_);

  // 首先在设备资源中查找
  auto it = qps_.find(qp_num);
  if (it != qps_.end()) {
    // TODO: 实现接收逻辑
    return true;
  }

  // 如果不在设备资源中，尝试在缓存中处理
  QPValue qp_info;
  if (!qp_cache_->get(qp_num, qp_info)) {
    return false;
  }

  // TODO: 实现接收逻辑
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
    if (it->second.completions.empty()) {
      return false;
    }

    size_t num_entries = std::min(static_cast<size_t>(max_entries),
                                  it->second.completions.size());
    completions.insert(completions.end(), it->second.completions.begin(),
                       it->second.completions.begin() + num_entries);
    it->second.completions.erase(it->second.completions.begin(),
                                 it->second.completions.begin() + num_entries);
    return true;
  }

  // 如果不在设备资源中，尝试从缓存中获取
  return cq_cache_->batch_get_completions(cq_num, max_entries).size() > 0;
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