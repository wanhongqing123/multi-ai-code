# IM AICLI 控制命令设计

## 背景

当前远程 IM 有两类完全不同的输入：

- 普通任务消息：用户希望把文本交给 AICLI 处理，并把 AICLI 的最终回复转回 IM。
- 控制命令：用户希望远程控制当前 AICLI 会话状态，例如查看状态、进入计划模式、切回执行模式。

普通任务消息现在会被包装成包含 `<remote-im-reply id="...">` 的 prompt，再写入 AICLI。这个设计适合模型回复闭环，但不适合 `/status`、`/plan` 这类命令。如果把命令也当普通 prompt 发送，会出现两个问题：

- 命令不再位于输入第一行，TUI 不一定识别为 slash command。
- 控制命令会污染普通 IM 回复协议，后续仍可能产生噪音。

因此，IM 控制命令必须和普通消息通道分离。

## 目标

第一版目标：

- 支持通过 IM 控制 Codex 和 OpenCode。
- Codex 和 OpenCode 都使用源码级控制能力，不模拟键盘输入，不依赖 TUI 焦点。
- 控制命令不生成 `replyId`，不拼接 `<remote-im-reply>`，不进入普通模型对话。
- 控制结果由宿主进程直接回 IM，保证一次命令一次结果。
- 只开放安全、可解释的白名单命令。

## 非目标

第一版不做这些能力：

- 不开放任意 slash command 透传。
- 不开放 `/delete`、`/archive`、`/logout`、`/exit` 等破坏性命令。
- 不开放 Codex `/stop`。Codex 的 `/stop` 是停止后台终端，不是停止当前 IM 任务，容易误伤正在执行的工作。
- 不通过输入 `/xxx` 到 PTY 的方式模拟控制。
- 不把控制命令输出交给 AICLI 回复解析器处理。

如果后续需要“停止当前 turn”，应新增独立命令 `/cancel` 或 `/interrupt`，语义明确为中断当前执行，而不是复用 Codex 的 `/stop`。

## 命令白名单

第一版建议支持：

| IM 命令 | Codex | OpenCode | 说明 |
| --- | --- | --- | --- |
| `/status` | 支持 | 支持 | 返回当前 AICLI 类型、会话、模型、模式、运行状态 |
| `/plan` | 支持 | 支持 | 切到计划模式 |
| `/build` | 需要补源码能力 | 支持 | 切回执行模式 |
| `/model` | 支持查看，切换后续再扩展参数 | 支持查看，切换后续再扩展参数 | 第一版优先只读 |
| `/usage` | 支持 | 可先返回不支持或本地统计 | 用量信息 |
| `/diff` | 支持 | 支持 | 返回当前工作区改动摘要 |

命令必须精确匹配，避免普通消息误触发。例如 `请帮我看 /status` 应作为普通任务消息处理，不作为控制命令。

## 总体流程

```text
IM 收到文本
  |
  |-- 精确命中控制命令白名单？
  |       |
  |       |-- 是：进入控制命令通道
  |       |       |
  |       |       |-- 校验发送人是否可信
  |       |       |-- 获取当前 active AICLI session
  |       |       |-- 根据 sourceKind 分发到 Codex/OpenCode 控制接口
  |       |       |-- 宿主直接发送控制结果到 IM
  |       |
  |       |-- 否：进入普通消息通道
  |               |
  |               |-- 生成 replyId
  |               |-- 构造 remote-im prompt
  |               |-- 发送给 AICLI
  |               |-- 只解析当前 replyId tag 内内容
  |               |-- 转发最终回复到 IM
```

## 宿主侧设计

新增统一控制接口：

```ts
type AicliControlCommand =
  | { name: "status" }
  | { name: "plan" }
  | { name: "build" }
  | { name: "model"; args?: string[] }
  | { name: "usage" }
  | { name: "diff" }

type AicliControlResult = {
  ok: boolean
  message: string
}
```

宿主侧根据当前会话来源分发：

```text
sourceKind = codex   -> CodexControlBridge.execute(...)
sourceKind = opencode -> OpenCodeControlBridge.execute(...)
sourceKind = claude  -> 不处理，返回“Claude 暂不支持源码级控制命令”
sourceKind = unknown -> 返回“当前 AICLI 类型未知”
```

普通消息路径保持不变。控制命令路径禁止调用普通 IM 的 `buildRemoteImAicliPrompt()`，也不启动普通 `startOutputForwarding()` 的 reply marker 解析。

## Codex 源码级控制

Codex 当前已有静态 slash command 枚举，相关源码在：

- `third_party/aicli/codex/codex-rs/tui/src/slash_command.rs`

确认到的相关命令：

- `/status`
- `/plan`
- `/model`
- `/usage`
- `/diff`

Codex 没看到内置 `/build`。因此需要在自维护 Codex 源码中补一个稳定的控制入口，至少提供这些能力：

```text
status
enter_plan
enter_build_or_normal
model_status
usage_status
diff_summary
```

实现时不要复用 TUI 键盘输入路径。更合理的是新增内部 control message 或 control IPC，让 TUI 主循环收到结构化命令后调用已有 handler。

任务运行中策略：

- `/status`、`/usage`、`/diff` 可以允许。
- `/plan`、`/build` 如果当前 turn 正在运行，第一版返回 busy，不排队切换。
- 后续如确实需要，可以扩展为“当前 turn 结束后切换”。

## OpenCode 源码级控制

OpenCode 已有更接近目标的源码级 API：

- `session.get`
- `session.active`
- `session.switchAgent`
- `session.switchModel`

相关源码在：

- `third_party/aicli/opencode/packages/protocol/src/groups/session.ts`
- `third_party/aicli/opencode/packages/server/src/handlers/session.ts`
- `third_party/aicli/opencode/packages/opencode/src/agent/agent.ts`

OpenCode 默认 agent 中已有：

- `build`
- `plan`
- `general`

因此第一版可以直接映射：

```text
/plan  -> session.switchAgent(agent: "plan")
/build -> session.switchAgent(agent: "build")
```

`/status` 可以组合读取：

- 当前 session id
- 当前 agent
- 当前 model
- 当前工作目录
- 当前 session status

和 Codex 一样，控制结果由宿主直接回 IM，不依赖 OpenCode TUI 渲染结果。

## 权限与安全

控制命令必须满足：

- 发送人是可信好友或管理员。
- 命令在白名单内。
- 当前存在可控制的 active AICLI session。
- 命令不会造成破坏性副作用。

如果校验失败，直接回 IM：

```text
当前账号无权限执行 AICLI 控制命令。
```

或：

```text
当前没有可控制的 AICLI 会话。
```

## 回 IM 文案

建议保持简短明确：

```text
已切换到 OpenCode plan 模式。
已切换到 OpenCode build 模式。
Codex 当前任务运行中，暂不能切换 plan。
Codex 暂未支持 build 模式切换。
当前 AICLI：OpenCode
Session：ses_xxx
Agent：build
Model：idealab/Qwen3.7-Max-DogFooding
状态：idle
```

## 测试点

需要覆盖：

- 普通 IM 消息不受影响，仍生成 replyId 并只转发 tag 内回复。
- `/status` 命中控制通道，不生成 replyId。
- `/plan` 在 OpenCode 中调用 `session.switchAgent("plan")`。
- `/build` 在 OpenCode 中调用 `session.switchAgent("build")`。
- Codex `/build` 在未实现前返回明确不支持。
- `/stop` 不在白名单中，作为普通文本或直接返回不支持，不能执行 Codex stop。
- 非可信好友不能执行控制命令。
- `请帮我看 /status` 不触发控制命令。

## 实施顺序

建议按以下顺序实现：

1. 宿主侧增加 IM 控制命令解析器和白名单。
2. 宿主侧分离普通消息通道与控制命令通道。
3. OpenCode 先接 `session.switchAgent`，完成 `/plan` 和 `/build`。
4. Codex 增加源码级控制入口，先支持 `/status`、`/plan`，再补 `/build`。
5. 补测试，重点验证控制命令不污染普通 IM 回复协议。

