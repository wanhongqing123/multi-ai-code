# Multi-AI Code —— 持续交付集成管理平台

## Context

用户希望把 6 个 Claude Code CLI 实例编排成一条持续交付流水线，覆盖：

1. 日常问答
2. 方案设计
3. 方案实施
4. 代码 Review
5. 项目测试
6. 交付验证

关键诉求：
- 方案设计（2）→ 方案实施（3）自动流转
- 实施遇到问题时可**反向**把问题反馈给方案设计（3→2）
- Review（4）完成后，问题**自动**回到方案实施（4→3）修复
- 修复完成进入测试（5），测试完成进入交付验证（6）

交付物是一个**跨平台桌面 GUI 工具**（目标工作目录 `/Users/hongqingwan/OpenSource/multi-ai-code`，当前为空目录），能：
- 同时管理多个项目，每个项目有独立的 6 CC 流水线
- 可视化阶段状态和流转
- 在阶段间传递产物（设计文档、Review 意见、测试报告等）
- 支持手动回退触发反向反馈

## 关键决策（已与用户确认）

| 决策点 | 选择 | 备注 |
|---|---|---|
| 形态 | Electron + React + TypeScript | 跨平台最成熟，node-pty / xterm.js 生态完备 |
| 实例数 | 6 个独立 CC 进程 | 每阶段一个 |
| 工作目录 | 1/2 独立空目录（纯对话）；3/4/5/6 共享同一 `target_repo` | 通过 symlink 指向 target_repo |
| 阶段流转 | 正向半自动（CC 输出结束标记 + 用户确认）；反向手动触发 | 反馈涉及判断，需人介入 |
| 完成信号 | CC 按系统 prompt 约定输出 `<<STAGE_DONE artifact=... verdict=...>>` | 主进程扫描 PTY 输出流 |
| CC 驱动 | 子进程 + PTY（node-pty + xterm.js） | 用户看到真实 CC CLI 交互界面 |
| 多项目 | 支持，每项目独立 6 实例 | 项目切换式 |
| 持久化 | SQLite（better-sqlite3）存项目/阶段/事件/产物索引；CC 子进程不恢复（重启时重新注入最近 artifact 作为上下文） | - |
| 主界面 | 6 宫格仪表盘，点击放大 | 底部 Pipeline 流水线条 |
| 确认弹框 | 侧边抽屉（非阻断） | 能边看终端边看产物 diff |
| MVP | 框架 + 单阶段走通（先做阶段 2 方案设计） | 验证 prompt 注入 / 完成标记检测 / artifact 落盘的完整链路 |

## 整体架构

```
┌─────────────── Electron 主进程（Node.js / TS）───────────────┐
│                                                               │
│  ProjectManager ── 管理多个 Project                           │
│        │                                                      │
│        └─> Project ── 持有 6 个 Stage + Orchestrator          │
│                 │                                             │
│                 └─> Stage(1..6) ── 持有 PtyCCProcess          │
│                        │                                      │
│                        ├── 监听 PTY 输出流：                  │
│                        │     • 转发给 Renderer（xterm.js）    │
│                        │     • 扫描 <<STAGE_DONE …>> 标记     │
│                        │                                      │
│                        └── Orchestrator 接收完成事件：        │
│                               ├─ 正向：读 artifact → 抽屉     │
│                               │         确认 → 注入下一 Stage │
│                               └─ 反向（用户手动回退按钮）：  │
│                                        问题 + 上下文 → 目标   │
│                                        Stage 的 stdin         │
│                                                               │
│  PersistenceStore（better-sqlite3）                           │
│       projects / stages / events / artifacts                  │
└───────────────────────────────────────────────────────────────┘
                ↕ IPC (contextBridge preload)
┌────────────── Renderer（React + TS + xterm.js）──────────────┐
│  6 宫格仪表盘 + 底部 Pipeline 条 + 右侧完成确认抽屉           │
└───────────────────────────────────────────────────────────────┘
```

## 工作目录与产物约定

```
~/MultiAICode/projects/<project-id>/
├── project.json                # 项目元数据：名称、target_repo 路径、当前阶段
├── target_repo/                # 真实代码仓（用户自选 clone / 新建 / symlink）
├── workspaces/
│   ├── stage1_qa/              # 空目录
│   ├── stage2_design/          # 空目录，设计文档产出处
│   ├── stage3_impl    -> target_repo   (symlink)
│   ├── stage4_review  -> target_repo
│   ├── stage5_test    -> target_repo
│   └── stage6_deliver -> target_repo
└── artifacts/                  # 阶段间传递的产物副本
    ├── design.md
    ├── review.md
    ├── test-report.md
    └── history.jsonl           # 流转事件日志
```

### 完成标记约定（注入到各 Stage 启动时的系统 prompt）

| Stage | 完成时输出 | artifact |
|---|---|---|
| 2 设计 | `<<STAGE_DONE artifact=artifacts/design.md>>` | design.md |
| 3 实施 | `<<STAGE_DONE summary="..." diff_ref=HEAD>>` | git diff |
| 4 Review | `<<STAGE_DONE artifact=artifacts/review.md verdict=pass\|fail>>` | review.md |
| 5 测试 | `<<STAGE_DONE artifact=artifacts/test-report.md verdict=pass\|fail>>` | test-report.md |
| 6 交付 | `<<STAGE_DONE verdict=delivered>>` | - |

## 关键模块接口

### 主进程

```ts
class PtyCCProcess {
  constructor(opts: { cwd: string; env: Record<string,string>; cols: number; rows: number })
  start(): void                                    // spawn 'claude' CLI
  write(data: string): void                        // 注入 stdin
  onData(cb: (chunk: string) => void): void        // 原始输出流
  onStageDone(cb: (meta: StageDoneMeta) => void): void
  resize(cols: number, rows: number): void
  kill(): void
}

class Stage {
  id: 1|2|3|4|5|6
  status: 'idle'|'running'|'awaiting-confirm'|'done'|'blocked'
  cc: PtyCCProcess
  artifact?: string
  injectSystemPrompt(text: string): void           // 启动时注入角色 + 完成标记约定
  injectHandoff(payload: HandoffPayload): void     // 注入上一阶段产物 / 反向反馈问题
}

class ProjectOrchestrator {
  stages: Record<1..6, Stage>
  advance(from: StageId): void                     // 正向：生成 handoff → 注入下一阶段
  feedback(from: StageId, to: 2|3, note: string): void  // 反向反馈
  resume(): void                                   // 启动时从 SQLite 恢复
}
```

### StageDoneScanner（处理 ANSI + 跨 chunk 边界）

- 维护 tail buffer
- 正则 `<<STAGE_DONE([^>]*)>>` 解析 key=value 元数据
- 发现标记后：读 artifact 文件 → 触发 `stage:done` IPC → 状态切 `awaiting-confirm`

### IPC 通道（contextBridge 白名单）

| 通道 | 方向 | 用途 |
|---|---|---|
| `cc:data` | main → renderer | PTY 输出流 |
| `cc:input` | renderer → main | 用户在终端打字 |
| `cc:resize` | renderer → main | 窗口 resize |
| `stage:done` | main → renderer | 完成标记检测 |
| `stage:advance` | renderer → main | 用户确认进入下一阶段 |
| `stage:feedback` | renderer → main | 回退反馈 |
| `project:create/open/list/delete` | renderer → main | 项目管理 |

## UI 设计

### 主界面：6 宫格仪表盘

- 每格一个 `StagePanel`：xterm.js 小号字体 + 状态徽章（idle / running / awaiting-confirm / done / blocked）
- 点击某格放大为全屏模式（保留侧边缩略图可切换回来）
- 底部 **Pipeline 条**：可视化当前位置 `① ② ─▶ [③] ─ ④ ─ ⑤ ─ ⑥`，可点击回退箭头
- 顶部右上：`[Projects ▾]` 项目切换下拉

### 完成确认：右侧抽屉（非阻断）

- 触发：扫描到 `<<STAGE_DONE ...>>`
- 内容：artifact markdown 预览（或 git diff）
- 按钮：`[编辑]` `[驳回]` `[确认 → 进入下一阶段]`
- 非阻断：用户可以边看终端边看产物，也能关掉抽屉稍后再决定

### 回退反馈：小浮层

- 每个 Stage 右上角 `↺ 回退` 菜单
- 选目标阶段 2 或 3 + 填问题描述
- 平台拼 prompt 注入目标 Stage 的 stdin

## MVP 里程碑

MVP 聚焦**框架走通 + 阶段 2 单点闭环**。

### M1：项目骨架
- Electron + Vite + React + TS 脚手架
- 预加载脚本（contextBridge 白名单 IPC）
- electron-builder 打包配置（macOS / Windows / Linux）
- SQLite schema 初始化：`projects`, `stages`, `events`, `artifacts`
- 工程目录生成器（创建 `~/MultiAICode/projects/<id>/...` 结构 + symlinks）

### M2：单 CC 终端接入
- `PtyCCProcess` 封装：node-pty spawn `claude`、stdin/stdout/resize
- xterm.js 渲染 + IPC 双向桥接
- 单阶段 demo：能启动 CC、交互、resize 正常

### M3：阶段 2 闭环
- 阶段启动时注入系统 prompt（角色定义 + `<<STAGE_DONE artifact=...>>` 约定 + 产物路径 `artifacts/design.md`）
- `StageDoneScanner`：ANSI 剥离、跨 chunk 缓冲、正则解析元数据
- 检测到标记 → 右侧抽屉显示 markdown 预览
- 用户 `[确认]` → artifact 归档、写 `events` 表、状态切 `done`

### M4：持久化与恢复
- 应用重启后：从 SQLite 恢复项目列表、各阶段状态、最近 artifact 路径
- CC 子进程**不恢复**（重新 spawn）；启动时把该阶段最近 artifact 作为上下文重新注入（让 CC 接上）

### 非 MVP（后续迭代）
- 扩展到阶段 1 / 3 / 4 / 5 / 6
- 反向反馈 UI（回退浮层）
- 6 宫格仪表盘完整交互 + 放大/缩略切换
- Pipeline 可视化条
- 多项目切换完善

## 需要修改/创建的关键文件（MVP）

| 路径 | 说明 |
|---|---|
| `package.json` | 项目配置 + 依赖（electron, react, xterm, node-pty, better-sqlite3, electron-builder） |
| `electron/main.ts` | 主进程入口 |
| `electron/preload.ts` | contextBridge IPC 白名单 |
| `electron/cc/PtyCCProcess.ts` | CC 子进程封装 |
| `electron/cc/StageDoneScanner.ts` | 完成标记扫描器 |
| `electron/orchestrator/Stage.ts` | 阶段模型 |
| `electron/orchestrator/ProjectOrchestrator.ts` | 项目编排 |
| `electron/orchestrator/handoff.ts` | 跨阶段 prompt 构造 |
| `electron/store/db.ts` | SQLite schema + 查询 |
| `electron/store/paths.ts` | `~/MultiAICode/projects/...` 目录生成 |
| `electron/prompts/stage2-design.md` | 阶段 2 系统 prompt 模板 |
| `src/App.tsx` | Renderer 根组件 |
| `src/components/StagePanel.tsx` | 单个阶段面板（xterm.js） |
| `src/components/CompletionDrawer.tsx` | 完成确认抽屉 |
| `src/ipc.ts` | Renderer 侧 IPC 封装 |
| `vite.config.ts` / `tsconfig.json` / `electron-builder.json` | 构建配置 |

## 验证方式（MVP）

1. **手动端到端**：
   - `pnpm dev` 启动 Electron
   - 新建项目 → 选 target_repo
   - 阶段 2 CC 启动，对话 → 生成 `artifacts/design.md`
   - CC 输出 `<<STAGE_DONE artifact=artifacts/design.md>>`
   - 抽屉弹出 markdown 预览
   - 点确认 → `stages.state` 变为 `done`，`events` 表有新记录

2. **持久化**：
   - 关闭应用 → 重开 → 项目列表、当前阶段、artifact 均恢复
   - CC 进程重新启动，能看到最近 artifact 的上下文已注入

3. **单元测试**（Vitest）：
   - `StageDoneScanner`：ANSI 转义、跨 chunk 边界、多种 artifact/verdict 参数
   - `handoff` 构造：产物路径正确、prompt 模板渲染

4. **集成测试**：
   - 用 mock PTY（回放预录制的 CC 输出流）模拟完整阶段 2 生命周期
   - 断言最终状态机转换和 artifact 落盘

5. **跨平台冒烟**：
   - macOS / Windows / Linux 至少各跑一次 M2 的 "CC 启动 + 交互 + resize"
