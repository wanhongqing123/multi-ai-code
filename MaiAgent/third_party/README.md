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

不在这个目录，走 CMake 的 FetchContent（见顶层 CMakeLists，URL + SHA256
都钉死了）。注意它是 HTTP **客户端**，和这里的 httplib（HTTP **服务端**）
是两回事，两个都要。
