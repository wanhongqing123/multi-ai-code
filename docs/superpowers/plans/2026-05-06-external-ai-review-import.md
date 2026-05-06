# External AI Review Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the Diff review dialog import external AI review notes as structured suggestion items, ask AICLI to judge each item with a structured decision and reason, and stop writing `CLAUDE.md` / `AGENT*.md` into the target repository.

**Architecture:** Keep the feature split into three seams: a renderer-side parsing layer for imported review text, an Electron-side structured-reply bridge that can ask the running CLI session for a tagged JSON answer, and a prompt-injection helper that redirects Claude system prompt files into platform-owned runtime paths. `DiffViewerDialog` owns imported suggestion state because it already owns the parsed diff file list, while `App.tsx` only owns the session-aware judge callback because it already owns `sessionId` and plan-path resolution.

**Tech Stack:** React 18, Electron IPC, TypeScript, Vitest, existing `window.api` preload bridge, existing `PtyCCProcess` session manager.

---

## File Structure

- Create: `src/components/externalAiReview.ts`
  - Pure parser + matcher for imported review suggestions.
- Create: `src/components/externalAiReview.test.ts`
  - Unit tests for splitting, path hints, line hints, and diff-file matching.
- Create: `src/components/ExternalAiReviewPanel.tsx`
  - Read/write UI for import, list rendering, single-item judge, and batch judge.
- Create: `src/components/ExternalAiReviewPanel.test.tsx`
  - Render and interaction coverage for suggestion list states.
- Modify: `src/components/DiffViewerDialog.tsx`
  - Mount the new panel and thread callbacks/state through the dialog.
- Modify: `src/components/DiffViewerDialog.test.tsx`
  - Verify panel presence and integration in the diff dialog.
- Modify: `src/App.tsx`
  - Implement the session-aware judge callback and pass it into the dialog.
- Create: `electron/cc/structuredReply.ts`
  - Build tagged prompts and parse tagged JSON replies from streamed terminal output.
- Create: `electron/cc/structuredReply.test.ts`
  - Unit tests for prompt contract and tagged JSON extraction.
- Modify: `electron/cc/ptyManager.ts`
  - Add a new structured-judge IPC path and session-local reply collector.
- Modify: `electron/preload.ts`
  - Expose the new `cc` API method to the renderer.
- Create: `electron/cc/systemPromptInjection.ts`
  - Pure helper for deciding where prompt files live and what bootstrap text to send.
- Create: `electron/cc/systemPromptInjection.test.ts`
  - Unit tests proving Claude no longer targets repo-root `CLAUDE.md`.
- Modify: `src/types/global.d.ts`
  - Only if the preload type export stops flowing automatically after the new API method is added.

## Task 1: Parse Imported AI Review Text

**Files:**
- Create: `src/components/externalAiReview.ts`
- Create: `src/components/externalAiReview.test.ts`

- [ ] **Step 1: Write the failing parser tests**

```ts
import { describe, expect, it } from 'vitest'
import {
  matchSuggestionsToDiffFiles,
  parseExternalReviewSuggestions
} from './externalAiReview.js'

describe('parseExternalReviewSuggestions', () => {
  it('splits markdown bullets into separate suggestions', () => {
    const suggestions = parseExternalReviewSuggestions(
      [
        '# Review',
        '- src/App.tsx line 42: rename this state',
        '- electron/main.ts L120-L128: add error handling'
      ].join('\n'),
      'review.md'
    )

    expect(suggestions).toHaveLength(2)
    expect(suggestions[0]).toMatchObject({
      sourceLabel: 'review.md',
      rawText: 'src/App.tsx line 42: rename this state',
      pathHint: 'src/App.tsx',
      lineHint: '42'
    })
    expect(suggestions[1]).toMatchObject({
      pathHint: 'electron/main.ts',
      lineHint: '120-128'
    })
  })

  it('falls back to blank-line paragraph splitting', () => {
    const suggestions = parseExternalReviewSuggestions(
      [
        'Consider extracting the dialog state.',
        '',
        'Code path: src/components/DiffViewerDialog.tsx'
      ].join('\n'),
      'notes.txt'
    )

    expect(suggestions).toHaveLength(2)
  })

  it('matches a path hint to the visible diff file list', () => {
    const parsed = parseExternalReviewSuggestions(
      '- docs/guide.md line 3: heading level looks wrong',
      'review.md'
    )

    const matched = matchSuggestionsToDiffFiles(parsed, [
      'src/App.tsx',
      'docs/guide.md'
    ])

    expect(matched[0]?.linkedDiffFile).toBe('docs/guide.md')
  })
})
```

- [ ] **Step 2: Run the parser tests to confirm failure**

Run: `npm.cmd test -- src/components/externalAiReview.test.ts`

Expected: FAIL with module-not-found errors for `externalAiReview.ts` exports.

- [ ] **Step 3: Implement the minimal parser and matcher**

```ts
export interface ExternalReviewSuggestion {
  id: string
  sourceLabel: string
  rawText: string
  pathHint: string | null
  lineHint: string | null
  linkedDiffFile: string | null
  status: 'idle' | 'accepted' | 'rejected' | 'needs-human' | 'error'
  decisionReason: string
}

const BULLET_RE = /^(?:[-*]\s+|\d+\.\s+)/
const PATH_RE = /(?:^|[\s`(])([A-Za-z0-9._/-]+\.[A-Za-z0-9]+)(?=$|[\s`):,])/
const LINE_RE = /\b(?:L|line\s+)?(\d+)(?:\s*[-–]\s*(\d+))?\b/i

export function parseExternalReviewSuggestions(
  text: string,
  sourceLabel: string
): ExternalReviewSuggestion[] {
  const normalized = text.replace(/\r\n/g, '\n').trim()
  if (!normalized) return []

  const blocks = normalized
    .split(/\n(?=(?:[-*]\s+|\d+\.\s+))/)
    .flatMap((block) =>
      BULLET_RE.test(block.trimStart())
        ? [block.trim().replace(BULLET_RE, '')]
        : block.split(/\n\s*\n/)
    )
    .map((block) => block.trim())
    .filter((block) => block.length > 0 && !/^#{1,6}\s+\S+$/.test(block))

  return blocks.map((rawText, index) => {
    const pathMatch = rawText.match(PATH_RE)
    const lineMatch = rawText.match(LINE_RE)
    const start = lineMatch?.[1] ?? null
    const end = lineMatch?.[2] ?? null

    return {
      id: `ext_review_${index + 1}`,
      sourceLabel,
      rawText,
      pathHint: pathMatch?.[1] ?? null,
      lineHint: start ? (end ? `${start}-${end}` : start) : null,
      linkedDiffFile: null,
      status: 'idle',
      decisionReason: ''
    }
  })
}

export function matchSuggestionsToDiffFiles(
  suggestions: ExternalReviewSuggestion[],
  diffFiles: string[]
): ExternalReviewSuggestion[] {
  return suggestions.map((suggestion) => {
    if (!suggestion.pathHint) return suggestion
    const match = diffFiles.find(
      (file) =>
        file === suggestion.pathHint ||
        file.endsWith(`/${suggestion.pathHint}`) ||
        file.endsWith(`\\${suggestion.pathHint}`)
    )
    return { ...suggestion, linkedDiffFile: match ?? null }
  })
}
```

- [ ] **Step 4: Re-run the parser tests**

Run: `npm.cmd test -- src/components/externalAiReview.test.ts`

Expected: PASS

- [ ] **Step 5: Commit the parser layer**

```bash
git add src/components/externalAiReview.ts src/components/externalAiReview.test.ts
git commit -m "feat: parse imported external ai review suggestions"
```

## Task 2: Add a Structured AICLI Judgement Bridge

**Files:**
- Create: `electron/cc/structuredReply.ts`
- Create: `electron/cc/structuredReply.test.ts`
- Modify: `electron/cc/ptyManager.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write failing structured-reply tests**

```ts
import { describe, expect, it } from 'vitest'
import {
  buildExternalReviewDecisionPrompt,
  extractTaggedJsonReply
} from './structuredReply.js'

describe('buildExternalReviewDecisionPrompt', () => {
  it('requests a tagged json decision payload', () => {
    const prompt = buildExternalReviewDecisionPrompt({
      planAbsPath: 'E:/repo/.multi-ai-code/designs/spec.md',
      suggestion: {
        rawText: 'src/App.tsx line 42: rename this state',
        pathHint: 'src/App.tsx',
        lineHint: '42',
        linkedDiffFile: 'src/App.tsx'
      }
    })

    expect(prompt).toContain('accepted')
    expect(prompt).toContain('rejected')
    expect(prompt).toContain('needs-human')
    expect(prompt).toContain('MAC_EXTERNAL_REVIEW_JSON_START')
    expect(prompt).toContain('MAC_EXTERNAL_REVIEW_JSON_END')
  })
})

describe('extractTaggedJsonReply', () => {
  it('parses a tagged decision block from terminal output', () => {
    const reply = extractTaggedJsonReply([
      'Some preamble',
      'MAC_EXTERNAL_REVIEW_JSON_START',
      '{"decision":"accepted","reason":"The rename reduces ambiguity."}',
      'MAC_EXTERNAL_REVIEW_JSON_END'
    ].join('\n'))

    expect(reply).toEqual({
      decision: 'accepted',
      reason: 'The rename reduces ambiguity.'
    })
  })
})
```

- [ ] **Step 2: Run the structured-reply tests to confirm failure**

Run: `npm.cmd test -- electron/cc/structuredReply.test.ts`

Expected: FAIL with module-not-found errors for `structuredReply.ts`.

- [ ] **Step 3: Implement tagged-prompt helpers and the IPC bridge**

```ts
export interface ExternalReviewDecision {
  decision: 'accepted' | 'rejected' | 'needs-human'
  reason: string
}

const START = 'MAC_EXTERNAL_REVIEW_JSON_START'
const END = 'MAC_EXTERNAL_REVIEW_JSON_END'

export function buildExternalReviewDecisionPrompt(input: {
  planAbsPath: string
  suggestion: {
    rawText: string
    pathHint: string | null
    lineHint: string | null
    linkedDiffFile: string | null
  }
}): string {
  return [
    `请判断下面这条外部 AI review 建议是否应该采纳。`,
    `只允许输出 ${START} 和 ${END} 包裹的单个 JSON 对象。`,
    `JSON 结构: {"decision":"accepted|rejected|needs-human","reason":"..."}`,
    `方案文件: ${input.planAbsPath}`,
    `建议原文: ${input.suggestion.rawText}`,
    `文件线索: ${input.suggestion.pathHint ?? 'none'}`,
    `行号线索: ${input.suggestion.lineHint ?? 'none'}`,
    `已匹配 diff 文件: ${input.suggestion.linkedDiffFile ?? 'none'}`,
    '',
    START,
    '{"decision":"accepted","reason":"example"}',
    END
  ].join('\n')
}

export function extractTaggedJsonReply(
  output: string
): ExternalReviewDecision | null {
  const start = output.indexOf(START)
  const end = output.indexOf(END)
  if (start < 0 || end < 0 || end <= start) return null
  const raw = output.slice(start + START.length, end).trim()
  const parsed = JSON.parse(raw) as ExternalReviewDecision
  if (!['accepted', 'rejected', 'needs-human'].includes(parsed.decision)) {
    throw new Error('invalid decision')
  }
  if (!parsed.reason?.trim()) {
    throw new Error('missing reason')
  }
  return parsed
}
```

```ts
// electron/cc/ptyManager.ts
interface PendingStructuredReply {
  buffer: string
  resolve: (value: ExternalReviewDecision) => void
  reject: (error: Error) => void
  timeout: NodeJS.Timeout
}

interface Session {
  // existing fields ...
  pendingStructuredReply?: PendingStructuredReply
}

async function collectStructuredReply(
  session: Session,
  prompt: string
): Promise<ExternalReviewDecision> {
  if (session.pendingStructuredReply) {
    throw new Error('structured reply already in progress')
  }

  return await new Promise<ExternalReviewDecision>(async (resolve, reject) => {
    const timeout = setTimeout(() => {
      session.pendingStructuredReply = undefined
      reject(new Error('timed out waiting for structured reply'))
    }, 20000)

    session.pendingStructuredReply = {
      buffer: '',
      resolve: (value) => {
        clearTimeout(timeout)
        session.pendingStructuredReply = undefined
        resolve(value)
      },
      reject: (error) => {
        clearTimeout(timeout)
        session.pendingStructuredReply = undefined
        reject(error)
      },
      timeout
    }

    await sendMessage(session.proc, prompt)
  })
}

proc.on('data', (chunk: string) => {
  if (session.pendingStructuredReply) {
    session.pendingStructuredReply.buffer += chunk
    try {
      const parsed = extractTaggedJsonReply(session.pendingStructuredReply.buffer)
      if (parsed) {
        session.pendingStructuredReply.resolve(parsed)
      }
    } catch (error) {
      session.pendingStructuredReply.reject(error as Error)
    }
  }
  // keep the existing broadcast path unchanged
})

```

```ts
// electron/cc/ptyManager.ts
ipcMain.handle(
  'cc:judge-external-review',
  async (
    _e,
    req: {
      sessionId: string
      planAbsPath: string
      suggestion: {
        rawText: string
        pathHint: string | null
        lineHint: string | null
        linkedDiffFile: string | null
      }
    }
  ) => {
    const session = sessions.get(req.sessionId)
    if (!session) return { ok: false as const, error: 'no session' }

    const prompt = buildExternalReviewDecisionPrompt(req)
    const result = await collectStructuredReply(session, prompt)
    return { ok: true as const, result }
  }
)
```

```ts
// electron/preload.ts
judgeExternalReview: (req: {
  sessionId: string
  planAbsPath: string
  suggestion: {
    rawText: string
    pathHint: string | null
    lineHint: string | null
    linkedDiffFile: string | null
  }
}) =>
  ipcRenderer.invoke('cc:judge-external-review', req) as Promise<
    | { ok: true; result: { decision: 'accepted' | 'rejected' | 'needs-human'; reason: string } }
    | { ok: false; error: string }
  >
```

- [ ] **Step 4: Re-run the structured-reply tests**

Run: `npm.cmd test -- electron/cc/structuredReply.test.ts`

Expected: PASS

- [ ] **Step 5: Commit the judgement bridge**

```bash
git add electron/cc/structuredReply.ts electron/cc/structuredReply.test.ts electron/cc/ptyManager.ts electron/preload.ts
git commit -m "feat: add structured external review judgement bridge"
```

## Task 3: Render the External Review Panel in Diff Review

**Files:**
- Create: `src/components/ExternalAiReviewPanel.tsx`
- Create: `src/components/ExternalAiReviewPanel.test.tsx`
- Modify: `src/components/DiffViewerDialog.tsx`
- Modify: `src/components/DiffViewerDialog.test.tsx`
- Modify: `src/App.tsx`

- [ ] **Step 1: Write failing UI tests**

```tsx
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import ExternalAiReviewPanel from './ExternalAiReviewPanel.js'

describe('ExternalAiReviewPanel', () => {
  it('renders imported suggestions with status and action buttons', () => {
    const markup = renderToStaticMarkup(
      <ExternalAiReviewPanel
        sourceLabel="review.md"
        suggestions={[
          {
            id: 's1',
            sourceLabel: 'review.md',
            rawText: 'src/App.tsx line 42: rename this state',
            pathHint: 'src/App.tsx',
            lineHint: '42',
            linkedDiffFile: 'src/App.tsx',
            status: 'accepted',
            decisionReason: 'The state name is ambiguous.'
          }
        ]}
        busy={false}
        onImport={vi.fn()}
        onJudgeOne={vi.fn()}
        onJudgeAll={vi.fn()}
      />
    )

    expect(markup).toContain('外部 AI 建议')
    expect(markup).toContain('review.md')
    expect(markup).toContain('accepted')
    expect(markup).toContain('The state name is ambiguous.')
  })
})
```

```tsx
it('renders the external review panel inside DiffViewerDialog', () => {
  const markup = renderToStaticMarkup(
    <DiffViewerDialog
      cwd="E:/OpenSource/multi-ai-code"
      onClose={vi.fn()}
      onSubmit={vi.fn()}
      sessionRunning={true}
      annotations={[]}
      onAnnotationsChange={vi.fn()}
      generalNote=""
      onGeneralNoteChange={vi.fn()}
      mode="working"
      onModeChange={vi.fn()}
      selectedCommit=""
      onSelectedCommitChange={vi.fn()}
      selectedFile=""
      onSelectedFileChange={vi.fn()}
      onJudgeExternalReviewItem={vi.fn()}
    />
  )

  expect(markup).toContain('外部 AI 建议')
})
```

- [ ] **Step 2: Run the UI tests to confirm failure**

Run: `npm.cmd test -- src/components/ExternalAiReviewPanel.test.tsx src/components/DiffViewerDialog.test.tsx`

Expected: FAIL because the panel component and new props do not exist yet.

- [ ] **Step 3: Implement the panel and wire it through the dialog/app**

```ts
// src/App.tsx
const judgeExternalReviewItem = useCallback(async (suggestion: {
  rawText: string
  pathHint: string | null
  lineHint: string | null
  linkedDiffFile: string | null
}) => {
  if (!sessionId || sessionStatus !== 'running') {
    return { ok: false as const, error: 'session not running' }
  }
  return await window.api.cc.judgeExternalReview({
    sessionId,
    planAbsPath: getPlanAbsPath(planName.trim()),
    suggestion
  })
}, [sessionId, sessionStatus, planName, getPlanAbsPath])
```

```ts
// src/components/DiffViewerDialog.tsx
export interface DiffViewerDialogProps {
  // existing props ...
  onJudgeExternalReviewItem: (suggestion: {
    rawText: string
    pathHint: string | null
    lineHint: string | null
    linkedDiffFile: string | null
  }) => Promise<
    | { ok: true; result: { decision: 'accepted' | 'rejected' | 'needs-human'; reason: string } }
    | { ok: false; error: string }
  >
}

const [externalReviewSourceLabel, setExternalReviewSourceLabel] = useState('')
const [externalReviewSuggestions, setExternalReviewSuggestions] = useState<ExternalReviewSuggestion[]>([])
const [externalReviewBusy, setExternalReviewBusy] = useState(false)

const importExternalReview = useCallback(async () => {
  const pick = await window.api.dialog.pickTextFile({ title: '选择外部 AI review 文件' })
  if (pick.canceled || !pick.path || pick.content === undefined) return
  const parsed = matchSuggestionsToDiffFiles(
    parseExternalReviewSuggestions(
      pick.content,
      pick.path.split(/[\\/]/).pop() ?? pick.path
    ),
    files.map((file) => file.path)
  )
  setExternalReviewSourceLabel(pick.path)
  setExternalReviewSuggestions(parsed)
}, [files])

const judgeOneExternalReview = useCallback(async (id: string) => {
  const target = externalReviewSuggestions.find((item) => item.id === id)
  if (!target) return
  setExternalReviewBusy(true)
  try {
    const res = await onJudgeExternalReviewItem(target)
    setExternalReviewSuggestions((prev) =>
      prev.map((item) =>
        item.id === id && res.ok
          ? {
              ...item,
              status: res.result.decision,
              decisionReason: res.result.reason
            }
          : item.id === id
            ? { ...item, status: 'error', decisionReason: res.error ?? '判断失败' }
            : item
      )
    )
  } finally {
    setExternalReviewBusy(false)
  }
}, [externalReviewSuggestions, onJudgeExternalReviewItem])

const judgeAllExternalReviews = useCallback(async () => {
  for (const item of externalReviewSuggestions) {
    await judgeOneExternalReview(item.id)
  }
}, [externalReviewSuggestions, judgeOneExternalReview])
```

```tsx
// src/components/ExternalAiReviewPanel.tsx
export default function ExternalAiReviewPanel(props: {
  sourceLabel: string
  suggestions: ExternalReviewSuggestion[]
  busy: boolean
  onImport: () => void
  onJudgeOne: (id: string) => void
  onJudgeAll: () => void
}) {
  return (
    <section className="dv-external-review">
      <div className="dv-external-review-head">
        <h3>外部 AI 建议</h3>
        <button onClick={props.onImport}>导入外部 AI review</button>
        <button onClick={props.onJudgeAll} disabled={props.busy || props.suggestions.length === 0}>
          全部交给 AICLI 判断
        </button>
      </div>
      <div className="dv-external-review-source">{props.sourceLabel || '尚未导入'}</div>
      {props.suggestions.map((suggestion) => (
        <article key={suggestion.id} className={`dv-external-review-item status-${suggestion.status}`}>
          <div className="dv-external-review-text">{suggestion.rawText}</div>
          <div className="dv-external-review-meta">
            <span>{suggestion.linkedDiffFile ?? suggestion.pathHint ?? '未定位到文件'}</span>
            <span>{suggestion.lineHint ?? '无行号'}</span>
          </div>
          <button onClick={() => props.onJudgeOne(suggestion.id)} disabled={props.busy}>
            发送给 AICLI 判断
          </button>
          {suggestion.decisionReason && (
            <p className="dv-external-review-reason">{suggestion.decisionReason}</p>
          )}
        </article>
      ))}
    </section>
  )
}
```

- [ ] **Step 4: Re-run the UI tests**

Run: `npm.cmd test -- src/components/externalAiReview.test.ts src/components/ExternalAiReviewPanel.test.tsx src/components/DiffViewerDialog.test.tsx`

Expected: PASS

- [ ] **Step 5: Commit the Diff review UI integration**

```bash
git add src/App.tsx src/components/ExternalAiReviewPanel.tsx src/components/ExternalAiReviewPanel.test.tsx src/components/DiffViewerDialog.tsx src/components/DiffViewerDialog.test.tsx src/components/externalAiReview.ts src/components/externalAiReview.test.ts
git commit -m "feat: import and judge external ai review items in diff review"
```

## Task 4: Stop Writing AI Markdown Files into the Repo Root

**Files:**
- Create: `electron/cc/systemPromptInjection.ts`
- Create: `electron/cc/systemPromptInjection.test.ts`
- Modify: `electron/cc/ptyManager.ts`

- [ ] **Step 1: Write failing prompt-injection tests**

```ts
import { describe, expect, it } from 'vitest'
import { planSystemPromptInjection } from './systemPromptInjection.js'

describe('planSystemPromptInjection', () => {
  it('places claude injection under .injections instead of repo-root CLAUDE.md', () => {
    const plan = planSystemPromptInjection({
      command: 'claude',
      cwd: 'E:/repo',
      systemPrompt: 'system prompt',
      initialUserMessage: 'hello'
    })

    expect(plan.writePath).toBe('E:/repo/.injections/claude-system.md')
    expect(plan.bootstrapMessage).toContain('.injections/claude-system.md')
    expect(plan.writePath.endsWith('CLAUDE.md')).toBe(false)
  })

  it('never targets AGENT.md or AGENTS.md', () => {
    const plan = planSystemPromptInjection({
      command: 'claude',
      cwd: 'E:/repo',
      systemPrompt: 'system prompt',
      initialUserMessage: 'hello'
    })

    expect(plan.writePath.endsWith('AGENT.md')).toBe(false)
    expect(plan.writePath.endsWith('AGENTS.md')).toBe(false)
  })
})
```

- [ ] **Step 2: Run the prompt-injection tests to confirm failure**

Run: `npm.cmd test -- electron/cc/systemPromptInjection.test.ts`

Expected: FAIL with module-not-found errors for `systemPromptInjection.ts`.

- [ ] **Step 3: Implement the injection planner and refactor ptyManager**

```ts
import { join } from 'path'

export function planSystemPromptInjection(input: {
  command: 'claude' | 'codex'
  cwd: string
  systemPrompt: string
  initialUserMessage: string
}): {
  writeDir: string
  writePath: string
  fileContents: string
  bootstrapMessage: string
} {
  const writeDir = join(input.cwd, '.injections')
  const fileName =
    input.command === 'claude' ? 'claude-system.md' : 'codex-system.md'
  const writePath = join(writeDir, fileName)

  return {
    writeDir,
    writePath,
    fileContents: input.systemPrompt,
    bootstrapMessage: [
      `请先完整读取 ${writePath} 作为本次任务的系统角色与约束说明，逐字遵守后再开始工作。`,
      '',
      input.initialUserMessage
    ].join('\n')
  }
}
```

```ts
// electron/cc/ptyManager.ts
const injection = planSystemPromptInjection({
  command: req.command as 'claude' | 'codex',
  cwd: finalCwd,
  systemPrompt: sysPrompt,
  initialUserMessage: req.initialUserMessage
})

await fs.mkdir(injection.writeDir, { recursive: true })
await fs.writeFile(injection.writePath, injection.fileContents, 'utf8')
await sendMessage(proc, injection.bootstrapMessage)
```

- [ ] **Step 4: Re-run the prompt-injection tests**

Run: `npm.cmd test -- electron/cc/systemPromptInjection.test.ts`

Expected: PASS

- [ ] **Step 5: Commit the injection rewrite**

```bash
git add electron/cc/systemPromptInjection.ts electron/cc/systemPromptInjection.test.ts electron/cc/ptyManager.ts
git commit -m "refactor: keep ai system prompt files out of repo root"
```

## Task 5: Verification and Final Cleanup

**Files:**
- Modify: any touched files above
- Verify: `package.json` scripts only

- [ ] **Step 1: Run the focused test suite**

Run: `npm.cmd test -- src/components/externalAiReview.test.ts src/components/ExternalAiReviewPanel.test.tsx src/components/DiffViewerDialog.test.tsx electron/cc/structuredReply.test.ts electron/cc/systemPromptInjection.test.ts`

Expected: PASS

- [ ] **Step 2: Run type checking**

Run: `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 3: Run the full test suite**

Run: `npm.cmd test`

Expected: PASS

- [ ] **Step 4: Review the final diff**

Run: `git diff --stat main...HEAD`

Expected: only the planned UI, IPC, parser, and injection files changed.

- [ ] **Step 5: Commit the verification pass**

```bash
git add src/App.tsx src/components/DiffViewerDialog.tsx src/components/DiffViewerDialog.test.tsx src/components/ExternalAiReviewPanel.tsx src/components/ExternalAiReviewPanel.test.tsx src/components/externalAiReview.ts src/components/externalAiReview.test.ts electron/cc/structuredReply.ts electron/cc/structuredReply.test.ts electron/cc/systemPromptInjection.ts electron/cc/systemPromptInjection.test.ts electron/cc/ptyManager.ts electron/preload.ts
git commit -m "test: verify external ai review import workflow"
```

## Self-Review

- Spec coverage:
  - External review import and rule-based splitting: Task 1
  - Per-item and batch judgement with structured result: Tasks 2 and 3
  - Diff dialog integration: Task 3
  - Repository AI markdown protection: Task 4
  - Verification and regression safety: Task 5
- Placeholder scan:
  - No `TODO`, `TBD`, or “handle appropriately” placeholders remain.
- Type consistency:
  - Renderer suggestion status uses `accepted | rejected | needs-human | error | idle`.
  - Electron structured reply returns `decision` and `reason`, which map directly to the renderer status and explanation fields.

## Notes

- Current isolated worktree baseline does not have a runnable local `vitest` binary yet, so execution must either reuse an already-prepared dependency tree or install dependencies before running verification commands.
- Do not introduce any fallback that writes `CLAUDE.md`, `AGENT.md`, or `AGENTS.md` into the repository root, even when those files do not exist.
