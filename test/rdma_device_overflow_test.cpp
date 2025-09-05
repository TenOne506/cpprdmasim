#include "../include/rdma_device.h"
#include "../include/rdma_types.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

int main() {
  std::cout << "RDMA 设备资源溢出(缓存)测试" << std::endl;

  // 将设备支持的CQ数量设为0，强制所有CQ进入缓存
  RdmaDevice dev(/*max_connections=*/16, /*max_qps=*/0, /*max_cqs=*/0,
                 /*max_mrs=*/0, /*max_pds=*/0);

  // 创建两个CQ（都会进入缓存）
  uint32_t send_cq = dev.create_cq(16);
  uint32_t recv_cq = dev.create_cq(16);
  if (send_cq == 0 || recv_cq == 0) {
    std::cerr << "创建CQ失败" << std::endl;
    return 1;
  }

  // 创建QP（由于max_qps=0，QP也会进入缓存）
  uint32_t qp = dev.create_qp(8, 8, send_cq, recv_cq);
  if (qp == 0) {
    std::cerr << "创建QP失败" << std::endl;
    return 1;
  }

  // 将QP切换到RTS（当前实现 validate_qp_transition 总是返回true）
  dev.modify_qp_state(qp, QpState::INIT);
  dev.modify_qp_state(qp, QpState::RTR);
  dev.modify_qp_state(qp, QpState::RTS);

  // 准备一个发送缓冲
  const char *msg = "overflow-cache";
  std::vector<char> buf(strlen(msg) + 1);
  std::memcpy(buf.data(), msg, buf.size());

  // 提交一个有信号的发送WR（将生成完成事件加入send_cq）
  RdmaWorkRequest wr;
  wr.opcode = RdmaOpcode::SEND;
  wr.local_addr = buf.data();
  wr.lkey = 0;
  wr.length = static_cast<uint32_t>(buf.size());
  wr.signaled = true;
  wr.wr_id = 42;

  if (!dev.post_send(qp, wr)) {
    std::cerr << "post_send 失败（可能QP不在RTS）" << std::endl;
    return 1;
  }

  // 轮询发送CQ，验证能从缓存CQ拿到完成事件
  std::vector<CompletionEntry> completions;
  bool got = false;
  for (int i = 0; i < 100; ++i) {
    if (dev.poll_cq(send_cq, completions, 1)) {
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!got) {
    std::cerr << "未能从缓存CQ轮询到完成事件" << std::endl;
    return 1;
  }

  if (completions.empty()) {
    std::cerr << "完成事件列表为空" << std::endl;
    return 1;
  }

  const auto &c = completions.front();
  std::cout << "拿到完成事件 wr_id=" << c.wr_id << ", len=" << c.length
            << std::endl;

  // 同样测试接收方向：给该QP提交接收，再投递SEND触发RECV完成
  std::vector<char> recv_buf(64, 0);
  RdmaWorkRequest recv_wr;
  recv_wr.opcode = RdmaOpcode::RECV;
  recv_wr.local_addr = recv_buf.data();
  recv_wr.lkey = 0;
  recv_wr.length = static_cast<uint32_t>(recv_buf.size());
  recv_wr.signaled = true;
  recv_wr.wr_id = 100;

  if (!dev.post_recv(qp, recv_wr)) {
    std::cerr << "post_recv 失败" << std::endl;
    return 1;
  }

  // 再次投递一个SEND以触发RECV完成
  if (!dev.post_send(qp, wr)) {
    std::cerr << "post_send 失败(第二次)" << std::endl;
    return 1;
  }

  completions.clear();
  got = false;
  for (int i = 0; i < 100; ++i) {
    if (dev.poll_cq(recv_cq, completions, 1)) {
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!got) {
    std::cerr << "未能从缓存接收CQ轮询到完成事件" << std::endl;
    return 1;
  }

  std::cout << "测试通过：缓存CQ路径工作正常" << std::endl;
  return 0;
}


