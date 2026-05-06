# Markdown Diff Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Markdown Preview` mode to the diff review dialog for `.md` and `.markdown` files while keeping the existing code diff workflow unchanged.

**Architecture:** Keep unified diff parsing inside the diff viewer flow, derive `oldText` and `newText` from the parsed `DiffFile.lines`, and render those texts through a small read-only Markdown preview component. Gate the new UI behind a file-extension check so non-Markdown files keep the exact current behavior.

**Tech Stack:** React 18, TypeScript, `react-markdown`, `remark-gfm`, Vitest, existing `.md-rendered` CSS

---

### Task 1: Add red tests for Markdown preview text derivation

**Files:**
- Create: `src/components/diffMarkdownPreview.ts`
- Create: `src/components/diffMarkdownPreview.test.ts`
- Modify: `src/components/DiffViewerDialog.tsx`

- [ ] **Step 1: Write the failing test**

```ts
import { describe, expect, it } from 'vitest'
import { buildMarkdownPreviewText, isMarkdownDiffPath } from './diffMarkdownPreview.js'

describe('isMarkdownDiffPath', () => {
  it('accepts .md and .markdown paths', () => {
    expect(isMarkdownDiffPath('docs/guide.md')).toBe(true)
    expect(isMarkdownDiffPath('docs/guide.markdown')).toBe(true)
    expect(isMarkdownDiffPath('src/App.tsx')).toBe(false)
  })
})

describe('buildMarkdownPreviewText', () => {
  it('splits modified markdown diff lines into old and new text', () => {
    expect(
      buildMarkdownPreviewText([
        { kind: 'context', text: '# Title' },
        { kind: 'del', text: '- old item' },
        { kind: 'add', text: '- new item' },
        { kind: 'context', text: '' }
      ])
    ).toEqual({
      oldText: '# Title\n- old item\n',
      newText: '# Title\n- new item\n'
    })
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm.cmd test -- src/components/diffMarkdownPreview.test.ts`
Expected: FAIL because `src/components/diffMarkdownPreview.ts` does not exist yet.

- [ ] **Step 3: Write minimal implementation**

```ts
import type { DiffLine } from './DiffViewerDialog.js'

const MARKDOWN_FILE_RE = /\.(md|markdown)$/i

export function isMarkdownDiffPath(path: string): boolean {
  return MARKDOWN_FILE_RE.test(path.trim())
}

export function buildMarkdownPreviewText(lines: DiffLine[]): {
  oldText: string
  newText: string
} {
  const oldParts: string[] = []
  const newParts: string[] = []
  for (const line of lines) {
    if (line.kind === 'context') {
      oldParts.push(line.text)
      newParts.push(line.text)
    } else if (line.kind === 'del') {
      oldParts.push(line.text)
    } else if (line.kind === 'add') {
      newParts.push(line.text)
    }
  }
  return {
    oldText: oldParts.join('\n'),
    newText: newParts.join('\n')
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm.cmd test -- src/components/diffMarkdownPreview.test.ts`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/components/diffMarkdownPreview.ts src/components/diffMarkdownPreview.test.ts src/components/DiffViewerDialog.tsx
git commit -m "test: cover markdown diff preview text derivation"
```

### Task 2: Add red tests for the preview component

**Files:**
- Create: `src/components/MarkdownDiffPreview.tsx`
- Create: `src/components/MarkdownDiffPreview.test.tsx`

- [ ] **Step 1: Write the failing test**

```tsx
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import MarkdownDiffPreview from './MarkdownDiffPreview.js'

describe('MarkdownDiffPreview', () => {
  it('renders old and new markdown columns', () => {
    const html = renderToStaticMarkup(
      <MarkdownDiffPreview
        filePath="docs/guide.md"
        oldText="# Old\n\n- one"
        newText="# New\n\n- two"
      />
    )

    expect(html).toContain('Markdown Preview')
    expect(html).toContain('dv-md-preview-col')
    expect(html).toContain('<h1>Old</h1>')
    expect(html).toContain('<h1>New</h1>')
    expect(html).toContain('<li>one</li>')
    expect(html).toContain('<li>two</li>')
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm.cmd test -- src/components/MarkdownDiffPreview.test.tsx`
Expected: FAIL because the component does not exist yet.

- [ ] **Step 3: Write minimal implementation**

```tsx
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'

export default function MarkdownDiffPreview({ oldText, newText }: Props): JSX.Element {
  return (
    <section className="dv-md-preview">
      <div className="dv-md-preview-col">
        <div className="dv-md-preview-head">Old</div>
        <div className="dv-md-preview-body md-rendered">
          <ReactMarkdown remarkPlugins={[remarkGfm]}>{oldText}</ReactMarkdown>
        </div>
      </div>
      <div className="dv-md-preview-col">
        <div className="dv-md-preview-head">New</div>
        <div className="dv-md-preview-body md-rendered">
          <ReactMarkdown remarkPlugins={[remarkGfm]}>{newText}</ReactMarkdown>
        </div>
      </div>
    </section>
  )
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npm.cmd test -- src/components/MarkdownDiffPreview.test.tsx`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/components/MarkdownDiffPreview.tsx src/components/MarkdownDiffPreview.test.tsx
git commit -m "feat: add markdown diff preview component"
```

### Task 3: Wire the preview into the diff dialog with red-green tests

**Files:**
- Modify: `src/components/DiffViewerDialog.tsx`
- Modify: `src/components/DiffViewerDialog.test.tsx`
- Modify: `src/styles.css`
- Test: `src/components/diffMarkdownPreview.test.ts`
- Test: `src/components/MarkdownDiffPreview.test.tsx`

- [ ] **Step 1: Write the failing test**

```tsx
it('shows markdown preview toggle and preview content for markdown files', () => {
  const html = renderToStaticMarkup(
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
      selectedFile="docs/guide.md"
      onSelectedFileChange={vi.fn()}
    />
  )

  expect(html).toContain('Markdown Preview')
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npm.cmd test -- src/components/DiffViewerDialog.test.tsx`
Expected: FAIL because the dialog does not render preview controls yet.

- [ ] **Step 3: Write minimal implementation**

```tsx
const [fileViewMode, setFileViewMode] = useState<'diff' | 'markdown-preview'>('diff')
const currentFileIsMarkdown = currentFile ? isMarkdownDiffPath(currentFile.path) : false
const markdownPreview = currentFile ? buildMarkdownPreviewText(currentFile.lines) : null

{currentFileIsMarkdown && (
  <div className="dv-file-view-tabs">
    <button ...>Diff</button>
    <button ...>Markdown Preview</button>
  </div>
)}

{fileViewMode === 'markdown-preview' && currentFile && markdownPreview ? (
  <MarkdownDiffPreview
    filePath={currentFile.path}
    oldText={markdownPreview.oldText}
    newText={markdownPreview.newText}
  />
) : (
  <VirtualizedFileRows ... />
)}
```

- [ ] **Step 4: Run focused tests to verify they pass**

Run: `npm.cmd test -- src/components/diffMarkdownPreview.test.ts src/components/MarkdownDiffPreview.test.tsx src/components/DiffViewerDialog.test.tsx`
Expected: PASS

- [ ] **Step 5: Run broader verification**

Run: `npm.cmd test -- src/components/diffMarkdownPreview.test.ts src/components/MarkdownDiffPreview.test.tsx src/components/DiffViewerDialog.test.tsx src/repo-view/CodePane.test.tsx`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/components/DiffViewerDialog.tsx src/components/DiffViewerDialog.test.tsx src/styles.css src/components/diffMarkdownPreview.ts src/components/diffMarkdownPreview.test.ts src/components/MarkdownDiffPreview.tsx src/components/MarkdownDiffPreview.test.tsx
git commit -m "feat: add markdown preview mode to diff review"
```

### Task 4: Final verification in the worktree

**Files:**
- Modify: `docs/superpowers/plans/2026-05-06-markdown-diff-preview.md`

- [ ] **Step 1: Run targeted verification commands**

Run: `npm.cmd test -- src/components/diffMarkdownPreview.test.ts src/components/MarkdownDiffPreview.test.tsx src/components/DiffViewerDialog.test.tsx src/repo-view/CodePane.test.tsx`
Expected: PASS

- [ ] **Step 2: Run type checking**

Run: `npm.cmd run typecheck`
Expected: PASS

- [ ] **Step 3: Run full test suite for evidence**

Run: `npm.cmd test`
Expected: Existing unrelated Windows path failures remain limited to:
- `electron/repo-view/memory.test.ts`
- `electron/orchestrator/prompts.test.ts`
- `electron/store/paths.test.ts`

- [ ] **Step 4: Update this plan checklist to reflect completion**

```md
- [x] Step finished
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/plans/2026-05-06-markdown-diff-preview.md
git commit -m "docs: record markdown diff preview execution"
```
