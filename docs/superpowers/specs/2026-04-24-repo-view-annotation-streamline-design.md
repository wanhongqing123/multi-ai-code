# Repo View 代码标注流程精简 + AI 持久记忆 — 设计

## 背景

repo-view 窗口右侧目前是两段式：
1. **上半（AnalysisPanel）** — 标注 + 问题输入 + "发送给 claude" + 流式 "AI 正在回复" 气泡
2. **下半（RepoTerminalPanel）** — 嵌入式 xterm，连接 repo-view 共享 PTY session，可直接看 CLI 全部输出并键入

下半终端落地后，上半的"流式气泡"实际就是终端输出的二次拷贝。同时支撑气泡的那套结构化流水线（临时 md 文件 + `[[MEMORY_UPDATE]]` / `[[END_OF_ANALYSIS]]` 标记 + 解析 + memory 自动更新）：
- 易卡死（Claude TUI 没就绪导致 marker 永远不出现）
- marker 文本会在终端里露出（"脏"）
- 与终端的输入路径割裂——用户在终端追问后，应用侧 memory 不会更新

本设计废除结构化流水线，把标注作为**纯文本注入**送进 AI CLI，同时把"已分析过的代码"从应用层 memory 转交给 **AI 自我维护的仓库内缓存**，让 Claude 与 Codex 共享。

## 目标

- 用户体验：标注 → 一次点击 → 出现在 AI CLI 输入框并提交，回答只在终端里看
- 不再有 "分析中…" 卡死状态
- 同一仓库二次分析同段代码时，AI 应主动复用上次结论，避免重复推理
- 跨 CLI（Claude ⇄ Codex）切换时记忆仍有效

## 非目标

- 不实现 "保存到记忆" 按钮（数据层留口，UI 待后续）
- 不展示 memory 内容（保留数据层但不再渲染）
- 不解析 / 校验 AI 是否真的写了缓存文件（不做 marker、不做 timeout）
- 不实现跨仓库的全局记忆

## 架构

### 数据流（新）

```
[CodePane 选区 + 标注]
        │
        ▼
[AnalysisPanel] ── 收集 N 条 annotation + 可选 question
        │
        ▼
buildCliInjectionText(annotations, question, repoRoot, filePath)
        │  纯文本，含"记忆约定"段
        ▼
window.api.repoView.analysisSend({ text })
        │
        ▼
sendRepoAnalysisPrompt(winId, text)   // 后端
        │  ├─ Claude/Codex 就绪等待（已存在）
        │  └─ chunked write + \r       // 已存在
        ▼
PTY → AI CLI 输入框 → 自动提交 → AI 回答（流到终端）
```

应用前端不再监听 `analysis-data` 来构建气泡，但终端组件仍订阅它做渲染——一份数据，两个独立消费者保持不变。

### AI 记忆缓存

**位置：** `<targetRepo>/.multi-ai-code/repo-view/analyses/`

**文件命名：** 源路径 → 安全文件名
- 规则：`'/'` → `'__'`，其余字符保持
- 示例：`libobs/obs-audio-controls.c` → `libobs__obs-audio-controls.c.md`
- 长度上限：超过 200 字符时尾部截断并附带 8-char SHA1 前缀（避免文件系统极限）

**文件结构（append-only）：**
```markdown
# {filePath} — Analysis cache

## 2026-04-24 22:15 · 第 52-53 行
- 标注: 这行是什么意思
- 结论:
  - …
  - …

## 2026-04-25 09:02 · 第 100-110 行
- …
```

**首次写入时同步创建：** `<targetRepo>/.multi-ai-code/.gitignore`，内容：
```
repo-view/analyses/
```
- 如果文件已存在，**追加** `repo-view/analyses/` 行（前提：未出现过）
- 选择 (i)：缓存视为本地内容，不污染团队仓库

**为什么放在仓库内：** Claude/Codex 的工具默认以 cwd 为根，不需要额外配置；用户切换 CLI 不丢记忆。

### 注入文本模板

```
仓库根: {targetRepo}
文件: {filePath}

## 标注 1（第 {start}-{end} 行）
```{lang}
{snippet}
```
说明: {comment}

[ … 第 N 条标注 … ]

## 问题
{question 或 "请按标注分析"}

## 记忆约定
- 已有分析缓存：.multi-ai-code/repo-view/analyses/{encodedPath}.md
- 若该文件存在，先读取并尽量复用既有结论；只补充新增内容，不重复推理
- 回答完成后，把本次稳定结论以 append 形式写入该文件，记录：日期 / 行号 / 标注摘要 / 结论要点
```

- 代码块 ```{lang}``` 的 lang 由文件后缀简单映射（`.c → c`、`.tsx → tsx`、未识别 → 留空）
- "记忆约定" 段统一拼到末尾，单源真相

### 后端改动

`electron/repo-view/repoAnalysisManager.ts`
- `sendRepoAnalysisPrompt(input)` 入参改为 `{ winId, text }`，函数内：
  - 沿用 Claude/Codex 就绪等待
  - 直接 `sendMessage(session.proc, input.text)`
  - **删除** 临时 md 文件生成
- 导出 `ensureAnalysisCacheDir(repoRoot): Promise<void>` 用于首次发送时创建目录 + 写入 `.gitignore`

`electron/main.ts`
- `repo-view:analysis-send` 入参改为 `{ repoRoot: string, text: string }`
- 处理函数：调 `ensureAnalysisCacheDir(repoRoot)`，再 `sendRepoAnalysisPrompt({ winId, text })`

`electron/preload.ts`
- `repoView.analysisSend(req: { repoRoot: string, text: string })`

`electron/repo-view/analysisPrompt.ts`
- **整文件删除**

### 前端改动

`src/repo-view/RepoViewerWindow.tsx`
- 删除 state：`analysisMessages`、`analysisPending`、`analysisRawRef`、`pendingMemoryFileRef`、`projectSummary`、`fileNote`、`recentTopics`、`historyHydrated`
- 删除 effects：memory load、history load、history save、`onAnalysisData`（应用层那一份；终端组件自己订阅，不受影响）
- 删除 callback：`onSendAnalysis` 替换为新的 `onSendToCli(question)`：
  ```ts
  async function onSendToCli(question: string) {
    if (!project || !selectedFile) return
    const targetAnns = annotations.filter(a => a.filePath === selectedFile)
    if (targetAnns.length === 0) return
    if (!sessionRunning) {
      const ok = await onStartCli()  // returns true/false
      if (!ok) return
    }
    const text = buildCliInjectionText({
      repoRoot: project.target_repo,
      filePath: selectedFile,
      annotations: targetAnns,
      question
    })
    await window.api.repoView.analysisSend({
      repoRoot: project.target_repo,
      text
    })
  }
  ```
- AnalysisPanel 的 props：去掉 `running`/`messages`/`recentTopics`/`onEditAnnotation`/`onRemoveAnnotation`（保留）/`onClearAnnotations`（保留）；新增按钮文案 `发送到 AI CLI`

`src/repo-view/AnalysisPanel.tsx`
- 删除聊天气泡渲染、最近话题渲染
- 按钮文案 `发送给 {cli}` → `发送到 AI CLI`，`分析中…` 状态删除
- 按钮 disabled 仅取决于：annotations 为空

`src/repo-view/parseAnalysisOutput.ts`、`src/repo-view/repoConversation.ts`、`src/repo-view/repoAnnotationMessage.ts`
- **删除**

`src/repo-view/buildCliInjectionText.ts`
- **新增**：纯函数，按上面"注入文本模板"格式拼字符串。包含小工具：
  - `encodeAnalysisFileName(filePath)` — 路径 → 安全文件名（与后端规则一致；如需要可放共享文件）

### 数据层保留（按 (b)）

- `electron/repo-view/memory.ts` — 保留全部函数
- IPC `repo-view:memory-load` / `memory-file-note` / `memory-apply` / `history-load` / `history-save` — 全部保留
- 调用方暂无（待将来 "保存到记忆" UI）

## 错误处理

- `analysisSend` 失败：在前端 toast 一行小提示（已有 `tile-btn` 头部空间放消息），不再写入气泡区
- `ensureAnalysisCacheDir` 失败：吞掉，不阻断发送（缓存只是优化，不是关键路径），后端 `console.warn`
- 注入后 AI 是否真的执行了"读缓存 / 写缓存"：**不验证**——失败也只是退化为"重复分析"

## 测试

- 单测：`buildCliInjectionText` 给定 fixture 输入 → 期望字符串
- 单测：`encodeAnalysisFileName('libobs/obs-audio-controls.c')` → `'libobs__obs-audio-controls.c.md'`
- 单测：长路径触发 SHA 截断
- 手动测：在 OBS Studio 仓库走完一次"标注 → 发送 → AI 在终端回答 → 应用退出 → 下次标注同文件 → AI 引用上次缓存"全链路
- 手动测：首次写入后 `.multi-ai-code/.gitignore` 含 `repo-view/analyses/`；二次发送不重复追加

## 迁移

- 不需要数据迁移：旧的 history/memory 记录留在原文件不动，新流程不读
- 已构建的客户端首次启动新版本时，旧的 history 气泡不再展示——可接受（用户已经看到对应回复在终端里）

## 决策记录

| 决策 | 选项 | 选定 | 理由 |
|------|------|------|------|
| 回答展示位置 | A 终端 / B 保留气泡 / C 混合 | **A** | 单一数据源，去掉 marker 解析与卡死风险 |
| 应用层 memory | a 整体删 / b 保留数据层 | **b** | 留口给将来"保存到记忆"按钮 |
| 分析缓存位置 | 仓库内 / app data dir | **仓库内** | AI 工具天然可达，跨 CLI 共享 |
| 缓存默认 git 行为 | i 加 .gitignore / ii 可 commit | **i** | 视为本地缓存，不污染团队仓库 |
