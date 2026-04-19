# Stage1 方案落盘路径迁移

日期：2026-04-19
作者：hongqingwan + Claude

## 背景

当前 Stage1（方案设计阶段）的最终产物 markdown 文件落在：

```
~/MultiAICode/projects/<projectId>/workspaces/stage1_design/<方案名>.md
```

这个目录是 Stage1 的 cwd，刻意设计为隔离空目录（防 AI 在方案设计阶段碰到源码）。但它同时也成了"方案文件的最终归档点"，带来两个问题：

1. 方案文件没法跟代码仓库一起被 git 管理。
2. 方案文件藏在 `~/MultiAICode/` 用户主目录下，跨设备/跨成员不可见。

## 目标

将 Stage1 最终产物落到**当前打开项目（target_repo）**内的专属隐藏目录：

```
<target_repo>/.multi-ai-code/designs/<方案名>.md
```

cwd **保持不变**（仍是隔离空目录），维持"AI 视野干净 + 物理隔离源码"的安全模型。

## 非目标

- **不迁移历史项目**的旧位置方案文件。历史项目需看老方案自行搬运。
- **不改动 Stage 2-4** 的产物路径。它们仍落在 `<projectDir>/artifacts/`。
- **不添加**迁移工具或向后兼容探测逻辑。

## 设计

### 路径与目录

| 项 | 旧 | 新 |
| --- | --- | --- |
| Stage1 cwd | `<projectDir>/workspaces/stage1_design/` | 不变 |
| Stage1 artifact | `<projectDir>/workspaces/stage1_design/<方案名>.md` | `<target_repo>/.multi-ai-code/designs/<方案名>.md` |
| Stage 2-4 | 不变 | 不变 |

### 代码改动

#### 1. `electron/store/paths.ts`

- 新增 `designArchiveDir(targetRepo: string): string`，返回 `<target_repo>/.multi-ai-code/designs`。
- `createProjectLayout(projectId, targetRepoPath)` 里额外 `mkdir -p` 上面那个目录。
- `workspaceDir` / `STAGE_DIR_NAME` 等保持不变——`workspaces/stage1_design/` 仍需作为 cwd。

#### 2. `electron/orchestrator/prompts.ts`

- `stageArtifactPath(stageId, label, targetRepo?)` 签名增加可选 `targetRepo`。
  - `stageId === 1` 且 `targetRepo` 存在时，返回**绝对路径** `<targetRepo>/.multi-ai-code/designs/<safeLabel>.md`（label 缺省用 `design`，仍走原有 sanitize 逻辑）。
  - 其他情况维持旧行为（project-dir-relative）。
- `STAGE_ARTIFACTS[1]` 由于不再是统一相对路径，变成说明性占位或直接删除——改由 `stageArtifactPath` 动态计算。推荐删除并让所有调用点显式走 `stageArtifactPath`。
- `renderTemplate`：`planPending` 分支的占位符路径改成 `<targetRepo>/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md`。需要 `RenderContext` 暴露 `targetRepo`（已有字段，直接用）。

#### 3. `electron/cc/ptyManager.ts`

所有调用 `stageArtifactPath(1, ...)` 的位置需要传入 `targetRepo`；原本 `join(pdir, stageArtifactPath(1, ...))` 这种组装绝对路径的方式：

- 对 stage 1：`stageArtifactPath` 返回的已是绝对路径，直接用。
- 对 stage 2-4：`join(pdir, ...)` 逻辑不变。

需要检查/更新的调用点（按行号索引，实现时以实际为准）：

- `:307` — 传入 `stageArtifactPath` 给 `renderTemplate.ctx.artifactPath`。
- `:397` — `stagePath`。
- `:441` — 字符串提示 `workspaces/stage1_design/<用户给的名字>.md` 改为 `.multi-ai-code/designs/<用户给的名字>.md`。
- `:558` — `join(pdir, stageArtifactPath(1, s.label))` 读取设计文档内容用于 handoff。
- `:586, :666, :746, :984` — 其他用到 stage 1 路径的地方。

#### 4. `electron/prompts/stage1-design.md`

- 第 42 行示例路径由 `…/workspaces/stage1_design/逐帧播放方案.md` 改为 `<target_repo>/.multi-ai-code/designs/逐帧播放方案.md`。

### 数据流

1. 用户启动 Stage1 → 平台进入 `<projectDir>/workspaces/stage1_design/`（空目录）作为 cwd。
2. 平台通过 `{{ARTIFACT_PATH}}` 告诉 AI 写入 `<target_repo>/.multi-ai-code/designs/<方案名>.md`。
3. AI 用 `Write` 工具（已在 Stage1 allowlist）写入该绝对路径。
4. Handoff 到下一阶段时，平台按相同绝对路径读取内容嵌入 handoff message。

### 错误处理

- `target_repo` 目录存在但不可写 → `createProjectLayout` 的 `mkdir` 会抛错，由现有错误通道处理。
- AI 尝试写入非 `.multi-ai-code/designs/<label>.md` 的路径：不额外拦截，由角色 prompt 约束即可（现有行为）。
- 旧项目的 `workspaces/stage1_design/*.md` 文件不动；下一次进入 Stage1 时，新方案会落到 target_repo 新位置——老项目首次使用新版本会感觉"之前的方案消失了"，这属于**预期行为**，不做迁移提示。

### 测试方式

- 跑一次完整的新项目 Stage1 流程，确认：
  - `<target_repo>/.multi-ai-code/designs/<方案名>.md` 被创建并有内容。
  - `<projectDir>/workspaces/stage1_design/` 仍被创建为空目录作 cwd。
  - 方案名含中文/空格时 sanitize 正常。
- Stage1 → Stage2 handoff 时，设计文档内容被正确读取并注入。
- Stage 2-4 产物路径无变化。

## 风险与权衡

| 风险 | 应对 |
| --- | --- |
| 老项目看不到旧方案 | 非目标：不迁移。用户可手动搬运。 |
| target_repo 可能在 Windows/WSL 符号链接路径下 | 路径处理统一用 `path.join`，与 Stage 2-4 当前行为一致。 |
| `.multi-ai-code/` 目录被 target_repo 的 `.gitignore` 忽略 | 预期行为——用户可以按需决定是否 track 这个目录。 |
