# OpenCode 智谱集成设计

## 状态

已确认的第一版方向：

- Multi-AI Code 增加 OpenCode 作为第三个 AICLI。
- 第一版主要支持智谱 API。
- 用户不需要手动安装 OpenCode；应用内置一个固定版本的 OpenCode CLI。
- 主仓库不直接放 OpenCode 源码，也不先使用 git submodule。
- Multi-AI Code 接管 OpenCode 的模型、API Key、Base URL 和启动配置。

## 背景

当前项目已经支持 Claude Code 和 Codex 两类 AICLI。相关配置、启动和 PTY 集中在 Electron 桌面应用中，现有代码已经有以下模式可以复用：

- `src/components/AiSettingsDialog.tsx` 管理 AICLI 类型和参数。
- `src/utils/cliLaunchArgs.ts` 和 `electron/orchestrator/prompts.ts` 生成 CLI 启动参数。
- `electron/cc/PtyCCProcess.ts` 负责 PTY 进程启动和敏感环境变量脱敏。
- `resources/asr` 已经存在“打包时携带外部原生/CLI 资源”的目录模式。

OpenCode 官方 CLI 支持：

- 默认启动 TUI。
- `opencode run` 非交互运行。
- `opencode serve` 启动服务端。
- `OPENCODE_CONFIG_CONTENT` 或 `OPENCODE_CONFIG` 传入配置。
- 配置 `provider`、`model`、`small_model`。

因此第一版不需要深度嵌入 OpenCode 源码。更稳妥的方式是内置 OpenCode 可执行文件，然后由 Multi-AI Code 负责配置生成和启动。

## 目标

1. 用户在 Multi-AI Code 中可以选择 OpenCode。
2. 用户无需单独安装 OpenCode。
3. 第一版默认接入智谱 API。
4. OpenCode 使用 Multi-AI Code 生成的配置，不要求用户理解 OpenCode 原始配置文件。
5. API Key 不写入项目文件，不进入日志，不出现在命令行参数里。
6. 保持 Claude Code 和 Codex 的现有逻辑不受影响。
7. 保留用户显式指定 OpenCode 路径的能力，便于调试或灰度新版本。

## 非目标

第一版不做：

- 把 OpenCode 源码拷贝进主仓库。
- 把 OpenCode 作为 git submodule。
- 魔改 OpenCode 内部源码。
- 做 OpenCode 服务端模式、HTTP API 或 SDK 深度集成。
- 支持所有 OpenCode provider。
- 支持 OpenCode 自动升级。
- 接管 OpenCode 的所有高级配置项。

## 推荐方案

采用“外部源码固定版本 + 打包内置 CLI + Multi-AI Code 生成配置”的方案。

主仓库只维护集成代码、版本清单、打包脚本和最终打包资源规则。OpenCode 源码由独立 fork 或固定上游 tag 管理，构建产物复制到 `resources/aicli/opencode`。

目录建议：

```text
resources/aicli/opencode/
  version.json
  licenses/
    opencode-LICENSE
    third-party-notices.txt
  darwin-arm64/bin/opencode
  darwin-x64/bin/opencode
  win32-x64/bin/opencode.exe
  linux-x64/bin/opencode
```

配套脚本建议：

```text
scripts/prepare-opencode-assets.mjs
scripts/verify-opencode-assets.mjs
```

`version.json` 记录：

```json
{
  "name": "opencode",
  "source": "https://github.com/anomalyco/opencode",
  "version": "v1.17.13",
  "commit": "prepare-script-recorded-upstream-commit",
  "license": "MIT",
  "artifacts": {
    "darwin-arm64": {
      "sha256": "verify-script-recorded-artifact-sha256"
    }
  }
}
```

## 为什么不用 submodule

submodule 的优点是能明确记录源码版本，但第一版成本偏高：

- 普通开发需要额外执行 `git submodule update --init`。
- CI、打包、浅克隆和用户本地环境更容易出错。
- OpenCode 源码和依赖更新频繁，会给主仓库带来大量与业务无关的状态。
- 第一版不改 OpenCode 源码，submodule 的收益不明显。

如果后续需要长期修改 OpenCode 内部行为，可以再引入独立 fork，并在主仓库中只记录 fork commit 和构建产物，不把源码直接混进业务代码。

## 为什么不直接放源码

直接放源码会让主仓库变重，且每次同步 OpenCode 都会产生大量无关 diff。主仓库的职责应该是 Multi-AI Code 产品代码和集成边界，不应该承载另一个大型工具的完整开发历史。

第一版只需要可执行文件和许可证声明。源码保留在独立 fork 或上游仓库中，主仓库通过 `version.json` 锁版本即可。

## OpenCode 查找顺序

运行时按以下顺序选择 OpenCode 可执行文件：

1. 用户在设置里显式配置的 OpenCode 路径。
2. 当前系统 `PATH` 中的 `opencode`。
3. 应用内置的 `resources/aicli/opencode/<platform>/bin/opencode`。

默认用户不用配置路径。内置文件缺失时，设置页和启动失败提示应明确说明是“内置 OpenCode 运行资源缺失，请重新安装或重新打包应用”。

## 智谱配置

第一版内置一个智谱 provider。考虑到 OpenCode 是 coding agent，默认使用 GLM Coding Plan 的 OpenAI Chat Completion 端点：

```text
https://open.bigmodel.cn/api/coding/paas/v4
```

如果用户使用的是智谱开放平台普通 API Key，可在高级设置里覆盖 Base URL 为：

```text
https://open.bigmodel.cn/api/paas/v4
```

生成给 OpenCode 的配置示例：

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "multi-ai-zhipu": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "智谱 AI",
      "options": {
        "baseURL": "https://open.bigmodel.cn/api/coding/paas/v4",
        "apiKey": "{env:ZAI_API_KEY}",
        "timeout": 600000,
        "chunkTimeout": 60000
      },
      "models": {
        "glm-5.2": {
          "name": "GLM-5.2"
        }
      }
    }
  },
  "model": "multi-ai-zhipu/glm-5.2",
  "small_model": "multi-ai-zhipu/glm-5.2",
  "autoupdate": false
}
```

实现时通过 `OPENCODE_CONFIG_CONTENT` 注入这段配置。API Key 只放在 `ZAI_API_KEY` 环境变量中，不写入配置文件和命令行参数。

## 设置界面

在现有 AICLI 设置中新增 OpenCode：

```text
AI CLI：Codex / OpenCode / Claude Code

OpenCode 设置：
  模型服务：智谱 AI
  API Key：********
  Base URL：https://open.bigmodel.cn/api/coding/paas/v4
  主模型：glm-5.2
  小模型：glm-5.2
  OpenCode 路径：自动 / 自定义
```

默认值：

- `AI CLI` 仍可继续按当前产品策略显示 Codex 优先。
- 选择 OpenCode 时，模型服务第一版固定为智谱 AI。
- Base URL 默认 `https://open.bigmodel.cn/api/coding/paas/v4`。
- 主模型默认 `glm-5.2`。
- 小模型默认 `glm-5.2`，后续可扩展为低成本模型。

API Key 保存到系统安全存储：

- macOS：Keychain。
- Windows：Credential Manager 或 DPAPI。
- Linux：Secret Service；不可用时再使用本地加密 fallback。

第一版如果 Electron 侧还没有统一安全存储封装，可以先加一个最小的 `secureCredentialStore`，只服务 OpenCode/Zhipu API Key。

## 启动和会话

第一版继续走现有 PTY 路径：

```text
用户选择 OpenCode
  -> 生成 OpenCode 启动配置
  -> 解析 OpenCode 可执行文件路径
  -> 注入 OPENCODE_CONFIG_CONTENT 和 ZAI_API_KEY
  -> PTY 启动 opencode <project>
```

普通交互使用 OpenCode TUI，不改变终端 UI 模式。自动任务或普通任务的一次性调用可以后续接 `opencode run`，但第一版先保持和当前 AICLI PTY 体验一致。

## 安全和日志

必须保证：

- `ZAI_API_KEY` 不进入 renderer。
- `ZAI_API_KEY` 不写入项目目录。
- `ZAI_API_KEY` 不出现在进程命令行。
- `ZAI_API_KEY`、`ZHIPU_API_KEY`、`BIGMODEL_API_KEY`、`GLM_API_KEY` 进入 PTY 日志时要脱敏。
- `OPENCODE_CONFIG_CONTENT` 里只允许出现 `{env:ZAI_API_KEY}`，不直接展开真实 Key。

OpenCode 内置版本关闭自动升级，避免运行时下载和版本漂移：

```json
{
  "autoupdate": false
}
```

## 打包

打包前执行：

```bash
node scripts/prepare-opencode-assets.mjs
node scripts/verify-opencode-assets.mjs
```

打包规则把 `resources/aicli/opencode` 放入应用资源目录。macOS 需要对内置可执行文件纳入签名和 notarization 流程；Windows 安装包需要包含 `opencode.exe`，并在校验阶段确认文件存在且 sha256 匹配。

`.gitignore` 建议忽略大体积二进制，只保留目录、版本清单和许可证：

```text
resources/aicli/opencode/**/bin/*
!resources/aicli/opencode/version.json
!resources/aicli/opencode/licenses/**
!resources/aicli/opencode/**/.gitkeep
```

如果最终决定把小体积二进制放进仓库，应使用 Git LFS 并记录原因。

## 测试策略

单元测试：

- OpenCode 路径解析：用户路径、PATH、内置路径的优先级。
- OpenCode 配置生成：智谱默认 Base URL、模型、small_model、`{env:ZAI_API_KEY}`。
- 脱敏：OpenCode/Zhipu 相关环境变量不会进入日志。
- 启动参数：OpenCode 不误用 Claude/Codex 的参数。

集成测试：

- 缺少内置 OpenCode 时给出可操作错误。
- 存在内置 OpenCode 时能启动到 PTY。
- 自定义 OpenCode 路径优先于内置路径。

手工验证：

- macOS 安装包内置 OpenCode 可启动。
- Windows 安装包内置 OpenCode 可启动。
- 使用智谱 API Key 能完成一次基本问答。
- Claude Code 和 Codex 原有启动路径不回归。

## 后续演进

第二阶段可以评估：

- 使用 `opencode run --format json` 支持普通任务或定时任务的结构化输出。
- 使用 `opencode serve` 或 OpenCode SDK 做更深的会话管理。
- 增加更多 provider，但仍由 Multi-AI Code 统一管理配置。
- 支持 OpenCode 版本检查和用户主动升级。
- 如果确实需要内部能力，再维护独立 fork，而不是把源码直接放入主仓库。

## 参考

- OpenCode CLI 文档：https://opencode.ai/docs/cli/
- OpenCode 配置文档：https://opencode.ai/docs/config/
- OpenCode GitHub 仓库：https://github.com/anomalyco/opencode
- 智谱 OpenAI API 兼容文档：https://docs.bigmodel.cn/cn/guide/develop/openai/introduction
- 智谱 API 快速开始：https://docs.bigmodel.cn/cn/api/introduction
- 智谱 GLM Coding Plan 接入工具说明：https://docs.bigmodel.cn/cn/coding-plan/tool/others
