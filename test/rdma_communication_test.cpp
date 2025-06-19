#include "../include/rdma_device.h"
#include "../include/rdma_types.h"
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

const uint16_t CONTROL_PORT = 5565;
const size_t MSG_SIZE = 1024;
const std::string TEST_MSG = "Hello RDMA!";

// 辅助函数：转换QP状态
bool transition_qp_state(RdmaDevice &device, uint32_t qp_num,
                         const std::vector<QpState> &states) {
  for (const auto &state : states) {
    if (!device.modify_qp_state(qp_num, state)) {
      std::cerr << "Failed to transition QP state to "
                << static_cast<int>(state) << std::endl;
      return false;
    }
  }
  return true;
}

// 设备A（发送方）的处理函数
void device_a_thread() {
  std::cout << "Device A: Starting..." << std::endl;

  // 创建RDMA设备A
  RdmaDevice device_a;
  std::cout << "Device A: Created RDMA device" << std::endl;

  // 创建控制通道
  RdmaControlChannel control_channel;
  std::cout << "Device A: Attempting to start server on port " << CONTROL_PORT
            << std::endl;

  // 尝试多次启动服务器，以防端口被占用
  const int MAX_SERVER_RETRIES = 3;
  bool server_started = false;

  for (int i = 0; i < MAX_SERVER_RETRIES; i++) {
    if (control_channel.start_server(CONTROL_PORT)) {
      server_started = true;
      std::cout
          << "Device A: Successfully started control channel server on port "
          << CONTROL_PORT << std::endl;
      break;
    }
    std::cerr << "Device A: Failed to start server (attempt " << i + 1
              << "), retrying..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (!server_started) {
    std::cerr << "Device A: Failed to start control channel server after "
              << MAX_SERVER_RETRIES << " attempts" << std::endl;
    return;
  }

  // 创建资源
  uint32_t pd_a = device_a.create_pd();
  uint32_t cq_a = device_a.create_cq(16);   // 创建完成队列，深度为16
  uint32_t qp_a = device_a.create_qp(8, 8); // 创建QP，发送和接收队列深度都为8
  std::cout << "Device A: Created resources (PD=" << pd_a << ", CQ=" << cq_a
            << ", QP=" << qp_a << ")" << std::endl;

  // 注册发送内存区域
  void *send_buf = malloc(MSG_SIZE);
  memcpy(send_buf, TEST_MSG.c_str(), TEST_MSG.length() + 1);
  uint32_t send_mr = device_a.register_mr(send_buf, MSG_SIZE, 0x1); // 读写权限
  std::cout << "Device A: Registered send buffer (MR=" << send_mr << ")"
            << std::endl;

  // 注册接收内存区域
  void *recv_buf = malloc(MSG_SIZE);
  memset(recv_buf, 0, MSG_SIZE); // 清空接收缓冲区
  uint32_t recv_mr = device_a.register_mr(recv_buf, MSG_SIZE, 0x1); // 读写权限
  std::cout << "Device A: Registered receive buffer (MR=" << recv_mr << ")"
            << std::endl;

  // 等待设备B连接
  std::cout << "Device A: Waiting for client connection..." << std::endl;

  // 使用超时参数，避免无限等待
  const uint32_t ACCEPT_TIMEOUT_MS = 10000; // 10秒超时
  if (!control_channel.accept_connection(ACCEPT_TIMEOUT_MS)) {
    std::cerr << "Device A: Failed to accept connection within "
              << ACCEPT_TIMEOUT_MS << "ms" << std::endl;
    std::cerr << "Device A: Error: " << control_channel.get_error()
              << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }

  std::cout << "Device A: Client connected from "
            << control_channel.get_peer_address() << ":"
            << control_channel.get_peer_port() << std::endl;

  // 准备QP信息
  QPValue qp_info;
  qp_info.qp_num = qp_a;
  qp_info.lid = 1; // 模拟值
  qp_info.port_num = 1;
  qp_info.qp_access_flags = 0x1; // 远程读写权限
  qp_info.psn = 1000;            // 模拟值
  qp_info.mtu = 1024;
  qp_info.state = QpState::RESET;

  std::cout << "Device A: Sending connect request with QP=" << qp_a
            << std::endl;
  // 发送连接请求
  if (!control_channel.send_connect_request(qp_info)) {
    std::cerr << "Device A: Failed to send connect request: "
              << control_channel.get_error() << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: Connect request sent successfully" << std::endl;

  // 接收连接响应
  std::cout << "Device A: Waiting for connect response..." << std::endl;
  RdmaControlMsg response;
  if (!control_channel.receive_message(response, 5000)) {
    std::cerr << "Device A: Failed to receive connect response: "
              << control_channel.get_error() << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: Received connect response, type="
            << static_cast<int>(response.type) << std::endl;

  if (response.type != RdmaControlMsgType::CONNECT_RESPONSE ||
      !response.accept) {
    std::cerr << "Device A: Connection rejected by Device B" << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: Connection accepted by Device B" << std::endl;

  // 连接QP
  std::cout << "Device A: Connecting QP with remote info (QP="
            << response.qp_info.qp_num << ")" << std::endl;
  if (!device_a.connect_qp(qp_a, response.qp_info)) {
    std::cerr << "Device A: Failed to connect QP" << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: QP connected successfully" << std::endl;

  // 转换QP状态
  std::cout << "Device A: Transitioning QP through states..." << std::endl;
  std::vector<QpState> states = {QpState::INIT, QpState::RTR, QpState::RTS};
  if (!transition_qp_state(device_a, qp_a, states)) {
    std::cerr << "Device A: Failed to transition QP states" << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: QP state transitions completed" << std::endl;

  // 发送就绪消息
  std::cout << "Device A: Sending ready message" << std::endl;
  if (!control_channel.send_ready()) {
    std::cerr << "Device A: Failed to send ready message: "
              << control_channel.get_error() << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: Ready message sent" << std::endl;

  // 接收就绪消息
  std::cout << "Device A: Waiting for ready message from Device B..."
            << std::endl;
  RdmaControlMsg ready_msg;
  if (!control_channel.receive_message(ready_msg, 5000) ||
      ready_msg.type != RdmaControlMsgType::READY) {
    std::cerr << "Device A: Failed to receive ready message: "
              << control_channel.get_error() << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }
  std::cout << "Device A: Received ready message from Device B" << std::endl;

  // 构造发送工作请求
  RdmaWorkRequest send_wr;
  send_wr.opcode = RdmaOpcode::RDMA_WRITE;
  send_wr.local_addr = send_buf;
  send_wr.lkey = send_mr;
  send_wr.length = TEST_MSG.length() + 1;
  send_wr.signaled = true;

  // 发送数据
  std::cout << "Device A: Sending message: \"" << TEST_MSG << "\"" << std::endl;
  if (!device_a.post_send(qp_a, send_wr)) {
    std::cerr << "Device A: Failed to post send request" << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }

  // 轮询完成队列
  std::vector<CompletionEntry> completions;
  while (!device_a.poll_cq(cq_a, completions, 1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cout << "Device A: Message sent successfully" << std::endl;

  // 准备接收缓冲区
  RdmaWorkRequest recv_wr;
  recv_wr.opcode = RdmaOpcode::RECV;
  recv_wr.local_addr = recv_buf;
  recv_wr.lkey = recv_mr;
  recv_wr.length = MSG_SIZE;
  recv_wr.signaled = true;

  // 提交接收请求
  if (!device_a.post_recv(qp_a, recv_wr)) {
    std::cerr << "Device A: Failed to post receive request" << std::endl;
    free(send_buf);
    free(recv_buf);
    return;
  }

  // 等待接收完成
  completions.clear();
  while (!device_a.poll_cq(cq_a, completions, 1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << "Device A: Received response: \"" << (char *)recv_buf << "\""
            << std::endl;

  // 清理资源
  std::cout << "Device A: Cleaning up resources" << std::endl;
  device_a.destroy_qp(qp_a);
  device_a.destroy_cq(cq_a);
  device_a.deregister_mr(send_mr);
  device_a.deregister_mr(recv_mr);
  device_a.destroy_pd(pd_a);
  free(send_buf);
  free(recv_buf);
  std::cout << "Device A: Resources cleaned up" << std::endl;
}

// 设备B（接收方）的处理函数
void device_b_thread() {
  std::cout << "Device B: Starting..." << std::endl;

  // 创建RDMA设备B
  RdmaDevice device_b;
  std::cout << "Device B: Created RDMA device" << std::endl;

  // 创建控制通道
  RdmaControlChannel control_channel;
  std::cout << "Device B: Waiting for server to start..." << std::endl;
  std::this_thread::sleep_for(
      std::chrono::seconds(2)); // 等待服务器启动，增加等待时间

  std::cout << "Device B: Connecting to server at 127.0.0.1:" << CONTROL_PORT
            << std::endl;
  if (!control_channel.connect_to_server("127.0.0.1", CONTROL_PORT)) {
    std::cerr << "Device B: Failed to connect to control channel server: "
              << control_channel.get_error() << std::endl;
    return;
  }
  std::cout << "Device B: Successfully connected to server" << std::endl;

  // 创建资源
  uint32_t pd_b = device_b.create_pd();
  uint32_t cq_b = device_b.create_cq(16);
  uint32_t qp_b = device_b.create_qp(8, 8);
  std::cout << "Device B: Created resources (PD=" << pd_b << ", CQ=" << cq_b
            << ", QP=" << qp_b << ")" << std::endl;

  // 注册接收内存区域
  void *recv_buf = malloc(MSG_SIZE);
  memset(recv_buf, 0, MSG_SIZE); // 清空接收缓冲区
  uint32_t recv_mr = device_b.register_mr(recv_buf, MSG_SIZE, 0x1); // 读写权限
  std::cout << "Device B: Registered receive buffer (MR=" << recv_mr << ")"
            << std::endl;

  // 注册发送内存区域（用于回复）
  void *send_buf = malloc(MSG_SIZE);
  const char *reply = "RDMA Reply!";
  memcpy(send_buf, reply, strlen(reply) + 1);
  uint32_t send_mr = device_b.register_mr(send_buf, MSG_SIZE, 0x1); // 读写权限
  std::cout << "Device B: Registered send buffer (MR=" << send_mr << ")"
            << std::endl;

  // 接收连接请求
  std::cout << "Device B: Waiting for connect request from Device A..."
            << std::endl;
  RdmaControlMsg request;
  if (!control_channel.receive_message(request, 10000)) { // 增加超时时间
    std::cerr << "Device B: Failed to receive connect request: "
              << control_channel.get_error() << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }
  std::cout << "Device B: Received connect request, type="
            << static_cast<int>(request.type)
            << ", QP=" << request.qp_info.qp_num << std::endl;

  // 准备QP信息
  QPValue qp_info;
  qp_info.qp_num = qp_b;
  qp_info.lid = 2; // 模拟值
  qp_info.port_num = 1;
  qp_info.qp_access_flags = 0x1; // 远程读写权限
  qp_info.psn = 2000;            // 模拟值
  qp_info.mtu = 1024;
  qp_info.state = QpState::RESET;

  // 发送连接响应
  std::cout << "Device B: Sending connect response with QP=" << qp_b
            << std::endl;
  if (!control_channel.send_connect_response(qp_info, true)) {
    std::cerr << "Device B: Failed to send connect response: "
              << control_channel.get_error() << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }
  std::cout << "Device B: Connect response sent successfully" << std::endl;

  // 连接QP
  std::cout << "Device B: Connecting QP with remote info (QP="
            << request.qp_info.qp_num << ")" << std::endl;
  if (!device_b.connect_qp(qp_b, request.qp_info)) {
    std::cerr << "Device B: Failed to connect QP" << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }
  std::cout << "Device B: QP connected successfully" << std::endl;

  // 转换QP状态
  std::cout << "Device B: Transitioning QP through states..." << std::endl;
  std::vector<QpState> states = {QpState::INIT, QpState::RTR, QpState::RTS};
  if (!transition_qp_state(device_b, qp_b, states)) {
    std::cerr << "Device B: Failed to transition QP states" << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }
  std::cout << "Device B: QP state transitions completed" << std::endl;

  // 接收就绪消息
  std::cout << "Device B: Waiting for ready message from Device A..."
            << std::endl;
  RdmaControlMsg ready_msg;
  if (!control_channel.receive_message(ready_msg, 5000) ||
      ready_msg.type != RdmaControlMsgType::READY) {
    std::cerr << "Device B: Failed to receive ready message: "
              << control_channel.get_error() << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }
  std::cout << "Device B: Received ready message from Device A" << std::endl;

  // 发送就绪消息
  std::cout << "Device B: Sending ready message" << std::endl;
  if (!control_channel.send_ready()) {
    std::cerr << "Device B: Failed to send ready message: "
              << control_channel.get_error() << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }
  std::cout << "Device B: Ready message sent" << std::endl;

  // 准备接收缓冲区
  RdmaWorkRequest recv_wr;
  recv_wr.opcode = RdmaOpcode::RECV;
  recv_wr.local_addr = recv_buf;
  recv_wr.lkey = recv_mr;
  recv_wr.length = MSG_SIZE;
  recv_wr.signaled = true;

  // 提交接收请求
  if (!device_b.post_recv(qp_b, recv_wr)) {
    std::cerr << "Device B: Failed to post receive request" << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }

  // 等待接收完成
  std::vector<CompletionEntry> completions;
  while (!device_b.poll_cq(cq_b, completions, 1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cout << "Device B: Received message: \"" << (char *)recv_buf << "\""
            << std::endl;

  // 构造发送工作请求
  RdmaWorkRequest send_wr;
  send_wr.opcode = RdmaOpcode::RDMA_WRITE;
  send_wr.local_addr = send_buf;
  send_wr.lkey = send_mr;
  send_wr.length = strlen(reply) + 1;
  send_wr.signaled = true;

  // 发送响应
  std::cout << "Device B: Sending response: \"" << reply << "\"" << std::endl;
  if (!device_b.post_send(qp_b, send_wr)) {
    std::cerr << "Device B: Failed to post send request" << std::endl;
    free(recv_buf);
    free(send_buf);
    return;
  }

  // 轮询完成队列
  completions.clear();
  while (!device_b.poll_cq(cq_b, completions, 1)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::cout << "Device B: Response sent successfully" << std::endl;

  // 清理资源
  std::cout << "Device B: Cleaning up resources" << std::endl;
  device_b.destroy_qp(qp_b);
  device_b.destroy_cq(cq_b);
  device_b.deregister_mr(send_mr);
  device_b.deregister_mr(recv_mr);
  device_b.destroy_pd(pd_b);
  free(send_buf);
  free(recv_buf);
  std::cout << "Device B: Resources cleaned up" << std::endl;
}

// 主函数
int main() {
  // 创建两个线程分别运行设备A和设备B
  std::thread thread_a(device_a_thread);
  std::thread thread_b(device_b_thread);

  // 等待两个线程完成
  thread_a.join();
  thread_b.join();

  return 0;
}