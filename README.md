# Multi-AI Code

> 本地优先的 AI CLI 工作台，支持从 IM 远程操控本机 AICLI。桌面端围绕本地仓库驱动 `codex` / `opencode` / `claude` 开发；离开电脑时，用手机或另一端 IM 直接给这台机器的 AICLI 发任务、切模型、看 diff、收结果。

**核心是 IM + AICLI**：它不内置大模型，而是启动你本机真实安装的 AI CLI；每台机器登录一个 IM 身份，你把它当聊天联系人，发消息即可驱动它的当前 AICLI，结果清成 Markdown 回传。多台机器就是多个联系人，各自独立驱动。

![远程 IM](docs/readme/remote-im-chat.png)

## IM 控制命令

任意 IM 客户端（iOS App、Qt 桌面 IM，或 AICLI 通过 `imcli`）给目标机器发消息即可。除直接发任务文本外：

| 命令 | 作用 |
| --- | --- |
| `/status` | 查看 AICLI 状态 |
| `/plan` · `/build` | 切换计划 / 执行模式 |
| `/models` · `/model <序号\|ID\|档位>` | 查看 / 切换模型或推理档位 |
| `/goal [目标\|clear\|pause\|resume]` | 管理 Goal |
| `/btw <任务>` | 起子 Agent 处理并回传 |
| `/diff [--stat] [路径]` | 发送未提交改动 |
| `/interrupt` · `/compact` · `/clear` | 中断 / 压缩 / 清空会话 |

> `codex` / `opencode` 支持全部命令；`claude` 仅任务注入 + `/status` + `/diff`。仅白名单联系人可下发，桌面端不执行远程 shell。

## 快速开始

```bash
npm install
npm run dev      # 开发启动
npm run build    # 构建
```

需本机安装并登录一种 AI CLI（推荐 `codex`）。可选从源码构建内置 AICLI：`git submodule update --init --recursive && npm run build:aicli`。数据按 IM 账号隔离存本机。

安装包见 [Releases](https://github.com/wanhongqing123/multi-ai-code/releases)（Electron tag `electron-<日期>`、Qt IM tag `qt-im-<日期>`）。
