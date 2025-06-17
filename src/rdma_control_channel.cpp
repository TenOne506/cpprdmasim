#include "../include/rdma_control_channel.h"

RdmaControlChannel::RdmaControlChannel()
    : socket_fd_(-1), server_socket_fd_(-1), client_socket_fd_(-1),
      state_(ConnectionState::DISCONNECTED), peer_port_(0) {}

RdmaControlChannel::~RdmaControlChannel() { close_connection(); }

bool RdmaControlChannel::start_server(uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::DISCONNECTED) {
    return false;
  }

  // 创建服务器socket
  server_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_fd_ < 0) {
    error_msg_ = "Failed to create socket: " + std::string(strerror(errno));
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 设置socket选项，允许地址重用
  int opt = 1;
  if (setsockopt(server_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                 sizeof(opt)) < 0) {
    error_msg_ =
        "Failed to set socket options: " + std::string(strerror(errno));
    close(server_socket_fd_);
    server_socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 绑定地址和端口
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  if (bind(server_socket_fd_, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    error_msg_ = "Failed to bind socket: " + std::string(strerror(errno));
    close(server_socket_fd_);
    server_socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 开始监听连接
  if (listen(server_socket_fd_, 5) < 0) {
    error_msg_ = "Failed to listen on socket: " + std::string(strerror(errno));
    close(server_socket_fd_);
    server_socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 设置为非阻塞模式
  int flags = fcntl(server_socket_fd_, F_GETFL, 0);
  fcntl(server_socket_fd_, F_SETFL, flags | O_NONBLOCK);

  state_ = ConnectionState::CONNECTING;
  return true;
}

bool RdmaControlChannel::accept_connection(uint32_t timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::CONNECTING || server_socket_fd_ < 0) {
    return false;
  }

  struct pollfd pfd;
  pfd.fd = server_socket_fd_;
  pfd.events = POLLIN;

  int poll_result = poll(&pfd, 1, timeout_ms);
  if (poll_result <= 0) {
    // 超时或错误
    if (poll_result < 0) {
      error_msg_ = "Poll error: " + std::string(strerror(errno));
      state_ = ConnectionState::ERROR;
    }
    return false;
  }

  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  client_socket_fd_ = accept(server_socket_fd_, (struct sockaddr *)&client_addr,
                             &client_addr_len);
  if (client_socket_fd_ < 0) {
    error_msg_ = "Failed to accept connection: " + std::string(strerror(errno));
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 保存客户端IP地址
  char client_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
  peer_address_ = client_ip;
  peer_port_ = ntohs(client_addr.sin_port);

  socket_fd_ = client_socket_fd_;
  state_ = ConnectionState::CONNECTED;
  return true;
}

bool RdmaControlChannel::connect_to_server(const std::string &server_ip,
                                           uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != ConnectionState::DISCONNECTED) {
    return false;
  }

  // 创建客户端socket
  socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd_ < 0) {
    error_msg_ = "Failed to create socket: " + std::string(strerror(errno));
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
    close(socket_fd_);
    socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  // 连接到服务器
  if (connect(socket_fd_, (struct sockaddr *)&server_addr,
              sizeof(server_addr)) < 0) {
    error_msg_ = "Connection failed: " + std::string(strerror(errno));
    close(socket_fd_);
    socket_fd_ = -1;
    state_ = ConnectionState::ERROR;
    return false;
  }

  peer_address_ = server_ip;
  peer_port_ = port;
  state_ = ConnectionState::CONNECTED;
  return true;
}

bool RdmaControlChannel::send_connect_request(const QPValue &qp_info) {
  RdmaControlMsg msg;
  msg.type = RdmaControlMsgType::CONNECT_REQUEST;
  msg.qp_info = qp_info;
  return send_message(msg);
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
    return false;
  }

  struct pollfd pfd;
  pfd.fd = socket_fd_;
  pfd.events = POLLIN;

  int poll_result = poll(&pfd, 1, timeout_ms);
  if (poll_result <= 0) {
    // 超时或错误
    if (poll_result < 0) {
      error_msg_ = "Poll error: " + std::string(strerror(errno));
      state_ = ConnectionState::ERROR;
    }
    return false;
  }

  // 接收消息长度
  uint32_t net_msg_len;
  if (recv(socket_fd_, &net_msg_len, sizeof(net_msg_len), 0) !=
      sizeof(net_msg_len)) {
    error_msg_ =
        "Failed to receive message length: " + std::string(strerror(errno));
    state_ = ConnectionState::ERROR;
    return false;
  }

  uint32_t msg_len = ntohl(net_msg_len);

  // 接收消息内容
  std::string serialized_msg;
  serialized_msg.resize(msg_len);

  size_t received = 0;
  while (received < msg_len) {
    ssize_t result =
        recv(socket_fd_, &serialized_msg[received], msg_len - received, 0);
    if (result <= 0) {
      error_msg_ = "Failed to receive message: " + std::string(strerror(errno));
      state_ = ConnectionState::ERROR;
      return false;
    }
    received += result;
  }

  // 反序列化消息
  return deserialize_message(serialized_msg, msg);
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