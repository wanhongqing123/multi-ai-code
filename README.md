# Multi-AI Code

> 持续交付集成管理平台 · 把多个 AI CLI（Claude Code / Codex）编排成一条流水线

![stages](build/icon-256.png)

## 核心能力

一条 4 阶段流水线，每个阶段由一个独立的 AI CLI 实例驱动：

```
① 方案设计 → ② 方案实施 → ③ 方案验收 → ④ 测试验证
```

- **方案设计**：codex `--full-auto`（隔离 workspace 沙箱，仅能写 design.md）
- **方案实施**：claude `--permission-mode auto`（**唯一能改源码的阶段**）
- **方案验收**：claude（读 + 对照设计严格验收，产出含测试标准的 acceptance.md）
- **测试验证**：claude（只读 + 跑测试命令，按 acceptance.md 的标准执行）

### 关键特性

- **自动流转 + 用户确认**：每阶段输出 `<<STAGE_DONE artifact=...>>` 标记，平台扫描到后弹抽屉，用户确认才进入下一阶段
- **Review 审批清单**：Stage 3 fail 时抽屉列出每条问题，用户勾选后才把需要修复的发回 Stage 2
- **反向反馈**：任意阶段点 `↺ 回退`，把问题 + 上下文注入上游阶段
- **设计文档贯穿**：Stage 3/4 的 handoff 自动附带 Stage 1 原始设计，保证验收/测试有权威基准
- **产物版本化归档**：每次完成自动快照到 `artifacts/history/stageN/<timestamp>.md` + SQLite 索引，顶栏「📋 历史」可查
- **项目仓库切换**：左上角「📂 打开项目」选择目标 repo，所有阶段共享该项目上下文
- **6 宫格仪表盘 + 双击放大 + 一键启动/终止全部**

## 运行

```bash
npm install           # 自动跑 electron-rebuild 处理 better-sqlite3 / node-pty
npm run dev           # 启动开发模式
npm run dist:mac      # macOS DMG
npm run dist:win      # Windows (需在 Windows 机器或 CI 上跑)
```

## 技术栈

- **Electron 33** + **React 18** + **TypeScript 5**
- **electron-vite** 构建
- **node-pty** 驱动真实 CLI 子进程（带 PTY）
- **xterm.js** 终端渲染
- **better-sqlite3** 持久化项目 / 阶段 / 产物索引

## 数据目录

```
~/MultiAICode/
├── multi-ai-code.db                  # SQLite: projects / stages / events / artifacts
└── projects/<id>/
    ├── project.json                  # 项目元数据 (name / target_repo)
    ├── workspaces/
    │   ├── stage1_design/            # 独立空目录（codex 沙箱）
    │   ├── stage2_impl  → target_repo
    │   ├── stage3_acceptance → target_repo
    │   └── stage4_test → target_repo
    └── artifacts/
        ├── impl-summary.md           # 最新副本
        ├── acceptance.md
        ├── test-report.md
        └── history/                  # 所有历史版本
            └── stage{N}/<timestamp>.md
```

## 跨平台打包

macOS 本机可直接出 DMG。Windows 因为 `node-pty` 是原生模块不支持跨编译，推荐两种方式：

1. **GitHub Actions**（已配置 `.github/workflows/build.yml`，push tag 或手动触发即可）
2. **Windows 本地**：装 Node 20 + Python 3.11 + VS Build Tools (Desktop C++)
