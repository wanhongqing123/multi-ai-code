# Repo View Quick Annotation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refine the repo viewer's code pane and annotation tray so quick annotation feels lighter, clearer, and more connected without changing the overall product style.

**Architecture:** Keep the existing three-column repo viewer and current annotation composer flow. Add a small pure helper to manage active/recent annotation UI state, wire that helper into `RepoViewerWindow`, and use lightweight presentational updates in `CodePane` and `AnalysisPanel` to create the visual association between code selection and the current annotation card. Keep all send-to-CLI behavior and file-scoped annotation ownership intact.

**Tech Stack:** TypeScript, React 18, Electron renderer, Vitest, `react-dom/server` for static render tests.

---

## File Structure

**Create:**
- `src/repo-view/annotationVisualState.ts` - pure state helpers for active/recent annotation highlighting
- `src/repo-view/annotationVisualState.test.ts` - Vitest coverage for add/edit/remove/clear transitions
- `src/repo-view/AnalysisPanel.test.tsx` - static-render tests for tray states and card classes
- `src/repo-view/CodePane.test.tsx` - static-render tests for file header and linked-line styling

**Modify:**
- `src/repo-view/RepoViewerWindow.tsx` - add `activeAnnotationId` and `recentlyAddedAnnotationId`, wire helper, preserve current send behavior
- `src/repo-view/AnalysisPanel.tsx` - render tray-style cards, active/recent states, clearer empty states, stronger send summary
- `src/repo-view/CodePane.tsx` - upgrade file context header, lighter annotate button, linked line-range styling for the active annotation
- `src/repo-view/analysisPanelState.ts` - update send button tooltip text to match the tray language
- `src/repo-view/analysisPanelState.test.ts` - refresh expectations for the updated send text
- `src/styles.css` - add repo quick-annotation styling for the code pane and annotation tray

---

### Task 1: Annotation Visual State Helper

**Files:**
- Create: `src/repo-view/annotationVisualState.ts`
- Test: `src/repo-view/annotationVisualState.test.ts`

- [ ] **Step 1: Write the failing test**

Create `src/repo-view/annotationVisualState.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import {
  clearAnnotationVisualState,
  removeAnnotationVisualState,
  startEditingAnnotationState,
  trackNewAnnotationState,
  type AnnotationVisualState
} from './annotationVisualState'

describe('trackNewAnnotationState', () => {
  it('focuses the new annotation and marks it as recently added', () => {
    expect(trackNewAnnotationState('ann_1')).toEqual({
      activeAnnotationId: 'ann_1',
      recentlyAddedAnnotationId: 'ann_1'
    })
  })
})

describe('startEditingAnnotationState', () => {
  it('moves focus to the edited annotation and clears the recent marker', () => {
    const prev: AnnotationVisualState = {
      activeAnnotationId: 'ann_1',
      recentlyAddedAnnotationId: 'ann_1'
    }
    expect(startEditingAnnotationState(prev, 'ann_2')).toEqual({
      activeAnnotationId: 'ann_2',
      recentlyAddedAnnotationId: null
    })
  })
})

describe('removeAnnotationVisualState', () => {
  it('clears the active and recent ids when the removed annotation matches both', () => {
    const prev: AnnotationVisualState = {
      activeAnnotationId: 'ann_2',
      recentlyAddedAnnotationId: 'ann_2'
    }
    expect(removeAnnotationVisualState(prev, 'ann_2')).toEqual({
      activeAnnotationId: null,
      recentlyAddedAnnotationId: null
    })
  })

  it('keeps unrelated state intact when a different annotation is removed', () => {
    const prev: AnnotationVisualState = {
      activeAnnotationId: 'ann_2',
      recentlyAddedAnnotationId: 'ann_3'
    }
    expect(removeAnnotationVisualState(prev, 'ann_1')).toEqual(prev)
  })
})

describe('clearAnnotationVisualState', () => {
  it('returns the idle state', () => {
    expect(clearAnnotationVisualState()).toEqual({
      activeAnnotationId: null,
      recentlyAddedAnnotationId: null
    })
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run src/repo-view/annotationVisualState.test.ts`

Expected: FAIL with module-not-found for `./annotationVisualState`.

- [ ] **Step 3: Write minimal implementation**

Create `src/repo-view/annotationVisualState.ts`:

```ts
export interface AnnotationVisualState {
  activeAnnotationId: string | null
  recentlyAddedAnnotationId: string | null
}

export function clearAnnotationVisualState(): AnnotationVisualState {
  return {
    activeAnnotationId: null,
    recentlyAddedAnnotationId: null
  }
}

export function trackNewAnnotationState(
  annotationId: string
): AnnotationVisualState {
  return {
    activeAnnotationId: annotationId,
    recentlyAddedAnnotationId: annotationId
  }
}

export function startEditingAnnotationState(
  _prev: AnnotationVisualState,
  annotationId: string
): AnnotationVisualState {
  return {
    activeAnnotationId: annotationId,
    recentlyAddedAnnotationId: null
  }
}

export function removeAnnotationVisualState(
  prev: AnnotationVisualState,
  annotationId: string
): AnnotationVisualState {
  return {
    activeAnnotationId:
      prev.activeAnnotationId === annotationId ? null : prev.activeAnnotationId,
    recentlyAddedAnnotationId:
      prev.recentlyAddedAnnotationId === annotationId
        ? null
        : prev.recentlyAddedAnnotationId
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run src/repo-view/annotationVisualState.test.ts`

Expected: PASS with 4 passing tests.

- [ ] **Step 5: Commit**

```bash
git add src/repo-view/annotationVisualState.ts src/repo-view/annotationVisualState.test.ts
git commit -m "test: add repo annotation visual state helper"
```

### Task 2: Analysis Panel as an Annotation Tray

**Files:**
- Create: `src/repo-view/AnalysisPanel.test.tsx`
- Modify: `src/repo-view/AnalysisPanel.tsx`
- Modify: `src/repo-view/analysisPanelState.ts`
- Modify: `src/repo-view/analysisPanelState.test.ts`

- [ ] **Step 1: Write the failing tests**

Create `src/repo-view/AnalysisPanel.test.tsx`:

```tsx
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import AnalysisPanel from './AnalysisPanel'

const baseProps = {
  filePath: 'src/repo-view/CodePane.tsx',
  annotations: [
    {
      id: 'ann_1',
      filePath: 'src/repo-view/CodePane.tsx',
      lineRange: '79-81',
      snippet: 'const fallback = lineFromNode(range.startContainer)',
      comment: 'Consider extracting the fallback branch.'
    }
  ],
  sessionRunning: true,
  sending: false,
  activeAnnotationId: null,
  recentlyAddedAnnotationId: null,
  onSendToCli: vi.fn(),
  onEditAnnotation: vi.fn(),
  onRemoveAnnotation: vi.fn(),
  onClearAnnotations: vi.fn()
}

describe('AnalysisPanel', () => {
  it('shows a file-first empty state when no file is selected', () => {
    const html = renderToStaticMarkup(
      <AnalysisPanel {...baseProps} filePath="" annotations={[]} />
    )
    expect(html).toContain('先从左侧选择一个文件')
  })

  it('renders tray cards with active and recent classes', () => {
    const html = renderToStaticMarkup(
      <AnalysisPanel
        {...baseProps}
        activeAnnotationId="ann_1"
        recentlyAddedAnnotationId="ann_1"
      />
    )
    expect(html).toContain('repo-analysis-item active recent')
    expect(html).toContain('当前文件待发送批注')
    expect(html).toContain('发送当前文件批注到 AI CLI')
  })

  it('shows the add-annotation empty state when the file has no cards', () => {
    const html = renderToStaticMarkup(
      <AnalysisPanel {...baseProps} annotations={[]} />
    )
    expect(html).toContain('在代码区选中文本后点击')
    expect(html).toContain('加入待发送批注托盘')
  })
})
```

Update `src/repo-view/analysisPanelState.test.ts` expectations:

```ts
it('uses the tray send hint when sending is allowed', () => {
  expect(repoSendButtonTitle(true, 2)).toBe('发送当前文件批注到 AI CLI')
})
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run src/repo-view/AnalysisPanel.test.tsx src/repo-view/analysisPanelState.test.ts`

Expected: FAIL because `AnalysisPanel` does not yet accept `activeAnnotationId` / `recentlyAddedAnnotationId`, the tray copy is missing, and the tooltip text still uses the old wording.

- [ ] **Step 3: Write minimal implementation**

Update `src/repo-view/analysisPanelState.ts`:

```ts
export function repoSendButtonTitle(
  sessionRunning: boolean,
  annotationCount: number,
  sending = false
): string {
  if (sending) return '发送中'
  if (!sessionRunning) return '请先启动下方 AI CLI'
  if (annotationCount <= 0) return '至少需要一条标注'
  return '发送当前文件批注到 AI CLI'
}
```

Update the `AnalysisPanel` signature and card rendering in `src/repo-view/AnalysisPanel.tsx`:

```tsx
export default function AnalysisPanel({
  filePath,
  annotations,
  sessionRunning,
  sending,
  activeAnnotationId,
  recentlyAddedAnnotationId,
  onSendToCli,
  onEditAnnotation,
  onRemoveAnnotation,
  onClearAnnotations
}: {
  filePath: string
  annotations: RepoCodeAnnotation[]
  sessionRunning: boolean
  sending: boolean
  activeAnnotationId: string | null
  recentlyAddedAnnotationId: string | null
  onSendToCli: (question: string) => void
  onEditAnnotation: (id: string) => void
  onRemoveAnnotation: (id: string) => void
  onClearAnnotations: () => void
}): JSX.Element {
  const [question, setQuestion] = useState('')
  const canSend = canSendRepoAnnotations(sessionRunning, annotations.length, sending)

  return (
    <div className="repo-analysis-panel">
      <div className="repo-analysis-head">代码标注</div>
      {!filePath ? (
        <div className="repo-analysis-empty">先从左侧选择一个文件。</div>
      ) : (
        <>
          <div className="repo-analysis-subhead">
            当前文件待发送批注（{annotations.length}）
          </div>
          {annotations.length === 0 ? (
            <div className="repo-analysis-empty">
              在代码区选中文本后点击“标注”，即可加入待发送批注托盘。
            </div>
          ) : (
            <ul className="repo-analysis-list">
              {annotations.map((a) => {
                const active = a.id === activeAnnotationId
                const recent = a.id === recentlyAddedAnnotationId
                const itemClass = [
                  'repo-analysis-item',
                  active ? 'active' : '',
                  recent ? 'recent' : ''
                ]
                  .filter(Boolean)
                  .join(' ')

                return (
                  <li key={a.id} className={itemClass}>
                    <div className="repo-analysis-item-head">
                      <span>#{annotations.indexOf(a) + 1}</span>
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
                      {a.snippet.length > 800
                        ? `${a.snippet.slice(0, 800)}\n[truncated]`
                        : a.snippet}
                    </blockquote>
                    <div className="repo-analysis-comment">{a.comment}</div>
                  </li>
                )
              })}
            </ul>
          )}
          <label className="repo-analysis-input-label">问题（可选）</label>
          <textarea
            className="repo-analysis-input"
            value={question}
            onChange={(e) => setQuestion(e.target.value)}
            rows={4}
          />
          <div className="repo-analysis-actions">
            <button className="drawer-btn" onClick={onClearAnnotations} disabled={annotations.length === 0}>
              清空标注
            </button>
            <button
              className="drawer-btn primary"
              disabled={!canSend}
              onClick={() => {
                onSendToCli(question.trim())
                setQuestion('')
              }}
              title={repoSendButtonTitle(sessionRunning, annotations.length, sending)}
            >
              {sending ? '发送中' : '发送当前文件批注到 AI CLI'}
            </button>
          </div>
        </>
      )}
    </div>
  )
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run src/repo-view/AnalysisPanel.test.tsx src/repo-view/analysisPanelState.test.ts`

Expected: PASS with the new tray copy and class names.

- [ ] **Step 5: Commit**

```bash
git add src/repo-view/AnalysisPanel.tsx src/repo-view/AnalysisPanel.test.tsx src/repo-view/analysisPanelState.ts src/repo-view/analysisPanelState.test.ts
git commit -m "feat: restyle repo annotation tray states"
```

### Task 3: Code Pane Header and Linked Line Range

**Files:**
- Create: `src/repo-view/CodePane.test.tsx`
- Modify: `src/repo-view/CodePane.tsx`

- [ ] **Step 1: Write the failing test**

Create `src/repo-view/CodePane.test.tsx`:

```tsx
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import CodePane from './CodePane'

describe('CodePane', () => {
  it('renders a file context header with a primary path and secondary meta text', () => {
    const html = renderToStaticMarkup(
      <CodePane
        filePath="src/repo-view/CodePane.tsx"
        content={'const a = 1\\nconst b = 2'}
        byteLength={24}
        loading={false}
        onAnnotateSelection={vi.fn()}
        onCancelEditing={vi.fn()}
      />
    )

    expect(html).toContain('repo-code-head-main')
    expect(html).toContain('repo-code-path')
    expect(html).toContain('2 行')
  })

  it('adds linked-line styling for the editing annotation range', () => {
    const html = renderToStaticMarkup(
      <CodePane
        filePath="src/repo-view/CodePane.tsx"
        content={'l1\\nl2\\nl3\\nl4'}
        byteLength={11}
        loading={false}
        onAnnotateSelection={vi.fn()}
        editingAnnotation={{
          id: 'ann_1',
          lineRange: '2-3',
          snippet: 'l2\\nl3',
          comment: 'Focus these lines'
        }}
        onCancelEditing={vi.fn()}
      />
    )

    expect(html).toContain('repo-code-line linked')
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run src/repo-view/CodePane.test.tsx`

Expected: FAIL because the current header is a flat row and the code lines do not apply a linked class from `editingAnnotation.lineRange`.

- [ ] **Step 3: Write minimal implementation**

Update `src/repo-view/CodePane.tsx`:

```tsx
function parseLineRange(lineRange: string): { start: number; end: number } | null {
  const match = lineRange.match(/^(\\d+)(?:-(\\d+))?$/)
  if (!match) return null
  const start = Number(match[1])
  const end = Number(match[2] ?? match[1])
  if (!Number.isFinite(start) || !Number.isFinite(end)) return null
  return { start, end }
}

export default function CodePane({
  filePath,
  content,
  byteLength,
  loading,
  onAnnotateSelection,
  editingAnnotation,
  onCancelEditing
}: CodePaneProps): JSX.Element {
  const linkedRange = useMemo(
    () => (editingAnnotation ? parseLineRange(editingAnnotation.lineRange) : null),
    [editingAnnotation]
  )

  return (
    <div className="repo-code-wrap">
      <div className="repo-code-head" title={filePath || '未选择文件'}>
        <div className="repo-code-head-main">
          <span className="repo-code-path">{filePath || '未选择文件'}</span>
        </div>
        {filePath && (
          <div className="repo-code-head-meta">
            <span className="repo-code-meta">{lineCount} 行</span>
            <span className="repo-code-meta">{byteLength} bytes</span>
          </div>
        )}
      </div>
      <div className="repo-code-pane" ref={paneRef} onMouseUp={handleMouseUp} onScroll={() => setDraft(null)}>
        {!filePath ? (
          <div className="repo-code-empty">从左侧选择一个文件以查看源码</div>
        ) : loading ? (
          <div className="repo-code-empty">读取中...</div>
        ) : (
          <pre className="repo-code-pre">
            {content.split('\n').map((line, index) => {
              const lineNumber = index + 1
              const linked =
                linkedRange !== null &&
                lineNumber >= linkedRange.start &&
                lineNumber <= linkedRange.end

              return (
                <div
                  key={index}
                  className={`repo-code-line${linked ? ' linked' : ''}`}
                  data-line={lineNumber}
                >
                  <span className="repo-code-gutter">{lineNumber}</span>
                  <span className="repo-code-text">{line || ' '}</span>
                </div>
              )
            })}
          </pre>
        )}
      </div>
    </div>
  )
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run src/repo-view/CodePane.test.tsx`

Expected: PASS with both header and linked-line assertions green.

- [ ] **Step 5: Commit**

```bash
git add src/repo-view/CodePane.tsx src/repo-view/CodePane.test.tsx
git commit -m "feat: polish repo code pane context and linked range"
```

### Task 4: Wire the Window State and Apply the Styles

**Files:**
- Modify: `src/repo-view/RepoViewerWindow.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing test**

Extend `src/repo-view/annotationVisualState.test.ts` with a send/clear behavior check:

```ts
it('returns to the idle state after a file-scoped clear', () => {
  expect(clearAnnotationVisualState()).toEqual({
    activeAnnotationId: null,
    recentlyAddedAnnotationId: null
  })
})
```

This test is already present from Task 1. Re-run it before wiring the window so the refactor stays anchored to the helper API instead of ad-hoc state updates.

- [ ] **Step 2: Run tests to verify the preconditions**

Run: `npx vitest run src/repo-view/annotationVisualState.test.ts src/repo-view/AnalysisPanel.test.tsx src/repo-view/CodePane.test.tsx`

Expected: PASS before the window wiring begins.

- [ ] **Step 3: Write minimal implementation**

Update `src/repo-view/RepoViewerWindow.tsx` to use the helper and pass the new props:

```tsx
import {
  clearAnnotationVisualState,
  removeAnnotationVisualState,
  startEditingAnnotationState,
  trackNewAnnotationState
} from './annotationVisualState'

const [activeAnnotationId, setActiveAnnotationId] = useState<string | null>(null)
const [recentlyAddedAnnotationId, setRecentlyAddedAnnotationId] = useState<string | null>(null)

useEffect(() => {
  setEditingAnnotationId(null)
  setActiveAnnotationId(null)
  setRecentlyAddedAnnotationId(null)
}, [selectedFile])

const onAnnotateSelection = useCallback(
  (selection: RepoSelection, comment: string, editingId?: string) => {
    if (!selectedFile) return
    if (editingId) {
      setAnnotations((prev) =>
        prev.map((annotation) =>
          annotation.id === editingId
            ? { ...annotation, lineRange: selection.lineRange, snippet: selection.snippet, comment }
            : annotation
        )
      )
      const nextVisual = startEditingAnnotationState(
        { activeAnnotationId, recentlyAddedAnnotationId },
        editingId
      )
      setActiveAnnotationId(nextVisual.activeAnnotationId)
      setRecentlyAddedAnnotationId(nextVisual.recentlyAddedAnnotationId)
      setEditingAnnotationId(null)
      return
    }

    const id = `ann_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`
    setAnnotations((prev) => [
      ...prev,
      { id, filePath: selectedFile, lineRange: selection.lineRange, snippet: selection.snippet, comment }
    ])
    const nextVisual = trackNewAnnotationState(id)
    setActiveAnnotationId(nextVisual.activeAnnotationId)
    setRecentlyAddedAnnotationId(nextVisual.recentlyAddedAnnotationId)
  },
  [activeAnnotationId, recentlyAddedAnnotationId, selectedFile]
)

useEffect(() => {
  if (!recentlyAddedAnnotationId) return
  const timer = window.setTimeout(() => setRecentlyAddedAnnotationId(null), 1800)
  return () => window.clearTimeout(timer)
}, [recentlyAddedAnnotationId])
```

Pass the new props into `CodePane` and `AnalysisPanel`:

```tsx
<CodePane
  filePath={selectedFile}
  content={selectedContent}
  byteLength={selectedSize}
  loading={loadingFile}
  onAnnotateSelection={onAnnotateSelection}
  editingAnnotation={
    annotations.find((annotation) => annotation.id === editingAnnotationId) ?? null
  }
  onCancelEditing={() => setEditingAnnotationId(null)}
/>

<AnalysisPanel
  filePath={selectedFile}
  annotations={annotations.filter((a) => a.filePath === selectedFile)}
  sessionRunning={sessionRunning}
  sending={sending}
  activeAnnotationId={activeAnnotationId}
  recentlyAddedAnnotationId={recentlyAddedAnnotationId}
  onEditAnnotation={(id) => {
    const nextVisual = startEditingAnnotationState(
      { activeAnnotationId, recentlyAddedAnnotationId },
      id
    )
    setEditingAnnotationId(id)
    setActiveAnnotationId(nextVisual.activeAnnotationId)
    setRecentlyAddedAnnotationId(nextVisual.recentlyAddedAnnotationId)
  }}
  onRemoveAnnotation={(id) => {
    if (editingAnnotationId === id) setEditingAnnotationId(null)
    const nextVisual = removeAnnotationVisualState(
      { activeAnnotationId, recentlyAddedAnnotationId },
      id
    )
    setActiveAnnotationId(nextVisual.activeAnnotationId)
    setRecentlyAddedAnnotationId(nextVisual.recentlyAddedAnnotationId)
    setAnnotations((prev) => prev.filter((a) => a.id !== id))
  }}
  onClearAnnotations={() => {
    setEditingAnnotationId(null)
    const nextVisual = clearAnnotationVisualState()
    setActiveAnnotationId(nextVisual.activeAnnotationId)
    setRecentlyAddedAnnotationId(nextVisual.recentlyAddedAnnotationId)
    setAnnotations((prev) => prev.filter((a) => a.filePath !== selectedFile))
  }}
/>
```

Add the stylesheet blocks in `src/styles.css`:

```css
.repo-code-head {
  min-height: 52px;
  padding: 10px 14px;
  background: linear-gradient(180deg, var(--mac-surface-raised), var(--mac-surface));
}

.repo-code-head-main {
  min-width: 0;
  display: flex;
  align-items: center;
}

.repo-code-path {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-weight: var(--mac-weight-medium);
}

.repo-code-head-meta {
  display: flex;
  gap: 8px;
}

.repo-code-line.linked {
  background: linear-gradient(90deg, var(--mac-primary-soft), transparent 72%);
}

.repo-code-annotate-floater {
  border-radius: var(--mac-r-pill);
  padding: 6px 12px;
  background: color-mix(in srgb, var(--mac-surface) 92%, var(--mac-primary-soft));
  box-shadow: var(--mac-elev-2);
}

.repo-analysis-item {
  background: var(--mac-surface);
  border-radius: var(--mac-r-lg);
  padding: 12px;
  transition: border-color var(--mac-dur-fast) var(--mac-ease),
              box-shadow var(--mac-dur-fast) var(--mac-ease);
}

.repo-analysis-item.active {
  border-color: var(--mac-primary);
  box-shadow: 0 0 0 1px var(--mac-primary-soft);
}

.repo-analysis-item.recent {
  background: color-mix(in srgb, var(--mac-surface) 86%, var(--mac-primary-soft));
}
```

- [ ] **Step 4: Run verification**

Run:

```bash
npx vitest run src/repo-view/annotationVisualState.test.ts src/repo-view/AnalysisPanel.test.tsx src/repo-view/CodePane.test.tsx src/repo-view/analysisPanelState.test.ts
npm run typecheck
```

Expected:

- All four test files PASS
- `npm run typecheck` exits 0

- [ ] **Step 5: Commit**

```bash
git add src/repo-view/RepoViewerWindow.tsx src/styles.css
git commit -m "feat: tighten repo quick annotation workflow"
```

### Task 5: Final Product Verification

**Files:**
- Modify: none
- Test: repo viewer manual smoke test

- [ ] **Step 1: Run the app in dev mode**

Run: `npm run dev`

Expected: Electron app opens with the repo viewer available.

- [ ] **Step 2: Verify the quick annotation flow**

Manual checklist:

- Open repo viewer and select a file.
- Confirm the code header shows the file path and split metadata cleanly.
- Select code and confirm the annotate button appears as a light chip.
- Add a new annotation and confirm the right-side card briefly shows the recent state.
- Click `编辑` on a card and confirm the related code lines get the linked styling.
- Click `清空标注` and confirm the tray resets to its idle empty state.
- Start/stop the AI CLI and verify the send button tooltip and disabled state still match the session status.

- [ ] **Step 3: Commit only if manual verification passes**

```bash
git status --short
```

Expected: only the repo quick-annotation implementation files remain modified. If the manual verification found layout or copy issues, fix them in the current branch before any additional commit.
