cmake_minimum_required(VERSION 3.20)
project(MaiAgent VERSION 0.1.0 LANGUAGES CXX)

# C++17 与 MaiChat/desktop 保持一致，方便以后两边互链。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 产物统一放一处，方便 Electron 那边指过来。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
  # httplib 在 MSVC 下会告一堆无关紧要的，先压掉这几个具体的，不整体关警告。
  add_compile_options(/wd4100 /wd4996)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── 第三方：全部单头文件，直接进仓库，不依赖包管理器 ──────────────
# 这样在没有网络的机器上（以及嵌入式交叉编译时）也能构建。
add_library(mai_thirdparty INTERFACE)
target_include_directories(mai_thirdparty INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
)

find_package(Threads REQUIRED)

# ── 核心库：纯 C++，无 Qt，无 HTTP 服务端依赖 ────────────────────
add_library(maiagent STATIC
  src/id.cpp
  src/session.cpp
  src/event_bus.cpp
  src/thread.cpp
)
target_include_directories(maiagent
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
)
target_link_libraries(maiagent PUBLIC Threads::Threads)

# ── HTTP 适配器：把核心包成 REST + SSE，喂现有的 Electron UI ──────
# 注意这是 HTTP 服务端。核心里调大模型用的 HTTP 客户端是另一回事（M2 引入 libcurl）。
add_library(maiagent_http STATIC
  adapters/http/server.cpp
)
target_include_directories(maiagent_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/adapters)
target_link_libraries(maiagent_http PUBLIC maiagent mai_thirdparty)
if(WIN32)
  target_link_libraries(maiagent_http PUBLIC ws2_32 crypt32)
endif()

# ── 可执行：HTTP 服务端 ─────────────────────────────────────────
add_executable(maiagent-server cli/server_main.cpp)
target_link_libraries(maiagent-server PRIVATE maiagent_http)

# ── 测试 ────────────────────────────────────────────────────────
option(MAIAGENT_BUILD_TESTS "Build MaiAgent tests" ON)
if(MAIAGENT_BUILD_TESTS)
  enable_testing()
  add_executable(maiagent_tests tests/test_main.cpp)
  target_link_libraries(maiagent_tests PRIVATE maiagent mai_thirdparty)
  add_test(NAME maiagent_tests COMMAND maiagent_tests)
endif()
