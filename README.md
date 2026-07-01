# Multi-AI Code

> 面向本地仓库的 AI CLI 工作台。桌面端负责项目、任务、终端、代码审查、仓库查看和远程 IM；iOS 端作为移动 IM 入口，方便从手机把消息发给桌面端 AICLI。

![Multi-AI Code](build/icon-256.png)

## 当前定位

Multi-AI Code 当前是单阶段工作台，不再是早期的四阶段流水线。

核心目标：

- 打开一个本地仓库作为项目。
- 选择普通任务或定时任务，把任务交给 Claude Code / Codex 等 AICLI。
- 在同一个桌面应用里完成终端协作、代码审查、仓库查看、项目构建和项目运行。
- 通过远程 IM 从手机或另一台机器给当前 AICLI 发任务，并把 AICLI 输出回传给联系人。

## 架构概览

```text
Electron Desktop
  src/                         React renderer
  electron/                    Electron main / preload / 本地存储 / PTY / IM 路由
  bin/imcli*                   给 AICLI 调用的 IM 命令行工具

iOS Remote IM
  ios/MultiAIIM/               SwiftUI iOS App
  ios/MultiAIIM/MultiAIIMCore  IM 状态、联系人、消息模型

Local Data
  ~/MultiAICode/               桌面端全局数据目录
  <target_repo>/.multi-ai-code 仓库级任务与记忆目录
```

桌面端是主控工作台；iOS 端只做远程 IM 客户端。AICLI 仍然是本机真实安装的 CLI，不是内置模型。

## 核心能力

### 主会话终端

主界面中间是一个真实 PTY 终端，由 AI CLI 驱动。

- 支持 `Claude Code` 或 `Codex`。
- 支持普通任务模式和定时任务模式。
- 支持继续上一次 AICLI 会话。
- 支持拖拽文件到终端插入路径。
- 终端输出做了 Markdown 风格格式化，便于阅读长文本。
- 远程 IM 消息会以受控 prompt 注入到当前会话。

### 普通任务

普通任务是跟仓库走的任务文档。

- 内部任务保存在 `<target_repo>/.multi-ai-code/designs/`。
- 可以创建、选择、预览、编辑任务描述和详情。
- 可以导入外部 Markdown 文件作为任务来源。
- 启动 AICLI 时会把任务路径交给 CLI，由 CLI 自行读取。

### 定时任务

定时任务用于按时间触发 AICLI。

- 任务列表按项目保存。
- 支持启用、禁用、立即运行和查看运行状态。
- 定时任务会进入专门允许接收定时任务 prompt 的会话，避免和普通任务混用。

### 代码审查

`代码审查` 是面向当前改动的双栏审查窗口。

- 查看工作区改动、最近一次提交或指定 commit。
- 支持对 diff 单行或多行写批注。
- 批注可以发送回当前主会话继续修正。
- 定时任务模式下会限制部分审查批注行为，避免任务上下文混乱。

### 仓库查看

`仓库查看` 是独立代码浏览与分析窗口。

- 左侧文件树，中间源码，右侧分析对话和标注列表。
- 自动过滤 `.git`、`node_modules`、`dist`、`build`、`out` 等大目录。
- 支持选中代码片段后添加标注并发送给独立分析 AI。
- 分析记忆可以写入仓库级目录，便于下次打开同一仓库时恢复上下文。

### 项目构建和项目运行

项目可以配置构建流程和运行流程。

- Windows 支持 MSYS / Visual Studio 相关环境。
- macOS 直接使用原始环境，不展示 Windows 专用环境选项。
- 构建和运行配置跟项目走，适合不同仓库维护不同命令。

## 远程 IM

远程 IM 基于 Tencent IM。当前已经去掉显式主人 / 奴隶模型，统一为可信好友联系人。

桌面端行为：

- 启动后先进入 IM 登录入口；登录成功或失败都可以进入主页。
- 登录只要求 UserID，SDKAppID 和 UserSig SecretKey 使用内置测试凭证。
- 当前默认 SDKAppID 是 `1600148979`。
- 设置中心只展示远程 IM 摘要，不再修改基础账号配置。
- 远程 IM 抽屉支持拖动、最近会话、好友列表、添加联系人、清空消息。
- 好友发来的文本消息默认送入当前本机 AICLI。
- AICLI 输出会被抽取成干净 Markdown，再回发给对应好友。
- 发送状态用对勾图标显示，减少消息气泡里的文字噪声。

### iOS 端

iOS App 是移动端远程 IM 客户端，主要用于手机上发消息给桌面端。

- 首屏是登录页，只需要输入 UserID。
- 底部为 `消息 / 通讯录 / 我` 三栏。
- `消息` 页展示会话列表，点击联系人进入聊天。
- `通讯录` 页可以添加可信好友 UserID。
- `我` 页展示账号、SDKAppID、连接状态，并支持连接 / 断开。
- 文本输入支持回车发送。
- 语音消息采用类似微信的按住说话交互，发送后本端和远端都可以播放。

## imcli

`imcli` 是给 AICLI 使用的本地命令行工具。桌面端启动后会在本机开一个带 token 的本地 bridge，并写入：

```text
~/MultiAICode/imcli-bridge.json
```

AICLI 收到需要查询或操作 IM 的任务时，可以先运行：

```bash
imcli help
```

常用命令：

```bash
imcli whoami --project <projectId>
imcli contacts --project <projectId>
imcli history --project <projectId> --peer <userId> --limit 20
imcli last --project <projectId> --peer <userId>
imcli send <userId> "消息内容" --project <projectId>
imcli forward <userId> --message-id <id> --project <projectId>
imcli broadcast <user1,user2> "消息内容" --project <projectId>
```

桌面端注入给 AICLI 的远程 IM prompt 中会提示：如果需要查询或操作 IM，请先运行 `imcli help`。

## 配置与数据归属

| 配置 / 数据 | 归属 | 位置 | 说明 |
| --- | --- | --- | --- |
| 项目列表、项目 ID、项目元数据 | 本机 | `~/MultiAICode/projects/<projectId>/project.json` | 记录项目名和目标仓库路径 |
| SQLite 数据库 | 本机 | `~/MultiAICode/multi-ai-code.db` | 事件、消息、定时任务、运行记录等 |
| IM 登录账号 | 本机账号配置 | `~/MultiAICode/remote-im-profiles/<profile>/remote-im-account.json` | UserID、SDKAppID、联系人等 |
| 远程 IM 项目开关和输出参数 | 项目配置 | `project.json` 内的 `remote_im_config` | 是否启用、输出分片参数等 |
| 普通任务文档 | 仓库 | `<target_repo>/.multi-ai-code/designs/` | 跟仓库走，可随仓库迁移 |
| 普通任务描述 / 详情 | 项目元数据 | `project.json` | 用于 UI 列表展示 |
| 定时任务 | 本机项目数据 | SQLite `scheduled_tasks` | 跟项目 ID 走，不写入仓库 |
| 仓库查看记忆 | 仓库 | `<target_repo>/.multi-ai-code/repo-memory/` | 私有分析记忆 |
| imcli bridge | 本机运行态 | `~/MultiAICode/imcli-bridge.json` | 当前桌面进程启动后生成 |
| 本地 ASR 运行资源 | 安装包资源 | `resources/asr` | 打包时生成，随安装包携带，不要求用户本机配置 |

原则：

- 任务文档和仓库分析记忆跟仓库走。
- IM 账号、SDKAppID、UserSig 凭证和当前登录态跟本机用户走。
- AICLI 会话运行态跟当前桌面进程和项目走。
- 普通任务 / 定时任务是不同运行模式；继续上一次会话时，应用会尽量保持会话和当前项目、当前任务模式一致。

## 远程 IM 语音转文字

手机或其它端发送语音消息给桌面端时，桌面端会调用内置 Whisper 做语音转文字，再把转写文本发送给当前 AICLI。

当前方案使用本地 `whisper.cpp`，不依赖腾讯 IM 付费语音转文字插件。

### 运行时行为

- 桌面端安装包会携带 `whisper-cli` 和 `ggml-base.bin` 模型。
- Windows 安装包同时携带 `ffmpeg.exe`，用于把 `m4a`、`amr` 等语音格式转成 Whisper 更稳定支持的 `wav`。
- macOS 优先使用安装包里的 ASR 资源，音频转码使用系统自带 `/usr/bin/afconvert`。
- 用户侧不需要配置 `MULTI_AI_CODE_WHISPER_*`、`ffmpeg` 路径或模型路径。

如果运行时找不到 ASR 组件或模型，桌面端会提示重新安装 Multi-AI Code。这通常说明安装包没有正确携带 `resources/asr`。

### 开发和打包

本地开发或打包前可以手动准备 ASR 资源：

```bash
npm run prepare-asr
```

`npm run dist`、`npm run dist:mac`、`npm run dist:win` 和 `npm run dist:all` 会自动执行这一步。生成后的资源目录结构为：

```text
resources/asr/
  models/ggml-base.bin        # Git LFS
  darwin-arm64/bin/whisper-cli
  darwin-arm64/bin/libggml-*.so
  darwin-arm64/lib/*.dylib
  win32-x64/bin/whisper-cli.exe
  win32-x64/bin/ffmpeg.exe
```

`ggml-base.bin` 通过 Git LFS 进入仓库。其它平台运行时二进制体积较大，默认被 `.gitignore` 忽略，由 `npm run prepare-asr` 在打包前生成；最终都会通过 Electron Builder 的 `extraResources` 放进安装包。

## 安装与运行

### 环境要求

- Node.js 20+
- macOS 或 Windows
- 本机至少安装一种 AI CLI：
  - `claude`
  - `codex`

如果使用 `Codex`，需要确认命令行可直接运行；如果使用 `Claude Code`，也需要保证本地 CLI 已安装并登录可用。

### 桌面端本地启动

```bash
npm install
npm run dev
```

说明：

- `npm install` 后会自动执行 `electron-rebuild`。
- 原生依赖包括 `better-sqlite3` 和 `node-pty`。
- `npm run dev` 会启动 Electron 桌面端和 renderer dev server。

### iOS 本地运行

iOS 工程位于：

```text
ios/MultiAIIM/MultiAIIM.xcworkspace
```

常用命令：

```bash
xcodebuild build -workspace ios/MultiAIIM/MultiAIIM.xcworkspace -scheme MultiAIIM -destination 'platform=iOS Simulator,name=iPhone 17'
xcodebuild test -workspace ios/MultiAIIM/MultiAIIM.xcworkspace -scheme MultiAIIMCoreTests -destination 'platform=iOS Simulator,name=iPhone 17'
```

安装到真机时需要本机 Xcode 已登录有效开发者账号，并指定 `DEVELOPMENT_TEAM`。

## 打包

### 构建桌面端

```bash
npm run build
```

### macOS

```bash
npm run dist:mac
```

### Windows

```bash
npm run dist:win
```

Windows 端安装包会携带语音转文字所需的 `whisper-cli.exe`、`ffmpeg.exe` 和模型文件，用户机器不需要额外配置环境变量。

### 同时构建多平台

```bash
npm run dist:all
```

## 测试

```bash
npm run typecheck
npm test
```

常用局部验证：

```bash
npx vitest run electron/remote-im/localWhisper.test.ts electron/remote-im/router.test.ts src/remote-im/tencentImClient.test.ts
```

iOS 单元测试：

```bash
xcodebuild test -workspace ios/MultiAIIM/MultiAIIM.xcworkspace -scheme MultiAIIMCoreTests -destination 'platform=iOS Simulator,name=iPhone 17'
```

## 技术栈

- Electron 33
- React 18
- TypeScript 5
- electron-vite
- node-pty
- xterm.js
- react-markdown + remark-gfm
- better-sqlite3
- Tencent IM Web SDK
- SwiftUI iOS App
- Tencent IM iOS SDK
- whisper.cpp + ffmpeg

## 使用边界

- AICLI 依赖本机真实安装的 `claude` 或 `codex`，不是内置模型。
- 远程 IM 当前使用内置测试凭证，不适合作为正式上架 App Store 的生产配置。
- 腾讯 IM SDK 自带的语音转文字属于增值能力；当前默认走本地 Whisper。
- ASR 模型通过 Git LFS 管理，平台运行时二进制由 `npm run prepare-asr` 生成并随安装包携带。
- 本地私有记忆默认不进入 git，但如果手动调整 `.git/info/exclude`，仍需要自行确认。
- README 以当前主分支实现为准；历史版本的界面和能力可能不同。
