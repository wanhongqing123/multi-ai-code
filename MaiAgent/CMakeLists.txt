cmake_minimum_required(VERSION 3.20)
project(MaiAgent VERSION 0.1.0 LANGUAGES CXX)

# C++17 与 MaiChat/desktop 保持一致，方便以后两边互链。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc)
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
#
# TLS 后端按平台挑，不是一律 OpenSSL：
#   Windows  Schannel —— 零额外依赖，走系统证书库，公司代理的证书自动认
#   Linux    OpenSSL  —— 发行版自带
#   嵌入式   mbedTLS  —— OpenSSL 三五兆，mbedTLS 两百来 K
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
# 文件名一律大驼峰 + Mai 前缀，见 docs/CodingStyle.md。
add_library(maiagent STATIC
  src/MaiTime.cpp
  src/MaiError.cpp
  src/MaiSession.cpp
  src/MaiMessage.cpp
  src/MaiIdGenerator.cpp
  src/MaiEventBus.cpp
  src/MaiMemoryStore.cpp
  src/MaiOpenAiClient.cpp
  src/MaiPathUtf8.cpp
  src/MaiPathGuard.cpp
  src/MaiTool.cpp
  src/MaiFileTools.cpp
  src/MaiAgent.cpp
  src/MaiTurnRunner.cpp
  src/MaiContextBuilder.cpp
  src/MaiEventEmitter.cpp
  src/MaiSessionTitler.cpp
)
target_include_directories(maiagent
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  # src 只对自己可见：内部头（MaiTurnRunner.h 等）不是公开 API。
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
)
# libcurl 是 HTTP **客户端**（核心用它调大模型），
# 和适配器里的 cpp-httplib（HTTP **服务端**）是两回事，两个都要。
target_link_libraries(maiagent PUBLIC Threads::Threads PRIVATE CURL::libcurl)

# ── HTTP 适配器：把核心包成 REST + SSE，喂现有的 Electron 界面 ────
add_library(maiagent_http STATIC
  adapters/MaiHttpServer.cpp
)
target_include_directories(maiagent_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/adapters)
target_link_libraries(maiagent_http PUBLIC maiagent mai_thirdparty)
if(WIN32)
  target_link_libraries(maiagent_http PUBLIC ws2_32 crypt32)
endif()

add_executable(maiagent-server cli/MaiServerMain.cpp)
target_link_libraries(maiagent-server PRIVATE maiagent_http)

# ── 测试 ────────────────────────────────────────────────────────
option(MAIAGENT_BUILD_TESTS "Build MaiAgent tests" ON)
if(MAIAGENT_BUILD_TESTS)
  enable_testing()

  # 测试要碰内部头（MaiPathUtf8 等），所以也要 src 的 include 路径。
  foreach(test_name
      MaiCoreTests          # id / 会话 / 事件总线
      MaiModelClientTests   # 流式解析 + 工具调用分片聚合
      MaiTurnRunnerTests    # 一轮对话的完整生命周期
      MaiToolTests          # 路径边界 + 四个内置工具
      MaiToolLoopTests)     # 工具循环：调用 -> 执行 -> 回灌 -> 再答
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE maiagent mai_thirdparty)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(WIN32)
      target_link_libraries(${test_name} PRIVATE ws2_32 crypt32)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
