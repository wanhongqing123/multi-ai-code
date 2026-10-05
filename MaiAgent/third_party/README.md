# 第三方依赖

依赖源码直接进仓库，**不用包管理器**。

这么做是为了两件事：一是没有网络的机器上也能构建（这个项目要交叉编译到
嵌入式设备，那边通常拿不到 vcpkg / Conan）；二是版本彻底钉死，不会因为
别人机器上的包版本不同而出现"我这儿是好的"。

| 目录 | 来源 | 版本 |
|---|---|---|
| `nlohmann/json.hpp` | https://github.com/nlohmann/json | v3.11.3 |
| `httplib/httplib.h` | https://github.com/yhirose/cpp-httplib | v0.18.3 |
| `sqlite/sqlite3.{c,h}` | https://sqlite.org/ | 3.49.1（amalgamation） |
| `curl/` | https://curl.se/ | 8.11.1（完整源码基线，含内嵌 CLI 补丁） |
| `zlib/` | https://zlib.net/ | 1.3.2（完整源码，SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`） |
| `openssl/` | https://www.openssl.org/source/ | 3.5.9 LTS（源码基线，剔除示例私钥、测试与演示目录；原包 SHA-256 `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`） |
| `libssh2/` | https://github.com/libssh2/libssh2 | 1.11.2 开发快照，固定提交 `740c2d33ad58720db1039031d2b6e13a3df9aab8` |
| `ffmpeg/` | https://ffmpeg.org/ | 9.0.2（完整源码） |
| `opencv/` | https://github.com/opencv/opencv | 4.14.0（完整源码） |
| `simde/` | https://github.com/simd-everywhere/simde | 0.8.2（OBS 使用的头文件与许可文件） |
| `md4c/` | https://github.com/mity/md4c | release-0.5.2（MIT，供 PDF 与桌面 Markdown 共用） |

OpenCV 4.14.0 来自官方 tag 源码包
`https://github.com/opencv/opencv/archive/refs/tags/4.14.0.tar.gz`，SHA-256 为
`ee8fb9b30eb60850431b4656447080e3737b56e45719c92b67f245950609f86e`。
源码原样解包到 `opencv/`，构建时无需联网。上游许可见
[`opencv/LICENSE`](opencv/LICENSE)。桌面图像模型单独放在
[`MaiChat/shared/agent/models/`](../../MaiChat/shared/agent/models/)。

zlib 使用上游 CMake 目标生成静态库，编译时启用 `Z_PREFIX`，避免与 Qt 和
FFmpeg 已链接的 zlib 符号冲突。OpenSSL 在上游源码上修改了 `apps` 的进程退出、
BIO 与重复调用路径；[`scripts/build-openssl.sh`](../scripts/build-openssl.sh) 为移动端
和 macOS 从源码生成 `libcrypto.a`、`libssl.a`、`libapps.a` 与内嵌 CLI 静态库。
Windows 当前仍使用源码构建的 `libcrypto.lib` 和类型化摘要/证书工具。
共享 SSH 客户端使用固定的 libssh2 源码快照和移动端同一份 OpenSSL `libcrypto.a`；
这避免了 iOS、Android 分别维护两套 SSH 协议实现。
该固定提交包含上游提交 `42e33d8`，修复 1.11.1 及更早版本的一处
[连接前缓冲区溢出问题](https://github.com/advisories/GHSA-v6rf-8q4r-r495)。

## 用法边界

`nlohmann/json.hpp` 用于模型协议和工具调用的 JSON 边界；公开核心头文件里用
原生结构体，不暴露 JSON 类型。

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

`curl/` 以 curl 8.11.1 完整发布包为基线；`src/` 的内嵌 CLI 补丁使上游
`argc/argv` 执行入口能在 App 工作线程反复调用。原始发布包 SHA-256：

    a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80

曾经想过只留 `lib/` `include/`，把 29M 压到 7M。没这么做，因为裁剪过的
依赖**就不再是"那个版本"了**：升级时要重新做一遍裁剪决定，出问题时也没法
直接和上游比对；而且构建里哪天需要某个被删掉的文件，报错会离原因很远。
体积换确定性，这笔划算。

构建时关掉的：`BUILD_CURL_EXE`（不生成外部进程；另编内嵌 CLI 静态库）、`BUILD_EXAMPLES`、
`CURL_BUILD_TESTING`、文档。`HTTP_ONLY` 把 FTP/LDAP/SMTP/telnet 一律关掉——
我们只打 HTTPS，少掉的这些既是体积也是攻击面。

以前这里走的是 CMake FetchContent（配置时去 curl.se 下载）。那和这份
README 开头说的"没有网络的机器上也能构建"自相矛盾——交叉编译到嵌入式的
那台机器多半没网，而**配置阶段失败比编译失败更难查**。

注意它是 HTTP **客户端**，和这里的 httplib（HTTP **服务端**）是两回事，
两个都要。
