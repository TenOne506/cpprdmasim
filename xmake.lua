set_toolchains("clang")
-- set_toolchains("cuda")
set_toolset("cc", "clang")
set_toolset("cxx", "clang++")


target("test")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++17")                     -- 使用 C++20 标准
    --add_files("main.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("*.cpp")     