# MaiAgent

MaiChatBuddy 的 agent 核心，纯 C++，可移植。

目标不是把 opencode 翻译成 C++，是**把 agent 核心做成一个能被到处链进去的库**——
Qt 桌面、iOS/Android、Linux 服务器、嵌入式。

## 形状

```
include/     核心公开头（MaiAgent.h / MaiTool.h / ...）。对外只有 submit(Op) 和事件流
src/         核心实现 + 内部头。无 Qt、无 HTTP 服务端、无 JSON
cli/         maiagent-console：只链核心的交互式控制台。**可摘**
tests/       单元测试 + 一个端到端脚本
docs/        编码规范
```

文件名、类名一律大驼峰加 `Mai` 前缀，不用命名空间——**规范见
[`docs/CodingStyle.md`](docs/CodingStyle.md)，动代码前先看**。
根目录的 `.clang-format` 是它的可执行部分。

## 这不是一个服务端

产物是 **`maiagent` 这个库**。这个仓库里现在**一个 HTTP 服务端都没有**——
连能提供服务端的代码都没有。

曾经有过：`adapters/MaiHttpAdapter`（REST + SSE）加 `maiagent-bridge`，
637 行，存在的唯一理由是当时的界面是 Electron、JS 写的、调不了 C++，
只会说 HTTP。那个界面要撤，这层壳跟着撤了。

这句话不是靠文档维持的，是**构建系统盯着的**：`maiagent-console` 只链
`maiagent`，而 httplib 只剩 `mai_thirdparty_for_tests` 这一个去处。
`tests/e2e/console_e2e.py` 还会翻一遍产出的 exe，确认里面没有 httplib、
也没有那几条 REST 路由——对照组是 `MaiModelClientTests`，它**故意**链了
httplib 当假的模型服务端，免得这条检查恒为真。

Qt 桌面 / iOS / Android / 嵌入式全都和控制台一样直接链库：

```cpp
MaiAgent agent(std::move(store), std::move(model), std::move(tools));
agent.eventBus().subscribe([](const MaiEvent& e) { /* 刷界面 */ });
agent.submit(MaiSendPrompt{sessionId, "你好"});
```

摘掉那层壳的时候核心一行都没改——这就是当初要有这条边界的意思。

**别把两个 HTTP 搞混**，它们方向相反，现在只剩一个：

| | 是什么 | 干嘛用 | 现状 |
|---|---|---|---|
| libcurl | HTTP **客户端** | 核心去调大模型 | 在，摘不掉 |
| httplib | HTTP **服务端** | 以前让 Electron 连进来 | 产物里没了，只剩测试当假模型用 |

核心的边界是 `MaiAgent::submit(MaiOperation)` 加一条事件流，形状抄 codex
（它的 `core/src/codex_thread.rs` 对外就 `submit` / `next_event` 两个方法）。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 五套单元测试
./build/bin/MaiCoreTests            # id / 会话 / 事件总线
./build/bin/MaiModelClientTests     # 流式解析 + 工具调用分片聚合
./build/bin/MaiTurnRunnerTests      # 一轮对话的完整生命周期
./build/bin/MaiToolTests            # 路径边界 + 四个内置工具
./build/bin/MaiToolLoopTests        # 工具循环：调用 -> 执行 -> 回灌 -> 再答
./build/bin/MaiPermissionTests      # 权限闸门：拦住 -> 等裁决 -> 放行或拒绝
./build/bin/MaiStoreTests           # 存储契约：内存和 SQLite 行为必须一致

# 只有 MaiModelClientTests 会起 socket——它测的就是传输和线格式。
# 其余几套直接实现 MaiModelClient 接口（tests/support/MaiFakeModelClient），
# 进程内跑，没有端口也没有 sleep。理由见 docs/CodingStyle.md 7.6。

# 端到端：自己起假模型和控制台，不需要 API key
python tests/e2e/console_e2e.py

# 交互式控制台：一个进程，不起服务端，中文输入输出都走宽字符 API
./build/bin/maiagent-console --help
./build/bin/maiagent-console                            # 空转，不接模型
./build/bin/maiagent-console --model-url http://127.0.0.1:11434/v1 --model qwen2.5
./build/bin/maiagent-console --db ./agent.db --dir E:/some/project

```

Windows 上要先进 MSVC 环境（`vcvars64.bat`）。

**构建不需要网络。** 第三方依赖（nlohmann / cpp-httplib / sqlite3 / libcurl）
全部在 `third_party/` 里，没有下载步骤——这个项目要交叉编译到嵌入式设备，
那边通常没网，而配置阶段失败比编译失败更难查。

## 进度

- [x] **M1 空转骨架** — session 增删查 + 事件流
- [x] **M2 能聊天** — Chat Completions 流式客户端 + agent loop + prompt/interrupt
- [x] **M3 能干活** — 工具注册表 + read/write/glob/grep + 工具循环
- [x] **M4 权限闸门** — write 跑之前停下来等用户点头
- [x] **M5 落库** — SQLite 持久化 + 切模型 + 写失败不再无声无息
- [x] **M6 脱壳验证** — `maiagent-console` 只链 `maiagent`，HTTP 整块摘掉

## 权限闸门

会改东西的工具（现在只有 `write`）跑之前会停下来等用户点头。
只读的 `read` / `glob` / `grep` 不问——每一次多余的确认都在训练用户闭眼点"允许"。

一次授权的完整链路：

```
工具卡进入 pending 状态          message.part.updated
核心广播"有人在等授权"            permission.asked   { permissionID, partID }
        ↓  这一轮在这里阻塞（只挂住它自己那个线程）
界面裁决                          POST /api/permission/<id>  {"decision": "approved"}
核心广播裁决结果                  permission.replied { permissionID, detail }
工具卡进入 running 并真正执行      message.part.updated
```

`decision` 三选一：`approved`（就这一次）、`approved_for_session`（这个会话里这个工具以后别问了）、
`denied`。**认不出来的取值一律 400**，不会兜底成放行——闸门不该被一个错别字拆掉。

`GET /api/permission` 列出当前所有待裁决的请求。界面重连之后必须拉一次：
界面断开的那个窗口期里发出的 `permission.asked` 是看不到的，只靠事件流会
漏掉整整一次授权请求，那一轮就一直挂着而界面上什么都没显示。

几个刻意的选择：

- **默认不超时。** 超时自动拒绝等于替用户做了决定，而用户可能只是走开了。
  等待期间会话是"在跑"状态，界面上看得见，随时可以中断。无人值守的场景用
  `--permission-timeout <ms>`。
- **所有兜底方向都是拒绝。** 中断、超时、没接闸门、decision 解析失败——
  全部按拒绝走。兜底成允许意味着"没人点头"也能改用户的文件。
- **被拒之后同样的调用不再问第二次。** 模型被拒后经常原样重试，每次都弹框
  会把用户烦死。同一轮里相同的 (工具, 参数) 直接回同样的拒绝。
- **拒绝时告诉模型"不要重试"。** 不写这句，模型会把 12 圈全烧在同一个被拒的
  操作上，用户看到的是 agent 卡住了。

## 落库

默认**不落盘**。`maiagent-console` 不给 `--db` 就是纯内存，进程退出什么都不留。
悄悄在用户机器上建个数据库文件不合适，要留历史就显式给路径：

```bash
./build/bin/maiagent-console --db ./agent.db
```

库里直接用是 `makeMaiSqliteStore(path)`，返回 `MaiResult`——打不开就是错误，
不会给你一个用起来处处出错的半死对象。传 `":memory:"` 能拿到一个走完整 SQL
路径但不落盘的库（测试里用它验 SQL 本身）。

表结构是 `sessions` / `messages` / `parts` 三张，part 的三种形态展开成列而不是
塞 JSON——这样 `sqlite3` 命令行能直接查，排障时看得见"那次 write 的 input
到底是什么"，也不用把 nlohmann 拖进核心。

消息和 part 的顺序靠 `ORDER BY id`。这不是偷懒：`MaiIdGenerator` 产出的 id 是
定长的"时间戳 + 同毫秒序号 + 随机"，用的 base32 字母表在 ASCII 里递增，
所以字典序就是生成顺序。`MaiStoreTests` 里有一条用例专门盯着这个性质。

**备份注意**：开了 WAL，已提交的数据先落在 `<db>-wal` 里，要到 checkpoint
才并回主文件。实测跑完一轮对话，`agent.db` 还是 4096 字节，数据全在
`agent.db-wal`。所以只拷 `agent.db` 不算备份。进程被硬杀不会丢数据——
下次打开时会从 `-wal` 恢复。

**写失败不再无声无息**：存储的写接口返回 void（它们在流式热路径上，
每次都检查会把代码淹掉），但每轮结束时 `MaiTurnRunner` 会查一次
`lastWriteError()`，失败就把会话标成错误让界面看见。磁盘满了还假装存上了，
是用户第二天打开发现对话没了的那种 bug。

## 文件操作

**库代码里不出现 `std::filesystem` 和 `fstream`。** 路径是 `MaiFilePath`，
操作是 `MaiFileSystem`：Windows 走宽字符 Win32 API（`CreateFileW` /
`FindFirstFileExW` / `GetFinalPathNameByHandleW`），其它平台走 POSIX
（`open` / `opendir` / `realpath`）。

形状参考 chromium 的 `base/files`。最关键的一点是**内部存平台原生串**
（Windows 上 `wstring`，其它平台 `string`），只在跟模型打交道的边界转
UTF-8——POSIX 的文件名是任意字节序列，不保证是合法 UTF-8，统一成 UTF-8
会改写文件名。

> POSIX 那一份（`MaiFileSystemPosix.cpp`）**没有在真机上跑过**，
> 本项目目前只在 Windows 上构建。`MaiFilePathTests` 是可移植的，
> 第一次在 Linux/mac 上构建时先跑那一套。

## 三条铁律

1. **公开头 `include/` 里不出现 JSON、HTTP、Qt。** 对外 API 一律原生结构体——
   这是"能被到处链进去"的前提。

   实现里 JSON 只在三个文件出现，每一个都是因为**协议本身就是 JSON**：
   `MaiOpenAiClient.cpp`（SSE 载荷）、`MaiFileTools.cpp`（工具参数）、
   `MaiSqliteStore.cpp`（片段存列）。核心自己的数据结构不经过 JSON。

   分层可以直接量：

   ```bat
   dumpbin /symbols build\lib\maiagent.lib | findstr /C:httplib   :: 应为 0 条
   ```

   核心链 libcurl（HTTP **客户端**，用来调大模型），不链 cpp-httplib
   （HTTP **服务端**）。现在这条线还有更硬的一道守着：`maiagent-console`
   只链 `maiagent`，httplib 只在 `mai_thirdparty_for_tests` 里，
   漏进核心就当场链不过。

2. **构建产物只放 `MaiAgent/build/`。** 仓库根目录那个 `build/` 是
   electron-builder 在用，里面有被 git 跟踪的图标文件，别污染它。

3. **路径一律走 `MaiPathUtf8::fromUtf8` / `toUtf8`**（`src/MaiPathUtf8.h`）。
   MSVC 的 `fs::path` 把 narrow 字符串按当前 ANSI 代码页解释，中文环境是 GBK，
   而我们的路径全来自 JSON、是 UTF-8。直接 `fs::path(s)` 会让中文路径出错——
   实测是进程直接挂掉，不是返回错误。测试里写中文路径要用 `std::filesystem::u8path`。
