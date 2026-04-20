# Stage 1 Plan Selector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Stage 1 panel's text-input + HistoryDrawer combo with a dropdown listing real plans (`<target_repo>/.multi-ai-code/designs/*.md` + `plan_sources` external entries) plus a `+ 新建方案` sentinel and a dedicated `📥 导入外部方案` button.

**Architecture:** Add a new backend module `electron/orchestrator/plans.ts` exposing pure async helpers (`listPlans`, `registerExternalPlan`). Surface them via two new IPCs registered in `main.ts`. Replace the StagePanel input control with a `<select>`. Replace App.tsx's `pickerStage`/`HistoryDrawer` machinery with `planList` + `onPlanSelect` + `onImportExternal`, where selecting any non-sentinel item auto-opens the existing `PlanReviewDialog`. Delete `HistoryDrawer.tsx`.

**Tech Stack:** TypeScript, Electron (main + preload + renderer), React 18, Vitest.

**Spec reference:** `docs/superpowers/specs/2026-04-20-stage1-plan-selector-design.md`

---

## File Structure

**Create:**
- `electron/orchestrator/plans.ts` — `PlanEntry` type, `listPlans`, `registerExternalPlan`.
- `electron/orchestrator/plans.test.ts` — vitest unit coverage.

**Modify:**
- `electron/main.ts` — register `plan:list` + `plan:registerExternal` IPC handlers.
- `electron/preload.ts` — expose `api.plan.list` + `api.plan.registerExternal`.
- `src/components/StagePanel.tsx` — replace input/datalist with `<select>`; rename `📋 选用历史` button + handler prop.
- `src/App.tsx` — replace `planNameSuggestions` state + effect + HistoryDrawer block with `planList` + `onPlanSelect` + `onImportExternal`. Delete `pickerStage` state and 4 closures (`onPick`/`onImportFile`/`onRefine`/`onMergeViaAI`). Drop `HistoryDrawer` import.

**Delete:**
- `src/components/HistoryDrawer.tsx`

---

## Task 1: Add `listPlans` + `registerExternalPlan` helpers

**Files:**
- Create: `electron/orchestrator/plans.ts`
- Test: `electron/orchestrator/plans.test.ts`

- [ ] **Step 1: Write failing tests**

Create `electron/orchestrator/plans.test.ts`:

```ts
import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { listPlans, registerExternalPlan } from './plans.js'

describe('listPlans', () => {
  let projectDir: string
  let targetRepo: string
  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-plans-pdir-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-plans-repo-'))
    await fs.mkdir(join(targetRepo, '.multi-ai-code', 'designs'), { recursive: true })
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })
  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('lists internal-only plans sorted alphabetically', async () => {
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'beta.md'), '#')
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'alpha.md'), '#')
    const items = await listPlans(projectDir)
    expect(items.map((i) => i.name)).toEqual(['alpha', 'beta'])
    expect(items.every((i) => i.source === 'internal')).toBe(true)
  })

  it('lists external-only plans from plan_sources', async () => {
    const ext1 = join(tmpdir(), 'mac-ext-foo.md')
    const ext2 = join(tmpdir(), 'mac-ext-bar.md')
    await fs.writeFile(ext1, '# foo')
    await fs.writeFile(ext2, '# bar')
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { foo: ext1, bar: ext2 }
      })
    )
    const items = await listPlans(projectDir)
    expect(items.map((i) => i.name)).toEqual(['bar', 'foo'])
    expect(items.every((i) => i.source === 'external')).toBe(true)
    expect(items.find((i) => i.name === 'foo')?.abs).toBe(ext1)
    await fs.rm(ext1)
    await fs.rm(ext2)
  })

  it('external entry wins on name conflict', async () => {
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'foo.md'), '# internal')
    const ext = join(tmpdir(), 'mac-ext-foo-conflict.md')
    await fs.writeFile(ext, '# external')
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({
        id: 'p',
        name: 'p',
        target_repo: targetRepo,
        plan_sources: { foo: ext, bar: ext }
      })
    )
    const items = await listPlans(projectDir)
    expect(items).toEqual([
      { name: 'bar', abs: ext, source: 'external' },
      { name: 'foo', abs: ext, source: 'external' }
    ])
    await fs.rm(ext)
  })

  it('returns empty when designs dir missing AND no plan_sources', async () => {
    await fs.rm(join(targetRepo, '.multi-ai-code'), { recursive: true })
    const items = await listPlans(projectDir)
    expect(items).toEqual([])
  })

  it('returns empty when project.json missing', async () => {
    await fs.rm(join(projectDir, 'project.json'))
    const items = await listPlans(projectDir)
    expect(items).toEqual([])
  })
})

describe('registerExternalPlan', () => {
  let projectDir: string
  let targetRepo: string
  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-reg-pdir-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-reg-repo-'))
    await fs.mkdir(join(targetRepo, '.multi-ai-code', 'designs'), { recursive: true })
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })
  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('registers a new external plan and returns its name', async () => {
    const ext = join(tmpdir(), 'mac-reg-new.md')
    await fs.writeFile(ext, '# x')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r).toEqual({ ok: true, name: 'mac-reg-new' })
    const meta = JSON.parse(await fs.readFile(join(projectDir, 'project.json'), 'utf8'))
    expect(meta.plan_sources['mac-reg-new']).toBe(ext)
    await fs.rm(ext)
  })

  it('rejects non-.md files', async () => {
    const ext = join(tmpdir(), 'mac-reg-not-md.txt')
    await fs.writeFile(ext, 'x')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/\.md/)
    await fs.rm(ext)
  })

  it('rejects nonexistent files', async () => {
    const r = await registerExternalPlan(projectDir, '/no/such/file.md')
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/not.*exist|找不到/i)
  })

  it('rejects relative paths', async () => {
    const r = await registerExternalPlan(projectDir, 'relative.md')
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/绝对|absolute/i)
  })

  it('rejects when name conflicts with an internal plan', async () => {
    await fs.writeFile(join(targetRepo, '.multi-ai-code', 'designs', 'dup.md'), '#')
    const ext = join(tmpdir(), 'dup.md')
    await fs.writeFile(ext, '#')
    const r = await registerExternalPlan(projectDir, ext)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/已存在同名方案/)
    await fs.rm(ext)
  })

  it('rejects when name conflicts with an existing external entry', async () => {
    const ext = join(tmpdir(), 'mac-reg-dup.md')
    await fs.writeFile(ext, '#')
    const meta = {
      id: 'p',
      name: 'p',
      target_repo: targetRepo,
      plan_sources: { 'mac-reg-dup': ext }
    }
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify(meta))
    const r = await registerExternalPlan(projectDir, ext)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.error).toMatch(/已存在同名方案/)
    await fs.rm(ext)
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run electron/orchestrator/plans.test.ts`
Expected: FAIL — `Cannot find module './plans.js'`.

- [ ] **Step 3: Implement `plans.ts`**

Create `electron/orchestrator/plans.ts`:

```ts
import { promises as fs } from 'fs'
import { basename, extname, isAbsolute, join } from 'path'

export interface PlanEntry {
  name: string
  abs: string
  source: 'internal' | 'external'
}

interface ProjectMeta {
  target_repo?: string
  plan_sources?: Record<string, string>
}

async function readMeta(projectDir: string): Promise<ProjectMeta> {
  try {
    return JSON.parse(
      await fs.readFile(join(projectDir, 'project.json'), 'utf8')
    ) as ProjectMeta
  } catch {
    return {}
  }
}

async function readDesignNames(targetRepo: string): Promise<string[]> {
  const dir = join(targetRepo, '.multi-ai-code', 'designs')
  try {
    const entries = await fs.readdir(dir)
    return entries
      .filter((f) => f.toLowerCase().endsWith('.md'))
      .map((f) => f.slice(0, -3))
  } catch {
    return []
  }
}

export async function listPlans(projectDir: string): Promise<PlanEntry[]> {
  const meta = await readMeta(projectDir)
  const targetRepo = meta.target_repo
  const planSources = meta.plan_sources ?? {}

  const externalEntries: PlanEntry[] = Object.entries(planSources).map(
    ([name, abs]) => ({ name, abs, source: 'external' as const })
  )
  const externalNames = new Set(externalEntries.map((e) => e.name))

  const internalEntries: PlanEntry[] = []
  if (targetRepo) {
    const names = await readDesignNames(targetRepo)
    for (const name of names) {
      if (externalNames.has(name)) continue
      internalEntries.push({
        name,
        abs: join(targetRepo, '.multi-ai-code', 'designs', `${name}.md`),
        source: 'internal'
      })
    }
  }

  return [...internalEntries, ...externalEntries].sort((a, b) =>
    a.name.localeCompare(b.name)
  )
}

export async function registerExternalPlan(
  projectDir: string,
  externalAbsPath: string
): Promise<{ ok: true; name: string } | { ok: false; error: string }> {
  if (!isAbsolute(externalAbsPath)) {
    return { ok: false, error: '必须是绝对路径 (absolute)' }
  }
  if (extname(externalAbsPath).toLowerCase() !== '.md') {
    return { ok: false, error: '仅支持 .md 文件' }
  }
  try {
    await fs.access(externalAbsPath)
  } catch {
    return { ok: false, error: `文件不存在 (does not exist): ${externalAbsPath}` }
  }
  const name = basename(externalAbsPath, '.md')
  const existing = await listPlans(projectDir)
  if (existing.some((p) => p.name === name)) {
    return {
      ok: false,
      error: `已存在同名方案 "${name}"，请改源文件名后再导入`
    }
  }
  const metaPath = join(projectDir, 'project.json')
  let meta: Record<string, unknown> = {}
  try {
    meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
  } catch {
    /* empty */
  }
  const prev = (meta.plan_sources as Record<string, string> | undefined) ?? {}
  meta.plan_sources = { ...prev, [name]: externalAbsPath }
  meta.updated_at = new Date().toISOString()
  await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
  return { ok: true, name }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run electron/orchestrator/plans.test.ts`
Expected: PASS — 11 tests (5 listPlans + 6 registerExternalPlan).

- [ ] **Step 5: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add electron/orchestrator/plans.ts electron/orchestrator/plans.test.ts
git commit -m "feat(plans): add listPlans + registerExternalPlan helpers"
```

---

## Task 2: Register `plan:list` + `plan:registerExternal` IPC handlers

**Files:**
- Modify: `electron/main.ts` — append two `ipcMain.handle` blocks.

This task has no tests of its own (the helpers were tested in Task 1; the IPC layer is a thin adapter).

- [ ] **Step 1: Add import + handlers in `main.ts`**

In `electron/main.ts`, add to the existing imports near the top of the file:

```ts
import { listPlans, registerExternalPlan } from './orchestrator/plans.js'
```

Inside `app.whenReady().then(async () => { ... })`, after the existing `ipcMain.handle('app:version', ...)` line (around line 77), add:

```ts
  ipcMain.handle(
    'plan:list',
    async (_e, { projectDir }: { projectDir: string }) => {
      try {
        const items = await listPlans(projectDir)
        return { ok: true as const, items }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message, items: [] }
      }
    }
  )

  ipcMain.handle(
    'plan:registerExternal',
    async (
      _e,
      { projectDir, externalPath }: { projectDir: string; externalPath: string }
    ) => {
      return await registerExternalPlan(projectDir, externalPath)
    }
  )
```

- [ ] **Step 2: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add electron/main.ts
git commit -m "feat(main): register plan:list + plan:registerExternal IPC"
```

---

## Task 3: Expose `api.plan` in preload.ts

**Files:**
- Modify: `electron/preload.ts:80-460` (within the `const api = { ... }` literal).

- [ ] **Step 1: Add `plan` namespace to preload api**

In `electron/preload.ts`, find the closing of the `artifact: { ... }` block (around line 459: just before `}`). Insert AFTER `artifact: { ... },` block and BEFORE the final `}` of `const api = {`:

```ts
  plan: {
    list: (projectDir: string) =>
      ipcRenderer.invoke('plan:list', { projectDir }) as Promise<{
        ok: boolean
        items: { name: string; abs: string; source: 'internal' | 'external' }[]
        error?: string
      }>,
    registerExternal: (req: { projectDir: string; externalPath: string }) =>
      ipcRenderer.invoke('plan:registerExternal', req) as Promise<
        | { ok: true; name: string }
        | { ok: false; error: string }
      >
  }
```

The exact insertion point: find the `artifact:` block; locate the final `}` that closes `artifact: { ... }`. Add a `,` after that closing `}` if not already there, then paste the `plan: { ... }` block, then the existing closing `}` of `const api`.

- [ ] **Step 2: Typecheck (verifies api shape used elsewhere)**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add electron/preload.ts
git commit -m "feat(preload): expose api.plan.list + api.plan.registerExternal"
```

---

## Task 4: StagePanel.tsx — swap input for select + rename history button

**Files:**
- Modify: `src/components/StagePanel.tsx` — interface (around lines 28-50), button area (around lines 473-482), input control (around lines 581-598).

- [ ] **Step 1: Update the `Props` interface fields**

In `src/components/StagePanel.tsx`, find the props block (around line 28-50). Locate the existing two fields:

```tsx
  /** When provided, renders a "选用历史方案" button that opens the picker. */
  onPickHistory?: () => void
```

and (likely a few lines below):

```tsx
  planNameSuggestions?: string[]
  planName?: string
  onPlanNameChange?: (s: string) => void
```

Replace them with:

```tsx
  /** When provided, renders a "📥 导入外部方案" button. */
  onImportExternal?: () => void
  /** Plan list for the dropdown selector. Empty array hides the bar. */
  planList?: { name: string; abs: string; source: 'internal' | 'external' }[]
  /** Currently selected plan name. Empty string = "+ 新建方案" sentinel. */
  planName?: string
  /** Called when user changes selection. Value is `__NEW__` for the sentinel
   *  or a plan name from `planList`. */
  onPlanSelect?: (value: string) => void
```

- [ ] **Step 2: Update the button area**

In the same file find the `📋 选用历史` button block (around lines 473-482):

```tsx
        {props.onPickHistory && (
          <button
            className="tile-btn"
            onClick={props.onPickHistory}
            disabled={disabled}
            title="直接选用此阶段的历史产物或外部文件，跳过 AI 执行"
          >
            📋 选用历史
          </button>
        )}
```

Replace with:

```tsx
        {props.onImportExternal && (
          <button
            className="tile-btn"
            onClick={props.onImportExternal}
            disabled={disabled}
            title="挑一个外部 .md 文件作为方案。不复制——后续修改直接写回原文件。"
          >
            📥 导入外部方案
          </button>
        )}
```

- [ ] **Step 3: Replace the plan-name input with a select**

In the same file find the `plan-name-bar` block (around lines 581-598):

```tsx
      {props.onPlanNameChange !== undefined && (
        <div className="plan-name-bar">
          <label>方案名称：</label>
          <input
            type="text"
            placeholder="必填 — 用于归档识别（下拉可选历史方案）"
            value={props.planName ?? ''}
            onChange={(e) => props.onPlanNameChange?.(e.target.value)}
            className="plan-name-input"
            list={`plan-names-${projectId}`}
          />
          <datalist id={`plan-names-${projectId}`}>
            {(props.planNameSuggestions ?? []).map((n) => (
              <option key={n} value={n} />
            ))}
          </datalist>
        </div>
      )}
```

Replace with:

```tsx
      {props.onPlanSelect !== undefined && (
        <div className="plan-name-bar">
          <label>方案选择：</label>
          <select
            value={props.planName ? props.planName : '__NEW__'}
            onChange={(e) => props.onPlanSelect?.(e.target.value)}
            className="plan-name-input"
          >
            <option value="__NEW__">+ 新建方案</option>
            {(props.planList ?? []).map((p) => (
              <option
                key={p.name}
                value={p.name}
                title={p.source === 'external' ? p.abs : ''}
              >
                {p.name}{p.source === 'external' ? '（外部）' : ''}
              </option>
            ))}
          </select>
        </div>
      )}
```

- [ ] **Step 4: Typecheck**

Run: `npm run typecheck`
Expected: errors will surface in `src/App.tsx` (it still passes `onPickHistory`, `planNameSuggestions`, `onPlanNameChange`). That's expected — Task 5 fixes them. The StagePanel file itself must be clean. Verify by inspecting the typecheck output: errors only in App.tsx, not StagePanel.tsx.

- [ ] **Step 5: Commit**

```bash
git add src/components/StagePanel.tsx
git commit -m "feat(StagePanel): swap plan-name input for select + rename history button"
```

---

## Task 5: App.tsx — adopt new selector, drop HistoryDrawer wiring

**Files:**
- Modify: `src/App.tsx` — multiple regions (state declarations, effects, JSX).

This task is the largest single edit but is mostly mechanical replacement.

- [ ] **Step 1: Drop HistoryDrawer import**

At the top of `src/App.tsx`, find:

```ts
import HistoryDrawer from './components/HistoryDrawer'
```

Delete that line entirely.

- [ ] **Step 2: Replace `planNameSuggestions` state with `planList`**

In `src/App.tsx`, find the state declaration (around line 89):

```ts
  const [planNameSuggestions, setPlanNameSuggestions] = useState<string[]>([])
```

Replace with:

```ts
  const [planList, setPlanList] = useState<
    { name: string; abs: string; source: 'internal' | 'external' }[]
  >([])
```

- [ ] **Step 3: Drop `pickerStage` state**

In the same file find:

```ts
  const [pickerStage, setPickerStage] = useState<number | null>(null)
```

Delete that line.

- [ ] **Step 4: Drop `pickerStage` reset inside `clearProjectScopedState`**

Find inside `clearProjectScopedState` (around line 185-197):

```ts
    setPickerStage(null)
```

Delete that line.

- [ ] **Step 5: Replace the suggestions effect with planList effect**

Find the effect that builds suggestions (around lines 113-141):

```ts
  // Refresh plan-name suggestions + per-stage "done" status when project/plan changes.
  useEffect(() => {
    if (!currentProjectId) {
      setPlanNameSuggestions([])
      setPlanStagesDone({})
      return
    }
    let cancelled = false
    void (async () => {
      const all = await window.api.artifact.list(currentProjectId)
      if (cancelled) return
      const names = new Set<string>()
      const stageSafe = planName.replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').replace(/\s+/g, '_').slice(0, 80)
      const done: Record<number, boolean> = { 1: false, 2: false, 3: false, 4: false }
      for (const r of all) {
        const m = r.path.match(/artifacts[/\\]history[/\\]stage(\d+)[/\\](.+?)(_\d[^.]*)?\.md$/)
        if (m) {
          const stage = Number(m[1])
          const decoded = m[2].replace(/_/g, ' ')
          names.add(decoded)
          if (stageSafe && m[2] === stageSafe) done[stage] = true
        }
      }
      setPlanNameSuggestions(Array.from(names).sort())
      setPlanStagesDone(done)
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId, pendingDone, planName])
```

Replace with:

```ts
  // Refresh plan-list (dropdown) + per-stage "done" status when project/plan changes.
  useEffect(() => {
    if (!currentProjectId || !projectDir) {
      setPlanList([])
      setPlanStagesDone({})
      return
    }
    let cancelled = false
    void (async () => {
      const [planRes, all] = await Promise.all([
        window.api.plan.list(projectDir),
        window.api.artifact.list(currentProjectId)
      ])
      if (cancelled) return
      const items = planRes.ok ? planRes.items : []
      setPlanList(items)
      // If the currently selected plan is no longer in the list, fall back to
      // "新建方案" (empty planName).
      if (planName && !items.some((p) => p.name === planName)) {
        setPlanName('')
      }
      const stageSafe = planName
        .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
        .replace(/\s+/g, '_')
        .slice(0, 80)
      const done: Record<number, boolean> = { 1: false, 2: false, 3: false, 4: false }
      for (const r of all) {
        const m = r.path.match(/artifacts[/\\]history[/\\]stage(\d+)[/\\](.+?)(_\d[^.]*)?\.md$/)
        if (m && stageSafe && m[2] === stageSafe) {
          done[Number(m[1])] = true
        }
      }
      setPlanStagesDone(done)
    })()
    return () => {
      cancelled = true
    }
  }, [currentProjectId, projectDir, pendingDone, planName])
```

- [ ] **Step 6: Add `onPlanSelect` and `onImportExternal` callbacks**

In `src/App.tsx`, find a good location for new callbacks (after `pickExternalFileForPreview`, around line 240). Add:

```ts
  const onPlanSelect = useCallback(
    async (value: string) => {
      if (value === '__NEW__') {
        setPlanName('')
        return
      }
      setPlanName(value)
      const r = await window.api.artifact.readCurrent(projectDir, 1, value)
      if (!r.ok) {
        alert(`读取方案失败：${r.error ?? '未知错误'}`)
        return
      }
      setPlanReview({ path: r.path ?? value, content: r.content ?? '' })
    },
    [projectDir]
  )

  const onImportExternal = useCallback(async () => {
    if (!projectDir) {
      alert('请先打开一个项目')
      return
    }
    const pick = await window.api.dialog.pickTextFile({
      title: '选择要导入的外部方案文件 (.md)'
    })
    if (pick.canceled) return
    if (pick.error || !pick.path) {
      alert(`读取文件失败：${pick.error ?? '未知错误'}`)
      return
    }
    const reg = await window.api.plan.registerExternal({
      projectDir,
      externalPath: pick.path
    })
    if (!reg.ok) {
      alert(`导入失败：${reg.error}`)
      return
    }
    const list = await window.api.plan.list(projectDir)
    if (list.ok) setPlanList(list.items)
    setPlanName(reg.name)
    const cur = await window.api.artifact.readCurrent(projectDir, 1, reg.name)
    if (cur.ok) {
      setPlanReview({ path: cur.path ?? reg.name, content: cur.content ?? '' })
    }
  }, [projectDir])
```

- [ ] **Step 7: Update StagePanel props passed in JSX**

Find the StagePanel render block (around lines 740-752):

```tsx
              planName={planName}
              onPlanNameChange={s.id === 1 ? setPlanName : undefined}
              planNameSuggestions={planNameSuggestions}
              onPickHistory={
                s.id === 1 ? () => setPickerStage(1) : undefined
              }
```

Replace with:

```tsx
              planName={planName}
              onPlanSelect={s.id === 1 ? onPlanSelect : undefined}
              planList={planList}
              onImportExternal={s.id === 1 ? onImportExternal : undefined}
```

- [ ] **Step 8: Delete HistoryDrawer JSX block + closures**

Find the entire `{pickerStage !== null && (<HistoryDrawer ... />)}` block (around lines 756-870). Delete the entire block including all closures (`onPick`, `onImportFile`, `onRefine`, `onMergeViaAI`, `onClose`).

The block to delete starts with:

```tsx
        {pickerStage !== null && (
          <HistoryDrawer
```

and ends after the block's closing `)}` of the conditional render. Keep the surrounding `</main>` etc. intact.

- [ ] **Step 9: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 10: Run all vitest tests**

Run: `npx vitest run`
Expected: all tests pass (16 from previous work + 11 from Task 1 = 27).

- [ ] **Step 11: Commit**

```bash
git add src/App.tsx
git commit -m "refactor(App): adopt plan selector dropdown, drop HistoryDrawer wiring"
```

---

## Task 6: Delete HistoryDrawer.tsx

**Files:**
- Delete: `src/components/HistoryDrawer.tsx`

- [ ] **Step 1: Confirm no other importers**

Run: `grep -rn "HistoryDrawer" src/ electron/`
Expected: zero matches (App.tsx no longer imports it).

If any remain, stop and report — Task 5 must have left a stray reference.

- [ ] **Step 2: Delete the file**

Run: `rm src/components/HistoryDrawer.tsx`

- [ ] **Step 3: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 4: Vitest**

Run: `npx vitest run`
Expected: 27/27 pass.

- [ ] **Step 5: Commit**

```bash
git add src/components/HistoryDrawer.tsx
git commit -m "chore: remove HistoryDrawer (replaced by plan selector dropdown)"
```

---

## Task 7: Manual end-to-end verification

**Files:** none modified.

This step verifies behaviors a unit test cannot.

- [ ] **Step 1: Launch dev app**

Run: `npm run dev`
Expected: Electron window opens.

- [ ] **Step 2: Open the obs-studio project (already has a real plan from Task 6 of the previous migration)**

In the app: open project pointing at `/Users/hongqingwan/OpenSource/obs-studio`.
Expected: Stage 1 panel shows a `方案选择` dropdown with two items:
- `+ 新建方案`
- `libobs-vulkan-design`

- [ ] **Step 3: Select the existing plan**

Pick `libobs-vulkan-design` from the dropdown.
Expected: PlanReviewDialog opens automatically showing the plan content.

- [ ] **Step 4: Pick "+ 新建方案" and run a fresh design**

Close the preview. In dropdown pick `+ 新建方案`. Click `Start`. Once Stage 1 boots, type a tiny request like "请写一份最小演示设计 demo-2 并落盘". Confirm:
- AI asks for a plan name before archiving (planPending flow).
- After archiving, dropdown auto-refreshes and `demo-2` appears as a new item.

- [ ] **Step 5: Import an external plan**

In another terminal:
```
mkdir -p /tmp/external-plans
cat > /tmp/external-plans/external-demo.md <<EOF
# external demo plan
hello
EOF
```

Click `📥 导入外部方案`, pick `/tmp/external-plans/external-demo.md`.
Expected: PlanReviewDialog opens with file contents; dropdown now contains a `external-demo（外部）` item; hovering it shows the absolute path.

- [ ] **Step 6: Verify project.json**

Run: `cat ~/MultiAICode/projects/<obs-studio-project-id>/project.json`
Expected: `plan_sources` contains `"external-demo": "/tmp/external-plans/external-demo.md"`.

- [ ] **Step 7: Verify legacy "选用历史" button is gone**

Confirm the Stage 1 button row contains `📥 导入外部方案 / 👁 方案预览` and no `📋 选用历史`.

- [ ] **Step 8: No commit needed**

If all checks pass, the refactor is complete.

---

## Self-Review Notes

- Spec requirement "下拉首项 + 新建方案 sentinel" → Task 4 step 3 hardcodes `__NEW__`.
- Spec requirement "选已有自动开预览" → Task 5 step 6 `onPlanSelect`.
- Spec requirement "导入外部方案 = 仅注册" → Task 1 `registerExternalPlan` + Task 5 step 6 `onImportExternal`.
- Spec requirement "delete HistoryDrawer + 4 closures" → Task 5 step 8 + Task 6.
- Spec requirement "external 同名优先" → Task 1 `listPlans` impl skips internal entry when external has same name; Task 1 step 1 test #3 covers this.
- Spec requirement "stage:done 后下拉刷新" → Task 5 step 5 effect depends on `pendingDone`.
- Spec requirement "planName 不在新列表里 → reset" → Task 5 step 5 effect's `if (planName && !items.some(...))` handles this.
- Spec requirement "onPlanSelect 翻译 sentinel" → Task 5 step 6 `if (value === '__NEW__') setPlanName('')`.
- Spec test plan #1-3 → Task 1 step 1 test cases; #4-5 → Task 1 step 1 #5,#6 (cover empty/missing).
- Spec test plan #6 (registerExternal nonexistent file) → Task 1 step 1 third registerExternalPlan test.
- Spec error handling table fully covered: missing designs dir / missing project.json / external file missing (handled at readCurrent layer, surfaced via alert in onPlanSelect Step 6) / .md-only validation / nonexistent file validation / planList stale → reset.
