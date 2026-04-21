# Single-Stage Architecture — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate Multi-AI Code from 4-stage pipeline to single-stage architecture. One AI session handles design + code + diff-annotation-driven adjustment; plans archive to `<target_repo>/.multi-ai-code/designs/<plan>.md`; `workspaces/` directory is retired.

**Architecture:** Backend strips STAGE_DONE / handoff / pipeline progression machinery; ptyManager spawns one CLI per plan with cwd = target_repo. Frontend replaces 2×2 grid with a single full-screen panel; DiffViewerDialog annotations are sent as live user messages to the active session via `window.api.cc.write` rather than handoff to a downstream stage. Settings collapse from 4-stage config to one AI-CLI config (Claude default, Codex optional).

**Tech Stack:** TypeScript, React 18, Electron 33, node-pty, Vitest, xterm.

**Spec reference:** `docs/superpowers/specs/2026-04-21-single-stage-architecture-design.md`

---

## File Structure

### Modify
- `electron/store/paths.ts` — remove `stage1TmpDir`; simplify `createProjectLayout`; expand `ensureRootDir` workspaces cleanup
- `electron/orchestrator/prompts.ts` — remove stage 2/3/4 constants; drop `buildForwardHandoff` + `buildFeedbackHandoff`; simplify `stageArtifactPath` + `resolveStageArtifactAbs` to always resolve to designs dir
- `electron/cc/ptyManager.ts` — delete stage progression, STAGE_DONE scanner integration, handoff injection, feedback routing
- `electron/main.ts` — remove `stage:inject-handoff`, `stage:done`, `stage:feedback-emitted` IPC handlers
- `electron/preload.ts` — remove `stage.injectHandoff`, `stage.onDone`, `stage.onFeedbackEmitted` from exposed API
- `electron/store/db.ts` — add startup migration cleaning `stage_events`/`stage_status` rows with `stage_id > 1`
- `electron/orchestrator/plans.ts` — adjust to single-stage plan concept (keep existing plan-list logic, drop any 4-stage assumptions)
- `electron/orchestrator/prompts.test.ts` — update for new exports
- `electron/store/paths.test.ts` — add tests for new layout
- `src/App.tsx` — replace 2×2 grid with single `MainPanel`; drop feedback/completion drawer wiring; remove advance IPC listeners
- `src/components/DiffViewerDialog.tsx` — change "发送到 Stage 3" → "发送到会话"; annotation payload goes via `window.api.cc.write`
- `src/components/TemplatesDialog.tsx` — collapse per-stage templates to a single list
- `src/components/TimelineDrawer.tsx` — filter events to `stage_id === 1` only
- `src/styles.css` — delete `.grid` + `.tile-*` + `.stage-settings-*` specifics; add `.main-panel`; rename stage-settings → ai-settings selectors

### Create
- `electron/prompts/main.md` — single merged prompt
- `electron/orchestrator/session-messages.ts` — `formatInitialMessage`, `formatAnnotationsForSession` helpers
- `electron/orchestrator/session-messages.test.ts` — unit tests
- `src/components/MainPanel.tsx` — single-stage replacement for StagePanel
- `src/components/AiSettingsDialog.tsx` — renamed + simplified StageSettingsDialog

### Delete
- `electron/prompts/stage1-design.md`
- `electron/prompts/stage2-impl.md`
- `electron/prompts/stage3-acceptance.md`
- `electron/prompts/stage4-test.md`
- `electron/cc/StageDoneScanner.ts`
- `src/components/StagePanel.tsx`
- `src/components/CompletionDrawer.tsx`
- `src/components/FeedbackDialog.tsx`
- `src/components/ReviewChecklist.tsx`
- `src/components/StageSettingsDialog.tsx`

---

## Conventions

- Run `npm run typecheck` and `npm run test` at the end of every task; both MUST be green before committing.
- **Do NOT** run `npm run dev` (Electron app, hangs in agent contexts).
- Each task concludes with exactly one commit; message template prescribed per task.
- Cross-file coordination within a single task is required to keep typecheck green — e.g., Task 4 touches backend + preload + App.tsx in one commit because removing exports from prompts.ts breaks all import sites simultaneously.

---

## Task 1: `paths.ts` — single-stage file layout

**Files:**
- Modify: `electron/store/paths.ts`
- Modify: `electron/store/paths.test.ts`

- [ ] **Step 1: Update `ensureRootDir` to unconditionally clean `workspaces/`**

Open `electron/store/paths.ts` and locate `ensureRootDir()`. Replace the "One-time migration" block with a simple unconditional cleanup (if the dir exists, remove it):

```ts
export async function ensureRootDir(): Promise<void> {
  await fs.mkdir(projectsDir(), { recursive: true })
  // Retire the platform-managed `workspaces/` subdir from every project.
  // Stage 1 now writes design.md directly to <target_repo>/.multi-ai-code/designs/;
  // Stages 2-4 no longer exist. Safe to remove on every startup.
  try {
    const entries = await fs.readdir(projectsDir())
    for (const id of entries) {
      const ws = join(projectsDir(), id, 'workspaces')
      try {
        await fs.rm(ws, { recursive: true, force: true })
      } catch {
        /* missing or locked — ignore */
      }
    }
  } catch {
    /* projectsDir freshly created and empty */
  }
}
```

- [ ] **Step 2: Remove `stage1TmpDir` export**

Delete the entire function `stage1TmpDir()` and its doc comment (approximately lines 22-30). Also remove the import of `tmpdir` from `os` if it's no longer used elsewhere in the file.

- [ ] **Step 3: Simplify `createProjectLayout`**

`createProjectLayout` already no longer creates `workspaces/`. Verify the current state: it should create `artifacts/` + `designArchiveDir(targetRepoPath)` + write `project.json` + init `history.jsonl`. No change needed unless it still references `stage1TmpDir` or `workspaceDir` — remove any residual calls.

- [ ] **Step 4: Remove `app` import from electron if no longer used**

The top of `paths.ts` imports `{ app } from 'electron'`. Check if `app` is referenced anywhere in the file; if not, remove the import.

- [ ] **Step 5: Update tests**

Open `electron/store/paths.test.ts`. Remove any import of `stage1TmpDir` or `workspaceDir` (both should be absent from paths.ts now). Add a regression test confirming `ensureRootDir` removes existing `workspaces/` dirs:

```ts
it('ensureRootDir removes existing workspaces/ dir in each project', async () => {
  const projRoot = await fs.mkdtemp(join(tmpdir(), 'mac-paths-ensure-'))
  process.env.MULTI_AI_ROOT = projRoot
  try {
    const pid = 'p_legacy'
    const pdir = join(projRoot, 'projects', pid, 'workspaces', 'stage1_design')
    await fs.mkdir(pdir, { recursive: true })
    await fs.writeFile(join(pdir, 'old.md'), 'legacy content')
    await ensureRootDir()
    const wsStat = await fs.stat(join(projRoot, 'projects', pid, 'workspaces')).catch(() => null)
    expect(wsStat).toBeNull()
  } finally {
    delete process.env.MULTI_AI_ROOT
    await fs.rm(projRoot, { recursive: true, force: true })
  }
})
```

(Imports needed at top of test file if not already present: `tmpdir` from `os`, `ensureRootDir` from `./paths.js`.)

- [ ] **Step 6: Run tests and typecheck**

```bash
npm run typecheck
npx vitest run electron/store/paths.test.ts
```

Both must be green.

- [ ] **Step 7: Commit**

```bash
git add electron/store/paths.ts electron/store/paths.test.ts
git commit -m "refactor(paths): retire stage1TmpDir + unconditional workspaces cleanup"
```

---

## Task 2: Session-message helpers (TDD)

**Files:**
- Create: `electron/orchestrator/session-messages.ts`
- Create: `electron/orchestrator/session-messages.test.ts`

- [ ] **Step 1: Write failing tests**

Create `electron/orchestrator/session-messages.test.ts`:

```ts
import { describe, it, expect } from 'vitest'
import {
  formatInitialMessage,
  formatAnnotationsForSession,
  type SessionAnnotation
} from './session-messages.js'

describe('formatInitialMessage', () => {
  it('returns a "please continue" message when plan file content is non-null', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planContent: '# 方案：增加 OAuth\n\n详细步骤...'
    })
    expect(out).toContain('# 方案：增加 OAuth')
    expect(out).toContain('详细步骤')
    expect(out).toContain('请基于当前方案继续工作')
  })

  it('returns a "kick off design" message when plan content is null', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planContent: null
    })
    expect(out).toContain('add-auth')
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
    expect(out).toContain('澄清需求')
  })
})

describe('formatAnnotationsForSession', () => {
  const ann1: SessionAnnotation = {
    file: 'src/auth.ts',
    lineRange: '10-12',
    snippet: 'const token = req.headers.auth',
    comment: '改为读取 Authorization Bearer'
  }

  it('produces a markdown block starting with the batch header', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out.startsWith('# 用户批注')).toBe(true)
  })

  it('references each annotation with file:line + snippet + comment', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('src/auth.ts:10-12')
    expect(out).toContain('const token = req.headers.auth')
    expect(out).toContain('改为读取 Authorization Bearer')
  })

  it('appends the general comment section when provided', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '整体结构 OK，改前加一层抽象',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('## 整体意见')
    expect(out).toContain('整体结构 OK')
  })

  it('omits the general comment section when empty', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '   ',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).not.toContain('## 整体意见')
  })

  it('embeds the plan absolute path so AI can update the plan if asked', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
  })

  it('handles multiple annotations in order', () => {
    const ann2: SessionAnnotation = {
      file: 'src/app.tsx',
      lineRange: '100',
      snippet: '<Login />',
      comment: '移到 <Router> 外'
    }
    const out = formatAnnotationsForSession({
      annotations: [ann1, ann2],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    const firstIdx = out.indexOf('src/auth.ts:10-12')
    const secondIdx = out.indexOf('src/app.tsx:100')
    expect(firstIdx).toBeGreaterThan(-1)
    expect(secondIdx).toBeGreaterThan(firstIdx)
  })
})
```

- [ ] **Step 2: Verify tests fail**

```bash
npx vitest run electron/orchestrator/session-messages.test.ts
```

Expected: FAIL (module missing).

- [ ] **Step 3: Implement `session-messages.ts`**

Create `electron/orchestrator/session-messages.ts`:

```ts
export interface SessionAnnotation {
  /** Relative path of the annotated file (from target_repo root). */
  file: string
  /** "10" or "10-12" — line number or inclusive range. */
  lineRange: string
  /** The exact code snippet the user highlighted. */
  snippet: string
  /** User's comment on this location. */
  comment: string
}

export interface InitialMessageParams {
  /** Plan name as the user typed it (e.g. "add-auth"). */
  planName: string
  /** Absolute path where the plan markdown lives (or should be written). */
  planAbsPath: string
  /** Plan file content if it already exists on disk; null for new plans. */
  planContent: string | null
}

export function formatInitialMessage(p: InitialMessageParams): string {
  if (p.planContent !== null) {
    return [
      p.planContent.trimEnd(),
      '',
      '---',
      '',
      '请基于当前方案继续工作（写代码 / 根据批注调整）。'
    ].join('\n')
  }
  return [
    `本次方案名：${p.planName}。`,
    '',
    `请先与用户对话澄清需求、确认方向，然后把方案写到 \`${p.planAbsPath}\`（完整绝对路径），再继续实施。`
  ].join('\n')
}

export interface AnnotationsForSessionParams {
  annotations: SessionAnnotation[]
  /** User's optional overall comment on the whole diff. */
  generalComment: string
  /** Absolute path of the current plan markdown (for "update plan if asked" reference). */
  planAbsPath: string
}

export function formatAnnotationsForSession(
  p: AnnotationsForSessionParams
): string {
  const lines: string[] = []
  lines.push('# 用户批注')
  lines.push('')
  lines.push(
    `以下是用户对当前改动的批注，请严格按照批注执行：修改代码、或更新方案文档（\`${p.planAbsPath}\`）。`
  )
  lines.push('')
  lines.push('## 逐行批注')
  lines.push('')
  for (const a of p.annotations) {
    lines.push(`### \`${a.file}:${a.lineRange}\``)
    lines.push('')
    for (const sl of a.snippet.split('\n')) {
      lines.push(`> ${sl}`)
    }
    lines.push('')
    lines.push(a.comment)
    lines.push('')
  }
  const gc = p.generalComment.trim()
  if (gc.length > 0) {
    lines.push('## 整体意见')
    lines.push('')
    lines.push(gc)
    lines.push('')
  }
  lines.push('---')
  lines.push('')
  lines.push('请按照以上批注调整代码 / 方案，完成后在终端里简述改了什么。')
  return lines.join('\n')
}
```

- [ ] **Step 4: Verify tests pass**

```bash
npx vitest run electron/orchestrator/session-messages.test.ts
```

Expected: 7 tests pass.

- [ ] **Step 5: Typecheck**

```bash
npm run typecheck
```

- [ ] **Step 6: Commit**

```bash
git add electron/orchestrator/session-messages.ts electron/orchestrator/session-messages.test.ts
git commit -m "feat(orchestrator): add session-messages helpers for single-stage flow"
```

---

## Task 3: New `main.md` prompt + delete old stage prompts

**Files:**
- Create: `electron/prompts/main.md`
- Delete: `electron/prompts/stage1-design.md`
- Delete: `electron/prompts/stage2-impl.md`
- Delete: `electron/prompts/stage3-acceptance.md`
- Delete: `electron/prompts/stage4-test.md`

**Note:** This task only creates the new prompt file and deletes old ones. Code that loads these prompts (`loadStagePromptTemplate`) will still reference the old names — Task 4 updates the loader. Until Task 4 lands, `buildSystemPrompt()` for any stage will throw "prompt template not found". That's fine because Task 4 runs immediately after and tests don't exercise prompt loading.

- [ ] **Step 1: Create `electron/prompts/main.md`**

Write exact content:

````markdown
# 角色

你同时承担三个职责：

1. **方案设计**：与用户对话澄清需求，产出高质量实施方案
2. **代码实施**：按方案修改 `target_repo` 里的代码
3. **根据批注调整**：当收到"# 用户批注"消息时，严格按批注修改代码或方案

---

# 工作流

## 新方案（方案 md 不存在时）

1. 先与用户对话，澄清目标、范围、约束、成功标准
2. 明确后，把方案写到 `{{ARTIFACT_PATH}}`（完整绝对路径）
3. 让用户确认方案后再开始写代码

## 已有方案（方案 md 已存在时）

1. 如果方案里还有未完成的任务，继续推进
2. 修改代码严格限定在 `{{TARGET_REPO}}` 范围内
3. 每完成一个可交付的改动，简述改了什么

## 收到 "# 用户批注" 消息

1. 该消息优先级高于你当前正在做的任何任务；先回应批注
2. 按每一条逐行批注和整体意见执行
3. 批注可能要求改代码、也可能要求更新方案 `md`（用批注里给出的绝对路径）
4. 完成后在终端里简述你做了什么

---

# 硬约束

- **方案文件绝对路径**：`{{ARTIFACT_PATH}}`
- **代码修改只能在**：`{{TARGET_REPO}}`
- 不得使用网络访问外部 URL（除非用户明确允许）
- 不得 `git push` 到远端
- 不得 `git reset --hard` 或 `git push --force`
- 若发现 `<target_repo>/.gitignore` 没有忽略 `.multi-ai-code/`，第一次写方案时顺手把 `.multi-ai-code/` 追加到 `.gitignore`

---

# 环境

- cwd: `{{STAGE_CWD}}`
- 项目名: `{{PROJECT_NAME}}`
- 项目根: `{{PROJECT_DIR}}`
- 目标仓库: `{{TARGET_REPO}}`

开始前若有不确定的地方，先向用户提问澄清；不要自行假设。
````

- [ ] **Step 2: Delete old stage prompts**

```bash
rm electron/prompts/stage1-design.md
rm electron/prompts/stage2-impl.md
rm electron/prompts/stage3-acceptance.md
rm electron/prompts/stage4-test.md
```

- [ ] **Step 3: Commit**

Do NOT run `npm run test` here — Task 4 is where the prompt loader is updated. Typecheck is not affected by this commit.

```bash
git add electron/prompts/main.md
git add electron/prompts/stage1-design.md electron/prompts/stage2-impl.md electron/prompts/stage3-acceptance.md electron/prompts/stage4-test.md
git commit -m "feat(prompts): introduce main.md + delete 4-stage prompt files"
```

(`git add` on deleted files records the deletion.)

---

## Task 4: Strip stage machinery across backend + preload + App.tsx

**Files:**
- Modify: `electron/orchestrator/prompts.ts`
- Modify: `electron/cc/ptyManager.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Modify: `src/App.tsx` (remove listeners for deleted events)
- Modify: `electron/orchestrator/prompts.test.ts`
- Delete: `electron/cc/StageDoneScanner.ts`

This is a coordinated task because removing backend exports breaks TypeScript at every import site simultaneously. All sites updated in one commit.

### Phase A — `prompts.ts` simplify

- [ ] **Step 1: Rewrite `electron/orchestrator/prompts.ts`**

Replace the file's content with the single-stage shape. Preserve: `loadStagePromptTemplate`, `renderTemplate`, `buildSystemPrompt`, `RenderContext`, `sanitizeLabel`, `stageArtifactPath`, `resolveStageArtifactAbs`, `STAGE_ARTIFACTS` (keep for historical UI display — see below).

Changes:
- Remove exports: `STAGE_NAMES`, `STAGE_COMMAND`, `STAGE_CLI_ARGS`, `buildForwardHandoff`, `buildFeedbackHandoff`, `HandoffContext`
- Add exports: `MAIN_COMMAND` (default CLI binary name), `mainCliArgs()` (default args for Claude, or Codex if overridden)
- `loadStagePromptTemplate(stageId)` → rename to `loadMainPromptTemplate()`; reads `main.md` only
- `stageArtifactPath(stageId, label?, targetRepo?)` → collapse to `planArtifactPath(label, targetRepo)` returning `<targetRepo>/.multi-ai-code/designs/<sanitized>.md` or `undefined` when `targetRepo` is missing; signature keeps `label` defaulting to `'design'`
- `resolveStageArtifactAbs` → rename `resolvePlanArtifactAbs(projectDir, label)` reading `project.json` for `target_repo`
- Remove `HandoffContext`, `buildForwardHandoff`, `buildFeedbackHandoff` entirely

Concrete new file content:

```ts
import { promises as fs } from 'fs'
import { join, dirname, isAbsolute } from 'path'
import { fileURLToPath } from 'url'
import { designArchiveDir } from '../store/paths.js'

const __dirname = dirname(fileURLToPath(import.meta.url))

function sanitizeLabel(label: string): string {
  return label
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
    .replace(/\s+/g, '_')
    .slice(0, 80)
}

/**
 * Absolute path for a plan's design markdown.
 * Returns undefined when targetRepo is missing (display-only paths use
 * `planDisplayPath` instead).
 */
export function planArtifactPath(
  label: string | null | undefined,
  targetRepo: string | null | undefined
): string | undefined {
  if (!targetRepo) return undefined
  const safe = label && label.trim() ? sanitizeLabel(label) : 'design'
  return join(designArchiveDir(targetRepo), `${safe}.md`)
}

/**
 * Reads `project.json` from `projectDir` to derive target_repo, then returns
 * the canonical design path. Throws if project.json is missing or malformed.
 */
export async function resolvePlanArtifactAbs(
  projectDir: string,
  label: string | null | undefined
): Promise<string> {
  const metaPath = join(projectDir, 'project.json')
  const raw = await fs.readFile(metaPath, 'utf8')
  const meta = JSON.parse(raw) as { target_repo?: string }
  if (!meta.target_repo) {
    throw new Error(`project.json missing target_repo: ${metaPath}`)
  }
  const p = planArtifactPath(label, meta.target_repo)
  if (!p) throw new Error('planArtifactPath returned undefined')
  return p
}

export const MAIN_COMMAND_DEFAULT = 'claude'

const SAFE_READS = [
  'Read',
  'Glob',
  'Grep',
  'Bash(ls:*)',
  'Bash(cat:*)',
  'Bash(head:*)',
  'Bash(tail:*)',
  'Bash(wc:*)',
  'Bash(find:*)',
  'Bash(pwd)',
  'Bash(echo:*)'
]

const SAFE_GIT = [
  'Bash(git status:*)',
  'Bash(git log:*)',
  'Bash(git diff:*)',
  'Bash(git show:*)',
  'Bash(git blame:*)',
  'Bash(git branch:*)',
  'Bash(git remote:*)',
  'Bash(git rev-parse:*)'
]

const WRITE_TOOLS = ['Write', 'Edit', 'MultiEdit']

/**
 * Default CLI args for the main single-stage AI session.
 *   - claude: auto permission mode + read/grep/git allowlist + write tools
 *   - codex: --full-auto (sandbox bounded by cwd = target_repo)
 */
export function mainCliArgs(
  binary: string = MAIN_COMMAND_DEFAULT
): string[] {
  if (binary === 'codex') return ['--full-auto']
  const allowed = [...SAFE_READS, ...SAFE_GIT, ...WRITE_TOOLS].join(' ')
  return ['--permission-mode', 'auto', '--allowedTools', allowed]
}

function promptsDir(): string {
  return join(__dirname, '..', '..', 'electron', 'prompts')
}

function fallbackPromptsDir(): string {
  return join(__dirname, 'prompts')
}

export async function loadMainPromptTemplate(): Promise<string> {
  for (const base of [promptsDir(), fallbackPromptsDir()]) {
    try {
      return await fs.readFile(join(base, 'main.md'), 'utf8')
    } catch {
      /* try next */
    }
  }
  throw new Error('prompt template not found: main.md')
}

export interface RenderContext {
  projectDir: string
  /** Absolute path to the plan markdown. */
  artifactPath: string
  projectName?: string
  targetRepo?: string
  stageCwd?: string
  /** When true, renderTemplate uses a placeholder in ARTIFACT_PATH so the
   *  CLI can ask the user to pick a plan name at archive time. */
  planPending?: boolean
}

export function renderTemplate(tpl: string, ctx: RenderContext): string {
  let artifactAbs: string
  if (ctx.planPending) {
    const root = (ctx.targetRepo ?? ctx.projectDir).replace(/[\/\\]+$/, '')
    artifactAbs = `${root}/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md`
  } else if (isAbsolute(ctx.artifactPath)) {
    artifactAbs = ctx.artifactPath
  } else {
    artifactAbs = `${ctx.projectDir.replace(/\/$/, '')}/${ctx.artifactPath}`
  }
  return tpl
    .replaceAll('{{PROJECT_DIR}}', ctx.projectDir)
    .replaceAll('{{PROJECT_NAME}}', ctx.projectName ?? '(未设置)')
    .replaceAll('{{TARGET_REPO}}', ctx.targetRepo ?? '(未设置)')
    .replaceAll('{{STAGE_CWD}}', ctx.stageCwd ?? ctx.projectDir)
    .replaceAll('{{ARTIFACT_PATH}}', artifactAbs)
}

function buildProjectContextBlock(ctx: RenderContext): string {
  return [
    '# 项目上下文（平台自动注入）',
    '',
    `- **项目名**：${ctx.projectName ?? '(未设置)'}`,
    `- **代码仓库绝对路径**：${ctx.targetRepo ?? '(未设置)'}`,
    `- **你的工作目录 (cwd)**：${ctx.stageCwd ?? ctx.projectDir}`,
    `- **项目根目录**：${ctx.projectDir}`,
    '',
    '---',
    ''
  ].join('\n')
}

export async function buildSystemPrompt(ctx: RenderContext): Promise<string> {
  const tpl = await loadMainPromptTemplate()
  const body = renderTemplate(tpl, ctx)
  return buildProjectContextBlock(ctx) + body
}
```

- [ ] **Step 2: Update `electron/orchestrator/prompts.test.ts`**

Replace the entire file's content with tests matching the new API:

```ts
import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  planArtifactPath,
  resolvePlanArtifactAbs,
  renderTemplate,
  mainCliArgs,
  MAIN_COMMAND_DEFAULT
} from './prompts.js'

describe('planArtifactPath', () => {
  it('returns <target_repo>/.multi-ai-code/designs/<label>.md when targetRepo is set', () => {
    expect(planArtifactPath('my-plan', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/my-plan.md'
    )
  })

  it('defaults label to "design" when empty/null', () => {
    expect(planArtifactPath(null, '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/design.md'
    )
  })

  it('sanitizes unsafe filename characters', () => {
    expect(planArtifactPath('foo/bar:baz', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/foo_bar_baz.md'
    )
  })

  it('returns undefined when targetRepo is missing', () => {
    expect(planArtifactPath('my-plan', null)).toBeUndefined()
    expect(planArtifactPath('my-plan', undefined)).toBeUndefined()
  })
})

describe('resolvePlanArtifactAbs', () => {
  let projectDir: string
  let targetRepo: string

  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-resolve-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-repo-'))
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })
  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('resolves to <target_repo>/.multi-ai-code/designs/<label>.md', async () => {
    const abs = await resolvePlanArtifactAbs(projectDir, 'my-plan')
    expect(abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'my-plan.md'))
  })

  it('defaults label to design.md', async () => {
    const abs = await resolvePlanArtifactAbs(projectDir, null)
    expect(abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'design.md'))
  })

  it('throws when project.json is missing', async () => {
    await fs.rm(join(projectDir, 'project.json'))
    await expect(resolvePlanArtifactAbs(projectDir, 'my-plan')).rejects.toThrow()
  })
})

describe('renderTemplate', () => {
  it('replaces all documented variables', () => {
    const out = renderTemplate(
      'P={{PROJECT_NAME}} R={{TARGET_REPO}} C={{STAGE_CWD}} D={{PROJECT_DIR}} A={{ARTIFACT_PATH}}',
      {
        projectDir: '/p',
        projectName: 'demo',
        targetRepo: '/repo',
        stageCwd: '/repo',
        artifactPath: '/repo/.multi-ai-code/designs/x.md'
      }
    )
    expect(out).toBe('P=demo R=/repo C=/repo D=/p A=/repo/.multi-ai-code/designs/x.md')
  })

  it('uses planPending placeholder when flag is set', () => {
    const out = renderTemplate('A={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: 'ignored',
      planPending: true,
      targetRepo: '/repo'
    })
    expect(out).toBe('A=/repo/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md')
  })

  it('resolves relative artifactPath against projectDir', () => {
    const out = renderTemplate('A={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: 'artifacts/foo.md'
    })
    expect(out).toBe('A=/p/artifacts/foo.md')
  })
})

describe('mainCliArgs', () => {
  it('default binary is claude', () => {
    expect(MAIN_COMMAND_DEFAULT).toBe('claude')
  })

  it('claude produces permission-mode auto + allowlist', () => {
    const args = mainCliArgs('claude')
    expect(args).toContain('--permission-mode')
    expect(args).toContain('auto')
    expect(args).toContain('--allowedTools')
  })

  it('codex produces --full-auto', () => {
    expect(mainCliArgs('codex')).toEqual(['--full-auto'])
  })
})
```

### Phase B — `StageDoneScanner.ts` delete

- [ ] **Step 3: Delete the scanner**

```bash
rm electron/cc/StageDoneScanner.ts
```

### Phase C — `ptyManager.ts` simplify

The current `ptyManager.ts` (~1090 lines) contains:
- `cc:spawn` handler that instantiates a Terminal-like session with a `StageDoneScanner`
- STAGE_DONE parsing + emitting `stage:done` events
- Handoff injection (`stage:inject-handoff`)
- Feedback routing (forward stdin after user clicks "发送到 Stage N")
- Artifact resolution per stage
- Per-stage CWD computation (Stage 1 was tmp, 2-4 was target_repo)

For single-stage: spawn one PTY per sessionId, cwd = `target_repo`, write `CLAUDE.md` (if Claude) or injections (if Codex), forward stdin/stdout, emit data/exit events. No scanning, no handoff, no feedback routing.

- [ ] **Step 4: Rewrite `electron/cc/ptyManager.ts`**

Read the current file and produce a slim replacement. Keep the public interface that `main.ts` calls:
- `spawn(req: SpawnRequest): Promise<{ ok, error? }>` — accepts sessionId, projectDir, targetRepo (becomes cwd), label (plan name), command + args (from AiSettings), and first user message; starts PTY; writes initial system prompt; queues the first user message after kickoff.
- `write(sessionId, data)` — forwards to pty stdin
- `resize(sessionId, cols, rows)` — forwards to pty
- `kill(sessionId)` — sends SIGTERM + cleanup
- `sendUser(sessionId, text)` — helper for DiffViewer annotation handoff; writes `text + '\r'` (or similar) to pty stdin
- `list()` / `has()` / `killAll()` — unchanged semantics

Remove from public interface:
- Stage IDs from spawn request
- STAGE_DONE events
- Handoff injection
- Feedback IPC hooks

Concrete replacement guidance:
1. Read the current `spawn` signature to identify field names (`stageId`, `label`, `projectDir`, `targetRepo`, `artifactPath`, etc.). Keep only the ones still useful.
2. Replace stage-dispatch blocks with a single path that:
   - Sets cwd = `req.targetRepo`
   - Writes `CLAUDE.md` (when `command === 'claude'`) to cwd with system prompt content from `buildSystemPrompt({ projectDir, artifactPath, projectName, targetRepo, stageCwd: targetRepo, planPending: ... })`
   - Writes `.codex-injection` or whatever the codex-side injection mechanism uses (look at current Stage 2 codex path for reference) when `command === 'codex'`
   - Spawns pty with `req.command, req.args, { cwd, env }`
   - After kickoff, writes `req.initialUserMessage` to stdin
3. Remove imports of StageDoneScanner, STAGE_NAMES, buildForwardHandoff, buildFeedbackHandoff, stage1TmpDir
4. Keep import of `buildSystemPrompt`, `planArtifactPath` (from new prompts.ts)

Full concrete rewrite is too long for this plan to inline literally; instead:

- Start from the current `ptyManager.ts`
- Delete these code regions:
  - Any `new StageDoneScanner(...)` + `scanner.on('done', ...)` + `scanner.on('feedback', ...)` blocks
  - The logic under `if (req.stageId === 1)` / `if (req.stageId === 2)` / `if (req.stageId === 3)` / `if (req.stageId === 4)` — collapse to one path that treats every spawn as "main stage"
  - Any `ipcMain.handle('stage:inject-handoff', ...)` if present inside ptyManager (it's likely in main.ts; if here, drop)
  - All `emit('stage:done', ...)` calls
  - All `plan_sources`-based stage-1-only branching around artifact path
  - Any function/helper only used by removed code
- Keep:
  - PTY spawn + event plumbing (data/exit/notice)
  - `write`/`resize`/`kill` entry points
  - `sendUser` (if exists) — rename or keep as-is for DiffViewer annotation use
  - Signal handling + session registry

- [ ] **Step 5: Update spawn request type across the boundary**

Open `electron/cc/ptyManager.ts`, `electron/preload.ts`, and `src/App.tsx`. Replace the `SpawnRequest`/`SpawnOptions` interface so it carries:

```ts
interface SpawnRequest {
  sessionId: string
  projectId: string
  projectDir: string
  targetRepo: string
  planName: string
  /** Absolute path resolved via resolvePlanArtifactAbs. Empty string if planPending. */
  planAbsPath: string
  /** true when plan file does not yet exist on disk (new plan flow). */
  planPending: boolean
  /** First user message to feed after kickoff. */
  initialUserMessage: string
  /** CLI binary (from AiSettings). */
  command: string
  /** CLI args. */
  args: string[]
  env?: Record<string, string>
}
```

Remove `stageId`, `artifactPath`, `label` (replaced by `planName`), and any other legacy fields from call sites.

### Phase D — `main.ts` IPC handlers

- [ ] **Step 6: Remove stage-specific IPC handlers**

Open `electron/main.ts`. Delete the following handlers and their associated listeners:
- `stage:inject-handoff`
- `stage:feedback-emitted` emitter (wherever the `webContents.send('stage:feedback-emitted', ...)` calls are)
- Any `stage:done` emitter (moved from ptyManager; the IPC event should no longer fire)

Keep:
- `cc:spawn` (now calls the new simplified ptyManager.spawn)
- `cc:input` / `cc:resize` / `cc:kill` / `cc:kill-all` / `cc:list` / `cc:has`
- `cc:send-user` — repurposed: DiffViewer sends annotation message via this handler
- All `project:*`, `shell:*`, `doctor:*`, `git:*`, `env:*` handlers

### Phase E — `preload.ts` API surface

- [ ] **Step 7: Simplify preload.ts exposed API**

Open `electron/preload.ts`. Delete:
- `stage.onDone`
- `stage.onFeedbackEmitted`
- `stage.injectHandoff`

Keep the rest of the `stage` namespace only if anything else is still there; otherwise delete the empty `stage: {}` block entirely.

Update `cc.spawn` signature's `SpawnOptions` type to match the new `SpawnRequest` above (shared via `electron/preload.ts` type export that `src/App.tsx` imports).

Update `cc.sendUser` to be the canonical "inject user message into active session" path (it already exists in current preload; keep).

Delete the `StageDoneEvent` and `FeedbackEmittedEvent` type exports if nothing else uses them, and remove their imports from `src/App.tsx`.

### Phase F — `src/App.tsx` listener cleanup

- [ ] **Step 8: Remove dead listeners from `src/App.tsx`**

Open `src/App.tsx`. Locate and delete:
- All `window.api.stage.onDone(...)` useEffects + their handlers (likely near the top of the component where other IPC listeners are wired)
- All `window.api.stage.onFeedbackEmitted(...)` useEffects
- Any `window.api.stage.injectHandoff(...)` call
- Any imports of `StageDoneEvent`, `FeedbackEmittedEvent`, `HandoffInjection` types

Leave untouched (for now) everything else in `App.tsx` — bigger rewrite happens in Task 9.

### Phase G — `plans.ts` trim

- [ ] **Step 9: Trim `electron/orchestrator/plans.ts`**

Open `electron/orchestrator/plans.ts` (and its test file). Remove:
- Any function that depends on `buildForwardHandoff` / `buildFeedbackHandoff` (deleted in Phase A)
- Any stage-id-keyed data structure (collapse to plan-level)
- Any `stage_id` parameter threading through plan-list / plan-source operations — plans are stage-agnostic now

If `plans.ts` turns out to contain only plan-list/plan-source logic with no stage references at all, this step is a no-op — confirm by grepping `stage` in the file after Phase A's deletes. If any dangling reference remains, delete the unused export + its test.

### Phase H — Verify

- [ ] **Step 10: Typecheck + tests**

```bash
npm run typecheck
npm run test
```

Both must be green. The paths.test.ts, prompts.test.ts, session-messages.test.ts, theme.test.ts should all pass. Any plans.test.ts that references deleted functions must be updated in this same task.

- [ ] **Step 11: Commit**

```bash
git add electron/orchestrator/prompts.ts electron/orchestrator/prompts.test.ts
git add electron/cc/ptyManager.ts electron/cc/StageDoneScanner.ts
git add electron/main.ts electron/preload.ts
git add src/App.tsx
# plus any other files touched
git commit -m "refactor(backend): strip stage machinery for single-stage architecture"
```

---

## Task 5: DB migration for stage rows

**Files:**
- Modify: `electron/store/db.ts` (or wherever the sqlite connection is initialized)

- [ ] **Step 1: Locate the DB init function**

Find where the better-sqlite3 connection is opened (likely `electron/store/db.ts` — use Grep for `new Database(` or `better-sqlite3`). This function runs once at app startup.

- [ ] **Step 2: Append a one-shot cleanup**

Immediately after table creation / existing migrations, add:

```ts
// One-shot migration: single-stage architecture retires stage 2/3/4.
// Drop any orphaned rows so UI filters / aggregates stay clean.
try {
  db.prepare('DELETE FROM stage_events WHERE stage_id > 1').run()
  db.prepare('DELETE FROM stage_status WHERE stage_id > 1').run()
} catch (err) {
  // Tables may not exist on fresh installs — that's fine.
}
```

If the table names differ in the actual codebase, adjust to match.

- [ ] **Step 3: Typecheck**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 4: Commit**

```bash
git add electron/store/db.ts
git commit -m "refactor(db): drop stage_events/stage_status rows for stages 2-4 on startup"
```

---

## Task 6: One-shot migrate existing stage1 artifacts

**Files:**
- Modify: `electron/store/paths.ts`

- [ ] **Step 1: Add a migration helper**

In `electron/store/paths.ts`, add (exported) function:

```ts
/**
 * One-shot: for each project that still has a legacy
 * `workspaces/stage1_design/<plan>.md`, copy the file into the new
 * `<target_repo>/.multi-ai-code/designs/<plan>.md` location before the
 * `workspaces/` directory itself is removed in `ensureRootDir`.
 * Safe to call repeatedly; overwrites only if target doesn't exist.
 */
export async function migrateLegacyStage1Artifacts(): Promise<void> {
  let entries: string[]
  try {
    entries = await fs.readdir(projectsDir())
  } catch {
    return
  }
  for (const pid of entries) {
    const pdir = projectDir(pid)
    const legacyDir = join(pdir, 'workspaces', 'stage1_design')
    let files: string[]
    try {
      files = await fs.readdir(legacyDir)
    } catch {
      continue
    }
    let meta: { target_repo?: string } | null = null
    try {
      meta = JSON.parse(await fs.readFile(join(pdir, 'project.json'), 'utf8'))
    } catch {
      /* missing or unreadable — skip migration for this project */
      continue
    }
    if (!meta?.target_repo) continue
    const dest = designArchiveDir(meta.target_repo)
    try {
      await fs.mkdir(dest, { recursive: true })
    } catch {
      continue
    }
    for (const f of files) {
      if (!f.endsWith('.md')) continue
      const src = join(legacyDir, f)
      const tgt = join(dest, f)
      try {
        // skip if target already exists (don't clobber user's current plan)
        await fs.access(tgt)
        continue
      } catch {
        /* target missing — copy */
      }
      try {
        await fs.copyFile(src, tgt)
      } catch {
        /* tolerate per-file failure */
      }
    }
  }
}
```

- [ ] **Step 2: Call from `ensureRootDir`**

Still in `paths.ts`, update `ensureRootDir` to call `migrateLegacyStage1Artifacts` BEFORE the workspaces cleanup block:

```ts
export async function ensureRootDir(): Promise<void> {
  await fs.mkdir(projectsDir(), { recursive: true })
  // Salvage any legacy stage-1 design md files before wiping workspaces/.
  await migrateLegacyStage1Artifacts()
  // Retire workspaces/ (see comment in Task 1).
  try {
    const entries = await fs.readdir(projectsDir())
    for (const id of entries) {
      const ws = join(projectsDir(), id, 'workspaces')
      try {
        await fs.rm(ws, { recursive: true, force: true })
      } catch {
        /* missing or locked */
      }
    }
  } catch {
    /* empty */
  }
}
```

- [ ] **Step 3: Add a test**

In `electron/store/paths.test.ts`:

```ts
it('migrateLegacyStage1Artifacts copies stage1 md into .multi-ai-code/designs/', async () => {
  const root = await fs.mkdtemp(join(tmpdir(), 'mac-migrate-'))
  process.env.MULTI_AI_ROOT = root
  try {
    const pid = 'p_legacy'
    const pdir = join(root, 'projects', pid)
    const legacyDir = join(pdir, 'workspaces', 'stage1_design')
    await fs.mkdir(legacyDir, { recursive: true })
    await fs.writeFile(join(legacyDir, 'my-plan.md'), '# plan body')
    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-migrate-repo-'))
    await fs.writeFile(
      join(pdir, 'project.json'),
      JSON.stringify({ id: pid, name: pid, target_repo: targetRepo })
    )
    await migrateLegacyStage1Artifacts()
    const migrated = await fs.readFile(
      join(targetRepo, '.multi-ai-code', 'designs', 'my-plan.md'),
      'utf8'
    )
    expect(migrated).toBe('# plan body')
    await fs.rm(targetRepo, { recursive: true, force: true })
  } finally {
    delete process.env.MULTI_AI_ROOT
    await fs.rm(root, { recursive: true, force: true })
  }
})
```

Add `migrateLegacyStage1Artifacts` to the imports at top of the test file.

- [ ] **Step 4: Verify**

```bash
npm run typecheck
npx vitest run electron/store/paths.test.ts
```

- [ ] **Step 5: Commit**

```bash
git add electron/store/paths.ts electron/store/paths.test.ts
git commit -m "feat(paths): one-shot migrate legacy stage1 artifacts to designs/"
```

---

## Task 7: New `MainPanel.tsx` component + delete `StagePanel.tsx`

**Files:**
- Create: `src/components/MainPanel.tsx`
- Delete: `src/components/StagePanel.tsx`

- [ ] **Step 1: Inspect current `StagePanel.tsx`**

Read it to identify what's stage-specific (STAGES, badges, feedback props, advance hooks, STAGE_DONE parsing) vs what's infra (xterm mount, dragdrop paste, context-menu, resize observer, session IPC wiring).

- [ ] **Step 2: Write `src/components/MainPanel.tsx`**

A simplified panel. Required props (interface prefixed comments adequate — be strict about what's needed):

```tsx
import { useCallback, useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { SearchAddon } from '@xterm/addon-search'
import '@xterm/xterm/css/xterm.css'
import { getTheme, THEME_CHANGE_EVENT, type Theme } from '../utils/theme.js'

const XTERM_DARK_THEME = {
  background: '#1e1e1e',
  foreground: '#e6e6e6'
}

const XTERM_LIGHT_THEME = {
  background: '#FFFFFF',
  foreground: '#202124',
  cursor: '#202124',
  cursorAccent: '#FFFFFF',
  selectionBackground: 'rgba(26, 115, 232, 0.2)',
  black: '#202124',
  red: '#D93025',
  green: '#1E8E3E',
  yellow: '#B06000',
  blue: '#1A73E8',
  magenta: '#9334E6',
  cyan: '#0086A3',
  white: '#5F6368',
  brightBlack: '#5F6368',
  brightRed: '#D93025',
  brightGreen: '#1E8E3E',
  brightYellow: '#B06000',
  brightBlue: '#1A73E8',
  brightMagenta: '#9334E6',
  brightCyan: '#0086A3',
  brightWhite: '#202124'
}

function xtermThemeFor(t: Theme): typeof XTERM_DARK_THEME {
  return t === 'dark' ? XTERM_DARK_THEME : XTERM_LIGHT_THEME
}

export interface MainPanelProps {
  sessionId: string
  projectId: string
  projectDir: string
  /** cwd = target_repo. */
  cwd: string
  /** Current plan name. */
  planName: string
  /** Called when user clicks Start. */
  onStart: () => void
  /** Called when user clicks Stop. */
  onStop: () => void
  /** Called when user clicks Restart. */
  onRestart: () => void
  /** Called when user clicks "Diff 审查". */
  onOpenDiff: () => void
  /** Session running state (driven from App.tsx). */
  status: 'idle' | 'running' | 'exited'
  /** Disabled everything while session is spawning or no project. */
  disabled?: boolean
}

export default function MainPanel(props: MainPanelProps): JSX.Element {
  const containerRef = useRef<HTMLDivElement>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)
  const searchRef = useRef<SearchAddon | null>(null)
  const unsubRef = useRef<Array<() => void>>([])
  const [dragActive, setDragActive] = useState(false)

  // Xterm instantiation — mirrors legacy StagePanel's effect minus stage logic.
  useEffect(() => {
    if (!containerRef.current) return
    const term = new Terminal({
      fontSize: 15,
      lineHeight: 1.2,
      fontFamily: 'Menlo, Monaco, Consolas, "Courier New", monospace',
      cursorBlink: true,
      convertEol: true,
      theme: xtermThemeFor(getTheme()),
      allowProposedApi: true
    })
    const onThemeChange = (e: Event) => {
      term.options.theme = xtermThemeFor((e as CustomEvent<Theme>).detail)
    }
    window.addEventListener(THEME_CHANGE_EVENT, onThemeChange)
    const fit = new FitAddon()
    const search = new SearchAddon()
    term.loadAddon(fit)
    term.loadAddon(search)
    searchRef.current = search
    term.open(containerRef.current)
    try {
      fit.fit()
    } catch {
      /* ignore */
    }
    termRef.current = term
    fitRef.current = fit

    term.onData((data) => {
      window.api.cc.write(props.sessionId, data)
    })

    const offData = window.api.cc.onData((evt) => {
      if (evt.sessionId !== props.sessionId) return
      term.write(evt.data)
    })
    unsubRef.current.push(offData)

    const offExit = window.api.cc.onExit((evt) => {
      if (evt.sessionId !== props.sessionId) return
      // status transition handled in App.tsx via same event
    })
    unsubRef.current.push(offExit)

    const ro = new ResizeObserver(() => {
      try {
        fit.fit()
        const { cols, rows } = term
        window.api.cc.resize(props.sessionId, cols, rows)
      } catch {
        /* ignore */
      }
    })
    ro.observe(containerRef.current)

    return () => {
      ro.disconnect()
      window.removeEventListener(THEME_CHANGE_EVENT, onThemeChange)
      unsubRef.current.forEach((fn) => fn())
      unsubRef.current = []
      term.dispose()
      termRef.current = null
      fitRef.current = null
    }
  }, [props.sessionId])

  // Drag & drop file-path paste (unchanged from StagePanel behavior).
  const handleDragOver = useCallback((e: React.DragEvent<HTMLDivElement>) => {
    e.preventDefault()
    setDragActive(true)
  }, [])
  const handleDragLeave = useCallback((e: React.DragEvent<HTMLDivElement>) => {
    if (e.currentTarget.contains(e.relatedTarget as Node | null)) return
    setDragActive(false)
  }, [])
  const handleDrop = useCallback((e: React.DragEvent<HTMLDivElement>) => {
    e.preventDefault()
    setDragActive(false)
    const files = Array.from(e.dataTransfer.files)
    if (files.length === 0) return
    const paths = files.map((f) => (f as unknown as { path: string }).path).filter(Boolean)
    if (paths.length === 0) return
    window.api.cc.write(props.sessionId, paths.join(' '))
    termRef.current?.focus()
  }, [props.sessionId])

  return (
    <div className="main-panel">
      <div className="main-panel-head">
        <div className="main-panel-title">
          <span className="main-panel-plan">{props.planName || '(未选择方案)'}</span>
          <span className={`tile-badge ${props.status}`}>
            {props.status === 'running' ? '运行中' : props.status === 'exited' ? '已退出' : '待启动'}
          </span>
        </div>
        <div className="main-panel-actions">
          <button
            className="tile-btn"
            onClick={props.onOpenDiff}
            disabled={props.disabled || props.status !== 'running'}
            title="打开 Diff 审查（把批注回灌给当前会话）"
          >
            Diff 审查
          </button>
          {props.status === 'running' ? (
            <button
              className="tile-btn"
              onClick={props.onStop}
              disabled={props.disabled}
            >
              停止
            </button>
          ) : (
            <button
              className="tile-btn"
              onClick={props.onStart}
              disabled={props.disabled}
            >
              {props.status === 'exited' ? '重启' : '启动'}
            </button>
          )}
        </div>
      </div>
      <div
        className="main-panel-body term-host"
        ref={containerRef}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        onDrop={handleDrop}
      >
        {dragActive && <div className="drop-hint">松开以粘贴文件路径</div>}
      </div>
    </div>
  )
}
```

- [ ] **Step 3: Delete `src/components/StagePanel.tsx`**

```bash
rm src/components/StagePanel.tsx
```

- [ ] **Step 4: Typecheck**

```bash
npm run typecheck
```

At this point App.tsx still imports StagePanel — typecheck will fail. **Keep going — the App.tsx rewrite (Task 9) closes the loop.** For this task's commit, accept transient App.tsx breakage. To keep the repo shippable per task, instead: update App.tsx's StagePanel import to MainPanel here with a minimal wrapper (props that still make sense pre-Task-9 rewrite).

Actually, do that: in App.tsx, change the import from `./components/StagePanel` to `./components/MainPanel` and adapt the four call sites (one per stage tile) to render MainPanel instead. **Note**: until Task 9 rewrites App.tsx, this will still render 4 MainPanels in a grid — ugly but compiles. Task 9 will collapse to one.

Concretely for this step, in App.tsx change each of the 4 StagePanel usages to MainPanel, and replace stage-specific props like `stageId={1}` / `stageName="方案设计"` / `badgeOverride` / `autoStart` with MainPanel-compatible props (planName from state, status placeholder, onStart/onStop/onRestart/onOpenDiff handlers — the App-level state machine is messy at this point but typecheck should pass).

If this is too disruptive, a simpler intermediate: keep a thin `StagePanel.tsx` shim that re-exports MainPanel with a default planName prop, so App.tsx compiles unchanged until Task 9. Either approach is acceptable.

- [ ] **Step 5: Tests**

```bash
npm run test
```

- [ ] **Step 6: Commit**

```bash
git add src/components/MainPanel.tsx src/components/StagePanel.tsx src/App.tsx
git commit -m "feat(MainPanel): introduce single-stage panel; retire StagePanel"
```

---

## Task 8: `AiSettingsDialog.tsx` (rename + simplify StageSettingsDialog)

**Files:**
- Create: `src/components/AiSettingsDialog.tsx`
- Delete: `src/components/StageSettingsDialog.tsx`
- Modify: `src/App.tsx` (update import + usage)
- Modify: `electron/main.ts` (project:get-stage-configs / project:set-stage-configs endpoints: add simpler single-config variants or mutate schema)
- Modify: `electron/preload.ts` (add new methods matching)

- [ ] **Step 1: Inspect StageSettingsDialog**

Read `src/components/StageSettingsDialog.tsx` to see what fields it currently edits (per-stage command / args / env / skip).

- [ ] **Step 2: Write `AiSettingsDialog.tsx`**

Create the simplified dialog. Props + structure:

```tsx
import { useEffect, useState } from 'react'

export interface AiSettings {
  /** 'claude' | 'codex' */
  ai_cli: 'claude' | 'codex'
  /** Optional override of the CLI binary name (defaults to ai_cli). */
  command?: string
  /** Extra args appended to the default ones. */
  args?: string[]
  env?: Record<string, string>
}

export interface AiSettingsDialogProps {
  projectId: string
  initial: AiSettings
  onClose: () => void
  onSaved: (next: AiSettings) => void
}

export default function AiSettingsDialog(props: AiSettingsDialogProps): JSX.Element {
  const [aiCli, setAiCli] = useState<'claude' | 'codex'>(props.initial.ai_cli ?? 'claude')
  const [command, setCommand] = useState<string>(props.initial.command ?? '')
  const [argsText, setArgsText] = useState<string>((props.initial.args ?? []).join(' '))
  const [envText, setEnvText] = useState<string>(
    Object.entries(props.initial.env ?? {})
      .map(([k, v]) => `${k}=${v}`)
      .join('\n')
  )
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const handleSave = async () => {
    setSaving(true)
    setError(null)
    const next: AiSettings = {
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
    try {
      const res = await window.api.project.setAiSettings(props.projectId, next)
      if (!res.ok) throw new Error(res.error ?? 'save failed')
      props.onSaved(next)
      props.onClose()
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="modal-backdrop" onClick={props.onClose}>
      <div className="modal ai-settings-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-head">
          <h3>AI 设置</h3>
          <button className="modal-close" onClick={props.onClose} aria-label="关闭">×</button>
        </div>
        <div className="modal-body">
          <div className="modal-field">
            <label>AI CLI</label>
            <select
              value={aiCli}
              onChange={(e) => setAiCli(e.target.value as 'claude' | 'codex')}
            >
              <option value="claude">Claude Code (默认)</option>
              <option value="codex">Codex (--full-auto)</option>
            </select>
          </div>
          <div className="modal-field">
            <label>Binary override (留空使用默认)</label>
            <input
              type="text"
              value={command}
              onChange={(e) => setCommand(e.target.value)}
              placeholder={aiCli === 'codex' ? 'codex' : 'claude'}
            />
          </div>
          <div className="modal-field">
            <label>附加 args (空格分隔)</label>
            <input
              type="text"
              value={argsText}
              onChange={(e) => setArgsText(e.target.value)}
              placeholder="--foo --bar"
            />
          </div>
          <div className="modal-field">
            <label>环境变量 (每行 KEY=VALUE)</label>
            <textarea
              value={envText}
              onChange={(e) => setEnvText(e.target.value)}
              rows={4}
            />
          </div>
          {error && <div className="modal-error">⚠ {error}</div>}
        </div>
        <div className="modal-actions">
          <button className="drawer-btn" onClick={props.onClose}>取消</button>
          <button
            className="drawer-btn primary"
            onClick={handleSave}
            disabled={saving}
          >
            {saving ? '保存中…' : '保存'}
          </button>
        </div>
      </div>
    </div>
  )
}
```

- [ ] **Step 3: Add IPC endpoint**

In `electron/main.ts` replace / augment the existing `project:get-stage-configs` and `project:set-stage-configs` handlers with:

```ts
ipcMain.handle('project:get-ai-settings', async (_e, { id }: { id: string }) => {
  const pdir = projectDir(id)
  try {
    const raw = await fs.readFile(join(pdir, 'project.json'), 'utf8')
    const meta = JSON.parse(raw) as { ai_settings?: AiSettings }
    return meta.ai_settings ?? { ai_cli: 'claude' as const }
  } catch {
    return { ai_cli: 'claude' as const }
  }
})

ipcMain.handle(
  'project:set-ai-settings',
  async (_e, { id, settings }: { id: string; settings: AiSettings }) => {
    const pdir = projectDir(id)
    const metaPath = join(pdir, 'project.json')
    const raw = await fs.readFile(metaPath, 'utf8')
    const meta = JSON.parse(raw) as Record<string, unknown>
    meta.ai_settings = settings
    await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
    return { ok: true }
  }
)
```

Types and imports:
- Add `AiSettings` interface definition at the top of main.ts or import from a shared types file.
- Keep the legacy `project:get-stage-configs` / `project:set-stage-configs` handlers removed or as no-ops (return `{}`). Remove if nothing in the new UI calls them.

- [ ] **Step 4: Expose in preload.ts**

Replace the legacy `getStageConfigs` / `setStageConfigs` entries with:

```ts
getAiSettings: (id: string) =>
  ipcRenderer.invoke('project:get-ai-settings', { id }) as Promise<AiSettings>,
setAiSettings: (id: string, settings: AiSettings) =>
  ipcRenderer.invoke('project:set-ai-settings', { id, settings }) as Promise<{
    ok: boolean
    error?: string
  }>,
```

Add the `AiSettings` type to the preload's exports.

- [ ] **Step 5: Delete `StageSettingsDialog.tsx`**

```bash
rm src/components/StageSettingsDialog.tsx
```

- [ ] **Step 6: Update App.tsx import**

Change `import StageSettingsDialog from './components/StageSettingsDialog'` → `import AiSettingsDialog from './components/AiSettingsDialog'`. Update the rendering site to pass the new props (`initial`, `onSaved`). Update the `showStageSettings` state to `showAiSettings`. Update any `stageConfigs` state to an `aiSettings` state initialized via `window.api.project.getAiSettings(projectId)`.

- [ ] **Step 7: Verify**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 8: Commit**

```bash
git add src/components/AiSettingsDialog.tsx src/components/StageSettingsDialog.tsx
git add electron/main.ts electron/preload.ts src/App.tsx
git commit -m "refactor(settings): AiSettingsDialog replaces per-stage settings"
```

---

## Task 9: `App.tsx` rewrite — single panel

**Files:**
- Modify: `src/App.tsx`

This task collapses the 4-tile grid into a single `MainPanel`, wires up plan selector + start/stop flow, and removes stage-progression state.

- [ ] **Step 1: Read current App.tsx**

Identify state variables tied to 4 stages:
- `stageStatus: Record<number, string>` → collapses to a single `status`
- `stageConfigs` → replaced by `aiSettings` (done in Task 8)
- `pendingDone: StageDoneEvent | null` → removed
- `feedbackFrom: number | null` + `feedbackForcedTarget` → removed
- `zoomedStage: number | null` → removed (no grid, no zoom)
- `planStagesDone: Record<number, boolean>` → removed
- `nextStageFor` helper → removed
- `planProgress` bar (if any) rendering → simplified to "当前方案：<name> (idle|running|exited)"

- [ ] **Step 2: Draft the new render**

Top-level structure:

```tsx
<div className={`app ${theme === 'dark' ? 'theme-dark' : ''}`}>
  <Topbar ... />
  <PlanNameBar ... />
  <div className="main-split">
    <MainPanel
      sessionId={sessionId}
      projectId={currentProjectId}
      projectDir={projectDir}
      cwd={targetRepo}
      planName={planName}
      status={status}
      onStart={handleStart}
      onStop={handleStop}
      onRestart={handleRestart}
      onOpenDiff={() => setDiffReviewOpen(true)}
      disabled={!currentProjectId || !planName}
    />
  </div>
  {/* existing dialogs, drawers, toasts, error panel, command palette */}
</div>
```

Replace the `.grid` + 4 StagePanel tiles with the single MainPanel.

- [ ] **Step 3: Rewrite `handleStart`**

```tsx
const handleStart = useCallback(async () => {
  if (!currentProjectId || !planName.trim()) return
  const proj = projects.find((p) => p.id === currentProjectId)
  if (!proj?.target_repo) {
    showToast('warn', '当前项目未设置 target_repo，请先在项目选择器里选一个代码仓库')
    return
  }
  const planAbsPath = planSources[planName]
    ? planSources[planName]
    : join(proj.target_repo, '.multi-ai-code', 'designs', sanitize(planName) + '.md')
  let planContent: string | null = null
  try {
    planContent = await window.api.file.read(planAbsPath)
  } catch {
    planContent = null
  }
  const initialMessage = formatInitialMessageForRenderer({
    planName,
    planAbsPath,
    planContent
  })
  const args = deriveArgsFromAiSettings(aiSettings)
  const command = aiSettings.command ?? aiSettings.ai_cli ?? 'claude'
  const sid = `sess_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 6)}`
  setSessionId(sid)
  setStatus('running')
  const res = await window.api.cc.spawn({
    sessionId: sid,
    projectId: currentProjectId,
    projectDir: projectDirFor(currentProjectId),
    targetRepo: proj.target_repo,
    planName,
    planAbsPath: planContent === null ? planAbsPath : planAbsPath,
    planPending: planContent === null,
    initialUserMessage: initialMessage,
    command,
    args,
    env: aiSettings.env ?? {}
  })
  if (!res.ok) {
    showToast('error', res.error ?? '启动失败')
    setStatus('idle')
  }
}, [currentProjectId, planName, projects, planSources, aiSettings])
```

Before writing the handler, create `src/utils/session-message-format.ts`:

```ts
// Mirror of electron/orchestrator/session-messages.ts — renderer-safe copy.
// Keep in sync manually when the backend copy changes.

export interface SessionAnnotation {
  file: string
  lineRange: string
  snippet: string
  comment: string
}

function sanitize(label: string): string {
  return label
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
    .replace(/\s+/g, '_')
    .slice(0, 80)
}

export function planNameToFilename(name: string): string {
  const safe = name && name.trim() ? sanitize(name) : 'design'
  return `${safe}.md`
}

export interface InitialMessageParams {
  planName: string
  planAbsPath: string
  planContent: string | null
}

export function formatInitialMessage(p: InitialMessageParams): string {
  if (p.planContent !== null) {
    return [
      p.planContent.trimEnd(),
      '',
      '---',
      '',
      '请基于当前方案继续工作（写代码 / 根据批注调整）。'
    ].join('\n')
  }
  return [
    `本次方案名：${p.planName}。`,
    '',
    `请先与用户对话澄清需求、确认方向，然后把方案写到 \`${p.planAbsPath}\`（完整绝对路径），再继续实施。`
  ].join('\n')
}

export interface AnnotationsForSessionParams {
  annotations: SessionAnnotation[]
  generalComment: string
  planAbsPath: string
}

export function formatAnnotationsForSession(
  p: AnnotationsForSessionParams
): string {
  const lines: string[] = []
  lines.push('# 用户批注')
  lines.push('')
  lines.push(
    `以下是用户对当前改动的批注，请严格按照批注执行：修改代码、或更新方案文档（\`${p.planAbsPath}\`）。`
  )
  lines.push('')
  lines.push('## 逐行批注')
  lines.push('')
  for (const a of p.annotations) {
    lines.push(`### \`${a.file}:${a.lineRange}\``)
    lines.push('')
    for (const sl of a.snippet.split('\n')) {
      lines.push(`> ${sl}`)
    }
    lines.push('')
    lines.push(a.comment)
    lines.push('')
  }
  const gc = p.generalComment.trim()
  if (gc.length > 0) {
    lines.push('## 整体意见')
    lines.push('')
    lines.push(gc)
    lines.push('')
  }
  lines.push('---')
  lines.push('')
  lines.push('请按照以上批注调整代码 / 方案，完成后在终端里简述改了什么。')
  return lines.join('\n')
}
```

Also add a helper in the same file for reading plan content via an IPC. The existing IPC surface (check `electron/preload.ts`) likely exposes a generic file-read via `window.api.fs?.readFile` or a path-specific one. If none exists, add:

**`electron/main.ts`:**
```ts
ipcMain.handle('fs:read-utf8', async (_e, { path }: { path: string }) => {
  try {
    const content = await fs.readFile(path, 'utf8')
    return { ok: true as const, content }
  } catch (err: unknown) {
    return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
  }
})
```

**`electron/preload.ts`:**
```ts
fs: {
  readUtf8: (path: string) =>
    ipcRenderer.invoke('fs:read-utf8', { path }) as Promise<
      { ok: true; content: string } | { ok: false; error: string }
    >
}
```

Then in App.tsx's `handleStart`, use `planNameToFilename` + `window.api.fs.readUtf8(planAbsPath)` + the `formatInitialMessage` helper from `session-message-format.ts`.

- [ ] **Step 4: Rewrite `handleStop` and `handleRestart`**

```tsx
const handleStop = useCallback(async () => {
  if (!sessionId) return
  await window.api.cc.kill(sessionId)
  setStatus('exited')
}, [sessionId])

const handleRestart = useCallback(async () => {
  if (sessionId) {
    await window.api.cc.kill(sessionId)
  }
  setSessionId(null)
  setStatus('idle')
  // User clicks Start again; alternately, auto-start:
  setTimeout(() => handleStart(), 50)
}, [sessionId, handleStart])
```

- [ ] **Step 5: Remove dead state + helpers**

Delete everything related to 4 stages from App.tsx:
- `stageStatus`, `setStageStatus`, `handleStatusChange`
- `zoomedStage`, `setZoomedStage`
- `killAllNonce` (replaced by single `handleStop`)
- `feedbackFrom`, `feedbackForcedTarget`
- `pendingDone` + CompletionDrawer rendering
- `planStagesDone`, `setPlanStagesDone`
- `nextStageFor` helper
- `STAGES` constant
- `FeedbackDialog` rendering
- The `.grid` + 4-tile map render block
- Any `window.api.stage.*` listener setup (already removed in Task 4)
- Imports of `StagePanel`, `CompletionDrawer`, `FeedbackDialog`, `ReviewChecklist`, `StageSettingsDialog` (latter replaced by AiSettingsDialog in Task 8)

Keep:
- Project picker state
- Plan selector state (`planName`, `planList`, `planSources`)
- Dialog visibility flags (`showDoctor`, `showTimeline`, `showOnboarding`, `showCmdk`, `showGlobalSearch`, `showAiSettings`, `showTemplates`, `previewImport`, `planReview`, `diffReviewOpen`)
- Theme state + handler
- Project-bound reload/load helpers

- [ ] **Step 6: Rewrite the `diffReviewOpen` handler**

Change the DiffViewerDialog's "发送" callback. Old code emitted a handoff IPC to Stage 3. New code formats annotations via `session-message-format.ts` and calls `window.api.cc.write(sessionId, formattedText)`.

```tsx
<DiffViewerDialog
  open={diffReviewOpen}
  onClose={() => setDiffReviewOpen(false)}
  projectId={currentProjectId}
  targetRepo={currentTargetRepo}
  onSendAnnotations={async (payload) => {
    if (!sessionId) return
    const planAbsPath = planSources[planName] ??
      join(currentTargetRepo, '.multi-ai-code', 'designs', sanitize(planName) + '.md')
    const text = formatAnnotationsForSession({
      annotations: payload.annotations,
      generalComment: payload.generalComment,
      planAbsPath
    })
    window.api.cc.write(sessionId, text + '\r')
    setDiffReviewOpen(false)
    showToast('info', `已发送 ${payload.annotations.length} 条批注到会话`)
  }}
/>
```

(Adjust prop names to match the actual `DiffViewerDialogProps` interface in `src/components/DiffViewerDialog.tsx`.)

- [ ] **Step 7: Verify**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 8: Commit**

```bash
git add src/App.tsx src/utils/session-message-format.ts
git commit -m "feat(App): single-panel layout; wire plan start/stop/diff flow"
```

---

## Task 10: Delete dead components

**Files:**
- Delete: `src/components/CompletionDrawer.tsx`
- Delete: `src/components/FeedbackDialog.tsx`
- Delete: `src/components/ReviewChecklist.tsx`

- [ ] **Step 1: Verify no imports remain**

```bash
grep -rn "CompletionDrawer\|FeedbackDialog\|ReviewChecklist" src/ electron/
```

If any match appears, Task 9 is incomplete — fix those imports first (in App.tsx or anywhere else).

- [ ] **Step 2: Delete the files**

```bash
rm src/components/CompletionDrawer.tsx
rm src/components/FeedbackDialog.tsx
rm src/components/ReviewChecklist.tsx
```

- [ ] **Step 3: Verify**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 4: Commit**

```bash
git add src/components/CompletionDrawer.tsx src/components/FeedbackDialog.tsx src/components/ReviewChecklist.tsx
git commit -m "refactor(frontend): delete stage-progression components"
```

---

## Task 11: `DiffViewerDialog.tsx` — annotations send to live session

**Files:**
- Modify: `src/components/DiffViewerDialog.tsx`

- [ ] **Step 1: Update dialog title + send button copy**

Locate the header string and change `Diff 审查 · 代码标注反馈给 Stage 3` → `Diff 审查 · 代码标注回灌给当前会话`.

Locate the send button label and change `发送到 Stage 3 (N 条批注)` → `发送到会话 (N 条批注)`.

- [ ] **Step 2: Replace the send handler interface**

Find the existing `onSend` or equivalent prop. It likely currently takes `(annotations, generalComment) => void`. Rename to `onSendAnnotations` or leave the name if preferred; the shape stays the same but the parent now writes to pty via `cc.write` (Task 9 already wired this).

If the dialog currently calls `window.api.stage.injectHandoff(...)` internally, remove that call — delegate to the `onSend` prop instead.

- [ ] **Step 3: Disabled state when session is not running**

Add a prop `sessionRunning: boolean`. When false:
- The "发送到会话" button is disabled
- Show an inline hint: `会话未启动 — 先启动再发送批注`

App.tsx passes `sessionRunning={status === 'running'}`.

- [ ] **Step 4: Verify**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 5: Commit**

```bash
git add src/components/DiffViewerDialog.tsx src/App.tsx
git commit -m "refactor(DiffViewer): annotations go to live session, not Stage 3 handoff"
```

---

## Task 12: `TemplatesDialog.tsx` — collapse per-stage templates

**Files:**
- Modify: `src/components/TemplatesDialog.tsx`
- Modify: `electron/main.ts` and `electron/store/*` if templates are persisted per-stage

- [ ] **Step 1: Inspect current shape**

Read the dialog. Currently it probably stores templates keyed by stageId. New model: a single flat list.

- [ ] **Step 2: Migrate schema**

In the project.json or wherever templates persist, collapse `templates: Record<stageId, Template[]>` to `templates: Template[]`. On first load after migration, merge all stage-keyed lists into one:

In the IPC handler that loads templates:

```ts
ipcMain.handle('project:get-templates', async (_e, { id }: { id: string }) => {
  const pdir = projectDir(id)
  try {
    const raw = await fs.readFile(join(pdir, 'project.json'), 'utf8')
    const meta = JSON.parse(raw)
    if (Array.isArray(meta.templates)) return meta.templates as Template[]
    if (meta.templates && typeof meta.templates === 'object') {
      const flat: Template[] = []
      for (const k of Object.keys(meta.templates)) {
        for (const t of meta.templates[k]) flat.push(t)
      }
      return flat
    }
    return [] as Template[]
  } catch {
    return [] as Template[]
  }
})
```

Update `set-templates` to save as a flat array.

- [ ] **Step 3: UI changes**

In `TemplatesDialog.tsx`, remove the stage picker/tabs. Render a single list with add/edit/delete/inject.

- [ ] **Step 4: Verify**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 5: Commit**

```bash
git add src/components/TemplatesDialog.tsx electron/main.ts
git commit -m "refactor(Templates): collapse per-stage templates to single list"
```

---

## Task 13: `TimelineDrawer.tsx` — filter stage_id === 1

**Files:**
- Modify: `src/components/TimelineDrawer.tsx`
- Modify: `electron/main.ts` (if the IPC layer does the filtering)

- [ ] **Step 1: Filter at fetch time**

If `TimelineDrawer` calls `window.api.project.getTimeline(id)` (or similar), have the underlying SQL add `WHERE stage_id = 1` to the event query. Only stage 1 rows exist after the Task 5 DB migration, but belt-and-suspenders.

- [ ] **Step 2: Remove stage filter UI**

In the dialog's JSX, remove the "按阶段过滤" dropdown if present. The whole list is always stage 1.

- [ ] **Step 3: Verify**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 4: Commit**

```bash
git add src/components/TimelineDrawer.tsx electron/main.ts
git commit -m "refactor(Timeline): single-stage filter; drop stage picker UI"
```

---

## Task 14: CSS cleanup

**Files:**
- Modify: `src/styles.css`

- [ ] **Step 1: Remove 4-tile grid specifics**

Delete rules for `.grid`, `.grid.grid-zoomed`, `.tile.tile-hidden`, `.tile.tile-zoomed` (the 2×2 grid + zoom mechanics). Keep `.tile`, `.tile-head`, `.tile-id`, `.tile-name`, `.tile-badge`, `.tile-body`, `.tile-btn`, `.tile-progress`, `.tile-error` — MainPanel reuses `.tile-badge`, `.tile-btn`, `.term-host`, `.drop-hint`.

- [ ] **Step 2: Add `.main-panel` rules**

```css
.main-panel {
  flex: 1;
  display: flex;
  flex-direction: column;
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-lg);
  margin: var(--mac-sp-3);
  overflow: hidden;
  box-shadow: var(--mac-elev-1);
}

.main-panel-head {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-3);
  padding: var(--mac-sp-3) var(--mac-sp-4);
  background: var(--mac-surface-raised);
  border-bottom: 1px solid var(--mac-border-subtle);
}

.main-panel-title {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  flex: 1;
  min-width: 0;
  font-size: var(--mac-text-md);
  font-weight: var(--mac-weight-medium);
  color: var(--mac-fg);
}

.main-panel-plan {
  color: var(--mac-fg);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.main-panel-actions {
  display: flex;
  gap: var(--mac-sp-2);
}

.main-panel-body {
  flex: 1;
  position: relative;
  overflow: hidden;
}
```

- [ ] **Step 3: Rename `.stage-settings-*` → `.ai-settings-*`**

Use search-replace across `styles.css` for `.stage-settings-modal` → `.ai-settings-modal`, `.stage-settings-body` → `.ai-settings-body`, etc. Make sure the React side (AiSettingsDialog) uses the new class names.

- [ ] **Step 4: Drop `.main-split` padding compensation for drawer**

Task 4 of the UI redesign added `.app.has-drawer .main-split { padding-right: 420px }` for CompletionDrawer. CompletionDrawer is gone — this compensation can be removed (or left as dead CSS; low priority).

- [ ] **Step 5: Verify visually via dev server**

Not runnable from agent context. In a manual pass: `npm run dev`, open app, check main panel renders.

- [ ] **Step 6: Verify typecheck + tests (sanity)**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 7: Commit**

```bash
git add src/styles.css
git commit -m "refactor(styles): drop 4-tile grid rules; add .main-panel; rename stage-settings→ai-settings"
```

---

## Task 15: End-to-end verification checklist

**This task has no code changes.** It's a manual verification pass before declaring the refactor complete.

- [ ] **Step 1: Final typecheck + tests**

```bash
npm run typecheck
npm run test
```

Both must be green. Tests should total previous count + 7 (session-messages) + 1 (migrateLegacyStage1Artifacts).

- [ ] **Step 2: Manual verification**

Launch `npm run dev` in a real environment (not agent context) and walk through this checklist:

- [ ] App starts, shows single full-screen main panel (no 2×2 grid)
- [ ] New project: pick target_repo, add a plan name, click Start → AI spawns with Claude, cwd is target_repo, first user message reaches the CLI
- [ ] AI asks clarifying questions; after confirming direction, writes `<target_repo>/.multi-ai-code/designs/<plan>.md`
- [ ] Plan file exists on disk at the expected path
- [ ] Stop the session → kills cleanly
- [ ] Start again with same plan → AI sees plan content + "请基于当前方案继续工作"
- [ ] AI writes code to target_repo
- [ ] Open Diff 审查 → see changes, highlight a line, add annotation, add general comment, click "发送到会话"
- [ ] AI receives the batch and responds with edits
- [ ] Switch AI to Codex in AI Settings → restart session → spawns with codex --full-auto
- [ ] Import an external plan file → select file → plan archives to external path → start → AI uses external plan
- [ ] Command palette (⌘K) opens, theme toggle works, timeline drawer only shows stage 1 events
- [ ] Legacy projects with `workspaces/` on disk: start app → `workspaces/` is gone, stage 1 md is copied to new location

- [ ] **Step 3: Report completion**

If all checks pass, the refactor is done. If any fail, open a follow-up issue with the failing step noted.

No commit for this task.

---

## Post-merge

Open a PR against `main` with the series of ~14 commits. In the PR description, include the checklist from Task 15 Step 2 so reviewers verify the same flow.

## Rollback strategy

Each task is a standalone commit. If a regression is found:
- Task 14 (CSS) can be reverted without impacting functionality
- Task 11-13 (dialogs) revertable independently
- Task 9 (App rewrite) is the keystone — reverting requires reverting Tasks 7, 8, 9, 10 together
- Task 4 (backend strip) is the most invasive — revert requires restoring StageDoneScanner.ts + all prompt files + old IPC

Prefer fix-forward unless a critical bug blocks productive work.
