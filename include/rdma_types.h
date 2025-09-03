#ifndef RDMA_TYPES_H
#define RDMA_TYPES_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// RDMA操作类型
enum class RdmaOpcode : uint8_t {
  SEND = 0,
  RECV = 1,
  RDMA_WRITE = 2,
  RDMA_READ = 3,
  ATOMIC_CMP_AND_SWP = 4,
  ATOMIC_FETCH_AND_ADD = 5
};

// QP状态
enum class QpState : uint8_t {
  RESET = 0,
  INIT = 1,
  RTR = 2, // Ready to Receive
  RTS = 3, // Ready to Send
  SQD = 4, // Send Queue Drain
  SQE = 5, // Send Queue Error
  ERR = 6  // Error
};

// 完成队列条目（统一 CompletionEntry 和 RdmaCompletion）
struct CompletionEntry {
  uint64_t wr_id;    // 工作请求ID
  uint32_t status;   // 完成状态
  RdmaOpcode opcode; // 操作类型
  uint32_t length;   // 数据长度
  uint32_t imm_data; // 立即数据

  CompletionEntry()
      : wr_id(0), status(0), opcode(RdmaOpcode::SEND), length(0), imm_data(0) {}
};

// RDMA工作请求结构体
struct RdmaWorkRequest {
  RdmaOpcode opcode; // 操作类型
  void *local_addr;  // 本地内存地址
  uint32_t lkey;     // 本地内存key
  uint32_t length;   // 数据长度
  void *remote_addr; // 远程内存地址（用于RDMA操作）
  uint32_t rkey;     // 远程内存key（用于RDMA操作）
  uint32_t imm_data; // 立即数据（可选）
  bool signaled;     // 是否产生完成事件
  uint64_t wr_id;    // 工作请求ID

  RdmaWorkRequest()
      : opcode(RdmaOpcode::SEND), local_addr(nullptr), lkey(0), length(0),
        remote_addr(nullptr), rkey(0), imm_data(0), signaled(true), wr_id(0) {}
};

// QP值结构体（统一 QPValue 和 RdmaQPInfo）
struct QPValue {
  uint32_t qp_num;                    // 本地 QP 编号
  uint32_t dest_qp_num;               // 对端 QP 编号
  uint16_t lid;                       // 本地 LID
  uint16_t remote_lid;                // 对端 LID
  uint8_t port_num;                   // 使用的端口
  uint32_t qp_access_flags;           // 权限（remote read/write）
  uint32_t psn;                       // 起始PSN
  uint32_t remote_psn;                // 对端起始PSN
  std::array<uint8_t, 16> gid;        // 本地 GID（用于 RoCE）
  std::array<uint8_t, 16> remote_gid; // 对端 GID
  uint32_t mtu;                       // 最大传输单元
  QpState state;                      // 当前状态
  uint32_t send_cq;                   // 发送完成队列
  uint32_t recv_cq;                   // 接收完成队列
  std::chrono::steady_clock::time_point created_time;

  // 用于模拟数据传输的字段
  void *recv_addr;                // 接收缓冲区地址
  uint32_t recv_length;           // 接收缓冲区长度
  std::vector<char> pending_data; // 待处理的数据（当接收缓冲区未准备好时）

  QPValue()
      : qp_num(0), dest_qp_num(0), lid(0), remote_lid(0), port_num(1),
        qp_access_flags(0), psn(0), remote_psn(0), mtu(1024),
        state(QpState::RESET), send_cq(0), recv_cq(0), recv_addr(nullptr),
        recv_length(0) {
    gid.fill(0);
    remote_gid.fill(0);
  }
};

// 内存区域块
struct MRBlock {
  void *addr;
  size_t size;
  uint32_t lkey;
  uint32_t rkey;
  uint32_t access_flags;
};

// 内存区域值
struct MRValue {
  uint32_t lkey;
  uint32_t access_flags;
  uint64_t length;
  void *addr;
};

// 保护域值
struct PDValue {
  uint32_t pd_handle;
  std::unordered_map<std::string, std::vector<uint32_t>> resources;
};

// 完成队列值
struct CQValue {
  uint32_t cq_num;
  uint32_t cqe;
  uint32_t comp_vector;
  std::vector<CompletionEntry> completions;
};

// 控制消息类型
enum class RdmaControlMsgType : uint8_t {
  CONNECT_REQUEST = 0,
  CONNECT_RESPONSE = 1,
  READY = 2,
  ERROR = 3
};

// 控制消息结构体
struct RdmaControlMsg {
  RdmaControlMsgType type; // 消息类型
  QPValue qp_info;         // QP信息
  bool accept;             // 连接响应是否接受（用于CONNECT_RESPONSE）
  std::string error_msg;   // 错误信息（用于ERROR类型）

  RdmaControlMsg() : type(RdmaControlMsgType::CONNECT_REQUEST), accept(false) {}
};

struct RdmaQPInfo {
  uint32_t qp_num;
  uint32_t lid;
  uint32_t qpn;
  uint32_t psn;
  uint32_t rkey;
  uint64_t vaddr;
  // 其他必要的QP信息字段
};
#endif // RDMA_TYPES_H