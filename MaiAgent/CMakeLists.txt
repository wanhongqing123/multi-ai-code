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

# ── libcurl：源码引入，不用包管理器 ────────────────────────────
# 为什么源码而不是系统库：这个项目要交叉编译到嵌入式和移动端，
# 那些目标上没有现成的 libcurl 可链，版本也必须钉死。
#
# 离线构建：把 curl-8.11.1 解好放到某处，configure 时加
#   -DFETCHCONTENT_SOURCE_DIR_CURL=/path/to/curl-8.11.1
# 就不会去下载。
#
# TLS 后端按平台挑，不是一律 OpenSSL：
#   Windows  Schannel —— 零额外依赖，而且走系统证书库，
#                        公司代理 / MITM 证书自动认（OpenSSL 要自己维护 CA bundle）
#   Linux    OpenSSL  —— 发行版自带
#   嵌入式   mbedTLS  —— OpenSSL 三五兆，mbedTLS 两百来 K，差一个量级
include(FetchContent)

set(BUILD_CURL_EXE      OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS   ON  CACHE BOOL "" FORCE)
set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS  OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL  OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
# HTTP_ONLY 把 FTP/LDAP/SMTP/telnet 等一律关掉。我们只打 HTTPS，
# 少掉的这些既是体积也是攻击面。
set(HTTP_ONLY           ON  CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2    OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2         OFF CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL     ON  CACHE BOOL "" FORCE)

if(WIN32)
  set(CURL_USE_SCHANNEL ON  CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL  OFF CACHE BOOL "" FORCE)
elseif(APPLE)
  set(CURL_USE_OPENSSL  ON  CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL  ON  CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(curl
  URL       https://curl.se/download/curl-8.11.1.tar.gz
  URL_HASH  SHA256=a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(curl)

find_package(Threads REQUIRED)

# ── 核心库：纯 C++，无 Qt，无 HTTP 服务端依赖 ────────────────────
add_library(maiagent STATIC
  src/id.cpp
  src/session.cpp
  src/event_bus.cpp
  src/thread.cpp
  src/llm_openai.cpp
)
target_include_directories(maiagent
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
)
# libcurl 是 HTTP **客户端**（核心用它调大模型），
# 和 adapters/http 里的 cpp-httplib（HTTP **服务端**）是两回事，两个都要。
target_link_libraries(maiagent PUBLIC Threads::Threads PRIVATE CURL::libcurl)

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

  # LLM 测试要起一个假的 Chat Completions 服务端，所以也要 httplib 和 ws2_32。
  add_executable(maiagent_llm_tests tests/test_llm.cpp)
  target_link_libraries(maiagent_llm_tests PRIVATE maiagent mai_thirdparty)
  if(WIN32)
    target_link_libraries(maiagent_llm_tests PRIVATE ws2_32 crypt32)
  endif()
  add_test(NAME maiagent_llm_tests COMMAND maiagent_llm_tests)

  # agent loop 的完整一圈：submit -> 流式 -> 事件 -> 落库。
  add_executable(maiagent_loop_tests tests/test_loop.cpp)
  target_link_libraries(maiagent_loop_tests PRIVATE maiagent mai_thirdparty)
  if(WIN32)
    target_link_libraries(maiagent_loop_tests PRIVATE ws2_32 crypt32)
  endif()
  add_test(NAME maiagent_loop_tests COMMAND maiagent_loop_tests)
endif()
