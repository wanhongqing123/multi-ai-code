# Project Build Sequential and Single-Step Execution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the build panel run either the full enabled build pipeline in order or one explicitly selected build step by itself, with accurate `not-run` vs `skipped` status semantics.

**Architecture:** Extend the existing build subsystem instead of creating a second runner. Add an explicit execution scope to the shared build request/state types, centralize scope validation in a small helper that both `main.ts` and tests can reuse, teach the existing `buildRunner` to initialize and execute only the selected scope, then expose the new mode through preload, `App.tsx`, and `ProjectBuildPanel.tsx`.

**Tech Stack:** Electron 33, React 18, TypeScript, existing `electron/build/*` runner, preload IPC bridge, Vitest, existing static-render panel tests, existing `src/styles.css`.

---

## File Structure

- Create: `electron/build/executionScope.ts`
  - Normalize and validate `all` vs `single-step` build-start options against the current `ProjectBuildConfig`.
- Create: `electron/build/executionScope.test.ts`
  - Cover valid and invalid scope selection cases without involving `main.ts`.
- Modify: `electron/build/types.ts`
  - Add execution-scope types, `not-run` status, and new runtime-state fields.
- Modify: `electron/build/runner.ts`
  - Run only the selected scope and preserve `not-run` for out-of-scope steps.
- Modify: `electron/build/runner.test.ts`
  - Cover single-step execution, `not-run` state, and stop semantics.
- Modify: `electron/main.ts`
  - Accept scoped `build:start` payloads and validate them before calling the runner.
- Modify: `electron/preload.ts`
  - Mirror the new execution-scope types and update `window.api.build.start(...)`.
- Modify: `src/App.tsx`
  - Route top-level “顺序构建” and per-step “单独构建” actions to the new preload API.
- Modify: `src/components/ProjectBuildPanel.tsx`
  - Rename the primary build button, add per-step single-build buttons, add `not-run` labels, and add single-step blocked-reason helpers.
- Modify: `src/components/ProjectBuildPanel.test.tsx`
  - Cover the new labels, status display, and disabled states.
- Modify: `src/styles.css`
  - Add styles for per-step action buttons and the neutral `not-run` badge.

## Design Notes Locked Before Implementation

- [ ] Keep one runner and one log stream; do not add a parallel “single-step runner.”
- [ ] Use `scope: 'all' | 'single-step'` plus `stepId` instead of mutating `ProjectBuildConfig`.
- [ ] Single-step execution runs only the selected enabled step. It never auto-runs prerequisite or following steps.
- [ ] `not-run` means “out of scope for this build,” not “disabled” and not “stopped midway.”
- [ ] `skipped` remains reserved for steps that were in-scope but were not completed because execution stopped or was aborted.
- [ ] `main.ts` should not duplicate scope-validation rules inline; reuse a dedicated helper so tests can cover the rules directly.

## Task 1: Add Shared Execution-Scope Contracts and Validation

**Files:**
- Create: `electron/build/executionScope.ts`
- Create: `electron/build/executionScope.test.ts`
- Modify: `electron/build/types.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write the failing scope-validation tests**

Create `electron/build/executionScope.test.ts` with cases like:

```ts
import { describe, expect, it } from 'vitest'
import { resolveBuildExecutionScope } from './executionScope.js'

const config = {
  enabled: true,
  steps: [
    { id: 'configure', name: 'Configure', envType: 'msys', cwd: '.', command: 'cmake -S . -B build', enabled: true, visualStudioInstanceId: '', outputEncoding: 'auto' },
    { id: 'package', name: 'Package', envType: 'msys', cwd: 'build', command: 'cpack', enabled: false, visualStudioInstanceId: '', outputEncoding: 'auto' },
  ],
}

describe('resolveBuildExecutionScope', () => {
  it('accepts full-pipeline builds without a step id', () => {
    expect(resolveBuildExecutionScope(config, { scope: 'all' })).toEqual({
      ok: true,
      scope: 'all',
      requestedStepId: null,
      runnableStepIds: ['configure'],
    })
  })

  it('accepts a selected enabled step for single-step builds', () => {
    expect(resolveBuildExecutionScope(config, { scope: 'single-step', stepId: 'configure' })).toEqual({
      ok: true,
      scope: 'single-step',
      requestedStepId: 'configure',
      runnableStepIds: ['configure'],
    })
  })

  it('rejects single-step builds without a step id', () => {
    expect(resolveBuildExecutionScope(config, { scope: 'single-step' })).toEqual({
      ok: false,
      error: 'build step is required for single-step scope',
    })
  })

  it('rejects disabled selected steps', () => {
    expect(resolveBuildExecutionScope(config, { scope: 'single-step', stepId: 'package' })).toEqual({
      ok: false,
      error: 'build step is disabled: package',
    })
  })
})
```

- [ ] **Step 2: Run the new test to verify it fails**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/executionScope.test.ts
```

Expected: FAIL because `executionScope.ts` does not exist yet.

- [ ] **Step 3: Add the shared scope types to the build contracts**

Update `electron/build/types.ts` with:

```ts
export type BuildExecutionScope = 'all' | 'single-step'
export type BuildStepStatus =
  | 'not-run'
  | 'pending'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'skipped'

export interface BuildRuntimeState {
  status: BuildOverallStatus
  scope: BuildExecutionScope | null
  requestedStepId: string | null
  projectId: string | null
  projectName: string | null
  targetRepo: string | null
  startedAt: string | null
  finishedAt: string | null
  activeStepId: string | null
  steps: BuildStepRuntime[]
  log: string
  lastFailure: BuildFailureContext | null
}

export interface StartBuildRequest {
  projectId: string
  projectName: string
  targetRepo: string
  config: ProjectBuildConfig
  scope: BuildExecutionScope
  stepId: string | null
}
```

Mirror the same `BuildExecutionScope`, `BuildStepStatus`, `scope`, and `requestedStepId` fields in `electron/preload.ts`.

- [ ] **Step 4: Implement the reusable scope resolver**

Create `electron/build/executionScope.ts` with:

```ts
import type { BuildExecutionScope, ProjectBuildConfig } from './types.js'

export interface BuildExecutionSelection {
  ok: true
  scope: BuildExecutionScope
  requestedStepId: string | null
  runnableStepIds: string[]
}

export interface BuildExecutionSelectionError {
  ok: false
  error: string
}

export function resolveBuildExecutionScope(
  config: ProjectBuildConfig,
  options?: { scope?: BuildExecutionScope; stepId?: string | null }
): BuildExecutionSelection | BuildExecutionSelectionError {
  const scope = options?.scope ?? 'all'

  if (scope === 'all') {
    const runnableStepIds = config.steps.filter((step) => step.enabled).map((step) => step.id)
    return { ok: true, scope, requestedStepId: null, runnableStepIds }
  }

  const stepId = typeof options?.stepId === 'string' ? options.stepId.trim() : ''
  if (!stepId) return { ok: false, error: 'build step is required for single-step scope' }

  const target = config.steps.find((step) => step.id === stepId)
  if (!target) return { ok: false, error: `build step not found: ${stepId}` }
  if (!target.enabled) return { ok: false, error: `build step is disabled: ${stepId}` }

  return {
    ok: true,
    scope,
    requestedStepId: stepId,
    runnableStepIds: [stepId],
  }
}
```

- [ ] **Step 5: Re-run the focused scope test and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/executionScope.test.ts
node .\node_modules\typescript\bin\tsc --noEmit -p tsconfig.node.json
node .\node_modules\typescript\bin\tsc --noEmit -p tsconfig.web.json
```

Expected: PASS

- [ ] **Step 6: Commit the shared scope contract changes**

```bash
git add electron/build/executionScope.ts electron/build/executionScope.test.ts electron/build/types.ts electron/preload.ts
git commit -m "feat: add build execution scope contracts"
```

## Task 2: Teach the Runner to Execute Only the Requested Scope

**Files:**
- Modify: `electron/build/runner.ts`
- Modify: `electron/build/runner.test.ts`
- Modify: `electron/build/types.ts`

- [ ] **Step 1: Write the failing runner tests for single-step execution**

Add tests to `electron/build/runner.test.ts` for:

```ts
it('runs only the selected step for single-step builds', async () => {
  const children: FakeChild[] = []
  const spawn = vi.fn(() => {
    const child = new FakeChild()
    children.push(child)
    return child
  })
  const runner = createBuildRunner({ ...deps, spawn })

  await runner.start({
    projectId: 'p-single',
    projectName: 'Demo',
    targetRepo: 'E:\\repo',
    scope: 'single-step',
    stepId: 'compile',
    config,
  })

  expect(spawn).toHaveBeenCalledTimes(1)
  expect(runner.getState()).toMatchObject({
    scope: 'single-step',
    requestedStepId: 'compile',
    steps: [
      { id: 'configure', status: 'not-run' },
      { id: 'compile', status: 'running' },
      { id: 'package', status: 'not-run' },
    ],
  })
})
```

and:

```ts
it('keeps out-of-scope steps as not-run when a single-step build is stopped', async () => {
  await runner.start({
    projectId: 'p-stop',
    projectName: 'Demo',
    targetRepo: 'E:\\repo',
    scope: 'single-step',
    stepId: 'compile',
    config,
  })

  expect(runner.stop()).toEqual({ ok: true })
  child.emit('close', null, 'SIGTERM')
  await flush()

  expect(runner.getState()).toMatchObject({
    status: 'stopped',
    steps: [
      { id: 'configure', status: 'not-run' },
      { id: 'compile', status: 'skipped' },
      { id: 'package', status: 'not-run' },
    ],
  })
})
```

- [ ] **Step 2: Run the runner test to verify it fails**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/runner.test.ts
```

Expected: FAIL because `scope`, `stepId`, and `not-run` are not implemented in the runner.

- [ ] **Step 3: Update runner state initialization and runnable-step selection**

In `electron/build/runner.ts`:

1. Extend `initialState()`:

```ts
return {
  status: 'idle',
  scope: null,
  requestedStepId: null,
  projectId: null,
  projectName: null,
  targetRepo: null,
  startedAt: null,
  finishedAt: null,
  activeStepId: null,
  steps: [],
  log: '',
  lastFailure: null,
}
```

2. In `start(request)`, initialize steps with scope-aware status:

```ts
steps: request.config.steps.map((step) => {
  const selected =
    request.scope === 'all'
      ? step.enabled
      : step.enabled && step.id === request.stepId

  return {
    ...step,
    visualStudioDisplayName: null,
    status:
      request.scope === 'all'
        ? step.enabled ? 'pending' : 'skipped'
        : selected ? 'pending' : 'not-run',
    resolvedCwd: null,
    startedAt: null,
    finishedAt: null,
    exitCode: null,
    signal: null,
  }
})
```

3. Change `runBuild(...)` to receive the full `StartBuildRequest` and derive runnable steps with:

```ts
const runnableSteps =
  request.scope === 'single-step'
    ? state.steps.filter((step) => step.id === request.stepId && step.enabled)
    : state.steps.filter((step) => step.enabled)
```

4. Leave `markPendingStepsSkipped()` unchanged so it only touches `pending` and `running`, never `not-run`.

- [ ] **Step 4: Re-run the focused runner tests**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/runner.test.ts
```

Expected: PASS

- [ ] **Step 5: Run the full build-core regression set**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/executionScope.test.ts electron/build/runner.test.ts electron/build/config.test.ts electron/build/visualStudio.test.ts
```

Expected: PASS

- [ ] **Step 6: Commit the runner scope changes**

```bash
git add electron/build/types.ts electron/build/runner.ts electron/build/runner.test.ts
git commit -m "feat: support single-step build execution"
```

## Task 3: Wire Scoped Start Requests Through Main, Preload, App, and the Build Panel

**Files:**
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`
- Modify: `src/App.tsx`
- Modify: `src/components/ProjectBuildPanel.tsx`
- Modify: `src/components/ProjectBuildPanel.test.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing panel tests for the new UI and state labels**

Extend `src/components/ProjectBuildPanel.test.tsx` with cases like:

```ts
it('renders the renamed sequential build button and per-step single-build buttons', () => {
  const markup = renderToStaticMarkup(
    <ProjectBuildPanel
      open={true}
      currentProjectId="project-1"
      currentProjectName="Demo"
      buildConfig={enabledBuildConfig}
      buildConfigReady={true}
      state={baseState}
      sessionId="session-1"
      sessionStatus="running"
      onClose={vi.fn()}
      onStartBuild={vi.fn()}
      onStartSingleBuild={vi.fn()}
      onStopBuild={vi.fn()}
      onAnalyzeFailure={vi.fn()}
    />
  )

  expect(markup).toContain('顺序构建')
  expect(markup).toContain('单独构建')
})
```

and:

```ts
it('renders the not-run status label for out-of-scope steps', () => {
  expect(getBuildStepStatusLabel('not-run')).toBe('未执行')
})
```

Also add a disabled-reason helper case:

```ts
expect(
  getBuildStartBlockedReason('project-1', true, enabledBuildConfig, 'single-step', 'missing-step')
).toContain('missing-step')
```

- [ ] **Step 2: Run the panel test to verify it fails**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildPanel.test.tsx
```

Expected: FAIL because the component still exposes only the full-build button and does not know `not-run`.

- [ ] **Step 3: Update preload and main-process build start signatures**

In `electron/preload.ts`, add:

```ts
export interface BuildStartOptions {
  scope?: BuildExecutionScope
  stepId?: string | null
}
```

and change the API to:

```ts
build: {
  start: (projectId: string, options?: BuildStartOptions) =>
    ipcRenderer.invoke('build:start', {
      id: projectId,
      scope: options?.scope ?? 'all',
      stepId: options?.stepId ?? null,
    }) as Promise<BuildStartResult>,
}
```

In `electron/main.ts`, update the handler to:

```ts
ipcMain.handle(
  'build:start',
  async (_e, { id, scope, stepId }: { id: string; scope?: BuildExecutionScope; stepId?: string | null }) => {
    // ... existing project/meta/config lookup ...
    const selection = resolveBuildExecutionScope(configResult.value, { scope, stepId })
    if (!selection.ok) {
      return { ok: false as const, error: selection.error, state: buildRunner.getState() }
    }

    return await buildRunner.start({
      projectId: id,
      projectName: metaName || id,
      targetRepo,
      config: configResult.value,
      scope: selection.scope,
      stepId: selection.requestedStepId,
    })
  }
)
```

- [ ] **Step 4: Update App handlers to start either all steps or one selected step**

Refactor `src/App.tsx` so the current handler becomes a scoped helper:

```ts
const handleStartBuild = useCallback(
  async (scope: 'all' | 'single-step', stepId: string | null = null) => {
    // existing cross-project-running guard
    const blockedReason = getBuildStartBlockedReason(
      currentProjectId,
      projectBuildConfigReady,
      visibleProjectBuildConfig,
      scope,
      stepId
    )
    if (blockedReason) {
      showToast(blockedReason, { level: 'warn' })
      setShowBuildPanel(true)
      return
    }
    if (!currentProjectId) return

    const result = await window.api.build.start(currentProjectId, { scope, stepId })
    setBuildState(result.state)
    setShowBuildPanel(true)
    if (!result.ok) showToast(result.error ?? '启动构建失败', { level: 'error' })
  },
  [currentProjectId, projectBuildConfigReady, visibleProjectBuildConfig, buildState.projectId, buildState.status]
)
```

and pass:

```tsx
onStartBuild={() => void handleStartBuild('all')}
onStartSingleBuild={(stepId) => void handleStartBuild('single-step', stepId)}
```

- [ ] **Step 5: Update the build panel UI and styles**

In `src/components/ProjectBuildPanel.tsx`:

1. Extend props:

```ts
onStartSingleBuild: (stepId: string) => void
```

2. Extend blocked-reason logic:

```ts
export function getBuildStartBlockedReason(
  projectId: string | null,
  buildConfigReady: boolean,
  buildConfig: ProjectBuildConfig,
  scope: 'all' | 'single-step' = 'all',
  stepId: string | null = null
): string | null {
  if (!projectId) return '请先选择项目'
  if (!buildConfigReady) return '正在读取该项目的构建配置，请稍后再试'
  if (!buildConfig.enabled) return '当前项目未启用构建配置，请先到设置中开启'
  if (scope === 'all' && !buildConfig.steps.some((step) => step.enabled)) {
    return '当前项目没有启用的构建步骤，请先到设置中配置'
  }
  if (scope === 'single-step') {
    const step = buildConfig.steps.find((item) => item.id === stepId)
    if (!step) return `未找到构建步骤：${stepId ?? ''}`
    if (!step.enabled) return `该构建步骤尚未启用：${step.name}`
  }
  return null
}
```

3. Add `not-run` label support:

```ts
case 'not-run':
  return '未执行'
```

4. Rename the main button text to `顺序构建`.

5. Add per-step actions:

```tsx
<div className="build-step-actions">
  <button
    className="tile-btn"
    onClick={() => props.onStartSingleBuild(step.id)}
    disabled={props.state.status === 'running' || !step.enabled}
    title={
      props.state.status === 'running'
        ? '当前已有构建正在执行'
        : step.enabled
          ? `单独执行 ${step.name}`
          : '该步骤未启用'
    }
  >
    单独构建
  </button>
</div>
```

In `src/styles.css`, add:

```css
.build-step-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

.build-step-status-not-run {
  background: rgba(148, 163, 184, 0.16);
  color: #64748b;
}
```

- [ ] **Step 6: Re-run the panel test and renderer/web typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildPanel.test.tsx
node .\node_modules\typescript\bin\tsc --noEmit -p tsconfig.web.json
```

Expected: PASS

- [ ] **Step 7: Commit the UI and IPC wiring**

```bash
git add electron/main.ts electron/preload.ts src/App.tsx src/components/ProjectBuildPanel.tsx src/components/ProjectBuildPanel.test.tsx src/styles.css
git commit -m "feat: add single-step controls to build panel"
```

## Task 4: Run End-to-End Regression and Manual Smoke Verification

**Files:**
- Modify: none
- Test: `electron/build/executionScope.test.ts`
- Test: `electron/build/runner.test.ts`
- Test: `src/components/ProjectBuildPanel.test.tsx`

- [ ] **Step 1: Run the focused regression suite**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/executionScope.test.ts electron/build/runner.test.ts src/components/ProjectBuildPanel.test.tsx
node .\node_modules\typescript\bin\tsc --noEmit -p tsconfig.node.json
node .\node_modules\typescript\bin\tsc --noEmit -p tsconfig.web.json
```

Expected: PASS

- [ ] **Step 2: Run the app locally and verify both modes manually**

Run:

```bash
npm.cmd run dev
```

Manual checks:

- Open the build panel for a project with at least two enabled steps.
- Confirm the top button now reads `顺序构建`.
- Confirm each step card shows `单独构建`.
- Click `单独构建` on a middle or last step.
- Verify only that step transitions through `进行中 -> 成功/失败`.
- Verify all other steps stay `未执行`.
- Click `顺序构建`.
- Verify enabled steps run top-to-bottom as before.
- Stop a running full pipeline and verify remaining in-scope steps become `跳过`.

- [ ] **Step 3: Commit the verification checkpoint**

```bash
git status --short
```

Expected: no unexpected files beyond the implementation changes already committed in prior tasks.

- [ ] **Step 4: Push the completed branch**

```bash
git push origin main
```

Expected: remote accepts the three implementation commits.
