#include "../include/rdma_device.h"
#include <cassert>
#include <functional>
#include <iostream>
#include <vector>

// 测试辅助宏
#define TEST_ASSERT(condition, message)                                        \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "Assertion failed: " << message << std::endl;               \
      std::cerr << "File: " << __FILE__ << ", Line: " << __LINE__              \
                << std::endl;                                                  \
      return false;                                                            \
    }                                                                          \
  } while (0)

// 测试保护域的创建和销毁
bool test_pd_operations() {
  std::cout << "\nTesting Protection Domain Operations..." << std::endl;

  RdmaDevice device;

  // 创建PD
  uint32_t pd = device.create_pd();
  TEST_ASSERT(pd != 0, "Failed to create protection domain");
  std::cout << "Created PD: " << pd << std::endl;

  // 销毁PD
  device.destroy_pd(pd);
  std::cout << "Destroyed PD: " << pd << std::endl;

  // 尝试销毁不存在的PD
  device.destroy_pd(999);
  std::cout << "Successfully detected invalid PD destruction attempt"
            << std::endl;

  return true;
}

// 测试完成队列的创建和销毁
bool test_cq_operations() {
  std::cout << "\nTesting Completion Queue Operations..." << std::endl;

  RdmaDevice device;

  // 创建CQ
  uint32_t cq = device.create_cq(16);
  TEST_ASSERT(cq != 0, "Failed to create completion queue");
  std::cout << "Created CQ: " << cq << " with depth 16" << std::endl;

  // 销毁CQ
  device.destroy_cq(cq);
  std::cout << "Destroyed CQ: " << cq << std::endl;

  // 尝试创建无效深度的CQ
  uint32_t invalid_cq = device.create_cq(0);
  TEST_ASSERT(invalid_cq == 0, "Creating CQ with invalid depth should fail");
  std::cout << "Successfully detected invalid CQ creation attempt" << std::endl;

  return true;
}

// 测试队列对的创建和销毁
bool test_qp_operations() {
  std::cout << "\nTesting Queue Pair Operations..." << std::endl;

  RdmaDevice device;

  // 创建CQ
  uint32_t cq = device.create_cq(16);
  TEST_ASSERT(cq != 0, "Failed to create completion queue");

  // 创建QP
  uint32_t qp = device.create_qp(8, 8, cq, cq);
  TEST_ASSERT(qp != 0, "Failed to create queue pair");
  std::cout << "Created QP: " << qp << " with send/recv depth 8" << std::endl;

  // 销毁QP
  device.destroy_qp(qp);
  std::cout << "Destroyed QP: " << qp << std::endl;

  // 尝试创建无效深度的QP
  uint32_t invalid_qp = device.create_qp(0, 8, cq, cq);
  TEST_ASSERT(invalid_qp == 0,
              "Creating QP with invalid send depth should fail");
  std::cout << "Successfully detected invalid QP creation attempt" << std::endl;

  return true;
}

// 测试内存区域的注册和注销
bool test_mr_operations() {
  std::cout << "\nTesting Memory Region Operations..." << std::endl;

  RdmaDevice device;

  // 分配测试缓冲区
  const size_t buf_size = 4096;
  void *buffer = malloc(buf_size);
  TEST_ASSERT(buffer != nullptr, "Failed to allocate buffer");

  // 注册MR
  uint32_t mr = device.register_mr(buffer, buf_size, 0x1); // 读写权限
  TEST_ASSERT(mr != 0, "Failed to register memory region");
  std::cout << "Registered MR: " << mr << " with size " << buf_size
            << std::endl;

  // 注销MR
  device.deregister_mr(mr);
  std::cout << "Deregistered MR: " << mr << std::endl;

  // 释放缓冲区
  free(buffer);

  // 尝试注册无效的内存区域
  uint32_t invalid_mr = device.register_mr(nullptr, buf_size, 0x1);
  TEST_ASSERT(invalid_mr == 0, "Registering invalid buffer should fail");
  std::cout << "Successfully detected invalid MR registration attempt"
            << std::endl;

  return true;
}

// 测试QP状态转换
bool test_qp_state_transitions() {
  std::cout << "\nTesting QP State Transitions..." << std::endl;

  RdmaDevice device;

  // 创建CQ
  uint32_t cq = device.create_cq(16);
  TEST_ASSERT(cq != 0, "Failed to create completion queue");

  // 创建QP
  uint32_t qp = device.create_qp(8, 8, cq, cq);
  TEST_ASSERT(qp != 0, "Failed to create queue pair");
  std::cout << "Created QP: " << qp << std::endl;

  // 测试状态转换序列
  std::vector<QpState> states = {
      QpState::RESET, // 初始状态
      QpState::INIT,  // 第一次转换
      QpState::RTR,   // Ready to Receive
      QpState::RTS    // Ready to Send
  };

  // 执行状态转换
  for (size_t i = 0; i < states.size(); ++i) {
    bool result = device.modify_qp_state(qp, states[i]);
    TEST_ASSERT(result, "Failed to transition QP state to " +
                            std::to_string(static_cast<int>(states[i])));
    std::cout << "Successfully transitioned QP to state "
              << static_cast<int>(states[i]) << std::endl;
  }

  // 尝试无效的状态转换（从RTS直接到INIT）
  bool invalid_transition = device.modify_qp_state(qp, QpState::INIT);
  TEST_ASSERT(!invalid_transition, "Invalid state transition should fail");
  std::cout << "Successfully detected invalid state transition attempt"
            << std::endl;

  // 清理
  device.destroy_qp(qp);
  std::cout << "Destroyed QP: " << qp << std::endl;

  return true;
}

// 主测试函数
int main() {
  std::cout << "Starting RDMA Device Tests..." << std::endl;

  bool all_tests_passed = true;

  // 运行所有测试
  std::vector<std::pair<std::string, std::function<bool()>>> tests = {
      {"Protection Domain Operations", test_pd_operations},
      {"Completion Queue Operations", test_cq_operations},
      {"Queue Pair Operations", test_qp_operations},
      {"Memory Region Operations", test_mr_operations},
      {"QP State Transitions", test_qp_state_transitions}};

  // 执行测试并收集结果
  for (const auto &test : tests) {
    std::cout << "\n=== Running Test: " << test.first << " ===" << std::endl;
    bool test_result = test.second();
    if (!test_result) {
      std::cerr << "Test Failed: " << test.first << std::endl;
      all_tests_passed = false;
    } else {
      std::cout << "Test Passed: " << test.first << std::endl;
    }
  }

  // 输出总结果
  std::cout << "\n=== Test Summary ===" << std::endl;
  if (all_tests_passed) {
    std::cout << "All tests passed successfully!" << std::endl;
    return 0;
  } else {
    std::cerr << "Some tests failed!" << std::endl;
    return 1;
  }
}