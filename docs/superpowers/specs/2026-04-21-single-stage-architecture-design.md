# Single-Stage Architecture — Design Spec

**Date:** 2026-04-21

**Goal:** 把 Multi-AI Code 从 4-stage pipeline（方案设计 → 方案实施 → 方案验收 → 测试验证）简化为**单 stage 架构**。一个 AI 会话覆盖方案设计、代码编写、Diff 审查后按批注修改的全过程。方案归档到 `<target_repo>/.multi-ai-code/designs/`，不再使用平台管理的 `workspaces/` 目录。

---

## 1. 拓扑

- **1 个 Stage**，无 pipeline、无 STAGE_DONE、无"下一步/上一步"按钮
- 一个活跃 AI 会话持续工作在 cwd = `target_repo`
- 每个 project 内可有多个 plan（方案），同时只有一个 plan 处于活跃会话状态
- Plan 切换 = 把当前会话停掉，spawn 新 plan 的会话

## 2. 存储

### 2.1 新方案
- 路径：`<target_repo>/.multi-ai-code/designs/<plan>.md`
- AI 自己把方案 md 写到这个路径（prompt 里明确告知）

### 2.2 导入的外部方案
- 路径：用户选择的外部原路径（例如 `/Users/x/docs/my-plan.md`）
- 由 `project.json.plan_sources` 映射 `<plan>` → 绝对路径
- AI 写方案时用该绝对路径

### 2.3 废弃的路径
- `<projectDir>/workspaces/` 整个目录 —— 启动时清理
- `stage1TmpDir()` 函数 —— 删除（方案不再需要临时 scratch 目录；AI 直接写到最终位置）

### 2.4 启动迁移
- `ensureRootDir()` 里对所有已有 project 目录 `rm -rf workspaces/`（已有部分逻辑，扩展成无条件执行）
- `createProjectLayout()` 不再创建 `workspaces/`，只创建 `artifacts/`
- DB `stage_events` / `stage_status` 中 `stage_id > 1` 的行无害，启动时 `DELETE FROM stage_events WHERE stage_id > 1` + 同样清理 `stage_status`

## 3. AI 工具

### 3.1 默认
- Claude Code（`claude`），`--permission-mode auto --allowedTools ...`

### 3.2 可选
- Codex（`codex --full-auto`）
- 由 `project.json.ai_cli` 字段控制（`"claude"` | `"codex"`），默认 `"claude"`
- UI：现有 `StageSettingsDialog` 改名为"AI 设置"或类似，从 per-stage 四套配置简化为单套 `{command, args, env}`

### 3.3 注入机制（已有，复用）
- Claude：写 `<cwd>/CLAUDE.md` 自动加载
- Codex：沿用现有 `.injections` 流程

## 4. AI 会话生命周期

### 4.1 启动（用户点"Start"或切换到一个 plan）
- spawn AI with cwd = `target_repo`
- 写 system prompt 到对应位置（CLAUDE.md 或 injections）
- 确定 plan 的绝对路径 `plan_abs_path`：
  - 内部方案：`<target_repo>/.multi-ai-code/designs/<plan>.md`
  - 导入方案：`project.json.plan_sources[<plan>]`（用户选的外部路径）
- 首条 user message 根据 `plan_abs_path` 实际是否存在（`fs.access`）分流：
  - **plan 文件已存在**：发送方案正文 + `"请基于当前方案继续工作（写代码 / 根据批注调整）。"`
  - **plan 文件不存在**（新建 / 首次）：发送 `"本次方案名：<plan>。请先与用户对话澄清需求、确认方向，然后把方案写到 <plan_abs_path>，再继续实施。"`

### 4.2 运行中
- Session 一直活着；stdout 显示在 xterm 终端里；用户可直接 stdin 与 AI 对话
- 用户随时可以打开 PlanReviewDialog 查看当前方案 md
- 用户随时可以打开 DiffViewerDialog 做代码审查

### 4.3 停止
- 用户显式停止：发 SIGTERM / 关会话
- 切换 plan：等价于停止 + 启动新 plan
- 关闭 app：所有 session 被 SIGTERM
- 方案 md 由 AI 自己持久化到文件系统，session 状态（终端输出历史）不持久化（重启后从头）

## 5. Diff 审查回灌

### 5.1 流程
1. 用户点顶栏 / 命令面板 / panel head 的 `Diff 审查` 按钮
2. 现有 `DiffViewerDialog` 照旧（左右对比、模式切换、文件选择、逐行标注、整体意见）
3. 用户点 `发送到会话`（原来写的是 `发送到 Stage 3`，改文案）
4. 所有批注组装成一个 markdown user message，通过 `window.api.cc.write(sessionId, msg)` 送到当前活跃的 AI 会话

### 5.2 批注消息格式
```
# 用户批注

以下是用户对当前改动的批注，请严格按照批注执行：修改代码、或更新方案文档（`<plan_abs_path>`）。

## 逐行批注

### `<file>:<line>`
> <quoted code snippet>

<comment>

（其他逐行批注...）

## 整体意见

<general_comment if any>

---

请按照以上批注调整代码 / 方案，完成后在终端里简述改了什么。
```

### 5.3 前置条件
- 会话必须处于 running 状态；若 idle / stopped，按钮 disabled + tooltip 提示"先启动会话"

## 6. UI 布局（全屏单面板）

### 6.1 顶栏
保持现有布局。按钮清单：
- 左：项目选择器、产品名标识、版本号、meta 信息
- 中：plan selector（选择/新建方案、导入外部方案）
- 右：⌘K、模板、时间线、设置、向导、体检、主题切换

### 6.2 主区（新）
- 之前的 `.grid`（2×2）**替换**为一个铺满剩余空间的 `.main-panel`
- 面板头：
  - 方案名（当前活跃 plan）
  - 状态 chip（idle / running / exited）
  - 按钮组：`Diff 审查`、`停止`、`重启`
- 面板体：xterm 终端 + 拖拽提示 overlay（沿用 `.term-host` / `.drop-hint`）

### 6.3 抽屉 / 弹窗
保留并适配：`TimelineDrawer`、`FilePreviewDialog`、`PlanReviewDialog`、`ProjectPicker`、`GlobalSearchDialog`、`CommandPalette`、`OnboardingWizard`、`DoctorDialog`、`TemplatesDialog`、`DiffViewerDialog`、`Toast`、`ErrorPanel`。

## 7. 删除的组件 / 概念

### 7.1 React 组件
- `StagePanel.tsx` → 删除；新建 `src/components/MainPanel.tsx`（精简版：只保留 xterm + dragdrop 核心逻辑，去掉 stage 进度、STAGE_DONE 标记解析、stage badge、反馈按钮、advance 相关 props）
- `CompletionDrawer.tsx`（"stage 完成 → 进入下一阶段"流程已不存在）
- `FeedbackDialog.tsx`（反向 feedback 回灌上游，现已无上游）
- `ReviewChecklist.tsx`（Stage 3 验收清单，已无 Stage 3）

### 7.2 Electron 逻辑
- `STAGE_DONE` marker / scanner / parser 全部移除
- pipeline 推进逻辑（`nextStageFor`、`advance`、IPC `cc:advance` 等）移除
- `STAGES` 常量从 `[1,2,3,4]` 变成 `[1]`（或完全去掉 stage 概念，用 "plan" 替代）
- `electron/prompts/stage2-impl.md`、`stage3-acceptance.md`、`stage4-test.md` 删除
- `STAGE_CLI_ARGS[2/3/4]`、`STAGE_COMMAND[2/3/4]`、`STAGE_NAMES[2/3/4]` 删除

### 7.3 路径
- `workspaceDir()`（早已删除）
- `stage1TmpDir()` 删除
- `createProjectLayout` 不再创建 `workspaces/` 或 symlink

### 7.4 DB 清理
启动时一次性 migration：
```sql
DELETE FROM stage_events WHERE stage_id > 1;
DELETE FROM stage_status WHERE stage_id > 1;
```

### 7.5 Settings 简化
`StageSettingsDialog.tsx` 重命名为 `AiSettingsDialog.tsx`（在 React 和 CSS 里同步改名），UI 文案 "阶段配置" → "AI 设置"。从 "per-stage 四套" 简化为 "单套 AI 配置"：
```json
{
  "ai_cli": "claude" | "codex",
  "command": "claude" | "codex",  // 可覆盖默认 binary 名
  "args": ["--foo"],              // 附加 args
  "env": { "KEY": "value" }
}
```
旧的 per-stage 字段启动时忽略或迁移（取 `stage_configs[1]` 作为默认）。

## 8. 保留的组件 / 行为

- 顶栏全部按钮 / 结构
- Plan selector + 方案命名 + 导入外部方案 + plan_sources 映射
- PlanReviewDialog：查看 / 编辑当前方案 md（AI 写完方案后用户可以视觉化 review）
- DiffViewerDialog：逐行/整体批注，回灌目标改为 live session
- TemplatesDialog：作为通用片段库（之前是 per-stage 模板，现在单套）
- TimelineDrawer：事件时间线（stage_events 表；过滤 `stage_id === 1`）
- 深浅主题 / 命令面板 / Toast / ErrorPanel / Onboarding / Doctor / GlobalSearch

## 9. Prompts

### 9.1 新文件：`electron/prompts/main.md`
单一 prompt，角色 = "方案设计 + 代码实施 + 按批注调整" 三合一。核心要点：

```markdown
# 角色

你同时承担三个职责：
1. **方案设计**：与用户对话澄清需求，产出高质量实施方案
2. **代码实施**：按方案修改 `target_repo` 里的代码
3. **根据批注调整**：当收到"# 用户批注"消息时，严格按批注修改代码或方案

# 工作流

## 新方案（方案 md 不存在时）
1. 先与用户对话，澄清目标、范围、约束
2. 明确后，把方案写到 `{{ARTIFACT_PATH}}`（完整绝对路径）
3. 让用户确认方案后再开始写代码

## 已有方案（方案 md 已存在时）
1. 如果有未完成的任务，继续推进
2. 修改代码严格限定在 `{{TARGET_REPO}}` 范围内
3. 每完成一个可交付的改动，简述改了什么

## 收到用户批注
1. 严格按 "# 用户批注" 消息里的每一条逐行批注和整体意见执行
2. 修代码 / 更新方案 md 都由批注指示
3. 完成后简述

# 硬约束
- 方案文件绝对路径：`{{ARTIFACT_PATH}}`
- 代码修改只能在：`{{TARGET_REPO}}`
- 不得使用网络 (除非用户明确允许)
- 不得 push 到远端、不得 --force、不得 reset --hard

# 环境
- cwd: `{{STAGE_CWD}}`
- 项目名: `{{PROJECT_NAME}}`
- 项目根: `{{PROJECT_DIR}}`
```

具体模板变量用现有 `renderTemplate()` 机制渲染。

### 9.2 删除的 prompt 文件
`stage2-impl.md`、`stage3-acceptance.md`、`stage4-test.md`。

### 9.3 `stage1-design.md` 处置
直接删除 `stage1-design.md`，新建 `main.md`（内容按 §9.1）。

## 10. 测试

### 10.1 自动测试
- `paths.test.ts`：
  - `designArchiveDir` 行为（已有）
  - `createProjectLayout` 不再创建 workspaces
  - `ensureRootDir` 清理 workspaces
- `prompts.test.ts`：
  - 单阶段 `stageArtifactPath` 路径解析
  - `renderTemplate` 用 main.md 的变量渲染
  - 批注消息格式化（新增 `formatAnnotationsForSession()` 函数 + 测试）
- `plans.test.ts`：
  - 新建 plan、切换 plan、导入外部 plan 的文件落位

### 10.2 手动验收 checklist
- [ ] 空项目：new plan → AI 对话澄清 → 产出方案到 `<target_repo>/.multi-ai-code/designs/foo.md`
- [ ] 让它继续写代码：验证只在 `target_repo` 里 write
- [ ] 打开 Diff 审查 → 逐行标注 + 整体意见 → 发送到会话 → AI 响应改代码
- [ ] 导入外部方案：选文件 → 归档到外部路径 → 启动 session → AI 基于该方案工作
- [ ] 切 plan：stop 当前会话、spawn 新 plan 会话
- [ ] 切 AI：settings 里切到 codex，验证 spawn 走 codex 路径
- [ ] 深浅色切换、⌘K、所有 dialog 打开一次不炸
- [ ] 重启 app：plan_sources 映射持久化，重启后能选到原 plan

## 11. 迁移策略

这是一次 breaking 重构。app 尚未 1.0 / 无外部用户，采用"推倒 + 一次性清理"：
- 升级后首次启动：自动删除 `~/MultiAICode/projects/*/workspaces/`
- DB 旧 stage 2/3/4 数据一次性清除
- plan_sources 映射保留（外部方案路径不变）
- 旧 stage1 artifact（`workspaces/stage1_design/<plan>.md`）如果存在 → 复制到 `<target_repo>/.multi-ai-code/designs/<plan>.md`，旧文件随 `workspaces/` 一起删掉

## 12. 非目标

- 不引入新的 AI CLI 适配（只保留 Claude / Codex）
- 不做会话历史持久化（终端输出重启后丢失）
- 不做多会话并发（一个 project 同时只跑一个 plan 的会话）
- 不做 DB schema breaking migration（旧表结构保留，仅清理超出 stage 1 的行）
- 不做向后兼容模式（不保留"旧四阶段"切换开关）

## 13. 风险

- **回灌批注会打断 AI 的当前任务**：如果 AI 正在思考/输出时用户发批注，AI 需要正确处理中断。xterm 直接 write 到 pty，AI 会看到 user message mid-stream。需要在 prompt 里让 AI 明确 "批注消息优先级高于当前任务"。
- **方案 md 路径写错**：AI 可能把 md 写到 cwd 根而不是 `.multi-ai-code/designs/`。prompt 里用绝对路径 `{{ARTIFACT_PATH}}` 明示，减少歧义。
- **用户中途切 AI（claude → codex）**：current session 仍是 claude，设置只影响下次 spawn。UI 上明示"生效需要重启会话"。
- **`.multi-ai-code/designs/` 放到用户仓库会被 git 追踪**：建议 prompt 要求 AI 第一次写方案时顺手在 `<target_repo>/.gitignore` 追加 `.multi-ai-code/`（如果还没有）。
