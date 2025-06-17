set_toolchains("clang")
-- set_toolchains("cuda")
set_toolset("cc", "clang")
set_toolset("cxx", "clang++")

add_includedirs("include")
target("test")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++17")                     -- 使用 C++17 标准
    
    --add_files("main.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("src/*.cpp")     

target("rdma_test")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++17")                     -- 使用 C++17 标准
    
    add_files("main.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("src/*.cpp")                     -- 添加src目录下的所有cpp文件