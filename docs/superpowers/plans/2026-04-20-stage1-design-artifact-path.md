# Stage1 Design Artifact Path Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Stage1 final design artifact output from `<projectDir>/workspaces/stage1_design/<plan>.md` to `<target_repo>/.multi-ai-code/designs/<plan>.md`, keeping Stage1 cwd isolated.

**Architecture:** Introduce a single absolute-path resolver `resolveStageArtifactAbs(projectDir, stageId, label)` that reads `project.json` to derive `<target_repo>/.multi-ai-code/designs/…` for stage 1, and keeps `join(projectDir, relative)` behavior for stages 2-4. All ptyManager call sites that previously did `join(pdir, stageArtifactPath(...))` switch to this resolver. `stageArtifactPath` itself gets an optional `targetRepo` param so the legacy call sites (events, DB, UI labels) can continue returning a display-path.

**Tech Stack:** TypeScript (Electron main), Node `path`/`fs`, Vitest.

**Spec reference:** `docs/superpowers/specs/2026-04-19-stage1-design-artifact-path.md`

---

## File Structure

**Modify:**
- `electron/store/paths.ts` — add `designArchiveDir(targetRepo)` + create that dir in `createProjectLayout`.
- `electron/orchestrator/prompts.ts` — extend `stageArtifactPath`, add `resolveStageArtifactAbs`, update `renderTemplate.planPending`.
- `electron/cc/ptyManager.ts` — 6 call sites switch to `resolveStageArtifactAbs`; 1 kickoff string updated.
- `electron/prompts/stage1-design.md` — update example path string.

**Create:**
- `electron/orchestrator/prompts.test.ts` — unit tests for path resolution.
- `electron/store/paths.test.ts` — unit tests for `designArchiveDir` + `createProjectLayout`.

---

## Task 1: Add `designArchiveDir` helper and wire into project layout

**Files:**
- Modify: `electron/store/paths.ts:22-92`
- Test: `electron/store/paths.test.ts` (new)

- [ ] **Step 1: Write failing test for `designArchiveDir`**

Create `electron/store/paths.test.ts`:

```ts
import { describe, it, expect, afterEach, beforeEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { designArchiveDir, createProjectLayout, workspaceDir } from './paths.js'

describe('designArchiveDir', () => {
  it('returns <target_repo>/.multi-ai-code/designs', () => {
    expect(designArchiveDir('/tmp/my-repo')).toBe('/tmp/my-repo/.multi-ai-code/designs')
  })

  it('handles trailing slash on target_repo', () => {
    expect(designArchiveDir('/tmp/my-repo/')).toBe('/tmp/my-repo/.multi-ai-code/designs')
  })
})

describe('createProjectLayout', () => {
  let root: string
  let targetRepo: string
  beforeEach(async () => {
    root = await fs.mkdtemp(join(tmpdir(), 'mac-paths-'))
    process.env.MULTI_AI_ROOT = root
    targetRepo = join(root, 'target')
    await fs.mkdir(targetRepo, { recursive: true })
  })
  afterEach(async () => {
    delete process.env.MULTI_AI_ROOT
    await fs.rm(root, { recursive: true, force: true })
  })

  it('creates .multi-ai-code/designs under target_repo', async () => {
    await createProjectLayout('p_test', targetRepo)
    const stat = await fs.stat(designArchiveDir(targetRepo))
    expect(stat.isDirectory()).toBe(true)
  })

  it('still creates the isolated stage1_design workspace (used as cwd)', async () => {
    await createProjectLayout('p_test', targetRepo)
    const stat = await fs.stat(workspaceDir('p_test', 1))
    expect(stat.isDirectory()).toBe(true)
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run electron/store/paths.test.ts`
Expected: FAIL — `designArchiveDir is not exported` or similar.

- [ ] **Step 3: Implement `designArchiveDir` and update `createProjectLayout`**

Edit `electron/store/paths.ts`:

Add after line 35 (after `artifactsDir`):

```ts
export function designArchiveDir(targetRepo: string): string {
  return join(targetRepo.replace(/[\/\\]+$/, ''), '.multi-ai-code', 'designs')
}
```

Inside `createProjectLayout`, after line 51 (after creating `workspaces` dir) add:

```ts
  // Stage 1 designs now archive into target_repo/.multi-ai-code/designs/
  // (previously lived in workspaces/stage1_design). The workspace dir below
  // is still created because Stage 1 uses it as an isolated empty cwd.
  await fs.mkdir(designArchiveDir(targetRepoPath), { recursive: true })
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run electron/store/paths.test.ts`
Expected: PASS (3 tests).

- [ ] **Step 5: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add electron/store/paths.ts electron/store/paths.test.ts
git commit -m "feat(paths): add designArchiveDir and create it in project layout"
```

---

## Task 2: Extend `stageArtifactPath` and `renderTemplate` for target_repo-based Stage1 output

**Files:**
- Modify: `electron/orchestrator/prompts.ts:7-32,196-214`
- Test: `electron/orchestrator/prompts.test.ts` (new)

- [ ] **Step 1: Write failing test for extended `stageArtifactPath`**

Create `electron/orchestrator/prompts.test.ts`:

```ts
import { describe, it, expect } from 'vitest'
import { stageArtifactPath, renderTemplate } from './prompts.js'

describe('stageArtifactPath', () => {
  it('stage 1 with targetRepo returns absolute path under .multi-ai-code/designs', () => {
    expect(stageArtifactPath(1, 'my-plan', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/my-plan.md'
    )
  })

  it('stage 1 with targetRepo and no label defaults to design.md', () => {
    expect(stageArtifactPath(1, null, '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/design.md'
    )
  })

  it('stage 1 sanitizes unsafe filename characters', () => {
    expect(stageArtifactPath(1, 'foo/bar:baz', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/foo_bar_baz.md'
    )
  })

  it('stage 1 without targetRepo keeps legacy project-dir-relative path', () => {
    expect(stageArtifactPath(1, 'my-plan')).toBe(
      'workspaces/stage1_design/my-plan.md'
    )
  })

  it('stage 2-4 ignore targetRepo and return legacy relative path', () => {
    expect(stageArtifactPath(2, null, '/tmp/repo')).toBe('artifacts/impl-summary.md')
    expect(stageArtifactPath(3, null, '/tmp/repo')).toBe('artifacts/acceptance.md')
    expect(stageArtifactPath(4, null, '/tmp/repo')).toBe('artifacts/test-report.md')
  })
})

describe('renderTemplate planPending', () => {
  it('when targetRepo is set, uses <targetRepo>/.multi-ai-code/designs/<placeholder>.md', () => {
    const out = renderTemplate('ART={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: 'ignored',
      planPending: true,
      targetRepo: '/tmp/repo'
    })
    expect(out).toBe(
      'ART=/tmp/repo/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md'
    )
  })

  it('when absolute artifactPath is passed, keeps it as-is', () => {
    const out = renderTemplate('ART={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: '/abs/path.md'
    })
    expect(out).toBe('ART=/abs/path.md')
  })
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run electron/orchestrator/prompts.test.ts`
Expected: FAIL — signature doesn't accept 3rd arg / planPending path wrong.

- [ ] **Step 3: Update `STAGE_ARTIFACTS` comment and `stageArtifactPath`**

Edit `electron/orchestrator/prompts.ts`. Replace the `STAGE_ARTIFACTS` const and `stageArtifactPath` function (currently lines 7-32) with:

```ts
/**
 * Default artifact path per stage.
 * Stage 1: relative form kept only as a legacy fallback when targetRepo is
 *          not available. The canonical Stage 1 artifact lives under
 *          <target_repo>/.multi-ai-code/designs/<label>.md — see
 *          stageArtifactPath() / resolveStageArtifactAbs() below.
 * Stages 2-4: relative to project dir.
 */
export const STAGE_ARTIFACTS: Record<number, string> = {
  1: 'workspaces/stage1_design/design.md',
  2: 'artifacts/impl-summary.md',
  3: 'artifacts/acceptance.md',
  4: 'artifacts/test-report.md'
}

function sanitizeLabel(label: string): string {
  return label
    .trim()
    .replace(/[<>:"/\\|?*\x00-\x1f]/g, '_')
    .replace(/\s+/g, '_')
    .slice(0, 80)
}

/**
 * Compute a stage's artifact path.
 *
 * Stage 1:
 *   - When `targetRepo` is supplied, returns the absolute canonical path
 *     `<targetRepo>/.multi-ai-code/designs/<label>.md`. Label defaults to
 *     `design` when empty/null.
 *   - Without `targetRepo`, falls back to the project-dir-relative legacy
 *     path (kept so display/event code paths that lack targetRepo don't
 *     break).
 * Stages 2-4: always return project-dir-relative paths; `targetRepo` is
 * ignored.
 */
export function stageArtifactPath(
  stageId: number,
  label?: string | null,
  targetRepo?: string | null
): string {
  if (stageId === 1) {
    const safe = label && label.trim() ? sanitizeLabel(label) : 'design'
    if (targetRepo) {
      const root = targetRepo.replace(/[\/\\]+$/, '')
      return `${root}/.multi-ai-code/designs/${safe}.md`
    }
    return `workspaces/stage1_design/${safe}.md`
  }
  return STAGE_ARTIFACTS[stageId]
}
```

- [ ] **Step 4: Update `RenderContext` and `renderTemplate.planPending` branch**

In the same file, locate `RenderContext` (currently around line 180) — it already has `targetRepo`; no change needed.

Replace the `planPending` branch inside `renderTemplate` (currently line 199-201):

```ts
  if (ctx.planPending) {
    const root = (ctx.targetRepo ?? ctx.projectDir).replace(/[\/\\]+$/, '')
    artifactAbs = `${root}/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md`
  } else if (isAbsolute(ctx.artifactPath)) {
```

- [ ] **Step 5: Run test to verify it passes**

Run: `npx vitest run electron/orchestrator/prompts.test.ts`
Expected: PASS (all 7 test cases).

- [ ] **Step 6: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 7: Commit**

```bash
git add electron/orchestrator/prompts.ts electron/orchestrator/prompts.test.ts
git commit -m "feat(prompts): stageArtifactPath returns target_repo absolute path for stage 1"
```

---

## Task 3: Add `resolveStageArtifactAbs` helper that reads `target_repo` from project.json

**Files:**
- Modify: `electron/orchestrator/prompts.ts` (end of file)
- Test: `electron/orchestrator/prompts.test.ts` (append)

- [ ] **Step 1: Write failing test**

Append to `electron/orchestrator/prompts.test.ts`:

```ts
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { resolveStageArtifactAbs } from './prompts.js'

describe('resolveStageArtifactAbs', () => {
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

  it('stage 1 resolves to <target_repo>/.multi-ai-code/designs/<label>.md', async () => {
    const abs = await resolveStageArtifactAbs(projectDir, 1, 'my-plan')
    expect(abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'my-plan.md'))
  })

  it('stage 1 with no label uses design.md', async () => {
    const abs = await resolveStageArtifactAbs(projectDir, 1, null)
    expect(abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'design.md'))
  })

  it('stage 1 falls back to legacy path when project.json missing', async () => {
    await fs.rm(join(projectDir, 'project.json'))
    const abs = await resolveStageArtifactAbs(projectDir, 1, 'my-plan')
    expect(abs).toBe(join(projectDir, 'workspaces', 'stage1_design', 'my-plan.md'))
  })

  it('stage 2 resolves to <projectDir>/artifacts/impl-summary.md', async () => {
    const abs = await resolveStageArtifactAbs(projectDir, 2, null)
    expect(abs).toBe(join(projectDir, 'artifacts', 'impl-summary.md'))
  })

  it('stage 3 resolves to <projectDir>/artifacts/acceptance.md', async () => {
    const abs = await resolveStageArtifactAbs(projectDir, 3, null)
    expect(abs).toBe(join(projectDir, 'artifacts', 'acceptance.md'))
  })
})
```

Add `beforeEach, afterEach` to the existing `vitest` import at the top of the file if not already there.

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run electron/orchestrator/prompts.test.ts`
Expected: FAIL — `resolveStageArtifactAbs is not a function`.

- [ ] **Step 3: Implement `resolveStageArtifactAbs`**

`electron/orchestrator/prompts.ts` already imports `promises as fs` from `'fs'` at the top of the file — reuse it.

Append to the end of `electron/orchestrator/prompts.ts`:

```ts
/**
 * Resolve a stage's artifact absolute path. For Stage 1 this reads
 * project.json to pick up target_repo and returns
 * <target_repo>/.multi-ai-code/designs/<label>.md. For Stages 2-4 it joins
 * the project-dir-relative path against projectDir. If project.json is
 * missing or unparseable for a Stage 1 lookup, falls back to the legacy
 * workspaces/stage1_design/<label>.md under projectDir so the caller
 * degrades gracefully instead of throwing.
 */
export async function resolveStageArtifactAbs(
  projectDir: string,
  stageId: number,
  label?: string | null
): Promise<string> {
  let targetRepo: string | undefined
  if (stageId === 1) {
    try {
      const meta = JSON.parse(
        await fs.readFile(join(projectDir, 'project.json'), 'utf8')
      ) as { target_repo?: string }
      if (meta.target_repo) targetRepo = meta.target_repo
    } catch {
      /* fall through to legacy relative */
    }
  }
  const p = stageArtifactPath(stageId, label, targetRepo)
  return isAbsolute(p) ? p : join(projectDir, p)
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `npx vitest run electron/orchestrator/prompts.test.ts`
Expected: PASS (all tests).

- [ ] **Step 5: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 6: Commit**

```bash
git add electron/orchestrator/prompts.ts electron/orchestrator/prompts.test.ts
git commit -m "feat(prompts): add resolveStageArtifactAbs helper"
```

---

## Task 4: Migrate ptyManager.ts call sites to `resolveStageArtifactAbs`

**Files:**
- Modify: `electron/cc/ptyManager.ts` — six call sites.

This task is pure refactor + bugfix: every place that currently does `join(pdir, stageArtifactPath(…))` now calls the new resolver. Do ALL edits in this task, commit once at the end.

- [ ] **Step 1: Update imports**

In `electron/cc/ptyManager.ts` (currently lines 8-17), add `resolveStageArtifactAbs` to the import:

```ts
import {
  STAGE_ARTIFACTS,
  stageArtifactPath,
  resolveStageArtifactAbs,
  STAGE_CLI_ARGS,
  STAGE_COMMAND,
  STAGE_CWD,
  buildSystemPrompt,
  buildForwardHandoff,
  buildFeedbackHandoff
} from '../orchestrator/prompts.js'
```

- [ ] **Step 2: Update call site in `scanner.on('done')` (currently line 306-313)**

Replace:

```ts
      const fallbackArtifact =
        req.stageId === 3 ? null : stageArtifactPath(req.stageId, req.label)
      const artifactRel = meta.params.artifact ?? fallbackArtifact ?? null
      const artifactAbs = artifactRel
        ? isAbsolute(artifactRel)
          ? artifactRel
          : join(req.projectDir, artifactRel)
        : null
```

with:

```ts
      let artifactAbs: string | null = null
      let artifactRel: string | null = null
      if (meta.params.artifact) {
        artifactRel = meta.params.artifact
        artifactAbs = isAbsolute(artifactRel)
          ? artifactRel
          : join(req.projectDir, artifactRel)
      } else if (req.stageId !== 3) {
        artifactAbs = await resolveStageArtifactAbs(
          req.projectDir,
          req.stageId,
          req.label
        )
        // Report absolute path for stage 1 (target_repo-based); for stages
        // 2/4 keep the project-dir-relative form for legacy UI/event
        // consumers.
        artifactRel =
          req.stageId === 1
            ? artifactAbs
            : stageArtifactPath(req.stageId, req.label)
      }
```

- [ ] **Step 3: Update kickoff logic for Stage 1 (currently line 396-411)**

Replace:

```ts
          const planPending = req.stageId === 1 && !req.label?.trim()
          const stagePath = stageArtifactPath(req.stageId, req.label)

          // If this Stage 1 plan was previously imported from an external
          // file, archive back to that file (not workspaces/). The mapping
          // is persisted in project.json by the import flow.
          let externalArtifactAbs: string | null = null
          if (req.stageId === 1 && req.label?.trim()) {
            const sources = await readPlanSources(req.projectDir)
            const s = sources[req.label.trim()]
            if (s && isAbsolute(s)) externalArtifactAbs = s
          }

          const artifactPathForPrompt = externalArtifactAbs ?? stagePath ?? ''
          const artifactAbs =
            externalArtifactAbs ?? join(req.projectDir, stagePath ?? '')
```

with:

```ts
          const planPending = req.stageId === 1 && !req.label?.trim()

          // If this Stage 1 plan was previously imported from an external
          // file, archive back to that file (not the default location). The
          // mapping is persisted in project.json by the import flow.
          let externalArtifactAbs: string | null = null
          if (req.stageId === 1 && req.label?.trim()) {
            const sources = await readPlanSources(req.projectDir)
            const s = sources[req.label.trim()]
            if (s && isAbsolute(s)) externalArtifactAbs = s
          }

          const defaultAbs = await resolveStageArtifactAbs(
            req.projectDir,
            req.stageId,
            req.label
          )
          const artifactAbs = externalArtifactAbs ?? defaultAbs
          // For stage 1 we pass the absolute path to the prompt (since the
          // canonical location is outside the cwd). Stages 2-4 keep their
          // existing relative-form behavior so legacy prompt text stays
          // valid.
          const artifactPathForPrompt =
            req.stageId === 1
              ? artifactAbs
              : externalArtifactAbs ??
                stageArtifactPath(req.stageId, req.label) ??
                ''
```

- [ ] **Step 4: Update Stage 1 `planPending` kickoff string (currently line 437-445)**

Replace the `if (planPending)` block:

```ts
            if (planPending) {
              kickoffLines.push(
                `【重要】用户还**没有**输入方案名称。请先跟用户对话澄清需求；`,
                `在即将开始写设计文档之前，**必须先问用户**："你希望把这份方案归档成什么名字？"`,
                `拿到名字后，把方案写到 \`workspaces/stage1_design/<用户给的名字>.md\` （相对于 cwd 的父级）的对应绝对路径，`,
                `并在 STAGE_DONE 标记里用这个完整的绝对路径作为 artifact= 的值。`,
                ``,
                `准备好后，请用 brainstorming 与我开始澄清这次方案的需求与目标。`
              )
            }
```

with:

```ts
            if (planPending) {
              kickoffLines.push(
                `【重要】用户还**没有**输入方案名称。请先跟用户对话澄清需求；`,
                `在即将开始写设计文档之前，**必须先问用户**："你希望把这份方案归档成什么名字？"`,
                `拿到名字后，把方案写到 \`${targetRepo ?? req.projectDir}/.multi-ai-code/designs/<用户给的名字>.md\` 这个绝对路径，`,
                `并在 STAGE_DONE 标记里用这个完整的绝对路径作为 artifact= 的值。`,
                ``,
                `准备好后，请用 brainstorming 与我开始澄清这次方案的需求与目标。`
              )
            }
```

- [ ] **Step 5: Update `stage:inject-handoff` design-spec read (currently line 554-563)**

Replace:

```ts
      const pdir = req.projectDir ?? s.projectDir
      // Auto-load design spec for stage >= 3 (authoritative source of truth)
      let designSpec: string | null = null
      if (req.toStage >= 3) {
        try {
          const designAbs = join(pdir, stageArtifactPath(1, s.label))
          designSpec = await fs.readFile(designAbs, 'utf8')
        } catch {
          designSpec = null
        }
      }
```

with:

```ts
      const pdir = req.projectDir ?? s.projectDir
      // Auto-load design spec for stage >= 3 (authoritative source of truth)
      let designSpec: string | null = null
      if (req.toStage >= 3) {
        try {
          const designAbs = await resolveStageArtifactAbs(pdir, 1, s.label)
          designSpec = await fs.readFile(designAbs, 'utf8')
        } catch {
          designSpec = null
        }
      }
```

- [ ] **Step 6: Update `stage:inject-handoff` toStage artifact path (currently line 586)**

Replace:

```ts
      const artifactAbs = join(pdir, stageArtifactPath(req.toStage, s.label) ?? '')
```

with:

```ts
      const artifactAbs = await resolveStageArtifactAbs(pdir, req.toStage, s.label)
```

- [ ] **Step 7: Update `stage:trigger-done` (currently line 664-671)**

Replace:

```ts
      const sessLabel = sessions.get(req.sessionId)?.label
      const artifactRel =
        req.artifactPath ?? stageArtifactPath(req.stageId, sessLabel) ?? null
      const artifactAbs = artifactRel
        ? isAbsolute(artifactRel)
          ? artifactRel
          : join(req.projectDir, artifactRel)
        : null
```

with:

```ts
      const sessLabel = sessions.get(req.sessionId)?.label
      let artifactRel: string | null
      let artifactAbs: string | null
      if (req.artifactPath) {
        artifactRel = req.artifactPath
        artifactAbs = isAbsolute(req.artifactPath)
          ? req.artifactPath
          : join(req.projectDir, req.artifactPath)
      } else {
        artifactAbs = await resolveStageArtifactAbs(
          req.projectDir,
          req.stageId,
          sessLabel
        )
        // Stage 1 path is outside the project dir, so surface the absolute
        // form to the UI/event layer; stages 2-4 keep the relative form.
        artifactRel =
          req.stageId === 1
            ? artifactAbs
            : stageArtifactPath(req.stageId, sessLabel)
      }
```

- [ ] **Step 8: Update `materializeArtifact` (currently line 745-750)**

Replace:

```ts
      const rel = stageArtifactPath(req.stageId, req.label)
      if (!rel) return { ok: false, error: `unknown stage ${req.stageId}` }
      artifactAbs = join(req.projectDir, rel)
      artifactPathForEvent = rel
```

with:

```ts
      const abs = await resolveStageArtifactAbs(
        req.projectDir,
        req.stageId,
        req.label
      )
      if (!abs) return { ok: false, error: `unknown stage ${req.stageId}` }
      artifactAbs = abs
      // For stage 1 we now report absolute path (target_repo-based);
      // stages 2-4 keep the legacy project-dir-relative form.
      artifactPathForEvent =
        req.stageId === 1
          ? abs
          : stageArtifactPath(req.stageId, req.label)
```

- [ ] **Step 9: Update `artifact:read-current` (currently line 976-987)**

Replace:

```ts
      let abs: string | null = null
      let rel: string | null = null
      if (stageId === 1 && label?.trim()) {
        const sources = await readPlanSources(projectDir)
        const s = sources[label.trim()]
        if (s && isAbsolute(s)) abs = s
      }
      if (!abs) {
        rel = stageArtifactPath(stageId, label)
        if (!rel) return { ok: false, error: '该阶段没有默认产物路径' }
        abs = join(projectDir, rel)
      }
```

with:

```ts
      let abs: string | null = null
      let rel: string | null = null
      if (stageId === 1 && label?.trim()) {
        const sources = await readPlanSources(projectDir)
        const s = sources[label.trim()]
        if (s && isAbsolute(s)) abs = s
      }
      if (!abs) {
        abs = await resolveStageArtifactAbs(projectDir, stageId, label)
        if (!abs) return { ok: false, error: '该阶段没有默认产物路径' }
        // Legacy relative form for display; for stage 1 (absolute) just
        // echo abs so the renderer has something to show.
        rel =
          stageId === 1 ? abs : stageArtifactPath(stageId, label)
      }
```

- [ ] **Step 10: Typecheck**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 11: Run all vitest tests**

Run: `npx vitest run`
Expected: all tests pass.

- [ ] **Step 12: Commit**

```bash
git add electron/cc/ptyManager.ts
git commit -m "refactor(ptyManager): route stage1 artifact path through resolveStageArtifactAbs"
```

---

## Task 5: Update Stage1 prompt template example path

**Files:**
- Modify: `electron/prompts/stage1-design.md:42`

- [ ] **Step 1: Edit the example path string**

In `electron/prompts/stage1-design.md` find the line:

```
3. 得到答案后，把占位符换成用户给的名字，再把设计文档写到相应的绝对路径。例如用户说"逐帧播放方案"，那就写到 `…/workspaces/stage1_design/逐帧播放方案.md`。
```

Replace with:

```
3. 得到答案后，把占位符换成用户给的名字，再把设计文档写到相应的绝对路径。例如用户说"逐帧播放方案"，那就写到 `<target_repo>/.multi-ai-code/designs/逐帧播放方案.md`。
```

- [ ] **Step 2: Commit**

```bash
git add electron/prompts/stage1-design.md
git commit -m "docs(stage1-prompt): update example artifact path to target_repo"
```

---

## Task 6: Manual end-to-end verification

**Files:** none modified.

This step verifies behavior a unit test cannot: the app actually writes the plan into `<target_repo>/.multi-ai-code/designs/`.

- [ ] **Step 1: Launch dev app**

Run: `npm run dev`
Expected: Electron app window opens.

- [ ] **Step 2: Create a new project pointing at a scratch target_repo**

In the app UI: create a new project. Pick any existing local folder (e.g. `/tmp/scratch-repo`, create it first if needed: `mkdir -p /tmp/scratch-repo`) as target_repo.

- [ ] **Step 3: Enter Stage 1 with a plan name**

Start Stage 1 and give it a plan name like `demo-plan`. Interact just enough that the AI writes the artifact (you can tell it "write a minimal 3-line design doc and emit STAGE_DONE").

- [ ] **Step 4: Verify the artifact lives in target_repo**

In a terminal: `ls -la /tmp/scratch-repo/.multi-ai-code/designs/`
Expected: `demo-plan.md` exists.

- [ ] **Step 5: Verify the isolation workspace still exists**

Run: `ls ~/MultiAICode/projects/<new-project-id>/workspaces/stage1_design/`
Expected: directory exists but is empty (only `CLAUDE.md` that the app writes, no plan md file).

- [ ] **Step 6: Verify handoff into Stage 2 reads from new location**

Advance to Stage 2. In the Stage 2 panel, confirm the handoff message contains the design content (indicating `resolveStageArtifactAbs` found the file at the new path).

- [ ] **Step 7: Verify Stage 2-4 artifact paths unchanged**

Progress through Stage 2 and confirm `~/MultiAICode/projects/<id>/artifacts/impl-summary.md` is written as before.

- [ ] **Step 8: No commit needed**

If all manual checks pass, the migration is complete. If a check fails, roll back the relevant task and diagnose.

---

## Self-Review Notes

- Spec requirement "Stage1 落盘到 `<target_repo>/.multi-ai-code/designs/<plan>.md`" → covered by Task 2/3/4.
- Spec requirement "cwd 保持不变" → no task touches `STAGE_CWD`; explicitly verified in Task 1 test.
- Spec requirement "Stage 2-4 产物路径无变化" → verified by Task 2 unit tests and Task 6 step 7.
- Spec requirement "createProjectLayout 下创建 `.multi-ai-code/designs`" → Task 1.
- Spec requirement "renderTemplate planPending 占位符路径改写" → Task 2 step 4 + unit test.
- Spec requirement "prompt template 示例路径更新" → Task 5.
- Spec risk "老项目看不到旧方案" → documented as non-goal; no task addresses it (intentional).
