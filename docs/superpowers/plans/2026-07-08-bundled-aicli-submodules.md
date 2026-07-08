# 内置 AICLI Submodule 实施计划

> **给 agent 工作者:** 必须使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按任务逐项执行。步骤使用 checkbox（`- [ ]`）格式跟踪进度。

**目标:** 将 Codex 和 OpenCode 作为源码 submodule 接入主仓，并让 Multi-AI Code 能编译和优先启动内置二进制。

**架构:** 主仓负责 submodule 管理、统一构建脚本、构建产物落位和运行时解析。Codex / OpenCode 的源码级定制留在各自 submodule 中完成；本阶段先打通“主仓能编译并启动内置版本”的闭环。

**技术栈:** Git submodule、Node.js 构建脚本、Electron 主进程、node-pty、TypeScript、Vitest。

---

### 任务 1：添加 AICLI Submodule

**文件:**
- 创建: `.gitmodules`
- 创建: `third_party/aicli/codex`
- 创建: `third_party/aicli/opencode`

- [x] 添加 Codex submodule，来源为 `https://github.com/wanhongqing123/codex`。
- [x] 添加 OpenCode submodule，来源为 `https://github.com/wanhongqing123/opencode`。
- [x] 确认两个 submodule 工作区都是干净状态。

### 任务 2：添加内置 AICLI 构建脚本

**文件:**
- 创建: `scripts/aicli-build-utils.mjs`
- 创建: `scripts/build-aicli-codex.mjs`
- 创建: `scripts/build-aicli-opencode.mjs`
- 创建: `scripts/build-aicli.mjs`
- 修改: `package.json`

- [x] 实现平台和架构映射，例如将 Node 的 `darwin` + `arm64` 映射为 `darwin-arm64`。
- [x] 实现 submodule 校验，目录缺失时给出明确错误。
- [x] Codex 在 `third_party/aicli/codex/codex-rs` 下通过 `cargo build` 编译。
- [x] 将 Codex 产物从 `target/debug/codex` 或 `target/debug/codex.exe` 复制到 `bin/aicli/codex/<platform-arch>/`。
- [x] OpenCode 在 `third_party/aicli/opencode` 下通过 Bun 兼容命令编译。
- [x] 将 OpenCode 产物从 `packages/opencode/dist/opencode-<platform-arch>/bin/opencode` 复制到 `bin/aicli/opencode/<platform-arch>/`。
- [x] 写入 `bin/aicli/manifest.json`，记录工具名、平台、source commit 和二进制路径。
- [x] 在 `package.json` 中新增 `npm run build:aicli`。

### 任务 3：添加运行时内置 CLI 解析器

**文件:**
- 创建: `electron/aicli/bundledCliResolver.ts`
- 测试: `electron/aicli/bundledCliResolver.test.ts`
- 修改: `electron/cc/PtyCCProcess.ts`
- 视情况修改: `electron/habit/cliSpawn.ts`

- [x] 编写测试：Codex 优先解析到内置路径。
- [x] 编写测试：OpenCode 优先解析到内置路径。
- [x] 编写测试：用户自定义绝对路径或自定义命令不被替换。
- [x] 编写测试：Claude 永远不被替换。
- [x] 实现 resolver，检查 `bin/aicli/<tool>/<platform-arch>/<binary>`。
- [x] 在 PTY 启动前接入 resolver，再进入 PATH 查找。

### 任务 4：将 OpenCode 加入支持的 AICLI 类型

**文件:**
- 修改: `src/components/AiSettingsDialog.tsx`
- 修改: `electron/preload.ts`
- 修改: `electron/orchestrator/prompts.ts`
- 修改: `src/utils/cliLaunchArgs.ts`
- 修改相关测试：当前只假设 `claude | codex` 的测试都需要更新。

- [x] 将 AICLI 联合类型扩展为 `claude | codex | opencode`。
- [x] 在 UI 中新增 OpenCode 选项，位置放在 Codex 后、Claude 前。
- [x] OpenCode 启动参数使用源码已确认支持的权限跳过参数，Codex 的 1M context 配置不套到 OpenCode。
- [x] Claude 风险提示逻辑保持不变。

### 任务 5：验证

**文件:**
- 除非测试发现覆盖缺口，否则不新增文件。

- [x] 运行 resolver 相关测试。
- [x] 运行已有 CLI 参数相关测试。
- [x] 运行 `npm run typecheck`。
- [x] 条件允许时在 macOS arm64 上运行 `npm run build:aicli`。
- [x] 确认 `git status` 只包含预期的 submodule、构建脚本和运行时改动。
