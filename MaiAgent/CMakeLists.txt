cmake_minimum_required(VERSION 3.20)
# C 也要开：sqlite3 是 C 源码。只写 CXX 的话 CMake 生成到一半会说
# CMAKE_C_COMPILE_OBJECT 没设，错误信息离真正原因很远。
project(MaiAgent VERSION 0.1.0 LANGUAGES C CXX)

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
  add_compile_options(/utf-8 /permissive- /EHsc)
  # 严格的告警级别只加给 C++。唯一的 C 源码是第三方的 sqlite3.c，
  # 给它加 /W4 再补一个 /w 压住，cl 会报 D9025（"正在重写 /W4"）——
  # 为了消掉第三方的噪音反而制造新噪音。一开始就别加。
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/wd4100>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/wd4996>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>)
endif()

# ── 第三方：全部单头文件，直接进仓库，不依赖包管理器 ──────────────
# 这样在没有网络的机器上（以及嵌入式交叉编译时）也能构建。
add_library(mai_thirdparty INTERFACE)
target_include_directories(mai_thirdparty INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
)

# ── sqlite3：amalgamation 两个文件，直接进仓库 ─────────────────
# 它是 C 不是 C++，所以单独一个目标；用 C++ 编译器去编 sqlite3.c 会出事。
add_library(mai_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(mai_sqlite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(mai_sqlite PUBLIC
  # 用不到的功能一律关掉：既是体积也是攻击面。
  SQLITE_OMIT_LOAD_EXTENSION      # 不允许加载 .so/.dll 扩展
  SQLITE_OMIT_DEPRECATED
  SQLITE_DQS=0                    # 双引号只当标识符，不当字符串——
                                  # 这是 SQLite 的历史怪癖，写错列名会被
                                  # 当成字符串字面量静默通过
  SQLITE_DEFAULT_MEMSTATUS=0      # 省掉每次分配的统计
  SQLITE_DEFAULT_FOREIGN_KEYS=0   # 我们显式逐级删，不靠外键
  SQLITE_THREADSAFE=2             # 多线程，但同一个连接由调用方保证串行
                                  # （我们在 SqliteStore 里整体加锁）
  SQLITE_USE_URI=0
)
# 不需要再给它补 /w：上面的告警选项本来就只加给 C++。

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
  src/MaiSqliteStore.cpp
  src/MaiOpenAiClient.cpp
  src/MaiPathUtf8.cpp
  src/MaiPathGuard.cpp
  src/MaiTool.cpp
  src/MaiPermissionGate.cpp
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
# sqlite 和 libcurl 都是 PRIVATE：它们是实现细节，公开头里不出现。
target_link_libraries(maiagent PUBLIC Threads::Threads PRIVATE CURL::libcurl mai_sqlite)

# ── HTTP 适配器：把核心包成 REST + SSE，喂现有的 Electron 界面 ────
add_library(maiagent_http_adapter STATIC
  adapters/MaiHttpAdapter.cpp
)
target_include_directories(maiagent_http_adapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/adapters)
target_link_libraries(maiagent_http_adapter PUBLIC maiagent mai_thirdparty)
if(WIN32)
  target_link_libraries(maiagent_http_adapter PUBLIC ws2_32 crypt32)
endif()

add_executable(maiagent-bridge cli/MaiBridgeMain.cpp)
target_link_libraries(maiagent-bridge PRIVATE maiagent_http_adapter)

# ── 测试 ────────────────────────────────────────────────────────
option(MAIAGENT_BUILD_TESTS "Build MaiAgent tests" ON)
if(MAIAGENT_BUILD_TESTS)
  enable_testing()

  # 测试共用的假实现。做成库而不是头文件里塞实现，是为了守住
  # "头文件里不写函数实现"那条规范——测试代码不该有豁免。
  add_library(maiagent_test_support STATIC
    tests/support/MaiFakeModelClient.cpp
  )
  target_include_directories(maiagent_test_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  target_link_libraries(maiagent_test_support PUBLIC maiagent)

  # 测试要碰内部头（MaiPathUtf8 等），所以也要 src 的 include 路径。
  foreach(test_name
      MaiCoreTests          # id / 会话 / 事件总线
      MaiStoreTests         # 存储契约：内存和 SQLite 行为必须一致
      MaiModelClientTests   # 流式解析 + 工具调用分片聚合
      MaiTurnRunnerTests    # 一轮对话的完整生命周期
      MaiToolTests          # 路径边界 + 四个内置工具
      MaiToolLoopTests      # 工具循环：调用 -> 执行 -> 回灌 -> 再答
      MaiPermissionTests)   # 权限闸门：拦住 -> 等裁决 -> 放行或拒绝
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE maiagent maiagent_test_support mai_thirdparty)
    target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(WIN32)
      target_link_libraries(${test_name} PRIVATE ws2_32 crypt32)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
