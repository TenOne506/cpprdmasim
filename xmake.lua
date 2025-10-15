-- 设置项目名称和版本
set_project("cpprdmasim")
set_version("0.1.0")

-- 设置C++标准
set_languages("c++17")

-- 设置构建模式
add_rules("mode.debug", "mode.release")

-- 设置编译选项
if is_mode("debug") then
    set_symbols("debug")
    set_optimize("none")
    add_defines("DEBUG")
elseif is_mode("release") then
    set_symbols("hidden")
    set_optimize("fastest")
    add_defines("NDEBUG")
end

-- 添加全局编译选项
add_cxflags("-Wall", "-Wextra", "-Wpedantic")

-- 添加包含目录
add_includedirs("include")

-- 创建静态库
target("rdmasim")
    set_kind("static")
    add_files("src/*.cpp")
    add_includedirs("include")
    add_links("pthread")

-- 创建测试可执行文件
target("test_rdma_device")
    set_kind("binary")
    add_files("test/test_rdma_device.cpp")
    add_deps("rdmasim")
    add_links("pthread")

target("rdma_communication_test")
    set_kind("binary")
    add_files("test/rdma_communication_test.cpp")
    add_deps("rdmasim")
    add_links("pthread")

-- 基准测试：缓存 vs 无缓存
target("rdma_cache_benchmark")
    set_kind("binary")
    add_files("test/rdma_cache_benchmark.cpp")
    add_deps("rdmasim")
    add_links("pthread")

-- 性能测试：三种延迟模型对比
target("rdma_performance_test")
    set_kind("binary")
    add_files("test/rdma_performance_test.cpp")
    add_deps("rdmasim")
    add_links("pthread")

-- 缓存创新对比测试
target("rdma_cache_innovation_test")
    set_kind("binary")
    add_files("test/rdma_cache_innovation_test.cpp")
    add_deps("rdmasim")
    add_links("pthread")

-- 硬件加速仿真测试
target("rdma_hardware_accel_sim_test")
    set_kind("binary")
    add_files("test/rdma_hardware_accel_sim_test.cpp")
    add_deps("rdmasim")
    add_links("pthread")

-- 设置安装规则
target("rdmasim")
    set_installdir("/usr/local")
    add_installfiles("include/*.h", {prefixdir = "include"})