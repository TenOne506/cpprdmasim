#ifndef RDMA_CONTROL_CHANNEL_H
#define RDMA_CONTROL_CHANNEL_H

#include "rdma_types.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief RDMA控制通道类，用于QP连接建立和控制消息交换
 *
 * 该类提供TCP/IP通信通道，用于在RDMA设备之间交换控制信息，
 * 如QP连接参数、连接请求和响应等。
 */
class RdmaControlChannel {
public:
  /**
   * @brief 构造函数
   */
  RdmaControlChannel();

  /**
   * @brief 析构函数
   */
  virtual ~RdmaControlChannel();

  /**
   * @brief 连接状态枚举
   */
  enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED, ERROR };

  /**
   * @brief 启动服务器监听
   * @param port 监听端口
   * @return 是否成功启动服务器
   */
  bool start_server(uint16_t port);

  /**
   * @brief 接受客户端连接
   * @param timeout_ms 超时时间(毫秒)，0表示不超时
   * @return 是否成功接受连接
   */
  bool accept_connection(uint32_t timeout_ms = 0);

  /**
   * @brief 连接到服务器
   * @param server_ip 服务器IP地址
   * @param port 服务器端口
   * @return 是否成功连接
   */
  bool connect_to_server(const std::string &server_ip, uint16_t port);

  /**
   * @brief 发送连接请求
   * @param qp_info QP信息
   * @return 是否成功发送
   */
  bool send_connect_request(const QPValue &qp_info);

  /**
   * @brief 发送连接响应
   * @param qp_info QP信息
   * @param accept 是否接受连接
   * @return 是否成功发送
   */
  bool send_connect_response(const QPValue &qp_info, bool accept);

  /**
   * @brief 发送就绪消息
   * @return 是否成功发送
   */
  bool send_ready();

  /**
   * @brief 发送错误消息
   * @param error 错误信息
   * @return 是否成功发送
   */
  bool send_error(const std::string &error);

  /**
   * @brief 发送控制消息
   * @param msg 控制消息
   * @return 是否成功发送
   */
  bool send_message(const RdmaControlMsg &msg);

  /**
   * @brief 接收控制消息
   * @param msg 接收到的控制消息
   * @param timeout_ms 超时时间(毫秒)
   * @return 是否成功接收
   */
  bool receive_message(RdmaControlMsg &msg, uint32_t timeout_ms);

  /**
   * @brief 获取当前连接状态
   * @return 连接状态
   */
  ConnectionState get_state() const;

  /**
   * @brief 获取错误信息
   * @return 错误信息
   */
  std::string get_error() const;

  /**
   * @brief 获取对端地址
   * @return 对端IP地址
   */
  std::string get_peer_address() const;

  /**
   * @brief 获取对端端口
   * @return 对端端口
   */
  uint16_t get_peer_port() const;

private:
  int socket_fd_;            // 当前活动的socket文件描述符
  int server_socket_fd_;     // 服务器监听socket
  int client_socket_fd_;     // 客户端连接socket
  ConnectionState state_;    // 连接状态
  std::mutex mutex_;         // 互斥锁
  std::string error_msg_;    // 错误信息
  std::string peer_address_; // 对端地址
  uint16_t peer_port_;       // 对端端口

  /**
   * @brief 关闭连接
   */
  void close_connection();

  /**
   * @brief 序列化控制消息
   * @param msg 控制消息
   * @return 序列化后的字符串
   */
  std::string serialize_message(const RdmaControlMsg &msg);

  /**
   * @brief 反序列化控制消息
   * @param data 序列化的数据
   * @param msg 输出的控制消息
   * @return 是否成功反序列化
   */
  bool deserialize_message(const std::string &data, RdmaControlMsg &msg);
};

#endif // RDMA_CONTROL_CHANNEL_H