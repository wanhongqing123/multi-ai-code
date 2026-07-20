# Multi-AI Code

> 本地优先的 AI CLI 工作台，支持从 IM 远程操控本机 AICLI。在桌面端围绕一个本地仓库驱动 `codex` / `opencode` / `claude` 完成开发；离开电脑时，用手机或另一端 IM 直接给这台机器上的 AICLI 发任务、切模型、看 diff、收结果。

## 它是什么

一个面向个人开发者的桌面工作台。它不内置大模型，也不替代 Codex / Claude Code，而是启动你本机真实安装、已登录的 AI CLI，把仓库上下文、任务、代码审查、构建运行和远程消息入口收在一个应用里。两条主线：

1. **本地 AI CLI 工作台** —— 打开一个仓库，用真实 PTY 终端驱动本机 AICLI，围绕它管理任务、定时任务、代码审查、仓库分析、构建运行与日志。
2. **IM 远程操控 AICLI（核心特性）** —— 每台机器登录一个 IM 身份；你在手机或另一端 IM 里把它当联系人，直接给它的当前 AICLI 发任务、图片、语音或控制命令，结果清成聊天友好的 Markdown 回传。多台机器就是多个联系人，各自独立驱动。

> AICLI 指可从命令行启动的 AI 编程工具：`codex`（默认）、`opencode`（一等支持）、`claude`（可选）。

## IM 远程操控 AICLI（核心特性）

![远程 IM](docs/readme/remote-im-chat.png)

任意 IM 客户端（iOS App、Qt 桌面 IM 客户端，或 AICLI 通过 `imcli`）把某台机器当联系人发消息，即可驱动它的当前 AICLI 并收回传。除了直接发任务文本，还支持一组斜杠控制命令：

| 命令 | 作用 |
| --- | --- |
| `/status` | 查看当前 AICLI 状态 |
| `/plan` · `/build` | 切换计划 / 执行模式 |
| `/models` · `/model <序号\|ID\|档位>` | 查看 / 切换模型或推理档位 |
| `/goal [目标\|clear\|pause\|resume]` | 查看 / 设置 / 管理 Goal |
| `/btw <任务>` | 起子 Agent 处理任务，完成后单独回传 |
| `/diff [--stat] [路径]` | 发送未提交改动的 Diff |
| `/interrupt` · `/compact` · `/clear` | 中断 / 压缩上下文 / 清空并开新会话 |
| `/help` | 查看命令帮助 |

- 控制命令对 `codex` / `opencode` 走源码级 IPC 通道（带逐条 ack/重发）；`claude` 目前仅支持任务注入、`/status` 与 `/diff`。
- 只有联系人白名单里的账号才能下发；桌面端不直接执行远程 shell，远程消息只作为输入进入当前 AI 会话。

AICLI 也能主动通过 `imcli` 往 IM 发消息 / 图片 / 文件：

```bash
imcli send <userId> "内容" --project <projectId>
imcli send-image <userId> /path/to/image.png --project <projectId>
imcli send-file <userId> /path/to/report.md --project <projectId>
```

## 桌面工作台

![桌面工作台](docs/readme/desktop-workspace.png)

- **AI CLI 终端** —— 真实 PTY 启动 AICLI，注入任务 / 文件路径 / 审查意见 / 远程消息，Markdown 展示长输出；可续上次会话或开新会话。
- **任务与定时任务** —— 普通任务文档存 `<repo>/.multi-ai-code/designs/`；定时任务按项目存本机 SQLite，可启用 / 禁用 / 立即运行 / 查记录。
- **代码审查与仓库查看** —— 对当前改动 / 提交 / 指定 commit 写行级批注并发回 AICLI；仓库查看含文件树 + 源码 + 分析对话。
- **构建、运行与日志** —— 每个项目维护自己的构建 / 运行流程，日志可在桌面查看并交给 AICLI 分析。
- **语音** —— 收到语音走本地 Whisper 转文字后交给 AICLI。

## 远程客户端

除桌面端外，两个独立 IM 客户端可远程接入：

- **iOS App**（`ios/MultiAIIM`）—— 消息 / 通讯录 / 我三栏，支持文本、图片、语音、联系人管理。
- **Qt 桌面 IM 客户端**（`desktop/qt-im`）—— Windows / macOS 原生 IM 客户端，支持文本、图片、文件卡与斜杠命令建议。

## 快速开始

环境：Node.js 20+、macOS 或 Windows、本机至少安装并登录一种 AI CLI（推荐 `codex`）。

```bash
npm install
npm run dev      # 开发启动
npm run build    # 构建
```

启动后先登录（填 IM 账号 ID），数据按账号隔离到 `~/multi-ai-code/accounts/<账号>/`（可用 `MULTI_AI_ROOT` 覆盖），同一账号任一时刻只允许一个窗口打开。

可选：从源码构建内置的 `codex` / `opencode`（git submodule 在 `third_party/aicli/`）：

```bash
git submodule update --init --recursive
npm run build:aicli
```

Qt 桌面 IM 客户端与内置 AICLI 的构建细节见 [desktop/qt-im/README.md](desktop/qt-im/README.md) 及仓库内脚本注释。

## 下载

安装包挂在 [GitHub Releases](https://github.com/wanhongqing123/multi-ai-code/releases)：Electron 桌面端 tag 形如 `electron-<日期>`，Qt IM 客户端 tag 形如 `qt-im-<日期>`，最新版见 Releases 的 Latest 标记。Windows / macOS 包未做签名，首次打开按系统提示放行即可。

## 数据与边界

- **数据本地优先**：项目、消息、运行记录存本机；仓库级任务与分析存目标仓库 `.multi-ai-code/`（建议加入忽略规则），不经中转服务器。
- 必须依赖本机真实安装的 AI CLI，项目本身不内置模型。
- 一台机器对应一个 IM 账号，远程消息只会进入当前前台项目的会话；多台机器各自作为独立联系人操作。
- 远程消息只作为 AICLI 输入，不提供远程任意 shell 执行入口（但所驱动的 AICLI 会在仓库内改文件 / 执行命令，安全边界取决于该 CLI 的沙箱与审批设置）。
- 远程 IM 当前使用内置测试凭证，不适合作为正式生产配置。

## 技术栈

Electron 33 · React 18 · TypeScript 5 · node-pty · xterm.js · better-sqlite3 · SwiftUI（iOS）· Qt5（桌面 IM）· whisper.cpp
