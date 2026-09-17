# MaiAgent

MaiChatBuddy 的 agent 核心，纯 C++，可移植。

目标不是把 opencode 翻译成 C++，是**把 agent 核心做成一个能被到处链进去的库**——
Qt 桌面、iOS/Android、Linux 服务器、嵌入式。

## 形状

```
include/     核心公开头（MaiAgent.h / MaiTool.h / ...）。对外只有 submit(Op) 和事件流
src/         核心实现 + 内部头。无 Qt、无 HTTP 服务端、无 JSON
adapters/    MaiHttpServer：REST + SSE 适配器，喂现有的 Electron UI
cli/         maiagent-server 可执行
tests/       单元测试 + e2e
docs/        编码规范
```

文件名、类名一律大驼峰加 `Mai` 前缀，不用命名空间——**规范见
[`docs/CodingStyle.md`](docs/CodingStyle.md)，动代码前先看**。
根目录的 `.clang-format` 是它的可执行部分。

核心的边界是 `MaiAgent::submit(MaiOperation)` 加一条事件流，形状抄 codex
（它的 `core/src/codex_thread.rs` 对外就 `submit` / `next_event` 两个方法）。
HTTP 只是贴在外面的一层——移动端和嵌入式直接链库，不经过它。

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

# 端到端（会自己起假模型和服务端，不需要 API key）
python tests/e2e/m2_loop.py

# 手动起服务
./build/bin/maiagent-server --help
./build/bin/maiagent-server                              # 空转，不接模型
./build/bin/maiagent-server --model-url http://127.0.0.1:11434/v1 --model qwen2.5
./build/bin/maiagent-server --model-url https://open.bigmodel.cn/api/paas/v4                            --model-key $MAIAGENT_API_KEY --model glm-5.3
```

Windows 上要先进 MSVC 环境（`vcvars64.bat`）。

## 进度

- [x] **M1 空转服务端** — health / session 增删查 / SSE 事件流（含心跳）
- [x] **M2 能聊天** — Chat Completions 流式客户端 + agent loop + prompt/interrupt
- [x] **M3 能干活** — 工具注册表 + read/write/glob/grep + 工具循环
- [ ] M4 权限闸门
- [ ] M5 SQLite 持久化 + interrupt/切模型
- [ ] M6 脱壳验证 — 摘掉 HTTP，写一个直接链库的 CLI

## 三条铁律

1. **公开头 `include/` 里不出现 JSON、HTTP、Qt。** 对外 API 一律原生结构体——
   这是"能被到处链进去"的前提。

   实现里 JSON 只在三个文件出现，每一个都是因为**协议本身就是 JSON**：
   `MaiOpenAiClient.cpp`（SSE 载荷）、`MaiFileTools.cpp`（工具参数）、
   `MaiHttpServer.cpp`（REST）。核心自己的数据结构不经过 JSON。

   分层可以直接量：

   ```bat
   dumpbin /symbols build\lib\maiagent.lib      | findstr /C:httplib   :: 应为 0 条
   dumpbin /symbols build\lib\maiagent_http.lib | findstr /C:curl_easy :: 应为 0 条
   ```

   核心链 libcurl（HTTP **客户端**，用来调大模型），不链 cpp-httplib
   （HTTP **服务端**）；适配器反过来。两个方向都为 0 才算分层没破。

2. **构建产物只放 `MaiAgent/build/`。** 仓库根目录那个 `build/` 是
   electron-builder 在用，里面有被 git 跟踪的图标文件，别污染它。

3. **路径一律走 `MaiPathUtf8::fromUtf8` / `toUtf8`**（`src/MaiPathUtf8.h`）。
   MSVC 的 `fs::path` 把 narrow 字符串按当前 ANSI 代码页解释，中文环境是 GBK，
   而我们的路径全来自 JSON、是 UTF-8。直接 `fs::path(s)` 会让中文路径出错——
   实测是进程直接挂掉，不是返回错误。测试里写中文路径要用 `std::filesystem::u8path`。
