# Repo View Annotation Streamline + AI Persistent Memory — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace repo-view's structured analysis pipeline with plain-text injection into the embedded AI CLI terminal, and have Claude/Codex persist analyses into a per-repo cache so re-analysis is avoided.

**Architecture:** Front-end builds a plain-text injection (annotations + question + a "记忆约定" footer pointing at `<repo>/.multi-ai-code/repo-view/analyses/<encoded>.md`). A repurposed backend `sendRepoAnalysisPrompt` writes the text into the PTY (with the existing readiness wait), and `ensureAnalysisCacheDir` creates the cache dir + `.gitignore` on first send. The AI is asked to read the cache file before analyzing and append its conclusions afterward. The streaming chat-bubble UI, marker parser, and history/memory auto-update are removed.

**Tech Stack:** TypeScript, Electron (main + preload), React (renderer), Vitest.

---

## File Structure

**Create:**
- `src/repo-view/buildCliInjectionText.ts` — pure function that builds the injection text + path encoding helper
- `src/repo-view/buildCliInjectionText.test.ts` — vitest unit tests
- `electron/repo-view/analysisCache.ts` — backend `ensureAnalysisCacheDir(repoRoot)` helper
- `electron/repo-view/analysisCache.test.ts` — vitest unit tests against `os.tmpdir()`

**Modify:**
- `electron/repo-view/repoAnalysisManager.ts` — `sendRepoAnalysisPrompt` repurposed to inject plain text (keep readiness wait, drop md-file logic)
- `electron/main.ts` — `repo-view:analysis-send` signature → `{ repoRoot, text }`, call `ensureAnalysisCacheDir`
- `electron/preload.ts` — `analysisSend(req: { repoRoot, text })`
- `src/repo-view/RepoViewerWindow.tsx` — remove old state/effects, add `onSendToCli`
- `src/repo-view/AnalysisPanel.tsx` — remove chat bubbles / recentTopics / running state; button text "发送到 AI CLI"
- `src/styles.css` — drop the `.repo-analysis-chat`, `.repo-analysis-bubble*`, `.repo-analysis-markdown*`, `.repo-analysis-recent*` rule blocks

**Delete:**
- `electron/repo-view/analysisPrompt.ts`
- `src/repo-view/parseAnalysisOutput.ts` and `src/repo-view/parseAnalysisOutput.test.ts`
- `src/repo-view/repoConversation.ts`
- `src/repo-view/repoAnnotationMessage.ts` and `src/repo-view/repoAnnotationMessage.test.ts`

---

### Task 1: `buildCliInjectionText` — pure function with tests

**Files:**
- Create: `src/repo-view/buildCliInjectionText.ts`
- Test: `src/repo-view/buildCliInjectionText.test.ts`

- [ ] **Step 1: Write the failing tests**

Create `src/repo-view/buildCliInjectionText.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import {
  buildCliInjectionText,
  encodeAnalysisFileName
} from './buildCliInjectionText'

describe('encodeAnalysisFileName', () => {
  it('replaces path separators with double underscore and appends .md', () => {
    expect(encodeAnalysisFileName('libobs/obs-audio-controls.c')).toBe(
      'libobs__obs-audio-controls.c.md'
    )
  })

  it('keeps a top-level filename intact', () => {
    expect(encodeAnalysisFileName('CMakeLists.txt')).toBe('CMakeLists.txt.md')
  })

  it('truncates very long paths and appends an 8-char sha1 suffix', () => {
    const deep = Array.from({ length: 40 }, (_, i) => `seg${i}`).join('/')
    const out = encodeAnalysisFileName(`${deep}/file.ts`)
    expect(out.length).toBeLessThanOrEqual(200)
    expect(out.endsWith('.md')).toBe(true)
    expect(out).toMatch(/__[0-9a-f]{8}\.md$/)
  })
})

describe('buildCliInjectionText', () => {
  const baseInput = {
    repoRoot: '/repo/obs-studio',
    filePath: 'libobs/obs-audio-controls.c',
    annotations: [
      {
        id: 'a1',
        filePath: 'libobs/obs-audio-controls.c',
        lineRange: '52-53',
        snippet: 'float cur_db;\nbool ignore_next_signal;',
        comment: '这行是什么意思'
      }
    ],
    question: ''
  }

  it('emits the file header, fenced snippet, comment, and default question', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).toContain('仓库根: /repo/obs-studio')
    expect(text).toContain('文件: libobs/obs-audio-controls.c')
    expect(text).toContain('## 标注 1（第 52-53 行）')
    expect(text).toContain('```c')
    expect(text).toContain('float cur_db;')
    expect(text).toContain('说明: 这行是什么意思')
    expect(text).toContain('## 问题')
    expect(text).toContain('请按标注分析')
  })

  it('uses the user-provided question instead of the default', () => {
    const text = buildCliInjectionText({
      ...baseInput,
      question: '主流程是什么？'
    })
    expect(text).toContain('主流程是什么？')
    expect(text).not.toContain('请按标注分析')
  })

  it('numbers multiple annotations in order', () => {
    const text = buildCliInjectionText({
      ...baseInput,
      annotations: [
        { ...baseInput.annotations[0], id: 'a1' },
        {
          id: 'a2',
          filePath: 'libobs/obs-audio-controls.c',
          lineRange: '60',
          snippet: 'return 0;',
          comment: '这里返回什么'
        }
      ]
    })
    expect(text).toContain('## 标注 1（第 52-53 行）')
    expect(text).toContain('## 标注 2（第 60 行）')
  })

  it('emits a 记忆约定 section pointing at the encoded cache path', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).toContain('## 记忆约定')
    expect(text).toContain(
      '.multi-ai-code/repo-view/analyses/libobs__obs-audio-controls.c.md'
    )
    expect(text).toContain('先读取并尽量复用既有结论')
    expect(text).toContain('append 形式写入该文件')
  })

  it('picks a fence language from the file extension and falls back to empty', () => {
    const tsx = buildCliInjectionText({
      ...baseInput,
      filePath: 'src/App.tsx',
      annotations: [
        { ...baseInput.annotations[0], filePath: 'src/App.tsx' }
      ]
    })
    expect(tsx).toContain('```tsx')

    const unknown = buildCliInjectionText({
      ...baseInput,
      filePath: 'data/blob.xyz',
      annotations: [
        { ...baseInput.annotations[0], filePath: 'data/blob.xyz' }
      ]
    })
    // unknown extension → bare ``` fence
    expect(unknown).toMatch(/```\nfloat cur_db;/)
  })
})
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run src/repo-view/buildCliInjectionText.test.ts`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement `buildCliInjectionText.ts`**

Create `src/repo-view/buildCliInjectionText.ts`:

```ts
import { createHash } from 'crypto'
import type { RepoCodeAnnotation } from './AnalysisPanel'

const MAX_FILENAME_LEN = 200

const EXT_TO_LANG: Record<string, string> = {
  c: 'c',
  h: 'c',
  cc: 'cpp',
  cpp: 'cpp',
  hpp: 'cpp',
  cs: 'csharp',
  go: 'go',
  java: 'java',
  js: 'js',
  jsx: 'jsx',
  json: 'json',
  kt: 'kotlin',
  m: 'objc',
  mm: 'objc',
  md: 'markdown',
  py: 'python',
  rb: 'ruby',
  rs: 'rust',
  sh: 'bash',
  swift: 'swift',
  toml: 'toml',
  ts: 'ts',
  tsx: 'tsx',
  yaml: 'yaml',
  yml: 'yaml'
}

function langForFile(filePath: string): string {
  const dot = filePath.lastIndexOf('.')
  if (dot < 0) return ''
  return EXT_TO_LANG[filePath.slice(dot + 1).toLowerCase()] ?? ''
}

export function encodeAnalysisFileName(filePath: string): string {
  const flat = filePath.replace(/\//g, '__')
  const withExt = `${flat}.md`
  if (withExt.length <= MAX_FILENAME_LEN) return withExt
  const hash = createHash('sha1').update(filePath).digest('hex').slice(0, 8)
  // budget: head + '__' + hash + '.md' ≤ MAX_FILENAME_LEN
  const head = flat.slice(0, MAX_FILENAME_LEN - (2 + 8 + 3))
  return `${head}__${hash}.md`
}

export interface BuildCliInjectionTextInput {
  repoRoot: string
  filePath: string
  annotations: RepoCodeAnnotation[]
  question: string
}

export function buildCliInjectionText(
  input: BuildCliInjectionTextInput
): string {
  const lang = langForFile(input.filePath)
  const fenceOpen = lang ? '```' + lang : '```'
  const cachePath = `.multi-ai-code/repo-view/analyses/${encodeAnalysisFileName(input.filePath)}`

  const annotationBlocks = input.annotations.map((a, i) =>
    [
      `## 标注 ${i + 1}（第 ${a.lineRange} 行）`,
      fenceOpen,
      a.snippet,
      '```',
      `说明: ${a.comment}`
    ].join('\n')
  )

  const question = input.question.trim() || '请按标注分析'

  return [
    `仓库根: ${input.repoRoot}`,
    `文件: ${input.filePath}`,
    '',
    annotationBlocks.join('\n\n'),
    '',
    '## 问题',
    question,
    '',
    '## 记忆约定',
    `- 已有分析缓存：${cachePath}`,
    '- 若该文件存在，先读取并尽量复用既有结论；只补充新增内容，不重复推理',
    '- 回答完成后，把本次稳定结论以 append 形式写入该文件，记录：日期 / 行号 / 标注摘要 / 结论要点'
  ].join('\n')
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run src/repo-view/buildCliInjectionText.test.ts`
Expected: PASS — all 7 cases green.

- [ ] **Step 5: Commit**

```bash
git add src/repo-view/buildCliInjectionText.ts src/repo-view/buildCliInjectionText.test.ts
git commit -m "feat(repo-view): add buildCliInjectionText for plain-text annotation injection"
```

---

### Task 2: Backend `ensureAnalysisCacheDir` helper with tests

**Files:**
- Create: `electron/repo-view/analysisCache.ts`
- Test: `electron/repo-view/analysisCache.test.ts`

- [ ] **Step 1: Write the failing tests**

Create `electron/repo-view/analysisCache.test.ts`:

```ts
import { describe, expect, it, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { mkdtemp } from 'fs/promises'
import { ensureAnalysisCacheDir } from './analysisCache'

async function makeRepo(): Promise<string> {
  return await mkdtemp(join(tmpdir(), 'multi-ai-code-test-'))
}

describe('ensureAnalysisCacheDir', () => {
  let repo: string

  beforeEach(async () => {
    repo = await makeRepo()
  })

  it('creates the analyses directory', async () => {
    await ensureAnalysisCacheDir(repo)
    const stat = await fs.stat(join(repo, '.multi-ai-code/repo-view/analyses'))
    expect(stat.isDirectory()).toBe(true)
  })

  it('creates a .gitignore with the cache rule when missing', async () => {
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    expect(gi).toContain('repo-view/analyses/')
  })

  it('appends the rule when .gitignore exists without it', async () => {
    await fs.mkdir(join(repo, '.multi-ai-code'), { recursive: true })
    await fs.writeFile(join(repo, '.multi-ai-code/.gitignore'), 'foo\n', 'utf8')
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    expect(gi).toContain('foo')
    expect(gi).toContain('repo-view/analyses/')
  })

  it('does not duplicate the rule when already present', async () => {
    await fs.mkdir(join(repo, '.multi-ai-code'), { recursive: true })
    await fs.writeFile(
      join(repo, '.multi-ai-code/.gitignore'),
      'foo\nrepo-view/analyses/\n',
      'utf8'
    )
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    const occurrences = gi.split('repo-view/analyses/').length - 1
    expect(occurrences).toBe(1)
  })

  it('is idempotent', async () => {
    await ensureAnalysisCacheDir(repo)
    await ensureAnalysisCacheDir(repo)
    const gi = await fs.readFile(join(repo, '.multi-ai-code/.gitignore'), 'utf8')
    const occurrences = gi.split('repo-view/analyses/').length - 1
    expect(occurrences).toBe(1)
  })
})
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run electron/repo-view/analysisCache.test.ts`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement `analysisCache.ts`**

Create `electron/repo-view/analysisCache.ts`:

```ts
import { promises as fs } from 'fs'
import { join } from 'path'

const RULE = 'repo-view/analyses/'

export async function ensureAnalysisCacheDir(repoRoot: string): Promise<void> {
  const baseDir = join(repoRoot, '.multi-ai-code')
  const cacheDir = join(baseDir, 'repo-view', 'analyses')
  await fs.mkdir(cacheDir, { recursive: true })

  const giPath = join(baseDir, '.gitignore')
  let current = ''
  try {
    current = await fs.readFile(giPath, 'utf8')
  } catch {
    /* missing — treat as empty */
  }

  const lines = current.split('\n').map((l) => l.trim())
  if (lines.includes(RULE)) return

  const next =
    current.length === 0 || current.endsWith('\n')
      ? `${current}${RULE}\n`
      : `${current}\n${RULE}\n`
  await fs.writeFile(giPath, next, 'utf8')
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run electron/repo-view/analysisCache.test.ts`
Expected: PASS — all 5 cases green.

- [ ] **Step 5: Commit**

```bash
git add electron/repo-view/analysisCache.ts electron/repo-view/analysisCache.test.ts
git commit -m "feat(repo-view): add ensureAnalysisCacheDir for AI-side memory cache"
```

---

### Task 3: Repurpose `sendRepoAnalysisPrompt` to inject plain text

**Files:**
- Modify: `electron/repo-view/repoAnalysisManager.ts`

- [ ] **Step 1: Replace the function body**

Open `electron/repo-view/repoAnalysisManager.ts` and replace the existing `sendRepoAnalysisPrompt` function (along with its now-unused imports for `fs`, `join`, `tmpdir`, `randomBytes`, and `buildRepoAnalysisPrompt`) with a plain-text injector.

Remove these imports near the top:

```ts
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { randomBytes } from 'crypto'
import { buildRepoAnalysisPrompt } from './analysisPrompt.js'
```

Replace the function:

```ts
export async function sendRepoAnalysisPrompt(input: {
  winId: number
  text: string
}): Promise<void> {
  const session = sessions.get(input.winId)
  if (!session) throw new Error('repo analysis session not started')
  if (session.command === 'codex') {
    await waitForCodexReady(input.winId, READY_TIMEOUT_MS_CODEX)
  } else if (session.command === 'claude') {
    await sleep(PRIMING_DELAY_MS_CLAUDE)
    await waitForClaudeReady(input.winId, READY_TIMEOUT_MS_CLAUDE)
  }
  await sendMessage(session.proc, input.text)
}
```

- [ ] **Step 2: Verify typecheck passes**

Run: `npm run typecheck`
Expected: PASS — no errors. (If `fs`/`join`/`tmpdir`/`randomBytes` are flagged as unused, remove the import lines that survived.)

- [ ] **Step 3: Commit**

```bash
git add electron/repo-view/repoAnalysisManager.ts
git commit -m "refactor(repo-view): inject plain text instead of building a prompt file"
```

---

### Task 4: Update `repo-view:analysis-send` IPC + preload signature

**Files:**
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Update `electron/main.ts`**

Replace the import block:

```ts
import {
  hasRepoAnalysisSession,
  resizeRepoAnalysisSession,
  sendRepoAnalysisPrompt,
  startRepoAnalysisSession,
  stopRepoAnalysisSession,
  writeRepoAnalysisInput
} from './repo-view/repoAnalysisManager.js'
```

with:

```ts
import {
  hasRepoAnalysisSession,
  resizeRepoAnalysisSession,
  sendRepoAnalysisPrompt,
  startRepoAnalysisSession,
  stopRepoAnalysisSession,
  writeRepoAnalysisInput
} from './repo-view/repoAnalysisManager.js'
import { ensureAnalysisCacheDir } from './repo-view/analysisCache.js'
```

Replace the `repo-view:analysis-send` handler:

```ts
ipcMain.handle(
  'repo-view:analysis-send',
  async (e, req: { repoRoot: string; text: string }) => {
    const win = BrowserWindow.fromWebContents(e.sender)
    if (!win) return { ok: false as const, error: 'window not found' }
    try {
      await ensureAnalysisCacheDir(req.repoRoot)
    } catch (err) {
      console.warn('[repo-view] ensureAnalysisCacheDir failed:', err)
    }
    try {
      await sendRepoAnalysisPrompt({ winId: win.id, text: req.text })
      return { ok: true as const }
    } catch (err) {
      return { ok: false as const, error: (err as Error).message }
    }
  }
)
```

- [ ] **Step 2: Update `electron/preload.ts`**

Replace the existing `analysisSend` definition:

```ts
analysisSend: (req: { repoRoot: string; text: string }) =>
  ipcRenderer.invoke('repo-view:analysis-send', req) as Promise<{
    ok: boolean
    error?: string
  }>,
```

- [ ] **Step 3: Verify typecheck passes**

Run: `npm run typecheck`
Expected: FAIL — `RepoViewerWindow.tsx` still calls the old `analysisSend` shape. That is fixed in Task 5; do not pre-emptively touch it here.

- [ ] **Step 4: Commit (skip the typecheck gate intentionally — the renderer is updated next task in the same series)**

```bash
git add electron/main.ts electron/preload.ts
git commit -m "refactor(repo-view): repo-view:analysis-send takes {repoRoot, text}; ensure cache dir"
```

---

### Task 5: Wire `onSendToCli` in `RepoViewerWindow.tsx`

**Files:**
- Modify: `src/repo-view/RepoViewerWindow.tsx`

- [ ] **Step 1: Replace the imports block**

Open `src/repo-view/RepoViewerWindow.tsx`. Replace the import lines:

```ts
import { type AiSettings } from '../components/AiSettingsDialog'
import AnalysisPanel, { type RepoCodeAnnotation } from './AnalysisPanel'
import CodePane, { type RepoSelection } from './CodePane'
import FileTree from './FileTree'
import RepoTerminalPanel from './RepoTerminalPanel'
import { parseAnalysisOutput } from './parseAnalysisOutput'
import { buildRepoAnnotationMessage } from './repoAnnotationMessage.js'
import {
  createUserMessage,
  syncAssistantMessage,
  type RepoConversationMessage
} from './repoConversation.js'
```

with:

```ts
import { type AiSettings } from '../components/AiSettingsDialog'
import AnalysisPanel, { type RepoCodeAnnotation } from './AnalysisPanel'
import CodePane, { type RepoSelection } from './CodePane'
import FileTree from './FileTree'
import RepoTerminalPanel from './RepoTerminalPanel'
import { buildCliInjectionText } from './buildCliInjectionText'
```

- [ ] **Step 2: Remove the `cleanTerminalChunk` helper and the `RecentTopic` type**

Delete the standalone `cleanTerminalChunk` function and the `type RecentTopic = { … }` line — neither is used after this task.

- [ ] **Step 3: Strip removed state and refs**

Inside the component, delete these state hooks and refs:

```ts
const [analysisPending, setAnalysisPending] = useState(false)
const [analysisMessages, setAnalysisMessages] = useState<RepoConversationMessage[]>([])
const [historyHydrated, setHistoryHydrated] = useState(false)
const [projectSummary, setProjectSummary] = useState('')
const [fileNote, setFileNote] = useState('')
const [recentTopics, setRecentTopics] = useState<RecentTopic[]>([])
const pendingMemoryFileRef = useRef<string | null>(null)
const analysisRawRef = useRef('')
```

Keep `sessionRunning`.

- [ ] **Step 4: Remove the memory/history-load effect entirely**

Delete this whole effect:

```ts
useEffect(() => {
  if (!project) return
  void Promise.all([
    window.api.repoView.memoryLoad(project.target_repo),
    window.api.repoView.historyLoad(project.target_repo)
  ]).then(([memoryRes, historyRes]) => {
    if (memoryRes.ok) {
      setProjectSummary(memoryRes.summary ?? '')
      setRecentTopics((memoryRes.recentTopics ?? []) as RecentTopic[])
    }
    if (historyRes.ok) {
      setAnalysisMessages(historyRes.messages ?? [])
    }
    setHistoryHydrated(true)
  })
}, [project])
```

- [ ] **Step 5: Trim the project-change reset effect**

Replace this effect:

```ts
useEffect(() => {
  setSelectedFile('')
  setSelectedContent('')
  setSelectedSize(0)
  setAnnotations([])
  setEditingAnnotationId(null)
  setAnalysisMessages([])
  setHistoryHydrated(false)
  setProjectSummary('')
  setFileNote('')
  setRecentTopics([])
}, [projectId])
```

with:

```ts
useEffect(() => {
  setSelectedFile('')
  setSelectedContent('')
  setSelectedSize(0)
  setAnnotations([])
  setEditingAnnotationId(null)
}, [projectId])
```

- [ ] **Step 6: Trim the file-load effect**

Replace this effect:

```ts
useEffect(() => {
  if (!project || !selectedFile) return
  let cancelled = false
  setLoadingFile(true)
  void Promise.all([
    window.api.repoView.readFile(project.target_repo, selectedFile),
    window.api.repoView.memoryFileNote(project.target_repo, selectedFile)
  ]).then(([readRes, noteRes]) => {
    if (cancelled) return
    setLoadingFile(false)
    if (!readRes.ok || readRes.content === undefined) {
      setSelectedContent(readRes.error ?? '无法读取文件')
      setSelectedSize(0)
    } else {
      setSelectedContent(readRes.content)
      setSelectedSize(readRes.byteLength ?? 0)
    }
    setFileNote(noteRes.ok ? noteRes.fileNote ?? '' : '')
  })
  return () => {
    cancelled = true
  }
}, [project, selectedFile])
```

with:

```ts
useEffect(() => {
  if (!project || !selectedFile) return
  let cancelled = false
  setLoadingFile(true)
  void window.api.repoView
    .readFile(project.target_repo, selectedFile)
    .then((readRes) => {
      if (cancelled) return
      setLoadingFile(false)
      if (!readRes.ok || readRes.content === undefined) {
        setSelectedContent(readRes.error ?? '无法读取文件')
        setSelectedSize(0)
      } else {
        setSelectedContent(readRes.content)
        setSelectedSize(readRes.byteLength ?? 0)
      }
    })
  return () => {
    cancelled = true
  }
}, [project, selectedFile])
```

- [ ] **Step 7: Replace the analysis-data/status listener effect**

Replace this whole effect:

```ts
useEffect(() => {
  const offData = window.api.repoView.onAnalysisData((evt) => {
    const chunk = cleanTerminalChunk(evt.chunk)
    if (!chunk) return
    analysisRawRef.current = (analysisRawRef.current + chunk).slice(-220000)
    const parsed = parseAnalysisOutput(analysisRawRef.current)
    if (parsed.answer.trim()) {
      setAnalysisMessages((prev) =>
        syncAssistantMessage(prev, parsed.answer, !parsed.complete)
      )
    }
    if (!parsed.complete || !project) return
    const pendingFile = pendingMemoryFileRef.current
    pendingMemoryFileRef.current = null
    setAnalysisPending(false)
    if (!pendingFile || !parsed.memoryUpdate.trim()) return
    void window.api.repoView
      .memoryApply(project.target_repo, pendingFile, parsed.memoryUpdate)
      .then((res) => {
        if (!res.ok) return
        setProjectSummary(res.summary ?? '')
        if (pendingFile === selectedFile) {
          setFileNote(res.fileNote ?? '')
        }
        setRecentTopics((res.recentTopics ?? []) as RecentTopic[])
      })
  })
  const offStatus = window.api.repoView.onAnalysisStatus((evt) => {
    if (evt.status === 'running') {
      setSessionRunning(true)
    } else if (evt.status === 'exited') {
      setSessionRunning(false)
      setAnalysisPending(false)
    }
  })
  return () => {
    offData()
    offStatus()
  }
}, [project, selectedFile])
```

with:

```ts
useEffect(() => {
  const offStatus = window.api.repoView.onAnalysisStatus((evt) => {
    if (evt.status === 'running') {
      setSessionRunning(true)
    } else if (evt.status === 'exited') {
      setSessionRunning(false)
    }
  })
  return () => {
    offStatus()
  }
}, [])
```

- [ ] **Step 8: Delete the history-save effect**

Delete this effect entirely:

```ts
useEffect(() => {
  if (!project || !historyHydrated) return
  const persistable = analysisMessages
    .filter((message) => !message.streaming)
    .map(({ id, role, text }) => ({ id, role, text }))
  void window.api.repoView.historySave(project.target_repo, persistable)
}, [analysisMessages, historyHydrated, project])
```

- [ ] **Step 9: Replace `onSendAnalysis` with `onSendToCli` and make `onStartCli` return ok**

Find the existing `onStartCli` callback and update it to return `boolean`:

```ts
const onStartCli = useCallback(async (): Promise<boolean> => {
  if (!project) return false
  const command = repoViewSettings.command ?? repoViewSettings.ai_cli
  const defaultArgs = command === 'codex' ? ['--full-auto'] : []
  const args = [...defaultArgs, ...(repoViewSettings.args ?? [])]
  const res = await window.api.repoView.analysisStart({
    projectId,
    targetRepo: project.target_repo,
    command,
    args,
    env: repoViewSettings.env ?? {}
  })
  if (res.ok) {
    setSessionRunning(true)
    return true
  }
  return false
}, [project, projectId, repoViewSettings])
```

Then replace the entire `onSendAnalysis` callback:

```ts
const onSendAnalysis = useCallback(
  async (question: string) => {
    if (!project || !selectedFile) return
    const targetAnns = annotations.filter((a) => a.filePath === selectedFile)
    if (targetAnns.length === 0) return
    const command = repoViewSettings.command ?? repoViewSettings.ai_cli
    const defaultArgs = command === 'codex' ? ['--full-auto'] : []
    const args = [...defaultArgs, ...(repoViewSettings.args ?? [])]
    const startRes = await window.api.repoView.analysisStart({
      projectId,
      targetRepo: project.target_repo,
      command,
      args,
      env: repoViewSettings.env ?? {}
    })
    if (!startRes.ok) {
      setAnalysisMessages((prev) =>
        syncAssistantMessage(prev, `分析会话启动失败：${startRes.error ?? '未知错误'}`, false)
      )
      return
    }

    const selection = buildRepoAnnotationMessage({
      filePath: selectedFile,
      question,
      annotations: targetAnns
    })

    pendingMemoryFileRef.current = selectedFile
    analysisRawRef.current = ''
    setAnalysisPending(true)
    setAnalysisMessages((prev) => [
      ...prev,
      createUserMessage({
        filePath: selectedFile,
        annotationCount: targetAnns.length,
        question
      })
    ])
    const sendRes = await window.api.repoView.analysisSend({
      repoRoot: project.target_repo,
      filePath: selectedFile,
      selection,
      question: '',
      projectSummary,
      fileNote
    })
    if (!sendRes.ok) {
      pendingMemoryFileRef.current = null
      setAnalysisPending(false)
      setAnalysisMessages((prev) =>
        syncAssistantMessage(prev, `分析请求发送失败：${sendRes.error ?? '未知错误'}`, false)
      )
    }
  },
  [annotations, fileNote, project, projectId, projectSummary, repoViewSettings, selectedFile]
)
```

with:

```ts
const onSendToCli = useCallback(
  async (question: string) => {
    if (!project || !selectedFile) return
    const targetAnns = annotations.filter((a) => a.filePath === selectedFile)
    if (targetAnns.length === 0) return
    if (!sessionRunning) {
      const ok = await onStartCli()
      if (!ok) return
    }
    const text = buildCliInjectionText({
      repoRoot: project.target_repo,
      filePath: selectedFile,
      annotations: targetAnns,
      question
    })
    const res = await window.api.repoView.analysisSend({
      repoRoot: project.target_repo,
      text
    })
    if (!res.ok) {
      console.warn('[repo-view] analysisSend failed:', res.error)
    }
  },
  [annotations, project, selectedFile, sessionRunning, onStartCli]
)
```

- [ ] **Step 10: Update the `<AnalysisPanel ... />` usage**

Replace the existing JSX:

```tsx
<AnalysisPanel
  projectId={projectId}
  repoRoot={project.target_repo}
  filePath={selectedFile}
  annotations={annotations.filter((a) => a.filePath === selectedFile)}
  aiCli={repoViewSettings.ai_cli}
  running={analysisPending}
  messages={analysisMessages}
  recentTopics={recentTopics}
  onSendAnalysis={onSendAnalysis}
  onEditAnnotation={(id) => setEditingAnnotationId(id)}
  onRemoveAnnotation={(id) => {
    if (editingAnnotationId === id) setEditingAnnotationId(null)
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
  }}
  onClearAnnotations={() => {
    setEditingAnnotationId(null)
    setAnnotations((prev) => prev.filter((a) => a.filePath !== selectedFile))
  }}
/>
```

with:

```tsx
<AnalysisPanel
  filePath={selectedFile}
  annotations={annotations.filter((a) => a.filePath === selectedFile)}
  onSendToCli={onSendToCli}
  onEditAnnotation={(id) => setEditingAnnotationId(id)}
  onRemoveAnnotation={(id) => {
    if (editingAnnotationId === id) setEditingAnnotationId(null)
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
  }}
  onClearAnnotations={() => {
    setEditingAnnotationId(null)
    setAnnotations((prev) => prev.filter((a) => a.filePath !== selectedFile))
  }}
/>
```

- [ ] **Step 11: Verify typecheck passes**

Run: `npm run typecheck`
Expected: FAIL — `AnalysisPanel` still declares the old prop shape; fixed in Task 6.

- [ ] **Step 12: Commit**

```bash
git add src/repo-view/RepoViewerWindow.tsx
git commit -m "refactor(repo-view): replace analysis pipeline with plain-text CLI injection"
```

---

### Task 6: Simplify `AnalysisPanel.tsx`

**Files:**
- Modify: `src/repo-view/AnalysisPanel.tsx`

- [ ] **Step 1: Replace the entire file**

Replace the contents of `src/repo-view/AnalysisPanel.tsx` with:

```tsx
import { useState } from 'react'

export interface RepoCodeAnnotation {
  id: string
  filePath: string
  lineRange: string
  snippet: string
  comment: string
}

export default function AnalysisPanel({
  filePath,
  annotations,
  onSendToCli,
  onEditAnnotation,
  onRemoveAnnotation,
  onClearAnnotations
}: {
  filePath: string
  annotations: RepoCodeAnnotation[]
  onSendToCli: (question: string) => void
  onEditAnnotation: (id: string) => void
  onRemoveAnnotation: (id: string) => void
  onClearAnnotations: () => void
}): JSX.Element {
  const [question, setQuestion] = useState('')
  const canSend = annotations.length > 0

  return (
    <div className="repo-analysis-panel">
      <div className="repo-analysis-head">代码标注</div>
      {!filePath ? (
        <div className="repo-analysis-empty">先从左侧选择一个文件。</div>
      ) : (
        <>
          <div className="repo-analysis-subhead">已标注片段（{annotations.length}）</div>
          {annotations.length === 0 ? (
            <div className="repo-analysis-empty">
              在代码区选中文本后点击“✏ 标注”，即可把片段加入分析队列。
            </div>
          ) : (
            <ul className="repo-analysis-list">
              {annotations.map((a, i) => (
                <li key={a.id} className="repo-analysis-item">
                  <div className="repo-analysis-item-head">
                    <span>#{i + 1}</span>
                    <span>{a.lineRange} 行</span>
                    <div className="repo-analysis-item-actions">
                      <button
                        className="repo-analysis-edit"
                        onClick={() => onEditAnnotation(a.id)}
                      >
                        编辑
                      </button>
                      <button
                        className="repo-analysis-remove"
                        onClick={() => onRemoveAnnotation(a.id)}
                      >
                        删除
                      </button>
                    </div>
                  </div>
                  <blockquote className="repo-analysis-quote">
                    {a.snippet.length > 260 ? `${a.snippet.slice(0, 260)}…` : a.snippet}
                  </blockquote>
                  <div className="repo-analysis-comment">{a.comment}</div>
                </li>
              ))}
            </ul>
          )}
          <label className="repo-analysis-input-label">问题（可选）</label>
          <textarea
            className="repo-analysis-input"
            value={question}
            onChange={(e) => setQuestion(e.target.value)}
            rows={4}
            placeholder="例如：这段代码的主流程、边界条件和潜在风险是什么？"
          />
          <div className="repo-analysis-actions">
            <button
              className="drawer-btn"
              onClick={onClearAnnotations}
              disabled={annotations.length === 0}
            >
              清空标注
            </button>
            <button
              className="drawer-btn primary"
              disabled={!canSend}
              onClick={() => {
                onSendToCli(question.trim())
                setQuestion('')
              }}
              title={canSend ? '注入到下方 AI CLI' : '至少需要一条标注'}
            >
              发送到 AI CLI
            </button>
          </div>
        </>
      )}
    </div>
  )
}
```

- [ ] **Step 2: Verify typecheck passes**

Run: `npm run typecheck`
Expected: PASS — no errors.

- [ ] **Step 3: Commit**

```bash
git add src/repo-view/AnalysisPanel.tsx
git commit -m "refactor(repo-view): drop chat bubbles, recent topics, and analysis-pending state"
```

---

### Task 7: Delete now-unused files and CSS

**Files:**
- Delete: `electron/repo-view/analysisPrompt.ts`
- Delete: `src/repo-view/parseAnalysisOutput.ts`
- Delete: `src/repo-view/parseAnalysisOutput.test.ts`
- Delete: `src/repo-view/repoConversation.ts`
- Delete: `src/repo-view/repoAnnotationMessage.ts`
- Delete: `src/repo-view/repoAnnotationMessage.test.ts`
- Modify: `src/styles.css`

- [ ] **Step 1: Remove the dead modules**

Run:

```bash
rm electron/repo-view/analysisPrompt.ts \
   src/repo-view/parseAnalysisOutput.ts \
   src/repo-view/parseAnalysisOutput.test.ts \
   src/repo-view/repoConversation.ts \
   src/repo-view/repoAnnotationMessage.ts \
   src/repo-view/repoAnnotationMessage.test.ts
```

- [ ] **Step 2: Remove the dead CSS rules**

Open `src/styles.css` and delete every rule whose selector starts with one of:

- `.repo-analysis-chat`
- `.repo-analysis-bubble` (any variant)
- `.repo-analysis-markdown` (any variant)
- `.repo-analysis-recent` (any variant)

These currently sit roughly between lines 3685 and 3768; delete the whole block. Verify with:

```bash
grep -n "repo-analysis-chat\|repo-analysis-bubble\|repo-analysis-markdown\|repo-analysis-recent" src/styles.css
```

Expected: no matches.

- [ ] **Step 3: Verify typecheck + tests pass**

Run: `npm run typecheck && npx vitest run`
Expected: PASS — no missing-module errors, all tests green.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore(repo-view): drop deprecated analysis pipeline modules and CSS"
```

---

### Task 8: Manual smoke test

**Files:** none

- [ ] **Step 1: Start the dev server**

Run: `npm run dev`
Expected: Electron window opens, no main-process errors.

- [ ] **Step 2: Open the repo-view window**

In the app, open a project that points at a real local repo (e.g. obs-studio). Verify the right column shows the simplified annotation panel up top and the AI CLI terminal below — no chat bubbles, no "最近主题" section.

- [ ] **Step 3: First-run injection**

Pick a file, select a few lines, mark them with a comment. Click "发送到 AI CLI". Verify:

- AI CLI session auto-starts (or reuses an existing one)
- The injected text appears in the terminal input box and is submitted
- Response streams in the terminal — not duplicated above
- Inside the target repo, `<repo>/.multi-ai-code/.gitignore` exists and contains `repo-view/analyses/`
- `<repo>/.multi-ai-code/repo-view/analyses/` is created (may be empty until the AI writes to it)

- [ ] **Step 4: Recall pass**

Wait for the AI to finish (it should write the per-file cache file). Mark another snippet in the SAME file and send again. Verify the AI references `.multi-ai-code/repo-view/analyses/<file>.md` (you should see it open the file in the terminal, or quote prior conclusions).

- [ ] **Step 5: Commit notes if anything surprising**

If everything works, no commit. If you found something off, file an issue / note in `docs/superpowers/notes/`.

---

## Self-Review Notes

- **Spec coverage:** every section in the spec maps to a task — buildCliInjectionText (T1), ensureAnalysisCacheDir (T2), backend signature change (T3+T4), frontend state purge (T5), AnalysisPanel simplification (T6), file/CSS cleanup (T7), manual recall test (T8).
- **Memory data layer (b):** `electron/repo-view/memory.ts` and the `memory-load` / `memory-file-note` / `memory-apply` / `history-load` / `history-save` IPC handlers are intentionally untouched — caller sites are removed (T5) but the data layer stays for a future "保存到记忆" UI.
- **Naming:** `onSendToCli` (renderer) → `analysisSend({ repoRoot, text })` (preload) → `repo-view:analysis-send` (IPC) → `sendRepoAnalysisPrompt({ winId, text })` (backend). One name per layer; matched.
- **Type discipline:** `RepoCodeAnnotation` is now declared in AnalysisPanel.tsx (after T6) and re-exported through its named export, matching the existing import in RepoViewerWindow.tsx and the new import in buildCliInjectionText.ts.
- **Tooling:** vitest config already picks up `**/*.test.ts` under both `src/` and `electron/` (existing tests `electron/cc/codexTrust.test.ts`, `src/repo-view/repoAnnotationMessage.test.ts` confirm). `crypto` is a Node built-in available in the renderer through electron's bundling.
