# 第三方依赖

全部是单头文件，直接进仓库，**不用包管理器**。

这么做是为了两件事：一是没有网络的机器上也能构建（这个项目要交叉编译到
嵌入式设备，那边通常拿不到 vcpkg / Conan）；二是版本彻底钉死，不会因为
别人机器上的包版本不同而出现"我这儿是好的"。

| 目录 | 来源 | 版本 |
|---|---|---|
| `nlohmann/json.hpp` | https://github.com/nlohmann/json | v3.11.3 |
| `httplib/httplib.h` | https://github.com/yhirose/cpp-httplib | v0.18.3 |
| `sqlite/sqlite3.{c,h}` | https://sqlite.org/ | 3.49.1（amalgamation） |
| `curl/` | https://curl.se/ | 8.11.1（裁剪版） |

## 用法边界

`nlohmann/json.hpp` **只允许在 `adapters/` 下面 include**。核心
（`include/mai/agent/` 与 `src/`）里一律用原生结构体，不出现 JSON 类型。

原因：nlohmann 的 DOM 每个节点一次堆分配、object 默认是 `std::map`，
当成内部数据结构用会很慢。把它关在边界里，既保住性能，也保住了
以后换 simdjson / RapidJSON 时只改边界几个函数的退路。

## sqlite3

amalgamation 只取两个文件：`sqlite3.c` 和 `sqlite3.h`。压缩包里另外那两个
没要——`shell.c` 是命令行工具的 main，`sqlite3ext.h` 是写扩展用的。

它是 **C** 不是 C++，所以在 CMake 里是单独一个目标（`mai_sqlite`）。
顶层 `project()` 因此必须写 `LANGUAGES C CXX`；只写 CXX 的话 CMake 会在
生成阶段说 `CMAKE_C_COMPILE_OBJECT` 没设，那个错误信息离真正原因很远。

关掉的编译期开关见 CMakeLists，其中一个值得单独说：`SQLITE_DQS=0`。
SQLite 有个历史怪癖——双引号括起来的东西如果不是已知列名，会被当成
**字符串字面量**而不是报错。列名写错了不会有任何提示，查询静默返回空。
关掉之后双引号只当标识符。

## libcurl

`curl/` 是 curl 8.11.1 的**完整发布包**，原样解开，一个文件没动。
sha256 和官网一致：

    a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80

曾经想过只留 `lib/` `include/`，把 29M 压到 7M。没这么做，因为裁剪过的
依赖**就不再是"那个版本"了**：升级时要重新做一遍裁剪决定，出问题时也没法
直接和上游比对；而且构建里哪天需要某个被删掉的文件，报错会离原因很远。
体积换确定性，这笔划算。

构建时关掉的：`BUILD_CURL_EXE`（只要库，不要 curl.exe）、`BUILD_EXAMPLES`、
`CURL_BUILD_TESTING`、文档。`HTTP_ONLY` 把 FTP/LDAP/SMTP/telnet 一律关掉——
我们只打 HTTPS，少掉的这些既是体积也是攻击面。

以前这里走的是 CMake FetchContent（配置时去 curl.se 下载）。那和这份
README 开头说的"没有网络的机器上也能构建"自相矛盾——交叉编译到嵌入式的
那台机器多半没网，而**配置阶段失败比编译失败更难查**。

注意它是 HTTP **客户端**，和这里的 httplib（HTTP **服务端**）是两回事，
两个都要。
