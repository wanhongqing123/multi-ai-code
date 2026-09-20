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
#
# **这个目标只给测试用，产物一行都不碰它。** 名字里带 test 是刻意的：
# 以前它叫 mai_thirdparty，链给谁都不显眼，httplib 就是那么进到产物里去的。
#
#   nlohmann  核心自己也用（走 maiagent 的 PRIVATE include），这里这份是给
#             测试直接构造线格式用的。
#   httplib   HTTP **服务端**。产物里没有、也不该有——它在这里的唯一身份是
#             MaiModelClientTests 的假模型服务端：要测 MaiOpenAiClient 这个
#             HTTP **客户端**，就得有个真的 socket 在对面，不然分片和 SSE
#             边界那些东西根本测不到。
add_library(mai_thirdparty_for_tests INTERFACE)
target_include_directories(mai_thirdparty_for_tests INTERFACE
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

# ── libcurl：源码进仓库，和别的第三方一致 ─────────────────────
# third_party/curl 是 curl 8.11.1 的**完整发布包**，原样解开，一个文件
# 没动（sha256 对得上官网：a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80）。
#
# 曾经想过只留 lib/ include/ 把 29M 压到 7M，但那样它就不再是"那个版本"
# 了：升级时要重新做一遍裁剪决定，出问题时也没法直接和上游比对。
# 完整放着还省掉一类怪事——构建里哪天需要某个被删掉的文件时，
# 报错会离原因很远。
#
# 以前这里走的是 FetchContent（配置时去 curl.se 下载）。那和
# third_party/README 里"没有网络的机器上也能构建"自相矛盾：交叉编译到
# 嵌入式的那台机器多半没网，而配置阶段失败比编译失败更难查。
#
# TLS 后端按平台挑，不是一律 OpenSSL：
#   Windows  Schannel —— 零额外依赖，走系统证书库，公司代理的证书自动认
#   Linux    OpenSSL  —— 发行版自带
#   嵌入式   mbedTLS  —— OpenSSL 三五兆，mbedTLS 两百来 K
set(BUILD_CURL_EXE      OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS   ON  CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)  # curl 自带的示例，不用编
set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
set(CURL_BUILD_TESTING  OFF CACHE BOOL "" FORCE)  # curl 自己的测试套件，不用跑
set(BUILD_LIBCURL_DOCS  OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL  OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
# HTTP_ONLY 把 FTP/LDAP/SMTP/telnet 等一律关掉。我们只打 HTTPS，
# 少掉的这些既是体积也是攻击面。
set(HTTP_ONLY           ON  CACHE BOOL "" FORCE)
# 不用 zlib。
#
# curl 这个选项默认是 AUTO——环境里找得到 zlib 就用上。**被别人 add_subdirectory
# 进去的时候这是个雷**：MaiChat 那边把 ZLIB_INCLUDE_DIR 指向 Qt 自带的私有副本
# （C:/Qt/.../include/QtZlib），而那份 zlib.h 第一行就 #include <qglobal.h>。
# curl 的 C 文件看不见 Qt 的头，编译直接 C1083，报错还落在 curl 的源码里，
# 和 Qt 八竿子打不着，很难往回推。
#
# zlib 对我们只影响 gzip 传输编码，而模型接口是 SSE 明文，用不上。
set(CURL_ZLIB           OFF CACHE STRING "" FORCE)
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

# EXCLUDE_FROM_ALL：curl 自己那些辅助目标不要跟着我们的 all 一起构建。
add_subdirectory(third_party/curl EXCLUDE_FROM_ALL)

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
  # 路径与文件系统：不用 std::filesystem，走系统 API。
  # 纯词法的那一半（MaiFilePath.cpp）是可移植的；真正调系统调用的那一半
  # 按平台各一份。形状参考 chromium 的 base/files。
  src/MaiBlockingCheck.cpp
  src/MaiFilePath.cpp
  src/MaiPathGuard.cpp
  src/MaiTool.cpp
  src/MaiPermission.cpp
  src/MaiQuestion.cpp
  src/MaiFileTools.cpp
  src/MaiEditTool.cpp
  src/MaiTimeTool.cpp
  src/MaiApplyPatchTool.cpp
  src/MaiWebFetchTool.cpp
  src/MaiTodoWriteTool.cpp
  src/MaiQuestionTool.cpp
  src/MaiSubAgentTools.cpp
  src/MaiShellTool.cpp
  src/MaiAgent.cpp
  src/MaiTurnRunner.cpp
  src/MaiContextBuilder.cpp
  src/MaiEventEmitter.cpp
  src/MaiSessionTitler.cpp
)
# 文件系统实现按平台挑一份。Windows 走宽字符 Win32 API，其余走 POSIX。
if(WIN32)
  target_sources(maiagent PRIVATE src/MaiFileSystemWindows.cpp src/MaiProcessWindows.cpp)
else()
  target_sources(maiagent PRIVATE src/MaiFileSystemPosix.cpp src/MaiProcessPosix.cpp)
endif()

target_include_directories(maiagent
  # 头全在 include/ 里，src/ 里一个 .h 都没有（见 docs/CodingStyle.md 第 1 节）。
  # 以前 src/ 也在包含路径上，因为 MaiTurnRunner.h 这几个"内部头"放在那儿——
  # 那套两个目录的分法要求每个新目标都记得补第二条包含路径，忘一次就是一串 C1083，
  # 而它想守的"别当公开 API 用"靠注释说一句就够了。
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
)
# libcurl 是 HTTP **客户端**（核心用它调大模型），
# 和适配器里的 cpp-httplib（HTTP **服务端**）是两回事，两个都要。
# sqlite 和 libcurl 都是 PRIVATE：它们是实现细节，公开头里不出现。
target_link_libraries(maiagent PUBLIC Threads::Threads PRIVATE CURL::libcurl mai_sqlite)

# 公开头里的中文注释要能被**别人**正确编译。
#
# 这些文件是无 BOM 的 UTF-8。MSVC 不带 /utf-8 就按当前 ANSI 代码页读（中文机器是 936），
# 中文注释里的字节被拆错，后面的代码跟着被吃掉——报出来是一句 C4819 加一长串
# C4430 / C2059，看上去像是头文件本身写错了，很难想到是编码。
#
# 上面那句 add_compile_options(/utf-8) 只管**本目录自己的目标**，link 过来的人带不上，
# 所以这里必须再挂一个 INTERFACE。MaiChat 把这个库 add_subdirectory 进去时就是这么炸的。
target_compile_options(maiagent INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# ── 唯一的宿主进程：只链核心库的控制台 ───────────────────────────
# 这一行是全部：`maiagent`，没有别的。产物里没有 HTTP 服务端，
# 因为压根就没有东西能提供一个——httplib 只在 mai_thirdparty_for_tests 里。
#
# 曾经这里还有 maiagent_http_adapter（REST + SSE）和 maiagent-bridge，
# 存在的唯一理由是 Electron 界面说不了 C++。那个界面要撤，这层壳跟着撤了。
# Qt / iOS / Android / 嵌入式都和这个 exe 一样：构造 MaiAgent，订阅事件总线，
# submit 操作，进程内调用。
#
# 说清楚这条线**不**管什么，免得下一个人误判：
#   - nlohmann 还是会进核心，走的是 maiagent 自己的 PRIVATE include 路径。
#     这是有意的：OpenAI 的线格式就是 JSON，客户端必须能解。守的那条线是
#     "公开头里不出现 JSON"，不是"核心不许碰 JSON"。
#   - libcurl 也会进来。它是 HTTP **客户端**（核心用它调模型），
#     和 httplib 这个 HTTP **服务端**是两件完全不同的事。
add_executable(maiagent-console cli/MaiConsoleMain.cpp)
target_link_libraries(maiagent-console PRIVATE maiagent)

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

  foreach(test_name
      MaiCoreTests          # id / 会话 / 事件总线
      MaiFilePathTests      # 路径词法 + 文件系统（走系统 API，不用 std::filesystem）
      MaiStoreTests         # 存储契约：内存和 SQLite 行为必须一致
      MaiModelClientTests   # 流式解析 + 工具调用分片聚合
      MaiTurnRunnerTests    # 一轮对话的完整生命周期
      MaiToolTests          # 路径边界 + 内置工具
      MaiEditToolTests      # edit：唯一匹配、替换全部、拒绝的那几种情况
      MaiApplyPatchToolTests  # apply_patch：多处/多文件、全有或全无、换行原样
      MaiWebToolsTests      # webfetch 的 HTML 转文本和协议限制、todowrite 的校验
      MaiQuestionTests      # 问答闸门 + question 工具：等不到答案时绝不能编一个
      MaiSubAgentTests      # 子 Agent：深度上限、数量上限、不能戳别人的会话
      MaiShellToolTests     # shell：审批粒度、超时、取消、输出上限
      MaiToolLoopTests      # 工具循环：调用 -> 执行 -> 回灌 -> 再答
      MaiPermissionTests)   # 权限闸门：拦住 -> 等裁决 -> 放行或拒绝
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE maiagent maiagent_test_support mai_thirdparty_for_tests)
    if(WIN32)
      target_link_libraries(${test_name} PRIVATE ws2_32 crypt32)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
