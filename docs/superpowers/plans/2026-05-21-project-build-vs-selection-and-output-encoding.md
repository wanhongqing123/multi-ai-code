# Project Build VS Selection and Output Encoding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let each `visual-studio` build step bind to a specific installed Visual Studio instance, block execution when that instance disappears, and let every build step override stdout/stderr decoding with `auto`, `UTF-8`, or `GBK`.

**Architecture:** Extend the existing project build subsystem instead of adding a parallel settings path. Persist the new step fields inside the existing `build_config`, add one main-process Visual Studio installation discovery helper reused by both settings UI and runner, update the runner to resolve the selected instance and decode stream chunks before appending logs, then surface the new controls and metadata through the existing preload and build/settings panels.

**Tech Stack:** Electron 33, React 18, TypeScript, existing `electron/build/*` pipeline, existing preload bridge, `vswhere.exe`, `VsDevCmd.bat`, Vitest, `iconv-lite`.

---

## File Structure

- Modify: `package.json`
  - Add the decoding dependency used by the build runner.
- Modify: `electron/build/types.ts`
  - Extend shared build-step and failure-context types with Visual Studio instance and output encoding fields.
- Modify: `electron/build/config.ts`
  - Normalize, migrate, and validate the new step fields.
- Modify: `electron/build/config.test.ts`
  - Cover defaulting, migration, and save-time validation for the new fields.
- Modify: `electron/build/visualStudio.ts`
  - Enumerate all usable Visual Studio instances and resolve one specific instance into a developer environment.
- Modify: `electron/build/visualStudio.test.ts`
  - Cover multi-instance discovery, per-instance environment resolution, and missing-instance errors.
- Modify: `electron/build/runner.ts`
  - Use the selected Visual Studio instance and decode stream chunks with the per-step encoding strategy.
- Modify: `electron/build/runner.test.ts`
  - Cover selected-instance spawn behavior, missing-instance failure, and `auto / utf8 / gbk` decoding.
- Modify: `electron/main.ts`
  - Expose a new IPC method to list Visual Studio instances.
- Modify: `electron/preload.ts`
  - Extend build/project APIs and exported renderer types for Visual Studio instance listing plus the new step fields.
- Modify: `src/components/ProjectBuildSettingsSection.tsx`
  - Add `Visual Studio 实例` and `输出编码` controls to each build step.
- Modify: `src/components/ProjectBuildSettingsSection.test.tsx`
  - Cover rendering, environment switching, and invalid-instance UI behavior.
- Modify: `src/components/ProjectBuildPanel.tsx`
  - Show instance and encoding metadata on the executed step cards.
- Modify: `src/components/ProjectBuildPanel.test.tsx`
  - Cover the new metadata rendering.

## Design Notes Locked Before Implementation

- [ ] Keep Visual Studio selection per build step; do not add a global “default VS version” setting.
- [ ] Save the selected Visual Studio installation by stable instance identifier, not by free-form version text and not by `latest`.
- [ ] When a saved Visual Studio instance is missing, fail fast and do not fall back to another instance.
- [ ] Show `输出编码` on both `MSYS2` and `Visual Studio` steps.
- [ ] Support only `auto`, `utf8`, and `gbk` in v1.
- [ ] Reuse the same Visual Studio discovery logic in settings validation and runtime execution.

## Task 1: Extend Shared Build Types and Config Persistence

**Files:**
- Modify: `electron/build/types.ts`
- Modify: `electron/build/config.ts`
- Modify: `electron/build/config.test.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write the failing config tests for the new step fields**

Add tests to `electron/build/config.test.ts` that assert:

```ts
expect(normalizeBuildConfig({
  enabled: true,
  steps: [{ id: 'compile', name: 'Compile', envType: 'visual-studio', cwd: 'build', command: 'cmake --build .', enabled: true }],
})).toEqual({
  enabled: true,
  steps: [{
    id: 'compile',
    name: 'Compile',
    envType: 'visual-studio',
    cwd: 'build',
    command: 'cmake --build .',
    enabled: true,
    visualStudioInstanceId: '',
    outputEncoding: 'auto',
  }],
})
```

Also add a save-time validation case:

```ts
expect(result).toEqual({
  ok: false,
  error: 'invalid build config',
  details: [
    {
      path: 'build_config.steps[0].visualStudioInstanceId',
      message: 'visualStudioInstanceId must be selected for visual-studio steps',
    },
  ],
})
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/config.test.ts
```

Expected: FAIL because the new fields and validation do not exist yet.

- [ ] **Step 2: Extend the shared build types**

Update `electron/build/types.ts` with:

```ts
export type BuildOutputEncoding = 'auto' | 'utf8' | 'gbk'

export interface BuildStepConfig {
  id: string
  name: string
  envType: BuildStepEnvType
  cwd: string
  command: string
  enabled: boolean
  visualStudioInstanceId: string
  outputEncoding: BuildOutputEncoding
}
```

Extend `BuildFailureContext` with:

```ts
visualStudioInstanceId: string | null
visualStudioDisplayName: string | null
outputEncoding: BuildOutputEncoding
```

- [ ] **Step 3: Normalize and validate the new config fields**

Update `electron/build/config.ts` so `normalizeBuildStep()` backfills:

```ts
const visualStudioInstanceId =
  typeof value.visualStudioInstanceId === 'string' ? value.visualStudioInstanceId.trim() : ''
const outputEncoding =
  value.outputEncoding === 'utf8' || value.outputEncoding === 'gbk' ? value.outputEncoding : 'auto'
```

Extend validation with:

```ts
if (step.envType === 'visual-studio' && !step.visualStudioInstanceId.trim()) {
  issues.push({
    path: `build_config.steps[${index}].visualStudioInstanceId`,
    message: 'visualStudioInstanceId must be selected for visual-studio steps',
  })
}
if (!['auto', 'utf8', 'gbk'].includes(step.outputEncoding)) {
  issues.push({
    path: `build_config.steps[${index}].outputEncoding`,
    message: 'outputEncoding must be one of: auto, utf8, gbk',
  })
}
```

- [ ] **Step 4: Expose the new types through preload**

Mirror the type changes in `electron/preload.ts`:

```ts
export type BuildOutputEncoding = 'auto' | 'utf8' | 'gbk'
```

and ensure the exported `BuildStepConfig` shape includes:

```ts
visualStudioInstanceId: string
outputEncoding: BuildOutputEncoding
```

- [ ] **Step 5: Re-run the focused config test and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/config.test.ts
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 6: Commit the config changes**

```bash
git add electron/build/types.ts electron/build/config.ts electron/build/config.test.ts electron/preload.ts
git commit -m "feat: extend build step config for VS selection and encoding"
```

## Task 2: Add Visual Studio Instance Discovery and Instance-Bound Environment Resolution

**Files:**
- Modify: `electron/build/visualStudio.ts`
- Modify: `electron/build/visualStudio.test.ts`
- Modify: `electron/preload.ts`
- Modify: `electron/main.ts`

- [ ] **Step 1: Write the failing Visual Studio discovery tests**

Add tests to `electron/build/visualStudio.test.ts` for:

```ts
expect(result).toEqual([
  {
    instanceId: 'a1',
    displayName: 'Visual Studio 2022 Community',
    installationPath: 'C:\\VS\\2022\\Community',
    productLineVersion: '2022',
    isPrerelease: false,
  },
  {
    instanceId: 'b2',
    displayName: 'Visual Studio 2026 Preview',
    installationPath: 'C:\\VS\\2026\\Preview',
    productLineVersion: '2026',
    isPrerelease: true,
  },
])
```

and:

```ts
expect(result).toEqual({
  ok: false,
  error: 'visual studio instance not found: b2',
})
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/visualStudio.test.ts
```

Expected: FAIL because discovery still only supports `-latest`.

- [ ] **Step 2: Add an explicit installation list type and discovery helper**

In `electron/build/visualStudio.ts`, add:

```ts
export interface VisualStudioInstallation {
  instanceId: string
  displayName: string
  installationPath: string
  productLineVersion: string | null
  isPrerelease: boolean
}
```

Add:

```ts
export async function listVisualStudioInstallations(...): Promise<VisualStudioInstallation[]>
```

Use `vswhere` JSON output:

```text
vswhere -products * -requires Microsoft.Component.MSBuild -format json
```

Map each item into the installation type and filter out entries without `installationPath`.

- [ ] **Step 3: Resolve one specific instance into a developer environment**

Refactor `resolveVisualStudioEnvironment()` to require an instance id:

```ts
export async function resolveVisualStudioEnvironment(options: {
  instanceId: string
  platform?: NodeJS.Platform
  baseEnv?: NodeJS.ProcessEnv
  execFile?: ExecFileLike
  vswherePath?: string
}): Promise<VisualStudioEnvironmentResult>
```

Resolve by:

1. `const installations = await listVisualStudioInstallations(...)`
2. `const selected = installations.find((item) => item.instanceId === options.instanceId)`
3. If missing, return:

```ts
{ ok: false, error: `visual studio instance not found: ${options.instanceId}` }
```

4. Otherwise run the selected instance’s `VsDevCmd.bat`.

Include `displayName` in the success result:

```ts
displayName: selected.displayName
```

- [ ] **Step 4: Expose the installation list over main/preload IPC**

In `electron/main.ts`, add:

```ts
ipcMain.handle('project:list-visual-studio-installations', async () => {
  return { ok: true, value: await listVisualStudioInstallations() }
})
```

In `electron/preload.ts`, add:

```ts
listVisualStudioInstallations: () =>
  ipcRenderer.invoke('project:list-visual-studio-installations') as Promise<{
    ok: boolean
    value?: VisualStudioInstallation[]
    error?: string
  }>
```

- [ ] **Step 5: Re-run focused tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/visualStudio.test.ts
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 6: Commit the Visual Studio discovery layer**

```bash
git add electron/build/visualStudio.ts electron/build/visualStudio.test.ts electron/main.ts electron/preload.ts
git commit -m "feat: add explicit Visual Studio installation selection"
```

## Task 3: Add Step-Level Output Decoding and Instance-Aware Runner Execution

**Files:**
- Modify: `package.json`
- Modify: `electron/build/runner.ts`
- Modify: `electron/build/runner.test.ts`
- Modify: `electron/build/types.ts`

- [ ] **Step 1: Add the failing runner tests for selected VS instances and decoding**

Extend `electron/build/runner.test.ts` with one Visual Studio spawn assertion:

```ts
expect(resolveVisualStudioEnvironment).toHaveBeenCalledWith(
  expect.objectContaining({
    instanceId: 'vs-2022-community',
  })
)
```

Add one decoding assertion:

```ts
child.stderr.write(Buffer.from([0xb1, 0xe0, 0xd2, 0xeb]))
child.emit('close', 2, null)
await flush()
expect(runner.getState().log).toContain('编译')
```

Add a missing-instance case:

```ts
expect(runner.getState().lastFailure?.reason).toContain('visual studio instance not found')
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/runner.test.ts
```

Expected: FAIL because the runner neither passes `instanceId` nor decodes by step.

- [ ] **Step 2: Add the decoding dependency**

Update `package.json` dependencies with:

```json
"iconv-lite": "^0.6.3"
```

Then install:

```bash
npm.cmd install
```

Expected: `iconv-lite` added to `package-lock.json`.

- [ ] **Step 3: Decode stdout/stderr per step before appending logs**

In `electron/build/runner.ts`, add a helper:

```ts
import iconv from 'iconv-lite'

function decodeBuildChunk(
  chunk: Buffer | string,
  step: BuildStepRuntime
): string {
  if (typeof chunk === 'string') return chunk
  if (step.outputEncoding === 'utf8') return chunk.toString('utf8')
  if (step.outputEncoding === 'gbk') return iconv.decode(chunk, 'gbk')
  if (step.envType === 'visual-studio') return iconv.decode(chunk, 'gbk')
  return chunk.toString('utf8')
}
```

Update listeners:

```ts
child.stdout?.on('data', (chunk) => {
  const text = decodeBuildChunk(chunk, step)
  ...
})
child.stderr?.on('data', (chunk) => {
  const text = decodeBuildChunk(chunk, step)
  ...
})
```

- [ ] **Step 4: Pass the selected Visual Studio instance into the runner**

Update the `visual-studio` branch in `spawnForStep()`:

```ts
const result = await deps.resolveVisualStudioEnvironment({
  platform: deps.platform,
  baseEnv: process.env,
  instanceId: step.visualStudioInstanceId,
})
```

Include new failure context fields:

```ts
visualStudioInstanceId: step.envType === 'visual-studio' ? step.visualStudioInstanceId : null,
visualStudioDisplayName: step.envType === 'visual-studio' ? result.displayName ?? null : null,
outputEncoding: step.outputEncoding,
```

Thread `displayName` from `spawnForStep()` back into the failure context on Visual Studio steps.

- [ ] **Step 5: Re-run runner tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/runner.test.ts
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 6: Commit the runner changes**

```bash
git add package.json package-lock.json electron/build/runner.ts electron/build/runner.test.ts electron/build/types.ts
git commit -m "feat: bind build steps to VS instances and decode output"
```

## Task 4: Add the Settings UI for VS Instance Selection and Output Encoding

**Files:**
- Modify: `src/components/ProjectBuildSettingsSection.tsx`
- Modify: `src/components/ProjectBuildSettingsSection.test.tsx`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write the failing settings UI tests**

Add tests in `src/components/ProjectBuildSettingsSection.test.tsx` that assert:

```ts
expect(markup).toContain('输出编码')
expect(markup).toContain('Visual Studio 实例')
expect(markup).toContain('自动')
expect(markup).toContain('GBK')
```

Add a case for a missing instance warning:

```ts
expect(markup).toContain('所选 Visual Studio 实例当前不可用')
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildSettingsSection.test.tsx
```

Expected: FAIL because the controls and warning do not exist yet.

- [ ] **Step 2: Extend the settings section props and default step shape**

In `src/components/ProjectBuildSettingsSection.tsx`, change `createBuildStep()` to:

```ts
return {
  id,
  name: 'New Step',
  envType: 'msys',
  cwd: '.',
  command: '',
  enabled: true,
  visualStudioInstanceId: '',
  outputEncoding: 'auto',
}
```

Add props needed for instance lists:

```ts
visualStudioInstallations: VisualStudioInstallation[]
visualStudioInstallationsLoading: boolean
onRefreshVisualStudioInstallations: () => void
```

- [ ] **Step 3: Render the new controls**

For every step, add:

```tsx
<label>
  输出编码
  <select
    value={step.outputEncoding}
    onChange={(event) =>
      props.onChange(updateBuildStep(props.value, index, {
        outputEncoding: event.target.value as BuildStepConfig['outputEncoding'],
      }))
    }
  >
    <option value="auto">自动</option>
    <option value="utf8">UTF-8</option>
    <option value="gbk">GBK</option>
  </select>
</label>
```

Inside the `visual-studio` branch, render:

```tsx
<label>
  Visual Studio 实例
  <select
    value={step.visualStudioInstanceId}
    onChange={(event) =>
      props.onChange(updateBuildStep(props.value, index, {
        visualStudioInstanceId: event.target.value,
      }))
    }
  >
    <option value="">请选择实例</option>
    {props.visualStudioInstallations.map((item) => (
      <option key={item.instanceId} value={item.instanceId}>
        {item.displayName}
      </option>
    ))}
  </select>
</label>
```

If the selected instance id is missing from the current list, render:

```tsx
<div className="ai-settings-note project-build-step-warning">
  所选 Visual Studio 实例当前不可用
</div>
```

- [ ] **Step 4: Re-run the settings UI test and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildSettingsSection.test.tsx
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 5: Commit the settings UI**

```bash
git add src/components/ProjectBuildSettingsSection.tsx src/components/ProjectBuildSettingsSection.test.tsx
git commit -m "feat: add VS instance and encoding controls to build settings"
```

## Task 5: Wire the App State, Build Panel Metadata, and Final Verification

**Files:**
- Modify: `src/components/AiSettingsDialog.tsx`
- Modify: `src/components/ProjectBuildPanel.tsx`
- Modify: `src/components/ProjectBuildPanel.test.tsx`
- Modify: `src/App.tsx`
- Modify: `src/styles.css`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write the failing panel test for instance/encoding metadata**

Add a test in `src/components/ProjectBuildPanel.test.tsx` that expects:

```ts
expect(markup).toContain('Visual Studio 2022 Community')
expect(markup).toContain('GBK')
```

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildPanel.test.tsx
```

Expected: FAIL because the panel does not render those fields yet.

- [ ] **Step 2: Thread Visual Studio installation state through the app/settings dialog**

In `src/App.tsx`, add state like:

```ts
const [visualStudioInstallations, setVisualStudioInstallations] = useState<VisualStudioInstallation[]>([])
const [visualStudioInstallationsLoading, setVisualStudioInstallationsLoading] = useState(false)
```

Load via preload:

```ts
const result = await window.api.project.listVisualStudioInstallations()
if (result.ok) setVisualStudioInstallations(result.value ?? [])
```

Pass those props through `AiSettingsDialog` into `ProjectBuildSettingsSection`.

- [ ] **Step 3: Show the metadata in the build panel**

Update `src/components/ProjectBuildPanel.tsx` step meta rendering to include:

```tsx
{step.envType === 'visual-studio' && step.visualStudioDisplayName ? (
  <span>VS: {step.visualStudioDisplayName}</span>
) : null}
<span>编码: {step.outputEncoding === 'auto' ? '自动' : step.outputEncoding.toUpperCase()}</span>
```

If the runtime step type does not yet include `visualStudioDisplayName`, extend the runtime shape in preload/main to carry it through.

- [ ] **Step 4: Add any minimal warning styles**

In `src/styles.css`, add a focused warning style:

```css
.project-build-step-warning {
  color: var(--mac-danger);
}
```

Only add the CSS needed for the new controls and warning row.

- [ ] **Step 5: Run the final focused verification set**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/config.test.ts electron/build/visualStudio.test.ts electron/build/runner.test.ts src/components/ProjectBuildSettingsSection.test.tsx src/components/ProjectBuildPanel.test.tsx
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 6: Commit the integration wiring**

```bash
git add src/components/AiSettingsDialog.tsx src/components/ProjectBuildPanel.tsx src/components/ProjectBuildPanel.test.tsx src/App.tsx src/styles.css electron/main.ts electron/preload.ts
git commit -m "feat: surface VS selection and output encoding in build UI"
```
