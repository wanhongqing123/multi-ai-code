# Repo Viewer Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an independent `仓库查看` native window that browses the current repository as a file tree + single-file code viewer, supports selected-code AI analysis via a separate CLI session, and stores compressed repo-local private memory under `<target_repo>/.multi-ai-code/repo-memory/`.

**Architecture:** Keep the existing `Diff 审查` modal unchanged. Add a second renderer mode (`repo-view`) opened in a dedicated `BrowserWindow`; its backend surface is a new `repo-view:*` IPC namespace for opening/focusing the window, listing filtered repo trees, reading file contents, running a hidden PTY-backed analysis session, and loading/updating repo-local memory. Persist a second project-level AI config (`repo_view_ai_settings`) separate from the main coding session config, defaulting to Claude.

**Tech Stack:** TypeScript, React 18, Electron 33, node-pty, Vitest, CSS, better-sqlite3-backed project metadata.

**Spec reference:** `docs/superpowers/specs/2026-04-22-repo-viewer-window-design.md`

---

## File Structure

### Create
- `electron/repo-view/windowMode.ts` — parse/format renderer window mode (`main` vs `repo-view`)
- `electron/repo-view/windowMode.test.ts` — unit tests for route parsing
- `electron/repo-view/filesystem.ts` — filtered tree listing, text/binary file loading, size guard helpers
- `electron/repo-view/filesystem.test.ts` — unit tests for ignore rules and tree sorting
- `electron/repo-view/memory.ts` — repo-memory path helpers, `.git/info/exclude` idempotent write, load/update logic
- `electron/repo-view/memory.test.ts` — unit tests for memory paths + exclude handling
- `electron/repo-view/analysisPrompt.ts` — build the analysis kickoff prompt contract
- `electron/repo-view/analysisPrompt.test.ts` — unit tests for prompt invariants
- `electron/repo-view/repoAnalysisManager.ts` — hidden PTY session manager keyed by repo-view window
- `src/repo-view/RepoViewerWindow.tsx` — top-level renderer page for the native window
- `src/repo-view/FileTree.tsx` — lazy file tree component
- `src/repo-view/CodePane.tsx` — single-column code viewer + selection capture
- `src/repo-view/AnalysisPanel.tsx` — analysis history, composer, streaming result panel
- `src/repo-view/parseAnalysisOutput.ts` — split visible answer vs persisted memory block
- `src/repo-view/parseAnalysisOutput.test.ts` — unit tests for analysis output parsing

### Modify
- `electron/main.ts` — create/focus repo-view windows; register new `repo-view:*` IPC handlers; persist `repo_view_ai_settings`
- `electron/preload.ts` — expose `repoView` APIs and repo-view AI settings getters/setters
- `src/types/global.d.ts` — widen `window.api` types
- `src/main.tsx` — branch renderer bootstrap between main app and repo-view window
- `src/App.tsx` — fetch/save repo-view AI settings; add `仓库查看` command + action
- `src/components/AiSettingsDialog.tsx` — add independent `仓库查看分析 AI` section
- `src/components/MainPanel.tsx` — add `仓库查看` button next to `Diff 审查`
- `src/styles.css` — add repo-view window layout/styles

### Conventions
- Keep the current dirty working tree untouched except for files listed in each task.
- Run focused tests for each new helper first; only run full `npm run test` once the UI wiring task lands.
- Do **not** run `npm run dev` in the plan steps; manual Electron validation happens after implementation.
- Every task ends with a commit that only stages files from that task.

---

## Task 1: Add repo-view window routing and open/focus plumbing

**Files:**
- Create: `electron/repo-view/windowMode.ts`
- Create: `electron/repo-view/windowMode.test.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Modify: `src/main.tsx`
- Modify: `src/types/global.d.ts`

- [ ] **Step 1: Write the failing tests for window mode parsing**

Create `electron/repo-view/windowMode.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { parseWindowModeSearch, buildRepoViewSearch } from './windowMode.js'

describe('parseWindowModeSearch', () => {
  it('defaults to main mode when no params are present', () => {
    expect(parseWindowModeSearch('')).toEqual({ kind: 'main' })
  })

  it('parses repo-view mode with projectId', () => {
    expect(parseWindowModeSearch('?window=repo-view&projectId=p_123')).toEqual({
      kind: 'repo-view',
      projectId: 'p_123'
    })
  })

  it('falls back to main mode when repo-view has no projectId', () => {
    expect(parseWindowModeSearch('?window=repo-view')).toEqual({ kind: 'main' })
  })
})

describe('buildRepoViewSearch', () => {
  it('creates a stable search string for repo-view windows', () => {
    expect(buildRepoViewSearch('p_abc')).toBe('?window=repo-view&projectId=p_abc')
  })
})
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
npx vitest run electron/repo-view/windowMode.test.ts
```

Expected: FAIL with module not found for `./windowMode.js`.

- [ ] **Step 3: Implement the route helper**

Create `electron/repo-view/windowMode.ts`:

```ts
export type WindowMode =
  | { kind: 'main' }
  | { kind: 'repo-view'; projectId: string }

export function parseWindowModeSearch(search: string): WindowMode {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search)
  if (params.get('window') !== 'repo-view') return { kind: 'main' }
  const projectId = params.get('projectId')?.trim()
  if (!projectId) return { kind: 'main' }
  return { kind: 'repo-view', projectId }
}

export function buildRepoViewSearch(projectId: string): string {
  const params = new URLSearchParams()
  params.set('window', 'repo-view')
  params.set('projectId', projectId)
  return `?${params.toString()}`
}
```

- [ ] **Step 4: Wire main-process window creation**

In `electron/main.ts`, add a window registry near the existing `isDev` constant:

```ts
const repoViewWindows = new Map<string, BrowserWindow>()
```

Add a new helper below `createWindow()`:

```ts
function createRepoViewWindow(projectId: string, title: string): BrowserWindow {
  const existing = repoViewWindows.get(projectId)
  if (existing && !existing.isDestroyed()) {
    existing.focus()
    return existing
  }

  const win = new BrowserWindow({
    width: 1440,
    height: 920,
    minWidth: 1100,
    minHeight: 720,
    show: false,
    autoHideMenuBar: true,
    title: `仓库查看 · ${title}`,
    webPreferences: {
      preload: join(__dirname, '../preload/preload.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  repoViewWindows.set(projectId, win)
  win.on('ready-to-show', () => win.show())
  win.on('closed', () => {
    repoViewWindows.delete(projectId)
  })

  win.webContents.setWindowOpenHandler((details) => {
    shell.openExternal(details.url)
    return { action: 'deny' }
  })

  const search = buildRepoViewSearch(projectId)
  if (isDev && process.env['ELECTRON_RENDERER_URL']) {
    win.loadURL(`${process.env['ELECTRON_RENDERER_URL']}/${search}`)
  } else {
    win.loadFile(join(__dirname, '../renderer/index.html'), { search })
  }
  return win
}
```

Also register a new IPC handler inside `app.whenReady()`:

```ts
ipcMain.handle('repo-view:open-window', async (_e, { projectId }: { projectId: string }) => {
  const row = getProject(projectId)
  if (!row) return { ok: false, error: 'project not found' }
  const pdir = projectDirFn(projectId)
  const meta = await readProjectMeta(pdir)
  const title = meta.name || row.name || projectId
  createRepoViewWindow(projectId, title)
  return { ok: true }
})
```

- [ ] **Step 5: Expose the open-window API through preload**

In `electron/preload.ts`, add:

```ts
  repoView: {
    openWindow: (projectId: string) =>
      ipcRenderer.invoke('repo-view:open-window', { projectId }) as Promise<{
        ok: boolean
        error?: string
      }>
  },
```

Keep it at the top level of `api` next to `git`, `project`, and `cc`.

- [ ] **Step 6: Branch renderer bootstrap by window mode**

In `src/main.tsx`, replace the unconditional `<App />` mount with:

```tsx
import { parseWindowModeSearch } from '../electron/repo-view/windowMode.js'
import RepoViewerWindow from './repo-view/RepoViewerWindow'

const mode = parseWindowModeSearch(window.location.search)

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ErrorBoundary>
      {mode.kind === 'repo-view' ? (
        <RepoViewerWindow projectId={mode.projectId} />
      ) : (
        <App />
      )}
    </ErrorBoundary>
  </React.StrictMode>
)
```

Create a temporary skeleton `src/repo-view/RepoViewerWindow.tsx` so typecheck stays green:

```tsx
export default function RepoViewerWindow({
  projectId
}: {
  projectId: string
}): JSX.Element {
  return <div>Repo viewer bootstrap for {projectId}</div>
}
```

- [ ] **Step 7: Update the global API type**

`src/types/global.d.ts` already imports `Api` from preload, so no runtime code is needed. Just verify `export type Api = typeof api` exists in `electron/preload.ts`; if missing, add:

```ts
export type Api = typeof api
```

right before `contextBridge.exposeInMainWorld('api', api)`.

- [ ] **Step 8: Verify the tests and typecheck**

Run:

```bash
npx vitest run electron/repo-view/windowMode.test.ts
npm run typecheck
```

Expected: tests pass; typecheck passes with the temporary `RepoViewerWindow`.

- [ ] **Step 9: Commit**

```bash
git add electron/repo-view/windowMode.ts electron/repo-view/windowMode.test.ts electron/main.ts electron/preload.ts src/main.tsx src/types/global.d.ts src/repo-view/RepoViewerWindow.tsx
git commit -m "feat(repo-view): add native window routing scaffold"
```

---

## Task 2: Add filtered repo tree + file read + repo-memory helpers

**Files:**
- Create: `electron/repo-view/filesystem.ts`
- Create: `electron/repo-view/filesystem.test.ts`
- Create: `electron/repo-view/memory.ts`
- Create: `electron/repo-view/memory.test.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write failing tests for ignore rules and memory paths**

Create `electron/repo-view/filesystem.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { shouldIgnoreRepoEntry, sortRepoEntries } from './filesystem.js'

describe('shouldIgnoreRepoEntry', () => {
  it('ignores heavy directories at any depth', () => {
    expect(shouldIgnoreRepoEntry('.git', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('node_modules', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('dist', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('build', true)).toBe(true)
    expect(shouldIgnoreRepoEntry('out', true)).toBe(true)
  })

  it('keeps ordinary source directories and files', () => {
    expect(shouldIgnoreRepoEntry('src', true)).toBe(false)
    expect(shouldIgnoreRepoEntry('main.ts', false)).toBe(false)
  })
})

describe('sortRepoEntries', () => {
  it('sorts directories first and then by name', () => {
    expect(
      sortRepoEntries([
        { name: 'z.ts', isDirectory: false },
        { name: 'src', isDirectory: true },
        { name: 'a.ts', isDirectory: false },
        { name: 'assets', isDirectory: true }
      ]).map((x) => x.name)
    ).toEqual(['assets', 'src', 'a.ts', 'z.ts'])
  })
})
```

Create `electron/repo-view/memory.test.ts`:

```ts
import { afterEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  ensureRepoMemoryExcluded,
  repoMemoryDir,
  repoMemoryProjectSummaryPath,
  repoMemoryFileNotePath
} from './memory.js'

describe('repo-memory paths', () => {
  it('maps repo root to the expected private memory paths', () => {
    const root = '/tmp/demo-repo'
    expect(repoMemoryDir(root)).toBe('/tmp/demo-repo/.multi-ai-code/repo-memory')
    expect(repoMemoryProjectSummaryPath(root)).toBe(
      '/tmp/demo-repo/.multi-ai-code/repo-memory/project-summary.md'
    )
    expect(repoMemoryFileNotePath(root, 'src/app.ts')).toContain(
      '/tmp/demo-repo/.multi-ai-code/repo-memory/file-notes/src/app.ts.md'
    )
  })
})

describe('ensureRepoMemoryExcluded', () => {
  const roots: string[] = []

  afterEach(async () => {
    await Promise.all(roots.map((x) => fs.rm(x, { recursive: true, force: true })))
  })

  it('appends repo-memory ignore only once', async () => {
    const root = await fs.mkdtemp(join(tmpdir(), 'repo-memory-'))
    roots.push(root)
    await fs.mkdir(join(root, '.git', 'info'), { recursive: true })
    await ensureRepoMemoryExcluded(root)
    await ensureRepoMemoryExcluded(root)
    const text = await fs.readFile(join(root, '.git', 'info', 'exclude'), 'utf8')
    expect(text.match(/\.multi-ai-code\/repo-memory\//g)?.length).toBe(1)
  })
})
```

- [ ] **Step 2: Verify tests fail**

Run:

```bash
npx vitest run electron/repo-view/filesystem.test.ts electron/repo-view/memory.test.ts
```

Expected: FAIL because the new modules do not exist.

- [ ] **Step 3: Implement filesystem helpers**

Create `electron/repo-view/filesystem.ts`:

```ts
import { promises as fs } from 'fs'
import { join } from 'path'

export interface RepoTreeEntry {
  name: string
  path: string
  isDirectory: boolean
}

const IGNORED_DIRS = new Set(['.git', 'node_modules', 'dist', 'build', 'out'])

export function shouldIgnoreRepoEntry(name: string, isDirectory: boolean): boolean {
  return isDirectory && IGNORED_DIRS.has(name)
}

export function sortRepoEntries<T extends { name: string; isDirectory: boolean }>(entries: T[]): T[] {
  return [...entries].sort((a, b) => {
    if (a.isDirectory !== b.isDirectory) return a.isDirectory ? -1 : 1
    return a.name.localeCompare(b.name, 'zh-CN')
  })
}

export async function listRepoTree(root: string, relDir = ''): Promise<RepoTreeEntry[]> {
  const abs = relDir ? join(root, relDir) : root
  const dirents = await fs.readdir(abs, { withFileTypes: true })
  const entries = dirents
    .filter((d) => !shouldIgnoreRepoEntry(d.name, d.isDirectory()))
    .map((d) => ({
      name: d.name,
      path: relDir ? `${relDir}/${d.name}` : d.name,
      isDirectory: d.isDirectory()
    }))
  return sortRepoEntries(entries)
}

export async function readRepoTextFile(root: string, relPath: string): Promise<{
  content: string
  byteLength: number
}> {
  const abs = join(root, relPath)
  const buf = await fs.readFile(abs)
  if (buf.includes(0)) {
    throw new Error('binary file is not previewable')
  }
  return {
    content: buf.toString('utf8'),
    byteLength: buf.byteLength
  }
}
```

- [ ] **Step 4: Implement repo-memory helpers**

Create `electron/repo-view/memory.ts`:

```ts
import { promises as fs } from 'fs'
import { dirname, join } from 'path'

export function repoMemoryDir(root: string): string {
  return join(root, '.multi-ai-code', 'repo-memory')
}

export function repoMemoryProjectSummaryPath(root: string): string {
  return join(repoMemoryDir(root), 'project-summary.md')
}

export function repoMemoryRecentTopicsPath(root: string): string {
  return join(repoMemoryDir(root), 'recent-topics.json')
}

export function repoMemoryFileNotePath(root: string, relPath: string): string {
  return join(repoMemoryDir(root), 'file-notes', `${relPath}.md`)
}

export async function ensureRepoMemoryExcluded(root: string): Promise<void> {
  const excludePath = join(root, '.git', 'info', 'exclude')
  await fs.mkdir(dirname(excludePath), { recursive: true })
  const line = '.multi-ai-code/repo-memory/'
  let text = ''
  try {
    text = await fs.readFile(excludePath, 'utf8')
  } catch {
    /* create below */
  }
  if (text.includes(line)) return
  const next = text.trimEnd().length > 0 ? `${text.trimEnd()}\n${line}\n` : `${line}\n`
  await fs.writeFile(excludePath, next, 'utf8')
}
```

- [ ] **Step 5: Register repo-view IPCs**

In `electron/main.ts`, add handlers:

```ts
ipcMain.handle('repo-view:list-tree', async (_e, { root, dir }: { root: string; dir?: string }) => {
  try {
    return { ok: true, entries: await listRepoTree(root, dir ?? '') }
  } catch (err) {
    return { ok: false, error: (err as Error).message, entries: [] }
  }
})

ipcMain.handle('repo-view:read-file', async (_e, { root, path }: { root: string; path: string }) => {
  try {
    const file = await readRepoTextFile(root, path)
    return { ok: true, ...file }
  } catch (err) {
    return { ok: false, error: (err as Error).message }
  }
})

ipcMain.handle('repo-view:memory-load', async (_e, { root }: { root: string }) => {
  try {
    await ensureRepoMemoryExcluded(root)
    const summaryPath = repoMemoryProjectSummaryPath(root)
    const recentPath = repoMemoryRecentTopicsPath(root)
    const [summary, recent] = await Promise.all([
      fs.readFile(summaryPath, 'utf8').catch(() => ''),
      fs.readFile(recentPath, 'utf8').catch(() => '[]')
    ])
    return { ok: true, summary, recentTopics: JSON.parse(recent) as unknown[] }
  } catch (err) {
    return { ok: false, error: (err as Error).message }
  }
})
```

Also add preload wrappers under `api.repoView`.

- [ ] **Step 6: Verify tests and typecheck**

Run:

```bash
npx vitest run electron/repo-view/filesystem.test.ts electron/repo-view/memory.test.ts
npm run typecheck
```

- [ ] **Step 7: Commit**

```bash
git add electron/repo-view/filesystem.ts electron/repo-view/filesystem.test.ts electron/repo-view/memory.ts electron/repo-view/memory.test.ts electron/main.ts electron/preload.ts
git commit -m "feat(repo-view): add filtered tree, file read, and repo-memory helpers"
```

---

## Task 3: Add independent repo-view AI settings

**Files:**
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Modify: `src/App.tsx`
- Modify: `src/components/AiSettingsDialog.tsx`

- [ ] **Step 1: Add repo-view AI settings IPC**

In `electron/main.ts`, next to the existing `project:get-ai-settings`/`project:set-ai-settings` handlers, add:

```ts
ipcMain.handle('project:get-repo-view-ai-settings', async (_e, { id }: { id: string }) => {
  const pdir = projectDirFn(id)
  try {
    const raw = await fs.readFile(join(pdir, 'project.json'), 'utf8')
    const meta = JSON.parse(raw) as { repo_view_ai_settings?: AiSettings }
    return meta.repo_view_ai_settings ?? { ai_cli: 'claude' as const }
  } catch {
    return { ai_cli: 'claude' as const }
  }
})

ipcMain.handle(
  'project:set-repo-view-ai-settings',
  async (_e, { id, settings }: { id: string; settings: AiSettings }) => {
    const pdir = projectDirFn(id)
    const metaPath = join(pdir, 'project.json')
    try {
      const raw = await fs.readFile(metaPath, 'utf8')
      const meta = JSON.parse(raw) as Record<string, unknown>
      meta.repo_view_ai_settings = settings as unknown as Record<string, unknown>
      await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
      return { ok: true }
    } catch (err) {
      return { ok: false, error: err instanceof Error ? err.message : String(err) }
    }
  }
)
```

- [ ] **Step 2: Expose getters/setters through preload**

In `electron/preload.ts`, extend `project`:

```ts
getRepoViewAiSettings: (id: string) =>
  ipcRenderer.invoke('project:get-repo-view-ai-settings', { id }) as Promise<AiSettings>,
setRepoViewAiSettings: (id: string, settings: AiSettings) =>
  ipcRenderer.invoke('project:set-repo-view-ai-settings', { id, settings }) as Promise<{
    ok: boolean
    error?: string
  }>,
```

- [ ] **Step 3: Load both settings in `App.tsx`**

Add state:

```ts
const [repoViewAiSettings, setRepoViewAiSettings] = useState<AiSettings>({
  ai_cli: 'claude'
})
```

In the `currentProjectId` effect, add:

```ts
void window.api.project.getRepoViewAiSettings(currentProjectId).then((settings) => {
  if (cancelled) return
  setRepoViewAiSettings(settings)
})
```

Also reset it to Claude in the `!currentProjectId` branch.

- [ ] **Step 4: Extend the dialog props and UI**

Change the dialog props in `src/components/AiSettingsDialog.tsx` to:

```ts
export interface AiSettingsDialogProps {
  projectId: string
  initial: AiSettings
  initialRepoView: AiSettings
  onClose: () => void
  onSaved: (next: AiSettings) => void
  onSavedRepoView: (next: AiSettings) => void
}
```

Create a small helper inside the component to normalize form state:

```ts
function fromForm(
  aiCli: 'claude' | 'codex',
  command: string,
  argsText: string,
  envText: string
): AiSettings {
  return {
    ai_cli: aiCli,
    command: command.trim() || undefined,
    args: argsText.trim().length ? argsText.trim().split(/\s+/) : undefined,
    env: Object.fromEntries(
      envText
        .split('\n')
        .map((l) => l.trim())
        .filter((l) => l.includes('='))
        .map((l) => {
          const idx = l.indexOf('=')
          return [l.slice(0, idx).trim(), l.slice(idx + 1).trim()]
        })
    )
  }
}
```

Render two sections:

- `主会话 AI`
- `仓库查看分析 AI（默认 Claude）`

Give each section its own `AI CLI` selector, binary override, args, env textarea. Save both in one click by calling both project setters in sequence.

- [ ] **Step 5: Pass the new props from `App.tsx`**

Update the dialog mount:

```tsx
<AiSettingsDialog
  projectId={currentProjectId}
  initial={aiSettings}
  initialRepoView={repoViewAiSettings}
  onClose={() => setShowAiSettings(false)}
  onSaved={(next) => setAiSettings(next)}
  onSavedRepoView={(next) => setRepoViewAiSettings(next)}
/>
```

- [ ] **Step 6: Verify typecheck**

Run:

```bash
npm run typecheck
```

- [ ] **Step 7: Commit**

```bash
git add electron/main.ts electron/preload.ts src/App.tsx src/components/AiSettingsDialog.tsx
git commit -m "feat(settings): add independent repo-view AI configuration"
```

---

## Task 4: Define the analysis protocol and memory-update block

**Files:**
- Create: `electron/repo-view/analysisPrompt.ts`
- Create: `electron/repo-view/analysisPrompt.test.ts`
- Create: `src/repo-view/parseAnalysisOutput.ts`
- Create: `src/repo-view/parseAnalysisOutput.test.ts`

- [ ] **Step 1: Write failing tests**

Create `electron/repo-view/analysisPrompt.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { buildRepoAnalysisPrompt } from './analysisPrompt.js'

describe('buildRepoAnalysisPrompt', () => {
  it('includes no-write constraints and the memory block contract', () => {
    const text = buildRepoAnalysisPrompt({
      repoRoot: '/repo',
      filePath: 'src/app.ts',
      selection: 'function demo() {}',
      question: '这段逻辑做什么？',
      projectSummary: 'summary',
      fileNote: 'file note'
    })
    expect(text).toContain('不要修改仓库文件')
    expect(text).toContain('[[MEMORY_UPDATE]]')
    expect(text).toContain('[[END_OF_ANALYSIS]]')
    expect(text).toContain('src/app.ts')
  })
})
```

Create `src/repo-view/parseAnalysisOutput.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { parseAnalysisOutput } from './parseAnalysisOutput.js'

describe('parseAnalysisOutput', () => {
  it('splits visible answer from memory block and end marker', () => {
    const out = parseAnalysisOutput([
      '## 分析\nanswer text\n',
      '[[MEMORY_UPDATE]]\nsummary text\n[[END_OF_ANALYSIS]]'
    ].join(''))
    expect(out.answer).toContain('answer text')
    expect(out.memoryUpdate).toContain('summary text')
    expect(out.complete).toBe(true)
  })
})
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
npx vitest run electron/repo-view/analysisPrompt.test.ts src/repo-view/parseAnalysisOutput.test.ts
```

Expected: FAIL because the modules do not exist.

- [ ] **Step 3: Implement the prompt builder**

Create `electron/repo-view/analysisPrompt.ts`:

```ts
export function buildRepoAnalysisPrompt(input: {
  repoRoot: string
  filePath: string
  selection: string
  question: string
  projectSummary: string
  fileNote: string
}): string {
  return [
    '# 角色',
    '',
    '你正在做代码逻辑分析，不是代码实现。',
    '不要修改仓库文件，不要运行写操作命令，不要生成补丁。',
    '',
    '# 仓库上下文',
    '',
    `仓库根：${input.repoRoot}`,
    `文件：${input.filePath}`,
    '',
    '## 项目记忆',
    input.projectSummary || '（暂无项目记忆）',
    '',
    '## 文件记忆',
    input.fileNote || '（暂无文件记忆）',
    '',
    '## 选中代码',
    '```ts',
    input.selection,
    '```',
    '',
    '## 用户问题',
    input.question,
    '',
    '# 输出格式',
    '',
    '先输出给用户看的分析内容。',
    '结尾必须追加：',
    '[[MEMORY_UPDATE]]',
    '1. 本次稳定结论',
    '2. 值得并入项目记忆的事实',
    '[[END_OF_ANALYSIS]]'
  ].join('\n')
}
```

- [ ] **Step 4: Implement the parser**

Create `src/repo-view/parseAnalysisOutput.ts`:

```ts
export function parseAnalysisOutput(raw: string): {
  answer: string
  memoryUpdate: string
  complete: boolean
} {
  const endIdx = raw.indexOf('[[END_OF_ANALYSIS]]')
  const memoryIdx = raw.indexOf('[[MEMORY_UPDATE]]')
  const complete = endIdx >= 0
  if (memoryIdx < 0) {
    return {
      answer: complete ? raw.slice(0, endIdx).trim() : raw,
      memoryUpdate: '',
      complete
    }
  }
  const answer = raw.slice(0, memoryIdx).trim()
  const memoryRaw = complete
    ? raw.slice(memoryIdx + '[[MEMORY_UPDATE]]'.length, endIdx)
    : raw.slice(memoryIdx + '[[MEMORY_UPDATE]]'.length)
  return {
    answer,
    memoryUpdate: memoryRaw.trim(),
    complete
  }
}
```

- [ ] **Step 5: Verify tests and typecheck**

```bash
npx vitest run electron/repo-view/analysisPrompt.test.ts src/repo-view/parseAnalysisOutput.test.ts
npm run typecheck
```

- [ ] **Step 6: Commit**

```bash
git add electron/repo-view/analysisPrompt.ts electron/repo-view/analysisPrompt.test.ts src/repo-view/parseAnalysisOutput.ts src/repo-view/parseAnalysisOutput.test.ts
git commit -m "feat(repo-view): define analysis prompt and memory-update protocol"
```

---

## Task 5: Add hidden PTY-backed repo analysis sessions

**Files:**
- Create: `electron/repo-view/repoAnalysisManager.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Create the session manager**

Create `electron/repo-view/repoAnalysisManager.ts` with the exact public API below:

```ts
import { BrowserWindow } from 'electron'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { randomBytes } from 'crypto'
import { PtyCCProcess } from '../cc/PtyCCProcess.js'
import { buildRepoAnalysisPrompt } from './analysisPrompt.js'

interface RepoAnalysisSession {
  winId: number
  proc: PtyCCProcess
  projectId: string
  targetRepo: string
}

const sessions = new Map<number, RepoAnalysisSession>()

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

async function sendMessage(proc: PtyCCProcess, text: string): Promise<void> {
  const chunk = 64
  for (let i = 0; i < text.length; i += chunk) {
    proc.write(text.slice(i, i + chunk))
    await sleep(6)
  }
  await sleep(300)
  proc.write('\r')
}

function emitTo(winId: number, channel: string, payload: unknown): void {
  const win = BrowserWindow.fromId(winId)
  if (win && !win.isDestroyed()) {
    win.webContents.send(channel, payload)
  }
}

export async function startRepoAnalysisSession(input: {
  winId: number
  projectId: string
  targetRepo: string
  command: string
  args: string[]
  env?: Record<string, string>
}): Promise<void> {
  if (sessions.has(input.winId)) return
  const proc = new PtyCCProcess({
    cwd: input.targetRepo,
    command: input.command,
    args: input.args,
    env: input.env
  })
  proc.on('data', (chunk) => emitTo(input.winId, 'repo-view:analysis-data', { chunk }))
  proc.on('exit', ({ exitCode, signal }) =>
    emitTo(input.winId, 'repo-view:analysis-status', {
      status: 'exited',
      exitCode,
      signal
    })
  )
  proc.start()
  sessions.set(input.winId, {
    winId: input.winId,
    proc,
    projectId: input.projectId,
    targetRepo: input.targetRepo
  })
  emitTo(input.winId, 'repo-view:analysis-status', { status: 'running' })
}

export async function sendRepoAnalysisPrompt(input: {
  winId: number
  repoRoot: string
  filePath: string
  selection: string
  question: string
  projectSummary: string
  fileNote: string
}): Promise<void> {
  const session = sessions.get(input.winId)
  if (!session) throw new Error('repo analysis session not started')
  const text = buildRepoAnalysisPrompt(input)
  const dir = join(tmpdir(), 'multi-ai-code', 'repo-view')
  await fs.mkdir(dir, { recursive: true })
  const file = join(dir, `analysis-${randomBytes(4).toString('hex')}.md`)
  await fs.writeFile(file, text, 'utf8')
  await sendMessage(
    session.proc,
    `请先完整读取 ${file}，然后严格按要求输出分析结果。`
  )
}

export function stopRepoAnalysisSession(winId: number): void {
  const session = sessions.get(winId)
  if (!session) return
  session.proc.kill()
  sessions.delete(winId)
}
```

- [ ] **Step 2: Register IPC handlers**

In `electron/main.ts`, add:

```ts
ipcMain.handle('repo-view:analysis-start', async (e, req) => {
  const win = BrowserWindow.fromWebContents(e.sender)
  if (!win) return { ok: false, error: 'window not found' }
  try {
    await startRepoAnalysisSession({ winId: win.id, ...req })
    return { ok: true }
  } catch (err) {
    return { ok: false, error: (err as Error).message }
  }
})

ipcMain.handle('repo-view:analysis-send', async (e, req) => {
  const win = BrowserWindow.fromWebContents(e.sender)
  if (!win) return { ok: false, error: 'window not found' }
  try {
    await sendRepoAnalysisPrompt({ winId: win.id, ...req })
    return { ok: true }
  } catch (err) {
    return { ok: false, error: (err as Error).message }
  }
})

ipcMain.handle('repo-view:analysis-stop', (e) => {
  const win = BrowserWindow.fromWebContents(e.sender)
  if (!win) return { ok: false, error: 'window not found' }
  stopRepoAnalysisSession(win.id)
  return { ok: true }
})
```

Also, when a repo-view window closes in `createRepoViewWindow`, call `stopRepoAnalysisSession(win.id)` before deleting the registry entry.

- [ ] **Step 3: Expose streaming APIs in preload**

Add to `api.repoView` in `electron/preload.ts`:

```ts
analysisStart: (req: {
  projectId: string
  targetRepo: string
  command: string
  args: string[]
  env?: Record<string, string>
}) => ipcRenderer.invoke('repo-view:analysis-start', req) as Promise<{ ok: boolean; error?: string }>,
analysisSend: (req: {
  repoRoot: string
  filePath: string
  selection: string
  question: string
  projectSummary: string
  fileNote: string
}) => ipcRenderer.invoke('repo-view:analysis-send', req) as Promise<{ ok: boolean; error?: string }>,
analysisStop: () =>
  ipcRenderer.invoke('repo-view:analysis-stop') as Promise<{ ok: boolean; error?: string }>,
onAnalysisData: (cb: (evt: { chunk: string }) => void) => {
  const handler = (_: IpcRendererEvent, evt: { chunk: string }) => cb(evt)
  ipcRenderer.on('repo-view:analysis-data', handler)
  return () => ipcRenderer.removeListener('repo-view:analysis-data', handler)
},
onAnalysisStatus: (
  cb: (evt: { status: string; exitCode?: number; signal?: number }) => void
) => {
  const handler = (
    _: IpcRendererEvent,
    evt: { status: string; exitCode?: number; signal?: number }
  ) => cb(evt)
  ipcRenderer.on('repo-view:analysis-status', handler)
  return () => ipcRenderer.removeListener('repo-view:analysis-status', handler)
},
```

- [ ] **Step 4: Verify typecheck**

Run:

```bash
npm run typecheck
```

- [ ] **Step 5: Commit**

```bash
git add electron/repo-view/repoAnalysisManager.ts electron/main.ts electron/preload.ts
git commit -m "feat(repo-view): add hidden PTY analysis session manager"
```

---

## Task 6: Build the repo-view window UI shell

**Files:**
- Modify: `src/repo-view/RepoViewerWindow.tsx`
- Create: `src/repo-view/FileTree.tsx`
- Create: `src/repo-view/CodePane.tsx`
- Create: `src/repo-view/AnalysisPanel.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Replace the placeholder window with a three-pane layout**

Implement `src/repo-view/RepoViewerWindow.tsx` with this state shape:

```tsx
import { useEffect, useMemo, useState } from 'react'
import FileTree from './FileTree'
import CodePane from './CodePane'
import AnalysisPanel from './AnalysisPanel'

export default function RepoViewerWindow({
  projectId
}: {
  projectId: string
}): JSX.Element {
  const [projects, setProjects] = useState<
    Array<{ id: string; name: string; target_repo: string }>
  >([])
  const [selectedFile, setSelectedFile] = useState('')
  const [selectedContent, setSelectedContent] = useState('')
  const [selectedSize, setSelectedSize] = useState(0)
  const [loadingFile, setLoadingFile] = useState(false)
  const project = useMemo(
    () => projects.find((p) => p.id === projectId) ?? null,
    [projects, projectId]
  )

  useEffect(() => {
    void window.api.project.list().then((list) => {
      setProjects(list.map((p) => ({ id: p.id, name: p.name, target_repo: p.target_repo })))
    })
  }, [])

  useEffect(() => {
    if (!project || !selectedFile) return
    let cancelled = false
    setLoadingFile(true)
    void window.api.repoView.readFile(project.target_repo, selectedFile).then((res) => {
      if (cancelled) return
      setLoadingFile(false)
      if (!res.ok || !res.content) {
        setSelectedContent(res.error ?? '无法读取文件')
        setSelectedSize(0)
        return
      }
      setSelectedContent(res.content)
      setSelectedSize(res.byteLength ?? 0)
    })
    return () => {
      cancelled = true
    }
  }, [project, selectedFile])

  if (!project) {
    return <div className="repo-view-empty">项目不存在或尚未加载</div>
  }

  return (
    <div className="repo-view-window">
      <div className="repo-view-sidebar">
        <FileTree
          repoRoot={project.target_repo}
          selectedFile={selectedFile}
          onSelectFile={setSelectedFile}
        />
      </div>
      <div className="repo-view-main">
        <CodePane
          filePath={selectedFile}
          content={selectedContent}
          byteLength={selectedSize}
          loading={loadingFile}
        />
      </div>
      <div className="repo-view-analysis">
        <AnalysisPanel projectId={projectId} repoRoot={project.target_repo} filePath={selectedFile} />
      </div>
    </div>
  )
}
```

- [ ] **Step 2: Implement a lazy file tree**

Create `src/repo-view/FileTree.tsx` with a minimal recursive row renderer that loads children on expand. The critical types and callback signature must be:

```tsx
export interface FileTreeProps {
  repoRoot: string
  selectedFile: string
  onSelectFile: (path: string) => void
}
```

Keep state for:

- `expanded: Set<string>`
- `childrenByDir: Record<string, RepoTreeEntry[]>`
- `loadingDirs: Set<string>`

Use the preload method:

```ts
window.api.repoView.listTree(repoRoot, dir)
```

and auto-load the root directory (`dir=''`) on mount.

- [ ] **Step 3: Implement the single-file code pane**

Create `src/repo-view/CodePane.tsx` with the exact prop type:

```tsx
export interface CodePaneProps {
  filePath: string
  content: string
  byteLength: number
  loading: boolean
}
```

Render:

- top bar showing `filePath || '未选择文件'`
- loading / empty / error placeholders
- otherwise a line-numbered `<pre>` code viewer

The line renderer should be:

```tsx
{content.split('\n').map((line, index) => (
  <div key={index} className="repo-code-line">
    <span className="repo-code-gutter">{index + 1}</span>
    <span className="repo-code-text">{line || ' '}</span>
  </div>
))}
```

- [ ] **Step 4: Add a placeholder analysis panel**

Create `src/repo-view/AnalysisPanel.tsx`:

```tsx
export default function AnalysisPanel({
  projectId,
  repoRoot,
  filePath
}: {
  projectId: string
  repoRoot: string
  filePath: string
}): JSX.Element {
  return (
    <div className="repo-analysis-panel">
      <div className="repo-analysis-head">代码分析</div>
      <div className="repo-analysis-empty">
        {filePath ? '选中代码后发起分析。' : '先从左侧选择一个文件。'}
      </div>
    </div>
  )
}
```

- [ ] **Step 5: Add the layout CSS**

Append to `src/styles.css`:

```css
.repo-view-window {
  display: grid;
  grid-template-columns: 300px minmax(0, 1fr) 360px;
  height: 100vh;
  background: var(--mac-bg);
  color: var(--mac-fg);
}

.repo-view-sidebar,
.repo-view-main,
.repo-view-analysis {
  min-width: 0;
  min-height: 0;
  overflow: hidden;
}

.repo-view-sidebar {
  border-right: 1px solid var(--mac-border-subtle);
  background: var(--mac-surface);
}

.repo-view-main {
  display: flex;
  flex-direction: column;
  background: var(--mac-surface);
}

.repo-view-analysis {
  border-left: 1px solid var(--mac-border-subtle);
  background: var(--mac-surface-raised);
}

.repo-code-pane {
  flex: 1;
  overflow: auto;
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
}

.repo-code-line {
  display: grid;
  grid-template-columns: 64px minmax(0, 1fr);
  white-space: pre;
}
```

- [ ] **Step 6: Verify typecheck**

Run:

```bash
npm run typecheck
```

- [ ] **Step 7: Commit**

```bash
git add src/repo-view/RepoViewerWindow.tsx src/repo-view/FileTree.tsx src/repo-view/CodePane.tsx src/repo-view/AnalysisPanel.tsx src/styles.css
git commit -m "feat(repo-view): build native window UI shell"
```

---

## Task 7: Add selection analysis, memory persistence, and main-window entry points

**Files:**
- Modify: `src/repo-view/CodePane.tsx`
- Modify: `src/repo-view/AnalysisPanel.tsx`
- Modify: `src/repo-view/RepoViewerWindow.tsx`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Modify: `src/App.tsx`
- Modify: `src/components/MainPanel.tsx`

- [ ] **Step 1: Add selection capture to `CodePane`**

Extend `CodePaneProps`:

```tsx
export interface CodePaneProps {
  filePath: string
  content: string
  byteLength: number
  loading: boolean
  onAnalyzeSelection: (payload: { snippet: string; lineRange: string }) => void
}
```

Capture selection inside the pane using `window.getSelection()`. When non-empty text is selected, show a floating `分析` button. Use the same pattern as `PlanReviewDialog`:

```tsx
const [draft, setDraft] = useState<{ quote: string; x: number; y: number; lineRange: string } | null>(null)
```

For line range, use the start/end rendered row indices touched by the selection:

```ts
const lineRange = lo === hi ? `${lo}` : `${lo}-${hi}`
```

Clicking the floating button calls:

```ts
onAnalyzeSelection({ snippet: draft.quote, lineRange: draft.lineRange })
```

- [ ] **Step 2: Turn `AnalysisPanel` into a streaming analysis UI**

Replace the placeholder component with state for:

- `question`
- `messages: Array<{ role: 'user' | 'assistant'; text: string }>`
- `running`
- `rawAssistantBuffer`
- `projectSummary`
- `recentTopics`
- `pendingSelection`

On mount:

```ts
void window.api.repoView.memoryLoad(repoRoot).then((res) => {
  if (!res.ok) return
  setProjectSummary(res.summary ?? '')
  setRecentTopics((res.recentTopics ?? []) as Array<{ topic: string; filePath: string }>)
})
```

On submit:

1. call `window.api.project.getRepoViewAiSettings(projectId)`
2. start the hidden analysis session if not already started
3. send the built request via `window.api.repoView.analysisSend(...)`

Subscribe to:

```ts
const offData = window.api.repoView.onAnalysisData(({ chunk }) => {
  setRawAssistantBuffer((prev) => prev + chunk)
})
```

Parse the accumulated buffer with `parseAnalysisOutput`. When `complete === true`, append:

```ts
setMessages((prev) => [...prev, { role: 'assistant', text: parsed.answer }])
```

and call `window.api.repoView.memoryUpdate(...)` with the parsed memory block.

- [ ] **Step 3: Add the memory update IPC**

In `electron/main.ts`, register:

```ts
ipcMain.handle(
  'repo-view:memory-update',
  async (
    _e,
    {
      root,
      filePath,
      topic,
      memoryUpdate
    }: {
      root: string
      filePath: string
      topic: string
      memoryUpdate: string
    }
  ) => {
    try {
      await ensureRepoMemoryExcluded(root)
      const summaryPath = repoMemoryProjectSummaryPath(root)
      const notePath = repoMemoryFileNotePath(root, filePath)
      const recentPath = repoMemoryRecentTopicsPath(root)
      await fs.mkdir(dirname(notePath), { recursive: true })
      const stamp = new Date().toISOString()
      await fs.appendFile(summaryPath, `\n\n## ${stamp}\n${memoryUpdate}\n`, 'utf8')
      await fs.appendFile(notePath, `\n\n## ${stamp}\n${memoryUpdate}\n`, 'utf8')
      const recent = await fs.readFile(recentPath, 'utf8').catch(() => '[]')
      const next = [
        { topic, filePath, updatedAt: stamp },
        ...JSON.parse(recent).filter((x: { topic?: string; filePath?: string }) => !(x.topic === topic && x.filePath === filePath))
      ].slice(0, 20)
      await fs.writeFile(recentPath, JSON.stringify(next, null, 2), 'utf8')
      return { ok: true }
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
  }
)
```

Expose the matching preload method:

```ts
memoryUpdate: (root: string, filePath: string, topic: string, memoryUpdate: string) =>
  ipcRenderer.invoke('repo-view:memory-update', {
    root,
    filePath,
    topic,
    memoryUpdate
  }) as Promise<{ ok: boolean; error?: string }>,
```

- [ ] **Step 4: Lift selection state through `RepoViewerWindow`**

In `RepoViewerWindow.tsx`, add:

```tsx
const [pendingSelection, setPendingSelection] = useState<{
  snippet: string
  lineRange: string
} | null>(null)
```

Pass it into the panes:

```tsx
<CodePane
  filePath={selectedFile}
  content={selectedContent}
  byteLength={selectedSize}
  loading={loadingFile}
  onAnalyzeSelection={setPendingSelection}
/>
<AnalysisPanel
  projectId={projectId}
  repoRoot={project.target_repo}
  filePath={selectedFile}
  pendingSelection={pendingSelection}
  onConsumeSelection={() => setPendingSelection(null)}
/>
```

- [ ] **Step 5: Add the main-window entry points**

In `src/components/MainPanel.tsx`, extend props:

```tsx
/** Called when user clicks "仓库查看". */
onOpenRepoView: () => void
```

Render a second button before `Diff 审查`:

```tsx
<button
  className="tile-btn"
  onClick={props.onOpenRepoView}
  disabled={props.disabled}
  title="打开仓库查看窗口"
>
  仓库查看
</button>
```

In `src/App.tsx`, add:

```ts
const openRepoView = useCallback(async () => {
  if (!currentProjectId || !targetRepo) {
    showToast('当前项目没有 target_repo，无法打开仓库查看', { level: 'warn' })
    return
  }
  const res = await window.api.repoView.openWindow(currentProjectId)
  if (!res.ok) {
    showToast(res.error ?? '打开仓库查看失败', { level: 'error' })
  }
}, [currentProjectId, targetRepo])
```

Wire it into:

- `MainPanel`
- Command palette item label `🗂 仓库查看`

- [ ] **Step 6: Run the final verification**

Run:

```bash
npm run typecheck
npm run test
```

Both must pass before committing.

- [ ] **Step 7: Commit**

```bash
git add src/repo-view/CodePane.tsx src/repo-view/AnalysisPanel.tsx src/repo-view/RepoViewerWindow.tsx electron/main.ts electron/preload.ts src/App.tsx src/components/MainPanel.tsx
git commit -m "feat(repo-view): add code selection analysis and window entry points"
```

---

## Self-Review

- Spec coverage:
  - Independent native window: Task 1 + Task 6
  - Filtered repo tree + full file reading: Task 2 + Task 6
  - Independent repo-view AI config defaulting to Claude: Task 3
  - Hidden independent AI CLI analysis session: Task 5
  - Code selection analysis UX: Task 7
  - Repo-local private memory with `.git/info/exclude`: Task 2 + Task 7
  - `Diff 审查` unchanged: preserved by isolating all new work under `repo-view:*` and new UI files
- Placeholder scan: no `TODO`/`TBD`; every task names exact files, commands, and commit messages.
- Type consistency:
  - `repo_view_ai_settings` is used consistently across main/preload/App/dialog
  - `repo-view:*` IPC names are consistent across main/preload/UI
  - `WindowMode` search param uses `window=repo-view&projectId=<id>` everywhere

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-22-repo-viewer-window.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
