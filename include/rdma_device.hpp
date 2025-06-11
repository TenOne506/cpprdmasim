#ifndef RDMA_DEVICE_H
#define RDMA_DEVICE_H

#include <memory>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>
#include <functional>
#include "rdma_cache.h"




// Forward declarations
struct RdmaWorkRequest;
struct RdmaCompletion;

// 添加QP状态枚举
enum class QPState {
    RESET,
    INIT,
    RTR,
    RTS
};

// 添加连接状态枚举
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

// 添加QP属性结构体
struct QPAttributes {
    uint32_t qp_access_flags;
    uint8_t pkey_index;
    uint8_t port_num;
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;
    uint32_t max_recv_sge;
    uint32_t max_rd_atomic;
    uint32_t max_dest_rd_atomic;
};

// 添加连接消息类型枚举
enum class ConnectionMessageType {
    REQ,
    REP,
    RTR,
    RTS,
    REJ
};

// 添加连接消息结构体
struct ConnectionMessage {
    ConnectionMessageType type;
    uint32_t qp_num;
    QPAttributes qp_attr;
    uint32_t timeout_ms;
    uint32_t remote_mr_key;
    uint64_t remote_addr;
    uint16_t remote_lid;
    uint8_t remote_gid[16];
};

// 添加缺失的类型定义
struct RdmaQPInfo {
    uint32_t qp_num;
    uint32_t qp_access_flags;
    uint8_t pkey_index;
    uint8_t port_num;
    uint8_t gid[16];
};

enum class RdmaControlMsgType {
    CONNECT_REQ,
    CONNECT_RESP,
    READY,
    ERROR
};

struct RdmaControlMsg {
    struct {
        RdmaControlMsgType type;
        uint32_t size;
    } header;
    std::vector<uint8_t> payload;
};

// 添加RdmaControlChannel的完整定义
class RdmaControlChannel {
public:
    RdmaControlChannel() = default;
    virtual ~RdmaControlChannel() = default;

    bool start_server(uint16_t port) {
        // TODO: 实现服务器启动逻辑
        return true;
    }

    bool connect_to_server(const std::string& server_ip, uint16_t port) {
        // TODO: 实现客户端连接逻辑
        return true;
    }

    bool send_connect_request(const RdmaQPInfo& qp_info) {
        // TODO: 实现发送连接请求逻辑
        return true;
    }

    bool send_connect_response(const RdmaQPInfo& qp_info, bool accept) {
        // TODO: 实现发送连接响应逻辑
        return true;
    }

    bool send_ready() {
        // TODO: 实现发送就绪消息逻辑
        return true;
    }

    bool send_error(const std::string& error) {
        // TODO: 实现发送错误消息逻辑
        return true;
    }

    bool receive_message(RdmaControlMsg& msg, uint32_t timeout_ms) {
        // TODO: 实现接收消息逻辑
        return true;
    }
};

/**
 * @brief RDMA设备类，模拟RDMA网卡(RNIC)的功能
 * 
 * 该类负责管理RDMA资源，包括队列对(QP)、完成队列(CQ)和内存区域(MR)。
 * 同时提供网络处理线程和缓存系统，用于优化RDMA操作性能。
 */
class RdmaDevice {
public:
    // 添加连接结构体定义
    struct Connection {
        uint32_t qp_num;
        uint32_t cq_num;
        uint32_t mr_key;
        void* buffer;
        size_t buffer_size;
        QPState state;
        uint32_t remote_qp_num;
        uint32_t remote_mr_key;
        void* remote_addr;
        uint16_t remote_lid;
        uint8_t remote_gid[16];
        ConnectionState conn_state;
        std::chrono::system_clock::time_point last_activity;
        size_t bytes_sent;
        size_t bytes_received;
        size_t error_count;
        std::unique_ptr<RdmaControlChannel> control_channel;
    };

    // 添加回调函数类型定义
    using ConnectionCallback = std::function<void(uint32_t, ConnectionState)>;
    using DataCallback = std::function<void(uint32_t, const void*, size_t)>;
    using ErrorCallback = std::function<void(uint32_t, const std::string&)>;

    /**
     * @brief 构造函数，初始化RDMA设备
     * @param max_connections 最大连接数
     */
    RdmaDevice(size_t max_connections = 1024);

    /**
     * @brief 析构函数，清理RDMA设备资源
     */
    ~RdmaDevice();
    
    // 基本资源管理函数
    uint32_t create_qp(uint32_t max_send_wr, uint32_t max_recv_wr);
    uint32_t create_cq(uint32_t max_cqe);
    uint32_t register_mr(void* addr, size_t length);
  

    // 监控接口

private:
   
};

#endif // RDMA_DEVICE_H