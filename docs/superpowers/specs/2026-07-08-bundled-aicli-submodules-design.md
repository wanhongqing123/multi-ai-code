# Bundled AICLI Submodules Design

## 背景

Multi-AI Code 当前通过系统 PATH 启动 Codex / Claude。后续需要对 Codex 和 OpenCode 做源码级定制，主要目标是让 Remote IM 不再解析 TUI / PTY 文本，而是读取结构化的 assistant 输出事件。由于 Codex 和 OpenCode 都是开源项目，Multi-AI Code 可以维护自己的定制版本，并在主仓编译、打包、默认调用这些内置二进制。

Claude 当前路径已经稳定，本设计不修改 Claude 的启动和 IM 转发逻辑。

## 仓库来源

Codex 使用两个远端：

- `origin`: `https://github.com/wanhongqing123/codex`
- `upstream`: `https://github.com/openai/codex.git`

OpenCode 使用两个远端：

- `origin`: `https://github.com/wanhongqing123/opencode`
- `upstream`: `https://github.com/anomalyco/opencode.git`

主仓通过 git submodule 锁定定制仓库的具体 commit。submodule 内部保留官方同步分支和 Multi-AI 定制分支，避免直接在官方同步分支上做产品改动。

## 目录结构

主仓新增两个 submodule：

```text
third_party/aicli/codex
third_party/aicli/opencode
```

主仓编译后复制产物到：

```text
bin/aicli/codex/darwin-arm64/codex
bin/aicli/codex/win32-x64/codex.exe
bin/aicli/opencode/darwin-arm64/opencode
bin/aicli/opencode/win32-x64/opencode.exe
bin/aicli/manifest.json
```

`package.json` 现有 Electron builder 配置已经包含 `bin/**/*`，所以 release 包只带编译产物，不把完整 submodule 源码打进 App。

## 构建流程

主仓新增构建脚本：

```text
scripts/build-aicli-codex.mjs
scripts/build-aicli-opencode.mjs
scripts/build-aicli.mjs
```

构建脚本职责：

1. 检查 submodule 是否初始化。
2. 校验本机必需工具链。
3. 编译当前平台可用的 Codex / OpenCode。
4. 将二进制复制到 `bin/aicli/<tool>/<platform-arch>/`。
5. 写入 `bin/aicli/manifest.json`，记录工具名、平台、版本、source commit 和构建时间。

普通 `npm run build` 不默认编译 AICLI，避免日常开发被 Codex/OpenCode 的重型构建拖慢。新增脚本：

```json
{
  "build:aicli": "node scripts/build-aicli.mjs"
}
```

release 构建可以在 `dist` 前显式运行 `npm run build:aicli`。

## 运行时解析

新增内置 CLI resolver：

```text
electron/aicli/bundledCliResolver.ts
```

解析规则：

1. 用户显式填写 `command` 时，尊重用户配置，不强行替换。
2. 用户选择 `codex` 时，优先使用内置 Codex 二进制。
3. 用户选择 `opencode` 时，优先使用内置 OpenCode 二进制。
4. 内置二进制不存在或不可执行时，fallback 到 PATH 中的同名命令。
5. Claude 仍保持原逻辑，不使用内置 resolver。

UI 配置中新增 `opencode` 作为 AICLI 类型，和 `codex` / `claude` 并列。默认推荐顺序为 Codex、OpenCode、Claude。

## IM Bridge 方向

Codex 和 OpenCode 的源码定制不通过 PTY 文本清洗解决 IM 噪音，而是在各自源码中新增 Multi-AI Bridge：

- TUI 仍正常显示，用户在桌面 AICLI 窗口中的体验不变。
- Remote IM 只读取 Bridge 输出的结构化事件。
- Bridge 输出只包含 assistant delta / assistant complete / turn started / turn completed / error 等事件。
- Bridge 通过环境变量启用，例如 `MULTI_AI_BRIDGE_SOCKET` 或 `MULTI_AI_BRIDGE_FD`。
- Bridge 不向 stdout / stderr 写业务事件，避免重新污染 PTY。

Codex 初步接入点：

- `codex-rs/tui/src/app/app_server_events.rs`
- `codex-rs/app-server-protocol/src/protocol/v2/item.rs`
- `AgentMessageDeltaNotification`
- `TurnStartedNotification`
- `TurnCompletedNotification`

OpenCode 初步接入点：

- `packages/opencode/src/session/processor.ts`
- `packages/opencode/src/cli/cmd/run.ts`
- assistant text delta / text end / message part updated 相关事件

Multi-AI Code 侧通过 replyId / turnId 映射，只转发当前 IM 请求对应的一轮输出。

## 上游同步

每个定制仓库保留两个远端：

```text
origin   = Multi-AI fork
upstream = official repository
```

Codex 建议分支：

```text
main              跟随 upstream/main
multi-ai/im-bridge Multi-AI 定制分支
```

OpenCode 建议分支：

```text
dev               跟随 upstream/dev
multi-ai/im-bridge Multi-AI 定制分支
```

同步流程：

```bash
git checkout main
git fetch upstream
git reset --hard upstream/main
git push origin main --force-with-lease

git checkout multi-ai/im-bridge
git rebase main
git push origin multi-ai/im-bridge --force-with-lease
```

OpenCode 同步官方代码时将 `main` 替换为 `dev`。主仓 submodule 始终锁定
`multi-ai/im-bridge` 上的具体 commit，而不是浮动分支。

## 错误处理

构建脚本遇到以下情况要给出明确错误：

- submodule 未初始化。
- 缺少 Rust/Cargo。
- 缺少 Bun 或 Bun 版本不满足 OpenCode 要求。
- 构建产物不存在。
- 目标平台没有对应产物。

运行时 resolver 遇到内置二进制缺失时不阻断启动，fallback 到 PATH，并在日志中记录 fallback 原因。

## 测试计划

主仓测试：

- resolver 优先返回内置 Codex。
- resolver 优先返回内置 OpenCode。
- 用户显式 `command` 时不替换。
- 内置二进制缺失时 fallback 到 PATH。
- Claude 不走内置 resolver。

构建验证：

- `npm run build:aicli` 能在 macOS arm64 生成 Codex / OpenCode 产物。
- `bin/aicli/manifest.json` 记录正确的 source commit。
- Electron 打包产物包含 `bin/aicli`。

Bridge 验证：

- Codex TUI 显示保持不变。
- OpenCode TUI 显示保持不变。
- Remote IM 不再收到 TUI 状态栏、提示语、快捷命令建议等噪音。
- 同一轮 IM 请求只转发当前 replyId / turnId 对应输出。

## 非目标

- 不修改 Claude 的 IM 转发路径。
- 不在第一阶段自研完整 AICLI agent。
- 不把完整 Codex / OpenCode 源码打包进最终 App。
- 不让普通 `npm run build` 默认编译 Codex / OpenCode。
