# 第三方依赖

全部是单头文件，直接进仓库，**不用包管理器**。

这么做是为了两件事：一是没有网络的机器上也能构建（这个项目要交叉编译到
嵌入式设备，那边通常拿不到 vcpkg / Conan）；二是版本彻底钉死，不会因为
别人机器上的包版本不同而出现"我这儿是好的"。

| 目录 | 来源 | 版本 |
|---|---|---|
| `nlohmann/json.hpp` | https://github.com/nlohmann/json | v3.11.3 |
| `httplib/httplib.h` | https://github.com/yhirose/cpp-httplib | v0.18.3 |

## 用法边界

`nlohmann/json.hpp` **只允许在 `adapters/` 下面 include**。核心
（`include/mai/agent/` 与 `src/`）里一律用原生结构体，不出现 JSON 类型。

原因：nlohmann 的 DOM 每个节点一次堆分配、object 默认是 `std::map`，
当成内部数据结构用会很慢。把它关在边界里，既保住性能，也保住了
以后换 simdjson / RapidJSON 时只改边界几个函数的退路。

## 还没进来的

- **sqlite3**：M5 落库时加（amalgamation，两个文件）。
- **libcurl**：M2 接大模型时加。注意它是 HTTP **客户端**，
  和这里的 httplib（HTTP **服务端**）是两回事，两个都要。
