#include "../include/rdma_control_channel.h"
#include <iostream>
#include <thread>

RdmaControlChannel::RdmaControlChannel()
    : socket_fd_(-1), server_socket_fd_(-1), client_socket_fd_(-1),
      state_(ConnectionState::DISCONNECTED), peer_port_(0) {}

RdmaControlChannel::~RdmaControlChannel() { close_connection(); }

bool RdmaControlChannel::start_server(uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::cout << "Starting control channel server on port " << port << std::endl;

  if (state_ != ConnectionState::DISCONNECTED) {
    error_msg_ = "Cannot start server: Invalid state " +
                 std::to_string(static_cast<int>(state_));
    std::cerr << error_msg_ << std::endl;
    return false;
  }

  // 检查是否有旧的socket需要关闭
  if (server_socket_fd_ >= 0) {
    std::cout << "Closing existing server socket: " << server_socket_fd_
              << std::endl;
    close(server_socket_fd_);
    server_socket_fd_ = -1;
  }

  // 创建服务器socket
  server_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_fd_ < 0) {
    error_msg_ = "Failed to create socket: " + std::string(strerror(errno));
    std::cerr << error_msg_ << std::endl;
    state_ = ConnectionState::ERROR;
    return false;
  }
  std::cout << "Created server socket: " << server_socket_fd_ << std::endl;

  // 设置socket选项，允许地址重用
  int opt = 1;
  if (setsockopt(server_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                 sizeof(opt)) < 0) {
    error_msg_ =
        "Failed to set socket options: " + std::string(strerror(errno));
    std::cerr << error_msg_ << std::endl;
    close(server_socket_fd_);
    server_socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }
  std::cout << "Set SO_REUSEADDR option" << std::endl;

  // 绑定地址和端口
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  std::cout << "Binding to port " << port << " (INADDR_ANY)..." << std::endl;
  if (bind(server_socket_fd_, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    error_msg_ = "Failed to bind socket: " + std::string(strerror(errno));
    std::cerr << error_msg_ << std::endl;

    // 检查端口是否已被占用
    if (errno == EADDRINUSE) {
      std::cerr << "Port " << port << " is already in use. Try another port."
                << std::endl;
    }

    close(server_socket_fd_);
    server_socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }
  std::cout << "Successfully bound to port " << port << std::endl;

  // 开始监听连接
  std::cout << "Starting to listen for connections..." << std::endl;
  if (listen(server_socket_fd_, 5) < 0) {
    error_msg_ = "Failed to listen on socket: " + std::string(strerror(errno));
    std::cerr << error_msg_ << std::endl;
    close(server_socket_fd_);
    server_socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 保持阻塞模式以确保可靠连接
  state_ = ConnectionState::CONNECTING;
  std::cout << "Server successfully started on port " << port << std::endl;
  return true;
}

bool RdmaControlChannel::accept_connection(uint32_t timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::CONNECTING || server_socket_fd_ < 0) {
    std::cerr << "Invalid state for accepting connection. State: "
              << static_cast<int>(state_) << ", Socket: " << server_socket_fd_
              << std::endl;
    return false;
  }

  std::cout << "Waiting for client connection..." << std::endl;

  // 设置超时时间
  const int MAX_RETRIES = 5;
  const int RETRY_INTERVAL_MS = 1000;
  int remaining_timeout = timeout_ms;

  for (int retry = 0; retry < MAX_RETRIES; ++retry) {
    struct pollfd pfd;
    pfd.fd = server_socket_fd_;
    pfd.events = POLLIN;

    int poll_timeout =
        (timeout_ms == 0)
            ? -1
            : (remaining_timeout > RETRY_INTERVAL_MS ? RETRY_INTERVAL_MS
                                                     : remaining_timeout);

    int poll_result = poll(&pfd, 1, poll_timeout);
    if (poll_result < 0) {
      std::cerr << "Poll error (attempt " << retry + 1
                << "): " << strerror(errno) << std::endl;
      continue;
    } else if (poll_result == 0) {
      std::cout << "Waiting for connection (attempt " << retry + 1 << ")..."
                << std::endl;
      if (timeout_ms > 0) {
        remaining_timeout -= poll_timeout;
        if (remaining_timeout <= 0) {
          std::cerr << "Connection timeout after " << timeout_ms << "ms"
                    << std::endl;
          return false;
        }
      }
      continue;
    }

    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    client_socket_fd_ = accept(
        server_socket_fd_, (struct sockaddr *)&client_addr, &client_addr_len);
    if (client_socket_fd_ < 0) {
      std::cerr << "Accept failed (attempt " << retry + 1
                << "): " << strerror(errno) << std::endl;
      continue;
    }

    // 连接成功
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    std::cout << "Client connected from " << client_ip << ":"
              << ntohs(client_addr.sin_port) << std::endl;

    // 保存客户端IP地址
    peer_address_ = client_ip;
    peer_port_ = ntohs(client_addr.sin_port);

    socket_fd_ = client_socket_fd_;
    state_ = ConnectionState::CONNECTED;
    return true;
  }

  // 如果执行到这里，说明所有重试都失败了
  error_msg_ = "Failed to accept connection after " +
               std::to_string(MAX_RETRIES) + " attempts";
  std::cerr << error_msg_ << std::endl;
  state_ = ConnectionState::ERROR;
  return false;
}

bool RdmaControlChannel::connect_to_server(const std::string &server_ip,
                                           uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::DISCONNECTED) {
    std::cerr << "Invalid state for connecting to server. State: "
              << static_cast<int>(state_) << std::endl;
    return false;
  }

  std::cout << "Connecting to server at " << server_ip << ":" << port
            << std::endl;

  // 创建客户端socket
  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    error_msg_ = "Failed to create socket: " + std::string(strerror(errno));
    std::cerr << error_msg_ << std::endl;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 设置服务器地址
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
    error_msg_ = "Invalid address: " + server_ip;
    std::cerr << error_msg_ << std::endl;
    close(socket_fd_);
    socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 添加连接重试机制
  const int MAX_RETRIES = 5;
  const int RETRY_INTERVAL_MS = 1000;

  for (int retry = 0; retry < MAX_RETRIES; ++retry) {
    std::cout << "Attempting to connect (attempt " << retry + 1 << ")..."
              << std::endl;

    if (connect(socket_fd_, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == 0) {
      // 连接成功
      std::cout << "Successfully connected to " << server_ip << ":" << port
                << std::endl;
      peer_address_ = server_ip;
      peer_port_ = port;
      state_ = ConnectionState::CONNECTED;
      return true;
    }

    // 连接失败，记录错误并重试
    error_msg_ = "Connection attempt " + std::to_string(retry + 1) +
                 " failed: " + std::string(strerror(errno));
    std::cerr << error_msg_ << std::endl;

    // 最后一次尝试失败
    if (retry == MAX_RETRIES - 1) {
      close(socket_fd_);
      socket_fd_ = -1;
      state_ = ConnectionState::ERROR;
      return false;
    }

    // 等待一段时间后重试
    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));

    // 重新创建socket
    close(socket_fd_);
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
      error_msg_ = "Failed to recreate socket: " + std::string(strerror(errno));
      std::cerr << error_msg_ << std::endl;
      state_ = ConnectionState::ERROR;
      return false;
    }
  }

  // 如果执行到这里，说明所有重试都失败了
  error_msg_ =
      "Failed to connect after " + std::to_string(MAX_RETRIES) + " attempts";
  std::cerr << error_msg_ << std::endl;
  state_ = ConnectionState::ERROR;
  return false;
}

bool RdmaControlChannel::send_connect_request(const QPValue &qp_info) {
  std::cout << "Sending connect request with QP number: " << qp_info.qp_num
            << std::endl;
  RdmaControlMsg msg;
  msg.type = RdmaControlMsgType::CONNECT_REQUEST;
  msg.qp_info = qp_info;
  bool result = send_message(msg);
  if (result) {
    std::cout << "Connect request sent successfully" << std::endl;
  } else {
    std::cerr << "Failed to send connect request: " << error_msg_ << std::endl;
  }
  return result;
}

bool RdmaControlChannel::send_connect_response(const QPValue &qp_info,
                                               bool accept) {
  RdmaControlMsg msg;
  msg.type = RdmaControlMsgType::CONNECT_RESPONSE;
  msg.qp_info = qp_info;
  msg.accept = accept;
  return send_message(msg);
}

bool RdmaControlChannel::send_ready() {
  RdmaControlMsg msg;
  msg.type = RdmaControlMsgType::READY;
  return send_message(msg);
}

bool RdmaControlChannel::send_error(const std::string &error) {
  RdmaControlMsg msg;
  msg.type = RdmaControlMsgType::ERROR;
  msg.error_msg = error;
  bool result = send_message(msg);
  state_ = ConnectionState::ERROR;
  return result;
}

bool RdmaControlChannel::send_message(const RdmaControlMsg &msg) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::CONNECTED || socket_fd_ < 0) {
    return false;
  }

  // 序列化消息
  std::string serialized_msg = serialize_message(msg);

  // 发送消息长度
  uint32_t msg_len = serialized_msg.size();
  uint32_t net_msg_len = htonl(msg_len);

  if (send(socket_fd_, &net_msg_len, sizeof(net_msg_len), 0) !=
      sizeof(net_msg_len)) {
    error_msg_ =
        "Failed to send message length: " + std::string(strerror(errno));
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 发送消息内容
  ssize_t sent = 0;
  while (sent < msg_len) {
    ssize_t result =
        send(socket_fd_, serialized_msg.c_str() + sent, msg_len - sent, 0);
    if (result < 0) {
      error_msg_ = "Failed to send message: " + std::string(strerror(errno));
      state_ = ConnectionState::ERROR;
      return false;
    }
    sent += result;
  }

  return true;
}

bool RdmaControlChannel::receive_message(RdmaControlMsg &msg,
                                         uint32_t timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::CONNECTED || socket_fd_ < 0) {
    error_msg_ =
        "Cannot receive message: Socket not connected or invalid state";
    std::cerr << error_msg_ << " (state=" << static_cast<int>(state_)
              << ", socket_fd=" << socket_fd_ << ")" << std::endl;
    return false;
  }

  std::cout << "Waiting to receive message (timeout: " << timeout_ms << "ms)..."
            << std::endl;

  struct pollfd pfd;
  pfd.fd = socket_fd_;
  pfd.events = POLLIN;

  int poll_result = poll(&pfd, 1, timeout_ms);
  if (poll_result <= 0) {
    // 超时或错误
    if (poll_result < 0) {
      error_msg_ = "Poll error: " + std::string(strerror(errno));
      std::cerr << error_msg_ << std::endl;
      state_ = ConnectionState::ERROR;
    } else {
      error_msg_ = "Receive timeout after " + std::to_string(timeout_ms) + "ms";
      std::cerr << error_msg_ << std::endl;
    }
    return false;
  }

  std::cout << "Data available for reading" << std::endl;

  // 接收消息长度
  uint32_t net_msg_len;
  ssize_t recv_result = recv(socket_fd_, &net_msg_len, sizeof(net_msg_len), 0);
  if (recv_result != sizeof(net_msg_len)) {
    error_msg_ = "Failed to receive message length: ";
    if (recv_result < 0) {
      error_msg_ += std::string(strerror(errno));
    } else if (recv_result == 0) {
      error_msg_ += "Connection closed by peer";
    } else {
      error_msg_ += "Incomplete data (received " + std::to_string(recv_result) +
                    " bytes, expected " + std::to_string(sizeof(net_msg_len)) +
                    " bytes)";
    }
    std::cerr << error_msg_ << std::endl;
    state_ = ConnectionState::ERROR;
    return false;
  }

  uint32_t msg_len = ntohl(net_msg_len);
  std::cout << "Message length received: " << msg_len << " bytes" << std::endl;

  // 验证消息长度的合理性
  if (msg_len > 4096 || msg_len == 0) {
    error_msg_ = "Invalid message length: " + std::to_string(msg_len);
    std::cerr << error_msg_ << std::endl;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 接收消息内容
  std::string serialized_msg;
  serialized_msg.resize(msg_len);

  size_t received = 0;
  // 为正文接收实现超时控制：按剩余时间分段 poll + recv
  auto start_time = std::chrono::steady_clock::now();
  while (received < msg_len) {
    int poll_timeout = -1; // -1 表示无限等待
    if (timeout_ms > 0) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time)
              .count();
      int remaining = static_cast<int>(timeout_ms) - static_cast<int>(elapsed);
      if (remaining <= 0) {
        error_msg_ = "Receive timeout while reading message body";
        std::cerr << error_msg_ << std::endl;
        return false;
      }
      // 每次轮询不超过 1000ms，避免长时间阻塞难以响应取消
      poll_timeout = remaining > 1000 ? 1000 : remaining;
    }

    struct pollfd pfd_body;
    pfd_body.fd = socket_fd_;
    pfd_body.events = POLLIN;
    int pr = poll(&pfd_body, 1, poll_timeout);
    if (pr <= 0) {
      if (pr < 0) {
        error_msg_ = "Poll error while receiving body: " +
                     std::string(strerror(errno));
        std::cerr << error_msg_ << std::endl;
      } else {
        error_msg_ = "Receive timeout while waiting for body data";
        std::cerr << error_msg_ << std::endl;
      }
      state_ = ConnectionState::ERROR;
      return false;
    }

    ssize_t result =
        recv(socket_fd_, &serialized_msg[received], msg_len - received, 0);
    if (result <= 0) {
      error_msg_ = "Failed to receive message content: ";
      if (result < 0) {
        error_msg_ += std::string(strerror(errno));
      } else {
        error_msg_ += "Connection closed by peer";
      }
      std::cerr << error_msg_ << std::endl;
      state_ = ConnectionState::ERROR;
      return false;
    }
    received += result;
    std::cout << "Received " << result << " bytes, total " << received << "/"
              << msg_len << std::endl;
  }

  std::cout << "Message content fully received, deserializing..." << std::endl;

  // 反序列化消息
  bool deserialize_result = deserialize_message(serialized_msg, msg);
  if (deserialize_result) {
    std::cout << "Message successfully deserialized, type="
              << static_cast<int>(msg.type) << std::endl;
  } else {
    std::cerr << "Failed to deserialize message: " << error_msg_ << std::endl;
  }
  return deserialize_result;
}

RdmaControlChannel::ConnectionState RdmaControlChannel::get_state() const {
  return state_;
}

std::string RdmaControlChannel::get_error() const { return error_msg_; }

std::string RdmaControlChannel::get_peer_address() const {
  return peer_address_;
}

uint16_t RdmaControlChannel::get_peer_port() const { return peer_port_; }

void RdmaControlChannel::close_connection() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (client_socket_fd_ >= 0) {
    close(client_socket_fd_);
    client_socket_fd_ = -1;
  }

  if (server_socket_fd_ >= 0) {
    close(server_socket_fd_);
    server_socket_fd_ = -1;
  }

  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }

  state_ = ConnectionState::DISCONNECTED;
}

std::string RdmaControlChannel::serialize_message(const RdmaControlMsg &msg) {
  std::string result;

  // 1. 序列化消息类型
  uint8_t type = static_cast<uint8_t>(msg.type);
  result.append(reinterpret_cast<char *>(&type), sizeof(type));

  // 2. 序列化QPValue结构体
  // qp_num
  result.append(reinterpret_cast<const char *>(&msg.qp_info.qp_num),
                sizeof(msg.qp_info.qp_num));
  // dest_qp_num
  result.append(reinterpret_cast<const char *>(&msg.qp_info.dest_qp_num),
                sizeof(msg.qp_info.dest_qp_num));
  // lid
  result.append(reinterpret_cast<const char *>(&msg.qp_info.lid),
                sizeof(msg.qp_info.lid));
  // remote_lid
  result.append(reinterpret_cast<const char *>(&msg.qp_info.remote_lid),
                sizeof(msg.qp_info.remote_lid));
  // port_num
  result.append(reinterpret_cast<const char *>(&msg.qp_info.port_num),
                sizeof(msg.qp_info.port_num));
  // qp_access_flags
  result.append(reinterpret_cast<const char *>(&msg.qp_info.qp_access_flags),
                sizeof(msg.qp_info.qp_access_flags));
  // psn
  result.append(reinterpret_cast<const char *>(&msg.qp_info.psn),
                sizeof(msg.qp_info.psn));
  // remote_psn
  result.append(reinterpret_cast<const char *>(&msg.qp_info.remote_psn),
                sizeof(msg.qp_info.remote_psn));
  // gid (16 bytes)
  result.append(reinterpret_cast<const char *>(msg.qp_info.gid.data()),
                msg.qp_info.gid.size());
  // remote_gid (16 bytes)
  result.append(reinterpret_cast<const char *>(msg.qp_info.remote_gid.data()),
                msg.qp_info.remote_gid.size());
  // mtu
  result.append(reinterpret_cast<const char *>(&msg.qp_info.mtu),
                sizeof(msg.qp_info.mtu));
  // state
  uint8_t state = static_cast<uint8_t>(msg.qp_info.state);
  result.append(reinterpret_cast<char *>(&state), sizeof(state));

  // 3. 序列化accept标志
  uint8_t accept = msg.accept ? 1 : 0;
  result.append(reinterpret_cast<char *>(&accept), sizeof(accept));

  // 4. 序列化error_msg
  uint32_t error_len = msg.error_msg.length();
  result.append(reinterpret_cast<char *>(&error_len), sizeof(error_len));
  if (error_len > 0) {
    result.append(msg.error_msg);
  }

  return result;
}

bool RdmaControlChannel::deserialize_message(const std::string &data,
                                             RdmaControlMsg &msg) {
  if (data.empty()) {
    error_msg_ = "Empty data for deserialization";
    return false;
  }

  size_t offset = 0;

  // 1. 反序列化消息类型
  if (offset + sizeof(uint8_t) > data.size()) {
    error_msg_ = "Insufficient data for message type";
    return false;
  }
  uint8_t type = *reinterpret_cast<const uint8_t *>(data.data() + offset);
  msg.type = static_cast<RdmaControlMsgType>(type);
  offset += sizeof(uint8_t);

  // 2. 反序列化QPValue结构体
  // qp_num
  if (offset + sizeof(msg.qp_info.qp_num) > data.size()) {
    error_msg_ = "Insufficient data for qp_num";
    return false;
  }
  msg.qp_info.qp_num =
      *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.qp_num);

  // dest_qp_num
  if (offset + sizeof(msg.qp_info.dest_qp_num) > data.size()) {
    error_msg_ = "Insufficient data for dest_qp_num";
    return false;
  }
  msg.qp_info.dest_qp_num =
      *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.dest_qp_num);

  // lid
  if (offset + sizeof(msg.qp_info.lid) > data.size()) {
    error_msg_ = "Insufficient data for lid";
    return false;
  }
  msg.qp_info.lid = *reinterpret_cast<const uint16_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.lid);

  // remote_lid
  if (offset + sizeof(msg.qp_info.remote_lid) > data.size()) {
    error_msg_ = "Insufficient data for remote_lid";
    return false;
  }
  msg.qp_info.remote_lid =
      *reinterpret_cast<const uint16_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.remote_lid);

  // port_num
  if (offset + sizeof(msg.qp_info.port_num) > data.size()) {
    error_msg_ = "Insufficient data for port_num";
    return false;
  }
  msg.qp_info.port_num =
      *reinterpret_cast<const uint8_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.port_num);

  // qp_access_flags
  if (offset + sizeof(msg.qp_info.qp_access_flags) > data.size()) {
    error_msg_ = "Insufficient data for qp_access_flags";
    return false;
  }
  msg.qp_info.qp_access_flags =
      *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.qp_access_flags);

  // psn
  if (offset + sizeof(msg.qp_info.psn) > data.size()) {
    error_msg_ = "Insufficient data for psn";
    return false;
  }
  msg.qp_info.psn = *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.psn);

  // remote_psn
  if (offset + sizeof(msg.qp_info.remote_psn) > data.size()) {
    error_msg_ = "Insufficient data for remote_psn";
    return false;
  }
  msg.qp_info.remote_psn =
      *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.remote_psn);

  // gid (16 bytes)
  if (offset + msg.qp_info.gid.size() > data.size()) {
    error_msg_ = "Insufficient data for gid";
    return false;
  }
  memcpy(msg.qp_info.gid.data(), data.data() + offset, msg.qp_info.gid.size());
  offset += msg.qp_info.gid.size();

  // remote_gid (16 bytes)
  if (offset + msg.qp_info.remote_gid.size() > data.size()) {
    error_msg_ = "Insufficient data for remote_gid";
    return false;
  }
  memcpy(msg.qp_info.remote_gid.data(), data.data() + offset,
         msg.qp_info.remote_gid.size());
  offset += msg.qp_info.remote_gid.size();

  // mtu
  if (offset + sizeof(msg.qp_info.mtu) > data.size()) {
    error_msg_ = "Insufficient data for mtu";
    return false;
  }
  msg.qp_info.mtu = *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(msg.qp_info.mtu);

  // state
  if (offset + sizeof(uint8_t) > data.size()) {
    error_msg_ = "Insufficient data for state";
    return false;
  }
  uint8_t state = *reinterpret_cast<const uint8_t *>(data.data() + offset);
  msg.qp_info.state = static_cast<QpState>(state);
  offset += sizeof(uint8_t);

  // 3. 反序列化accept标志
  if (offset + sizeof(uint8_t) > data.size()) {
    error_msg_ = "Insufficient data for accept flag";
    return false;
  }
  uint8_t accept = *reinterpret_cast<const uint8_t *>(data.data() + offset);
  msg.accept = (accept != 0);
  offset += sizeof(uint8_t);

  // 4. 反序列化error_msg
  if (offset + sizeof(uint32_t) > data.size()) {
    error_msg_ = "Insufficient data for error_msg length";
    return false;
  }
  uint32_t error_len =
      *reinterpret_cast<const uint32_t *>(data.data() + offset);
  offset += sizeof(uint32_t);

  if (error_len > 0) {
    if (offset + error_len > data.size()) {
      error_msg_ = "Insufficient data for error_msg content";
      return false;
    }
    msg.error_msg = data.substr(offset, error_len);
  } else {
    msg.error_msg.clear();
  }

  return true;
}