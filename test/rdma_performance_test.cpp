#include "../include/rdma_device.h"
#include "../include/rdma_types.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <numeric>

using Clock = std::chrono::high_resolution_clock;
using ns = std::chrono::nanoseconds;
using ms = std::chrono::milliseconds;

struct PerformanceStats {
    uint64_t total_time_ns;
    uint64_t min_time_ns;
    uint64_t max_time_ns;
    uint64_t avg_time_ns;
    uint64_t p50_time_ns;
    uint64_t p95_time_ns;
    uint64_t p99_time_ns;
    size_t success_count;
    size_t total_operations;
    
    // 通信速率统计
    double throughput_mbps;      // 吞吐量 (MB/s)
    double qps;                  // 每秒查询数
    double avg_latency_us;       // 平均延迟 (微秒)
    size_t total_bytes;          // 总传输字节数
};

class RdmaPerformanceTest {
private:
    static constexpr int DEFAULT_ITERATIONS = 1000;
    static constexpr size_t DEFAULT_MSG_SIZE = 1024;
    static constexpr int DEFAULT_CONCURRENT_CONNECTIONS = 10;
    
    std::vector<uint64_t> latencies_;
    size_t total_bytes_transferred_;
    
public:
    // 测试单次RDMA操作延迟
    uint64_t measure_single_operation(RdmaDevice &dev, uint32_t cq, uint32_t qp,
                                     const void *data, size_t len) {
        std::vector<char> buf(len);
        std::memcpy(buf.data(), data, len);

        RdmaWorkRequest wr;
        wr.opcode = RdmaOpcode::SEND;
        wr.local_addr = buf.data();
        wr.length = static_cast<uint32_t>(len);
        wr.signaled = true;
        wr.wr_id = 1;

        auto t0 = Clock::now();
        if (!dev.post_send(qp, wr)) {
            return UINT64_MAX; // 失败返回最大值
        }
        
        std::vector<CompletionEntry> comps;
        while (!dev.poll_cq(cq, comps, 1)) {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        auto t1 = Clock::now();
        
        total_bytes_transferred_ += len;
        return std::chrono::duration_cast<ns>(t1 - t0).count();
    }

    // 测试吞吐量（批量操作）
    PerformanceStats measure_throughput(RdmaDevice &dev, uint32_t cq, uint32_t qp,
                                       const void *data, size_t len, int iterations) {
        std::cout << "开始吞吐量测试: 消息大小=" << len << " bytes, 迭代次数=" << iterations << std::endl;
        
        latencies_.clear();
        latencies_.reserve(iterations);
        total_bytes_transferred_ = 0;
        
        auto total_start = Clock::now();
        size_t success_count = 0;
        
        for (int i = 0; i < iterations; ++i) {
            uint64_t latency = measure_single_operation(dev, cq, qp, data, len);
            if (latency != UINT64_MAX) {
                latencies_.push_back(latency);
                success_count++;
            }
        }
        
        auto total_end = Clock::now();
        auto total_duration_ns = std::chrono::duration_cast<ns>(total_end - total_start).count();
        
        return calculate_throughput_stats(success_count, iterations, total_duration_ns, len);
    }

    // 测试并发连接性能
    PerformanceStats measure_concurrent_throughput(RdmaDevice &dev, const void *data, 
                                                  size_t len, int iterations, int concurrent_connections) {
        std::cout << "开始并发吞吐量测试: 连接数=" << concurrent_connections 
                  << ", 消息大小=" << len << " bytes, 每连接迭代=" << iterations << std::endl;
        
        latencies_.clear();
        total_bytes_transferred_ = 0;
        
        // 创建多个QP和CQ
        std::vector<std::pair<uint32_t, uint32_t>> connections; // {cq, qp}
        for (int i = 0; i < concurrent_connections; ++i) {
            uint32_t cq = dev.create_cq(64);
            uint32_t qp = dev.create_qp(8, 8, cq, cq);
            if (cq != 0 && qp != 0) {
                dev.modify_qp_state(qp, QpState::INIT);
                dev.modify_qp_state(qp, QpState::RTR);
                dev.modify_qp_state(qp, QpState::RTS);
                connections.push_back({cq, qp});
            }
        }
        
        if (connections.empty()) {
            std::cerr << "无法创建并发连接" << std::endl;
            return PerformanceStats{};
        }
        
        auto total_start = Clock::now();
        size_t total_success = 0;
        
        // 并发执行操作
        for (int iter = 0; iter < iterations; ++iter) {
            for (const auto &conn : connections) {
                uint64_t latency = measure_single_operation(dev, conn.first, conn.second, data, len);
                if (latency != UINT64_MAX) {
                    latencies_.push_back(latency);
                    total_success++;
                }
            }
        }
        
        auto total_end = Clock::now();
        auto total_duration_ns = std::chrono::duration_cast<ns>(total_end - total_start).count();
        
        return calculate_throughput_stats(total_success, iterations * connections.size(), 
                                        total_duration_ns, len);
    }

    // 运行性能测试
    PerformanceStats run_performance_test(RdmaDevice &dev, uint32_t cq, uint32_t qp,
                                        const std::string &test_name, 
                                        const void *data, size_t len,
                                        int iterations = DEFAULT_ITERATIONS) {
        std::cout << "开始测试: " << test_name << " (迭代次数: " << iterations << ")" << std::endl;
        
        latencies_.clear();
        latencies_.reserve(iterations);
        
        size_t success_count = 0;
        
        for (int i = 0; i < iterations; ++i) {
            uint64_t latency = measure_single_operation(dev, cq, qp, data, len);
            if (latency != UINT64_MAX) {
                latencies_.push_back(latency);
                success_count++;
            }
        }
        
        return calculate_stats(success_count, iterations);
    }

    // 计算性能统计
    PerformanceStats calculate_stats(size_t success_count, size_t total_operations) {
        PerformanceStats stats{};
        stats.success_count = success_count;
        stats.total_operations = total_operations;
        
        if (latencies_.empty()) {
            stats.total_time_ns = 0;
            stats.min_time_ns = 0;
            stats.max_time_ns = 0;
            stats.avg_time_ns = 0;
            stats.p50_time_ns = 0;
            stats.p95_time_ns = 0;
            stats.p99_time_ns = 0;
            stats.throughput_mbps = 0;
            stats.qps = 0;
            stats.avg_latency_us = 0;
            stats.total_bytes = 0;
            return stats;
        }
        
        std::sort(latencies_.begin(), latencies_.end());
        
        stats.total_time_ns = std::accumulate(latencies_.begin(), latencies_.end(), 0ULL);
        stats.min_time_ns = latencies_.front();
        stats.max_time_ns = latencies_.back();
        stats.avg_time_ns = stats.total_time_ns / latencies_.size();
        
        // 计算百分位数
        stats.p50_time_ns = latencies_[latencies_.size() * 0.5];
        stats.p95_time_ns = latencies_[latencies_.size() * 0.95];
        stats.p99_time_ns = latencies_[latencies_.size() * 0.99];
        
        // 计算通信速率
        stats.avg_latency_us = stats.avg_time_ns / 1000.0; // 转换为微秒
        stats.total_bytes = total_bytes_transferred_;
        
        return stats;
    }

    // 计算吞吐量统计
    PerformanceStats calculate_throughput_stats(size_t success_count, size_t total_operations, 
                                               uint64_t total_duration_ns, size_t msg_size) {
        PerformanceStats stats = calculate_stats(success_count, total_operations);
        
        if (total_duration_ns > 0 && success_count > 0) {
            // 计算QPS (每秒查询数)
            double duration_seconds = total_duration_ns / 1e9;
            stats.qps = success_count / duration_seconds;
            
            // 计算吞吐量 (MB/s)
            double total_bytes_mb = (success_count * msg_size) / (1024.0 * 1024.0);
            stats.throughput_mbps = total_bytes_mb / duration_seconds;
            
            stats.total_bytes = success_count * msg_size;
        } else {
            stats.qps = 0;
            stats.throughput_mbps = 0;
            stats.total_bytes = 0;
        }
        
        return stats;
    }

    // 打印性能统计
    void print_stats(const PerformanceStats &stats, const std::string &test_name) {
        std::cout << "\n=== " << test_name << " 性能统计 ===" << std::endl;
        std::cout << "成功率: " << stats.success_count << "/" << stats.total_operations 
                  << " (" << std::fixed << std::setprecision(2) 
                  << (100.0 * stats.success_count / stats.total_operations) << "%)" << std::endl;
        
        if (stats.success_count > 0) {
            std::cout << "总耗时: " << stats.total_time_ns << " ns" << std::endl;
            std::cout << "平均延迟: " << stats.avg_time_ns << " ns (" << stats.avg_latency_us << " μs)" << std::endl;
            std::cout << "最小延迟: " << stats.min_time_ns << " ns" << std::endl;
            std::cout << "最大延迟: " << stats.max_time_ns << " ns" << std::endl;
            std::cout << "P50延迟: " << stats.p50_time_ns << " ns" << std::endl;
            std::cout << "P95延迟: " << stats.p95_time_ns << " ns" << std::endl;
            std::cout << "P99延迟: " << stats.p99_time_ns << " ns" << std::endl;
            
            // 通信速率统计
            std::cout << "\n--- 通信速率统计 ---" << std::endl;
            std::cout << "QPS (每秒查询数): " << std::fixed << std::setprecision(2) << stats.qps << std::endl;
            std::cout << "吞吐量: " << std::fixed << std::setprecision(2) << stats.throughput_mbps << " MB/s" << std::endl;
            std::cout << "总传输字节: " << stats.total_bytes << " bytes" << std::endl;
        } else {
            std::cout << "所有操作均失败！" << std::endl;
        }
        std::cout << std::endl;
    }

    // 创建测试设备
    std::pair<uint32_t, uint32_t> create_test_device(RdmaDevice &dev, const std::string &scenario) {
        uint32_t cq = dev.create_cq(64);
        uint32_t qp = dev.create_qp(8, 8, cq, cq);
        
        if (cq == 0 || qp == 0) {
            std::cerr << "创建测试设备失败 (" << scenario << ")" << std::endl;
            return {0, 0};
        }
        
        // 设置QP状态
        dev.modify_qp_state(qp, QpState::INIT);
        dev.modify_qp_state(qp, QpState::RTR);
        dev.modify_qp_state(qp, QpState::RTS);
        
        return {cq, qp};
    }
};

int main() {
    std::cout << "RDMA通信性能测试程序" << std::endl;
    std::cout << "测试三种延迟模型下的RDMA通信速率和并发性能" << std::endl;
    std::cout << "===============================================" << std::endl;

    const int iterations = 1000;
    const int concurrent_connections = 20;
    const char *test_msg = "RDMA性能测试消息";
    const size_t msg_len = std::strlen(test_msg) + 1;
    
    RdmaPerformanceTest tester;
    std::vector<PerformanceStats> all_stats;
    std::vector<std::string> test_names;

    // 测试不同消息大小的吞吐量
    std::vector<size_t> msg_sizes = {64, 256, 1024, 4096, 16384}; // 不同消息大小
    
    std::cout << "\n【测试1】不同消息大小的吞吐量测试" << std::endl;
    std::cout << "===============================================" << std::endl;
    
    // 场景1：设备内存直接访问
    std::cout << "\n--- 设备内存直接访问 ---" << std::endl;
    RdmaDevice::set_simulation_mode(false, 0, 0, 0);
    RdmaDevice dev_fast(128, 8, 8, 8, 4);
    auto [cq_fast, qp_fast] = tester.create_test_device(dev_fast, "设备内存");
    
    if (cq_fast != 0 && qp_fast != 0) {
        for (size_t msg_size : msg_sizes) {
            std::string test_data(msg_size, 'A');
            auto stats = tester.measure_throughput(dev_fast, cq_fast, qp_fast, 
                                                 test_data.c_str(), msg_size, iterations);
            std::string test_name = "设备内存-" + std::to_string(msg_size) + "B";
            tester.print_stats(stats, test_name);
        }
    }

    // 场景2：中间缓存访问
    std::cout << "\n--- 中间缓存访问 ---" << std::endl;
    RdmaDevice::set_simulation_mode(true, 0, 0, 2000);
    RdmaDevice dev_middle(128, 0, 0, 0, 0);
    auto [cq_middle, qp_middle] = tester.create_test_device(dev_middle, "中间缓存");
    
    if (cq_middle != 0 && qp_middle != 0) {
        for (size_t msg_size : msg_sizes) {
            std::string test_data(msg_size, 'B');
            auto stats = tester.measure_throughput(dev_middle, cq_middle, qp_middle, 
                                                 test_data.c_str(), msg_size, iterations);
            std::string test_name = "中间缓存-" + std::to_string(msg_size) + "B";
            tester.print_stats(stats, test_name);
        }
    }

    // 场景3：主机交换访问
    std::cout << "\n--- 主机交换访问 ---" << std::endl;
    RdmaDevice::set_simulation_mode(false, 10000, 0, 0);
    RdmaDevice dev_slow(128, 0, 0, 0, 0);
    auto [cq_slow, qp_slow] = tester.create_test_device(dev_slow, "主机交换");
    
    if (cq_slow != 0 && qp_slow != 0) {
        for (size_t msg_size : msg_sizes) {
            std::string test_data(msg_size, 'C');
            auto stats = tester.measure_throughput(dev_slow, cq_slow, qp_slow, 
                                                 test_data.c_str(), msg_size, iterations);
            std::string test_name = "主机交换-" + std::to_string(msg_size) + "B";
            tester.print_stats(stats, test_name);
        }
    }

    // 测试2：并发连接性能测试
    std::cout << "\n【测试2】并发连接性能测试" << std::endl;
    std::cout << "===============================================" << std::endl;
    
    std::vector<int> connection_counts = {1, 5, 10, 20, 50};
    
    // 设备内存并发测试
    std::cout << "\n--- 设备内存并发测试 ---" << std::endl;
    RdmaDevice::set_simulation_mode(false, 0, 0, 0);
    RdmaDevice dev_concurrent_fast(128, 50, 50, 50, 25);
    
    for (int conn_count : connection_counts) {
        std::string test_data(msg_len, 'D');
        auto stats = tester.measure_concurrent_throughput(dev_concurrent_fast, 
                                                         test_data.c_str(), msg_len, 
                                                         iterations / conn_count, conn_count);
        std::string test_name = "设备内存-" + std::to_string(conn_count) + "连接";
        tester.print_stats(stats, test_name);
        all_stats.push_back(stats);
        test_names.push_back(test_name);
    }

    // 中间缓存并发测试
    std::cout << "\n--- 中间缓存并发测试 ---" << std::endl;
    RdmaDevice::set_simulation_mode(true, 0, 0, 2000);
    RdmaDevice dev_concurrent_middle(128, 0, 0, 0, 0);
    
    for (int conn_count : connection_counts) {
        std::string test_data(msg_len, 'E');
        auto stats = tester.measure_concurrent_throughput(dev_concurrent_middle, 
                                                         test_data.c_str(), msg_len, 
                                                         iterations / conn_count, conn_count);
        std::string test_name = "中间缓存-" + std::to_string(conn_count) + "连接";
        tester.print_stats(stats, test_name);
    }

    // 主机交换并发测试
    std::cout << "\n--- 主机交换并发测试 ---" << std::endl;
    RdmaDevice::set_simulation_mode(false, 10000, 0, 0);
    RdmaDevice dev_concurrent_slow(128, 0, 0, 0, 0);
    
    for (int conn_count : connection_counts) {
        std::string test_data(msg_len, 'F');
        auto stats = tester.measure_concurrent_throughput(dev_concurrent_slow, 
                                                         test_data.c_str(), msg_len, 
                                                         iterations / conn_count, conn_count);
        std::string test_name = "主机交换-" + std::to_string(conn_count) + "连接";
        tester.print_stats(stats, test_name);
    }

    // 性能对比分析
    std::cout << "\n===============================================" << std::endl;
    std::cout << "并发性能对比分析" << std::endl;
    std::cout << "===============================================" << std::endl;
    
    if (!all_stats.empty()) {
        std::cout << std::left << std::setw(20) << "测试场景" 
                  << std::setw(12) << "QPS" 
                  << std::setw(15) << "吞吐量(MB/s)" 
                  << std::setw(12) << "平均延迟(μs)" 
                  << std::setw(10) << "成功率(%)" << std::endl;
        std::cout << std::string(75, '-') << std::endl;
        
        for (size_t i = 0; i < all_stats.size(); ++i) {
            const auto &stats = all_stats[i];
            double success_rate = 100.0 * stats.success_count / stats.total_operations;
            
            std::cout << std::left << std::setw(20) << test_names[i]
                      << std::setw(12) << std::fixed << std::setprecision(1) << stats.qps
                      << std::setw(15) << std::fixed << std::setprecision(2) << stats.throughput_mbps
                      << std::setw(12) << std::fixed << std::setprecision(2) << stats.avg_latency_us
                      << std::setw(10) << std::fixed << std::setprecision(1) << success_rate
                      << std::endl;
        }
    }

    std::cout << "\n测试完成！" << std::endl;
    return 0;
}
