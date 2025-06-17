#include "include/rdma_device.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>

// 创建一个简单的QPValue用于测试
QPValue create_test_qp_value(uint32_t qp_num) {
  QPValue qp;
  qp.qp_num = qp_num;
  qp.dest_qp_num = 0;
  qp.lid = 1;
  qp.remote_lid = 0;
  qp.port_num = 1;
  qp.qp_access_flags = 0;
  qp.psn = 100;
  qp.remote_psn = 0;

  // 初始化GID (通常是IPv6地址格式)
  std::fill(qp.gid.begin(), qp.gid.end(), 0);
  std::fill(qp.remote_gid.begin(), qp.remote_gid.end(), 0);

  qp.mtu = 1024;
  qp.state = QpState::INIT;

  return qp;
}

// 服务器线程函数
void server_thread_func(uint16_t port, std::atomic<bool> &server_ready,
                        std::atomic<bool> &test_complete) {
  std::cout << "服务器: 启动中..." << std::endl;

  RdmaControlChannel server;

  // 启动服务器
  if (!server.start_server(port)) {
    std::cerr << "服务器: 启动失败: " << server.get_error() << std::endl;
    server_ready = false;
    return;
  }

  std::cout << "服务器: 已启动，等待连接..." << std::endl;
  server_ready = true;

  // 接受连接
  bool connected = false;
  while (!connected && !test_complete) {
    connected = server.accept_connection(100); // 100ms超时
    if (!connected &&
        server.get_state() == RdmaControlChannel::ConnectionState::ERROR) {
      std::cerr << "服务器: 接受连接时出错: " << server.get_error()
                << std::endl;
      return;
    }
  }

  if (!connected) {
    std::cerr << "服务器: 未能接受连接" << std::endl;
    return;
  }

  std::cout << "服务器: 已接受来自 " << server.get_peer_address() << ":"
            << server.get_peer_port() << " 的连接" << std::endl;

  // 等待连接请求
  RdmaControlMsg msg;
  if (!server.receive_message(msg, 5000)) {
    std::cerr << "服务器: 接收消息失败: " << server.get_error() << std::endl;
    return;
  }

  if (msg.type != RdmaControlMsgType::CONNECT_REQUEST) {
    std::cerr << "服务器: 收到意外的消息类型: " << static_cast<int>(msg.type)
              << std::endl;
    return;
  }

  std::cout << "服务器: 收到连接请求，QP号: " << msg.qp_info.qp_num
            << std::endl;

  // 创建服务器QP信息
  QPValue server_qp = create_test_qp_value(1000);
  server_qp.dest_qp_num = msg.qp_info.qp_num;
  server_qp.remote_lid = msg.qp_info.lid;
  server_qp.remote_psn = msg.qp_info.psn;
  server_qp.remote_gid = msg.qp_info.gid;

  // 发送连接响应
  if (!server.send_connect_response(server_qp, true)) {
    std::cerr << "服务器: 发送连接响应失败: " << server.get_error()
              << std::endl;
    return;
  }

  std::cout << "服务器: 已发送连接响应" << std::endl;

  // 等待就绪消息
  if (!server.receive_message(msg, 5000)) {
    std::cerr << "服务器: 接收就绪消息失败: " << server.get_error()
              << std::endl;
    return;
  }

  if (msg.type != RdmaControlMsgType::READY) {
    std::cerr << "服务器: 收到意外的消息类型，期望READY: "
              << static_cast<int>(msg.type) << std::endl;
    return;
  }

  std::cout << "服务器: 收到就绪消息" << std::endl;

  // 发送就绪消息
  if (!server.send_ready()) {
    std::cerr << "服务器: 发送就绪消息失败: " << server.get_error()
              << std::endl;
    return;
  }

  std::cout << "服务器: 已发送就绪消息，连接建立完成" << std::endl;

  // 等待测试完成
  while (!test_complete) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "服务器: 测试完成，关闭中..." << std::endl;
}

// 客户端线程函数
void client_thread_func(const std::string &server_ip, uint16_t port,
                        std::atomic<bool> &server_ready,
                        std::atomic<bool> &test_complete) {
  // 等待服务器准备就绪
  while (!server_ready && !test_complete) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (test_complete) {
    return;
  }

  std::cout << "客户端: 连接到服务器 " << server_ip << ":" << port << std::endl;

  RdmaControlChannel client;

  // 连接到服务器
  if (!client.connect_to_server(server_ip, port)) {
    std::cerr << "客户端: 连接失败: " << client.get_error() << std::endl;
    test_complete = true;
    return;
  }

  std::cout << "客户端: 已连接到服务器" << std::endl;

  // 创建客户端QP信息
  QPValue client_qp = create_test_qp_value(2000);

  // 发送连接请求
  if (!client.send_connect_request(client_qp)) {
    std::cerr << "客户端: 发送连接请求失败: " << client.get_error()
              << std::endl;
    test_complete = true;
    return;
  }

  std::cout << "客户端: 已发送连接请求" << std::endl;

  // 等待连接响应
  RdmaControlMsg msg;
  if (!client.receive_message(msg, 5000)) {
    std::cerr << "客户端: 接收连接响应失败: " << client.get_error()
              << std::endl;
    test_complete = true;
    return;
  }

  if (msg.type != RdmaControlMsgType::CONNECT_RESPONSE) {
    std::cerr << "客户端: 收到意外的消息类型: " << static_cast<int>(msg.type)
              << std::endl;
    test_complete = true;
    return;
  }

  if (!msg.accept) {
    std::cerr << "客户端: 服务器拒绝了连接请求" << std::endl;
    test_complete = true;
    return;
  }

  std::cout << "客户端: 收到连接响应，QP号: " << msg.qp_info.qp_num
            << std::endl;

  // 更新客户端QP信息
  client_qp.dest_qp_num = msg.qp_info.qp_num;
  client_qp.remote_lid = msg.qp_info.lid;
  client_qp.remote_psn = msg.qp_info.psn;
  client_qp.remote_gid = msg.qp_info.gid;

  // 发送就绪消息
  if (!client.send_ready()) {
    std::cerr << "客户端: 发送就绪消息失败: " << client.get_error()
              << std::endl;
    test_complete = true;
    return;
  }

  std::cout << "客户端: 已发送就绪消息" << std::endl;

  // 等待服务器就绪消息
  if (!client.receive_message(msg, 5000)) {
    std::cerr << "客户端: 接收服务器就绪消息失败: " << client.get_error()
              << std::endl;
    test_complete = true;
    return;
  }

  if (msg.type != RdmaControlMsgType::READY) {
    std::cerr << "客户端: 收到意外的消息类型，期望READY: "
              << static_cast<int>(msg.type) << std::endl;
    test_complete = true;
    return;
  }

  std::cout << "客户端: 收到服务器就绪消息，连接建立完成" << std::endl;

  // 测试错误处理
  std::cout << "客户端: 测试错误处理..." << std::endl;
  if (!client.send_error("测试错误消息")) {
    std::cerr << "客户端: 发送错误消息失败: " << client.get_error()
              << std::endl;
  } else {
    std::cout << "客户端: 已发送错误消息" << std::endl;
  }

  // 标记测试完成
  test_complete = true;

  std::cout << "客户端: 测试完成" << std::endl;
}

int main() {
  std::cout << "RDMA控制通道测试程序" << std::endl;

  // 随机选择一个端口号(10000-60000)
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(10000, 60000);
  uint16_t port = dis(gen);

  std::string server_ip = "127.0.0.1";
  std::atomic<bool> server_ready(false);
  std::atomic<bool> test_complete(false);

  // 启动服务器线程
  std::thread server(server_thread_func, port, std::ref(server_ready),
                     std::ref(test_complete));

  // 等待服务器启动
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // 启动客户端线程
  std::thread client(client_thread_func, server_ip, port,
                     std::ref(server_ready), std::ref(test_complete));

  // 等待线程完成
  client.join();
  server.join();

  std::cout << "测试完成" << std::endl;

  return 0;
}