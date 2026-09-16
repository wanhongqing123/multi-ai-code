# MaiAgent

MaiChatBuddy 的 agent 核心，纯 C++，可移植。

目标不是把 opencode 翻译成 C++，是**把 agent 核心做成一个能被到处链进去的库**——
Qt 桌面、iOS/Android、Linux 服务器、嵌入式。

## 形状

```
include/mai/agent/   核心公开头。对外只有两个概念：submit(Op) 和事件流
src/                 核心实现。无 Qt、无 HTTP 服务端、无 JSON
adapters/http/       REST + SSE 适配器，喂现有的 Electron UI
cli/                 maiagent-server 可执行
tests/               单元测试 + e2e
```

核心的边界是 `Agent::submit(Op)` 加一条事件流，形状抄 codex
（它的 `core/src/codex_thread.rs` 对外就 `submit` / `next_event` 两个方法）。
HTTP 只是贴在外面的一层——移动端和嵌入式直接链库，不经过它。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/maiagent_tests          # 单元测试
./build/bin/maiagent-server         # 起服务，端口会打印出来
python tests/e2e/m1_smoke.py http://127.0.0.1:<port>
```

Windows 上要先进 MSVC 环境（`vcvars64.bat`）。

## 进度

- [x] **M1 空转服务端** — health / session 增删查 / SSE 事件流（含心跳）
- [ ] M2 能聊天 — OpenAI Chat Completions 客户端 + prompt + 流式
- [ ] M3 能干活 — 工具注册表 + read/write/glob/grep
- [ ] M4 权限闸门
- [ ] M5 SQLite 持久化 + interrupt/切模型
- [ ] M6 脱壳验证 — 摘掉 HTTP，写一个直接链库的 CLI

## 两条铁律

1. **JSON 只出现在 `adapters/`。** 核心里一律原生结构体。详见 `third_party/README.md`。
2. **构建产物只放 `MaiAgent/build/`。** 仓库根目录那个 `build/` 是
   electron-builder 在用，里面有被 git 跟踪的图标文件，别污染它。
