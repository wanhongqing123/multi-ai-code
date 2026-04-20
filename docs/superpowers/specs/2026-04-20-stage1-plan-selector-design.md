# Stage 1 方案选择器交互重构

日期：2026-04-20
作者：hongqingwan + Claude

## 背景

完成 Stage 1 落盘路径迁移之后（spec `2026-04-19-stage1-design-artifact-path.md`），方案文件已经以"target_repo 内的实文件"形式管理。但 Stage 1 面板的 UX 还停留在旧模型：

- "方案名称" 是一个文本输入 + datalist，建议项来自 `artifacts/history/stageN/*.md` 的快照（不是真正的方案文件）。
- "选用历史" 按钮打开 `HistoryDrawer`，混合了"恢复快照 / 导入外部 / AI 完善 / AI 合并"四件事，认知成本高。
- 没有把"target_repo 下当前真正存在的方案"作为一等公民列出来，新建/选已有 的语义模糊。

本次把交互重构为"方案选择器 + 新建 + 导入"三件事，跟新存储模型对齐。

## 目标

1. 用户打开任意一个项目后，能立即在 Stage 1 面板**看到该 target_repo 下所有已有方案**（含 internal 和 external 两类）。
2. 选择某个已有方案 → 自动打开方案预览。
3. 新建方案与选已有方案对称，通过下拉里的固定 sentinel 项触发。
4. "导入外部方案"作为独立按钮，仅完成 plan_sources 注册（不复制文件、不触发 AI）。
5. 移除 HistoryDrawer 及其驱动的 4 路操作；UI 收敛到"选 + 预览 + Start"。

## 非目标

- **不**做"删除方案"功能（用户直接在文件系统操作）。
- **不**为外部方案的"失效路径"做自动清理（v1 用户手动改 `project.json`）。
- **不**做"新建方案立即输入名字再 Start"——保留 planPending 流程（落盘前问名）。
- **不**改 Stage 2-4 面板。
- **不**做 UI 主题/样式重设计；沿用现有 tile-btn 等类。
- **不**迁移历史快照数据。

## 设计

### UI 改动（`src/components/StagePanel.tsx`）

#### 按钮区

| 旧 | 新 |
| --- | --- |
| `📋 选用历史` | `📥 导入外部方案` |
| `👁 方案预览` | 不变 |

#### 输入控件

把第 581-598 行的 `<input list>` + `<datalist>` 整段替换为一个 `<select>`：

```jsx
<div className="plan-name-bar">
  <label>方案选择：</label>
  <select
    value={props.planName ?? ''}
    onChange={(e) => props.onPlanSelect?.(e.target.value)}
    className="plan-name-input"
  >
    <option value="__NEW__">+ 新建方案</option>
    {(props.planList ?? []).map((p) => (
      <option key={p.name} value={p.name} title={p.source === 'external' ? p.abs : ''}>
        {p.name}{p.source === 'external' ? '（外部）' : ''}
      </option>
    ))}
  </select>
</div>
```

**Props 替换**：
- 旧：`planNameSuggestions: string[]`、`onPlanNameChange: (s: string) => void`
- 新：`planList: Array<{name: string; abs: string; source: 'internal'|'external'}>`、`onPlanSelect: (value: string) => void`

`onPlanSelect` 收到的 `value` 可能是 `'__NEW__'` 或某个 plan name。由 `App.tsx` 翻译成 planName + 副作用。

### App.tsx 状态机

`planName` state 保留含义：空字符串 = 新建模式（planPending）；非空 = 选定方案。

新增 state：

```ts
const [planList, setPlanList] = useState<
  Array<{ name: string; abs: string; source: 'internal' | 'external' }>
>([])
```

替换原 `planNameSuggestions` state（同语义但不同形状）。

`onPlanSelect` handler：

```ts
const onPlanSelect = useCallback(
  async (value: string) => {
    if (value === '__NEW__') {
      setPlanName('')
      return
    }
    setPlanName(value)
    // 自动打开方案预览
    const r = await window.api.artifact.readCurrent({
      projectDir,
      stageId: 1,
      label: value
    })
    if (!r.ok) {
      alert(`读取方案失败：${r.error}`)
      return
    }
    setPlanReview({ path: r.path, content: r.content })
  },
  [projectDir]
)
```

`planList` 重建 effect 替换原 `planNameSuggestions` 的 effect（约 113-141 行）：

```ts
useEffect(() => {
  if (!currentProjectId || !projectDir) {
    setPlanList([])
    return
  }
  let cancelled = false
  void (async () => {
    const r = await window.api.plan.list(projectDir)
    if (cancelled) return
    setPlanList(r.ok ? r.items : [])
  })()
  return () => { cancelled = true }
}, [currentProjectId, projectDir, pendingDone])  // pendingDone 触发刷新（stage:done 后）
```

"导入外部方案"按钮 handler：

```ts
const onImportExternal = useCallback(async () => {
  const pick = await window.api.dialog.pickTextFile({ title: '选择要导入的外部方案文件（.md）' })
  if (pick.canceled || !pick.path) return
  const r = await window.api.plan.registerExternal({ projectDir, externalPath: pick.path })
  if (!r.ok) {
    alert(`导入失败：${r.error}`)
    return
  }
  // 刷新下拉 + 自动选中 + 触发预览
  const list = await window.api.plan.list(projectDir)
  if (list.ok) setPlanList(list.items)
  setPlanName(r.name!)
  const cur = await window.api.artifact.readCurrent({
    projectDir,
    stageId: 1,
    label: r.name
  })
  if (cur.ok) setPlanReview({ path: cur.path, content: cur.content })
}, [projectDir])
```

**删除**：
- `pickerStage` state + 所有引用（约 760-870 行：`HistoryDrawer` 渲染块 + `onPick`/`onImportFile`/`onRefine`/`onMergeViaAI` 闭包）。
- `<HistoryDrawer>` 组件文件 `src/components/HistoryDrawer.tsx` 整个删除。
- `StagePanel` 里 `onPickHistory` prop 整体删除。

`requirePlanName` 函数（154-160 行）：行为不变，仍然在某些手动路径（如 manual done 之类）上做兜底；保留。

### 后端改动

#### 新 IPC：`plan:list`

文件：新建 `electron/orchestrator/plans.ts`，集中"方案集合"相关后端逻辑。

```ts
// electron/orchestrator/plans.ts
export interface PlanEntry {
  name: string
  abs: string
  source: 'internal' | 'external'
}

export async function listPlans(projectDir: string): Promise<PlanEntry[]> {
  // 1. read project.json → target_repo + plan_sources
  // 2. internal: scan <target_repo>/.multi-ai-code/designs/*.md, basename(去 .md) 作 name
  // 3. external: project.json.plan_sources 各项；name = basename
  // 4. 同名冲突：external 优先（显式注册）
  // 5. 按 name 字母序排
}
```

`electron/main.ts` 注册 IPC：

```ts
ipcMain.handle('plan:list', async (_e, { projectDir }) => {
  try {
    const items = await listPlans(projectDir)
    return { ok: true as const, items }
  } catch (err) {
    return { ok: false as const, error: (err as Error).message, items: [] }
  }
})
```

#### 新 IPC：`plan:registerExternal`

```ts
// electron/orchestrator/plans.ts
export async function registerExternalPlan(
  projectDir: string,
  externalAbsPath: string
): Promise<{ ok: true; name: string } | { ok: false; error: string }> {
  // 1. 校验 externalAbsPath 是绝对路径、文件存在、扩展名 .md
  // 2. name = basename(externalAbsPath, '.md')
  // 3. 重名冲突检测：跟 listPlans() 结果对比；同名则 return { ok:false, error:'已存在同名方案，请改源文件名后再导入' }
  // 4. 读 project.json，merge plan_sources[name] = externalAbsPath，原子写回
  // 5. return { ok:true, name }
}
```

`electron/main.ts` 注册：

```ts
ipcMain.handle('plan:registerExternal', async (_e, { projectDir, externalPath }) => {
  return await registerExternalPlan(projectDir, externalPath)
})
```

#### preload.ts

加：

```ts
plan: {
  list: (projectDir: string) =>
    ipcRenderer.invoke('plan:list', { projectDir }) as Promise<...>,
  registerExternal: (req: { projectDir: string; externalPath: string }) =>
    ipcRenderer.invoke('plan:registerExternal', req) as Promise<...>
}
```

#### 不动的现有逻辑

- `electron/cc/ptyManager.ts` 里 spawn 时的 `externalArtifactAbs ?? defaultAbs` 选路径逻辑：plan_sources 已经被它消费；本次只是补"如何把 entry 写进去"的入口。
- `artifact:read-current` IPC：已经在 stage 1 + label 路径上先查 `readPlanSources`，能正确读外部方案；不动。
- `resolveStageArtifactAbs`：不动。

### 数据流

#### 选已有方案

```
user 选下拉项 'libobs-vulkan-design'
  → onPlanSelect('libobs-vulkan-design')
  → setPlanName('libobs-vulkan-design')
  → window.api.artifact.readCurrent({ projectDir, stageId:1, label:'libobs-vulkan-design' })
       后端 → readPlanSources(projectDir)['libobs-vulkan-design'] || resolveStageArtifactAbs → 读文件
  → setPlanReview({ path, content })
  → PlanReviewDialog 弹出
```

#### 新建方案

```
user 选下拉项 '+ 新建方案' (__NEW__)
  → onPlanSelect('__NEW__')
  → setPlanName('')
  → 不弹任何对话框

user 点 Start
  → ptyManager spawn Stage 1 with label=undefined
  → planPending = true
  → AI 在落盘前问用户名字（已有逻辑）
  → 落盘到 <target_repo>/.multi-ai-code/designs/<新名>.md
  → stage:done 事件 → pendingDone state 改变 → planList useEffect 重新拉 → 下拉新增此项
```

#### 导入外部方案

```
user 点 [📥 导入外部方案]
  → file picker
  → window.api.plan.registerExternal({ projectDir, externalPath })
       后端 → 读 project.json → 合并 plan_sources → 写回
  → window.api.plan.list 刷新 planList
  → setPlanName(<basename>)
  → readCurrent → setPlanReview → 弹预览
```

### 错误处理

| 场景 | 处理 |
| --- | --- |
| target_repo 下无 designs 目录 | listPlans 把扫描错误吞掉，返回 internal 部分为空；下拉只有"+ 新建方案" |
| project.json 缺失/损坏 | listPlans 把 plan_sources 视为空；不抛异常 |
| 选中外部方案，原文件已被删 | readCurrent 返回 `ok:false, error: ENOENT`；UI alert 报错；用户需手动清理 plan_sources |
| 导入时 basename 跟现有内部 / 外部方案重名 | registerExternal 返回 `ok:false, error:'已存在同名方案'`；UI alert |
| 导入文件不是 .md | registerExternal 返回 `ok:false, error:'仅支持 .md 文件'` |
| Stage 1 spawn 时 planName 非空但 designs/<name>.md 跟 plan_sources[name] 都不存在（首次新建用户中途选了一个还没落盘的） | 不可能——下拉只列已落盘的；新建模式 planName=''，走 planPending |
| planList 刷新后，当前 planName 不在新列表里（如外部文件被删 + 用户在 plan_sources 里手动删除了 entry） | `planList` useEffect 末尾追加：若 `planName && !list.find(p=>p.name===planName)`，`setPlanName('')` 自动回退到"新建模式"；下拉显示 `+ 新建方案` |

### 测试

#### 单元测试（vitest）

新建 `electron/orchestrator/plans.test.ts`：

1. `listPlans` 仅 internal：mkdtemp 一个 target_repo + project.json (无 plan_sources)，designs 下放两个 .md，期望返回 2 项 internal，按字母序。
2. `listPlans` 仅 external：plan_sources 写两条，designs 目录不存在，期望返回 2 项 external。
3. `listPlans` 混合 + 同名优先级：designs 有 `foo.md`，plan_sources 有 `foo` 和 `bar`，期望 `foo` 标 external（external 优先），`bar` external，无 internal。
4. `registerExternalPlan` 成功：写入 plan_sources 后回读 project.json 验证。
5. `registerExternalPlan` 同名冲突：先放一条 external 同名再注册同名 → ok:false。
6. `registerExternalPlan` 文件不存在：ok:false，error 含 'not exist' 或类似。

#### 手动 E2E

1. 用 obs-studio 项目（已有 `libobs-vulkan-design.md`）：打开 Stage 1，下拉应有 "+ 新建方案" + "libobs-vulkan-design"；选后者，方案预览自动弹出。
2. 选 "+ 新建方案"，planName 显示为空；Start，AI 起方案、落盘前问名字（输 `demo-2`），完成后下拉自动新增 `demo-2`。
3. 点"导入外部方案"，挑 `/tmp/some-external/foo.md`；下拉新增 `foo（外部）`，自动选中并弹预览；project.json 验证 `plan_sources.foo` 已存。
4. 重启 app，重新打开同一项目：下拉项与刚才一致。

## 风险

| 风险 | 应对 |
| --- | --- |
| HistoryDrawer 删掉后用户失去"AI 合并多份方案"能力 | 已确认非目标；后续若需要重做，可用"选两个 + 单独按钮"重新设计 |
| 下拉项过多（项目里有 50+ 方案） | v1 不分组/不搜索；后续若变成痛点再加搜索框 |
| External 方案路径含中文/特殊字符在 Win 下 | 沿用现有 readPlanSources / 路径处理逻辑，不引入新风险 |
| 旧用户升级后 datalist 的方案名建议消失 | 预期行为；新下拉只列真实存在于 designs/ 或 plan_sources 的方案，比旧建议更准 |
