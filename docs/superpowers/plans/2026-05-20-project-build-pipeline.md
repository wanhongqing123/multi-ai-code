# Project Build Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add one project-level default build pipeline that can run multiple sequential steps with per-step `msys` or `visual-studio` environments, stream logs into a dedicated build panel, stop immediately on step failure, and let the user manually send a failure-diagnosis prompt to the active main AICLI session without granting the AI permission to modify code.

**Architecture:** Store `build_config` in each project's `project.json`, normalize it through a focused main-process config helper, execute it through a non-interactive child-process runner with environment adapters for `msys` and `visual-studio`, surface live run state through new build IPC channels, edit the pipeline from the existing settings dialog, and render a dedicated build panel in the main app. Build failure analysis is manual: the build subsystem prepares a bounded diagnosis prompt, and the renderer forwards that prompt into the active main AICLI session with `cc.sendUser`.

**Tech Stack:** Electron 33, React 18, TypeScript, Node `child_process.spawn`, existing `project.json` metadata helpers, existing `detectMsys()` utility, new `vswhere` / `VsDevCmd.bat` detection helper, Vitest, existing preload bridge, existing main-session `cc.sendUser`.

---

## File Structure

- Create: `electron/build/types.ts`
  - Shared build config, build step, build run, and failure-context types for main-process build modules.
- Create: `electron/build/config.ts`
  - Read/write/normalize/validate `build_config` through `readProjectMetaFile()` and `writeProjectMetaFile()`.
- Create: `electron/build/config.test.ts`
  - Unit tests for empty config, normalized config, step ordering, invalid env rejection, and metadata round-trip.
- Create: `electron/build/visualStudio.ts`
  - Detect `vswhere.exe`, resolve `VsDevCmd.bat`, and build the `cmd.exe` bootstrap command for `visual-studio` steps.
- Create: `electron/build/visualStudio.test.ts`
  - Unit tests for `vswhere` parsing, fallback handling, and generated command quoting.
- Create: `electron/build/runner.ts`
  - Sequential child-process runner, stop handling, log fan-out, step status transitions, and last-failure capture.
- Create: `electron/build/runner.test.ts`
  - Unit tests for success flow, stop-on-first-failure, disabled-step skip, user stop, and environment-error cases.
- Create: `electron/build/analysisPrompt.ts`
  - Build a bounded plain-text diagnosis prompt from the last failed run.
- Create: `electron/build/analysisPrompt.test.ts`
  - Unit tests for prompt structure, log truncation, and "analyze only, do not modify code" guardrails.
- Modify: `electron/main.ts`
  - Register project build IPC, hold the active build session, broadcast log/status events, and expose failure-analysis prompt retrieval.
- Modify: `electron/preload.ts`
  - Add `window.api.project.getBuildConfig/setBuildConfig` and `window.api.build.*` APIs plus event subscriptions.
- Create: `src/components/ProjectBuildSettingsSection.tsx`
  - Project-scoped settings UI for the default multi-step build pipeline.
- Create: `src/components/ProjectBuildSettingsSection.test.tsx`
  - Static and interactive tests for adding steps, reordering, toggling, and validation hints.
- Create: `src/components/BuildPanel.tsx`
  - Dedicated build drawer/panel with run controls, step statuses, log view, and manual failure-analysis action.
- Create: `src/components/BuildPanel.test.tsx`
  - Tests for run-state rendering, button gating, and failed-run analysis button visibility.
- Modify: `src/components/AiSettingsDialog.tsx`
  - Embed the project build settings section alongside existing project-level settings.
- Modify: `src/App.tsx`
  - Load/save build config, open the build panel, track build session state, and forward diagnosis prompts to AICLI.
- Modify: `src/styles.css`
  - Add settings-row styles and dedicated build-panel layout/log styles.

## Design Notes Locked In Before Implementation

- [ ] Keep `build_config` project-scoped in `project.json`; do not create a separate build config file.
- [ ] Keep exactly one default build pipeline per project; the pipeline may contain multiple ordered steps.
- [ ] Allow each step to pick its own `envType`, `cwd`, and `command`.
- [ ] Support only `msys` and `visual-studio` in v1.
- [ ] Treat `project.json.msys_enabled` as AI-session-only; do not couple build execution to that toggle.
- [ ] Stop the pipeline immediately on the first failed step.
- [ ] Keep build output in a dedicated build panel, not inside the main AICLI terminal.
- [ ] Keep failure analysis manual and diagnostics-only; do not let the AICLI prompt ask for code changes.
- [ ] Require an active main AICLI session before enabling "Analyze Failure"; when no session exists, keep the button disabled and show a hint.
- [ ] Limit v1 to one active build run app-wide to avoid concurrent-run complexity.

## Task 1: Add Project Build Config Types and Metadata Persistence

**Files:**
- Create: `electron/build/types.ts`
- Create: `electron/build/config.ts`
- Create: `electron/build/config.test.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Define the normalized project build types**

Add explicit types in `electron/build/types.ts`:

```ts
export type BuildEnvType = 'msys' | 'visual-studio'

export interface ProjectBuildStep {
  id: string
  name: string
  envType: BuildEnvType
  cwd: string
  command: string
  enabled: boolean
}

export interface ProjectBuildConfig {
  enabled: boolean
  steps: ProjectBuildStep[]
}

export type BuildStepStatus = 'pending' | 'running' | 'succeeded' | 'failed' | 'skipped'
export type BuildRunStatus = 'idle' | 'running' | 'succeeded' | 'failed' | 'stopped'
```

`id` is an implementation detail for stable UI list editing and should be auto-filled when missing.

- [ ] **Step 2: Write the failing config persistence tests**

Cover these cases in `electron/build/config.test.ts`:
- missing `build_config` returns the default `{ enabled: false, steps: [] }`
- invalid `envType` is rejected by validation
- missing `id` is backfilled on normalize
- step order survives read/write round-trip
- unrelated project meta fields stay intact after saving `build_config`

Run: `node .\\node_modules\\vitest\\vitest.mjs run electron/build/config.test.ts`

Expected: FAIL because `electron/build/config.ts` does not exist yet.

- [ ] **Step 3: Implement `build_config` read/write/normalize helpers**

`electron/build/config.ts` should:
- read metadata through `readProjectMetaFile(metaPath)`
- expose `loadProjectBuildConfig(metaPath)` and `saveProjectBuildConfig(metaPath, config)`
- normalize:
  - `enabled` defaults to `false`
  - `steps` defaults to `[]`
  - each step trims `name`, `cwd`, and `command`
  - missing/blank `id` becomes `crypto.randomUUID()`
- validate:
  - `name`, `cwd`, and `command` must be non-empty for enabled steps
  - `envType` must be exactly `msys` or `visual-studio`

Return structured errors such as:

```ts
type BuildConfigSaveResult =
  | { ok: true; value: ProjectBuildConfig; repairedMeta: boolean }
  | { ok: false; error: 'invalid-build-config' | 'project settings corrupted and unrecoverable'; details: string }
```

- [ ] **Step 4: Expose build config APIs on preload**

Add to `window.api.project` in `electron/preload.ts`:

```ts
getBuildConfig: (id: string) => Promise<{ ok: boolean; value?: ProjectBuildConfig; error?: string }>
setBuildConfig: (
  id: string,
  config: ProjectBuildConfig
) => Promise<{ ok: boolean; value?: ProjectBuildConfig; repairedMeta?: boolean; error?: string; details?: string }>
```

- [ ] **Step 5: Re-run the focused config test and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run electron/build/config.test.ts`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 6: Commit the config layer**

```bash
git add electron/build/types.ts electron/build/config.ts electron/build/config.test.ts electron/preload.ts
git commit -m "feat: add project build config metadata support"
```

## Task 2: Implement `msys` and `visual-studio` Environment Adapters

**Files:**
- Create: `electron/build/visualStudio.ts`
- Create: `electron/build/visualStudio.test.ts`
- Modify: `electron/build/types.ts`
- Modify: `electron/build/config.ts`

- [ ] **Step 1: Write the failing Visual Studio detection tests**

Cover these cases in `electron/build/visualStudio.test.ts`:
- `vswhere.exe` returns a `VsDevCmd.bat` path and the helper accepts it
- missing `vswhere.exe` returns a structured "not available" result
- generated bootstrap command safely quotes spaces in both `VsDevCmd.bat` and `cwd`

Run: `node .\\node_modules\\vitest\\vitest.mjs run electron/build/visualStudio.test.ts`

Expected: FAIL because the module does not exist.

- [ ] **Step 2: Implement `visualStudio.ts`**

Expose:

```ts
export interface VisualStudioInfo {
  available: boolean
  vswherePath: string | null
  vsDevCmdPath: string | null
  source: 'vswhere' | 'fallback' | null
}

export async function detectVisualStudio(): Promise<VisualStudioInfo> { ... }

export function buildVisualStudioStepCommand(input: {
  vsDevCmdPath: string
  cwd: string
  command: string
}): { file: string; args: string[] } { ... }
```

Detection strategy:
- probe `%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe`
- run:

```powershell
vswhere -latest -products * -requires Microsoft.Component.MSBuild -find Common7\Tools\VsDevCmd.bat
```

- if no `VsDevCmd.bat` is found, return `available: false`

Execution strategy:
- use `cmd.exe /d /s /c`
- chain:
  - call `VsDevCmd.bat`
  - `cd /d <cwd>`
  - run the configured build command

- [ ] **Step 3: Keep the existing MSYS utility as the `msys` source of truth**

Do not duplicate MSYS detection. `runner.ts` should consume `detectMsys()` from `electron/util/msys.ts` directly. The new build code should not read `project.json.msys_enabled`; a build step with `envType: 'msys'` is enough to require MSYS.

- [ ] **Step 4: Re-run the Visual Studio test and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run electron/build/visualStudio.test.ts`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 5: Commit the environment adapters**

```bash
git add electron/build/visualStudio.ts electron/build/visualStudio.test.ts
git commit -m "feat: add visual studio build environment adapter"
```

## Task 3: Build the Sequential Main-Process Runner and Failure Context Capture

**Files:**
- Create: `electron/build/runner.ts`
- Create: `electron/build/runner.test.ts`
- Modify: `electron/build/types.ts`
- Modify: `electron/main.ts`

- [ ] **Step 1: Write the failing runner tests**

Cover these cases in `electron/build/runner.test.ts`:
- all enabled steps succeed in order
- disabled steps emit `skipped` and do not spawn a process
- first failed step stops the pipeline and later steps remain `pending`
- stop request interrupts the active child process and yields `stopped`
- missing MSYS or missing Visual Studio yields an environment error before command spawn

Run: `node .\\node_modules\\vitest\\vitest.mjs run electron/build/runner.test.ts`

Expected: FAIL because the runner does not exist.

- [ ] **Step 2: Implement `ProjectBuildRunner` as a non-interactive child-process controller**

Use `child_process.spawn`, not PTY. The build pipeline does not need interactive input.

`runner.ts` should own:
- one active child process at a time
- per-run:
  - `runId`
  - `projectId`
  - `projectName`
  - `targetRepo`
  - `status`
  - ordered step states
  - aggregated log lines
  - last failure context
- event callbacks:

```ts
onLog(chunk: string): void
onStatus(snapshot: BuildRunSnapshot): void
```

For `msys` steps:
- resolve `cwd` against `targetRepo`
- require `detectMsys().available === true`
- spawn:

```text
<bash.exe> -lc "cd '<cwd>' && <command>"
```

For `visual-studio` steps:
- resolve `cwd` against `targetRepo`
- require `detectVisualStudio().available === true`
- spawn the `cmd.exe` bootstrap generated by `buildVisualStudioStepCommand()`

Failure capture should store:
- failed step index and id
- failed step config
- exit code / signal
- the tail of stdout/stderr
- a compact array of all step statuses

Log retention rule:
- keep the full log for panel display in memory for the active run
- separately keep a bounded failure-tail buffer such as the last 16 KB or 200 lines for diagnosis prompts

- [ ] **Step 3: Add app-wide singleton lifecycle in `electron/main.ts`**

Introduce one build session holder in `electron/main.ts`:
- `activeBuildRunner: ProjectBuildRunner | null`
- reject `build:start` when another run is already `running`
- clean up on app quit

Add IPC contracts:

```ts
'build:start'
'build:stop'
'build:get-state'
'build:get-last-failure'
```

Broadcast renderer events:

```ts
'build:data'
'build:status'
```

Each event payload should be serializable and window-agnostic so `App.tsx` can fully reconstruct panel state.

- [ ] **Step 4: Re-run runner tests and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run electron/build/runner.test.ts`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 5: Commit the build runner**

```bash
git add electron/build/runner.ts electron/build/runner.test.ts electron/main.ts
git commit -m "feat: add sequential project build runner"
```

## Task 4: Add a Manual Failure-Diagnosis Prompt Builder

**Files:**
- Create: `electron/build/analysisPrompt.ts`
- Create: `electron/build/analysisPrompt.test.ts`
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write the failing prompt-builder tests**

Cover these cases in `electron/build/analysisPrompt.test.ts`:
- the prompt includes project path, failed step name, env type, cwd, command, exit code, and step summary
- the prompt explicitly says "analyze only" and "do not modify code"
- logs are truncated to the configured maximum

Run: `node .\\node_modules\\vitest\\vitest.mjs run electron/build/analysisPrompt.test.ts`

Expected: FAIL because the module does not exist.

- [ ] **Step 2: Implement the diagnosis prompt builder**

Create a plain-text prompt shaped like:

```text
Analyze the following project build failure.

Constraints:
- Diagnose the failure cause only.
- Do not modify code.
- Do not propose or apply patches.
- Do not run commands.

Project:
- name: ...
- repo: ...

Failed step:
- name: ...
- env: msys | visual-studio
- cwd: ...
- command: ...
- exit code: ...

Step summary:
- 1. Build SDK: succeeded
- 2. Build Demo: failed

Failure log tail:
...

Reply in concise sections:
1. Failure category
2. Most likely cause
3. Evidence
4. What to check first
```

- [ ] **Step 3: Expose prompt retrieval through IPC**

Add:

```ts
window.api.build.getFailureAnalysisPrompt(): Promise<{ ok: boolean; value?: string; error?: string }>
```

Main-process behavior:
- return `{ ok: false }` if there is no failed run
- otherwise build the prompt from the runner's last failure context

This keeps prompt construction centralized and testable; the renderer only forwards it.

- [ ] **Step 4: Re-run prompt tests and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run electron/build/analysisPrompt.test.ts`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 5: Commit the diagnosis prompt path**

```bash
git add electron/build/analysisPrompt.ts electron/build/analysisPrompt.test.ts electron/main.ts electron/preload.ts
git commit -m "feat: add build failure analysis prompt support"
```

## Task 5: Add Project Build Editing UI to the Settings Dialog

**Files:**
- Create: `src/components/ProjectBuildSettingsSection.tsx`
- Create: `src/components/ProjectBuildSettingsSection.test.tsx`
- Modify: `src/components/AiSettingsDialog.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing settings-section tests**

Cover these cases in `src/components/ProjectBuildSettingsSection.test.tsx`:
- renders existing steps in order
- adds a new blank step with default `envType: 'msys'`
- reorders steps with move up / move down
- toggles step enabled state
- surfaces inline validation hints for blank `name`, `cwd`, or `command`

Run: `node .\\node_modules\\vitest\\vitest.mjs run src/components/ProjectBuildSettingsSection.test.tsx`

Expected: FAIL because the component does not exist.

- [ ] **Step 2: Implement `ProjectBuildSettingsSection.tsx`**

The section should support:
- project-scoped enable toggle for the entire build pipeline
- ordered steps list
- per-step fields:
  - `name`
  - `envType` select with `msys` / `visual-studio`
  - `cwd`
  - `command`
  - `enabled`
- list actions:
  - add
  - delete
  - move up
  - move down

Keep it controlled via props from `AiSettingsDialog.tsx` so the dialog still owns save/cancel flow.

- [ ] **Step 3: Load and save `build_config` from `AiSettingsDialog.tsx`**

Extend the settings dialog to:
- load the project's current build config when `projectId` changes
- show a hint block instead of the build editor when `projectId` is `null`
- save build config together with the rest of the project-scoped settings

Suggested save order:
1. app-wide settings (existing screenshot settings)
2. project AI settings
3. project repo-view AI settings
4. project build config

If build-config save fails, keep the dialog open and surface the returned validation or metadata error.

- [ ] **Step 4: Add styles for the build settings section**

Add styles for:
- step cards
- step header row
- inline move/delete actions
- command textarea
- validation message text

Keep the build editor visually grouped with the existing project settings cards rather than introducing a new modal.

- [ ] **Step 5: Re-run settings tests and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run src/components/ProjectBuildSettingsSection.test.tsx`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 6: Commit the settings UI**

```bash
git add src/components/ProjectBuildSettingsSection.tsx src/components/ProjectBuildSettingsSection.test.tsx src/components/AiSettingsDialog.tsx src/styles.css
git commit -m "feat: add project build pipeline settings editor"
```

## Task 6: Add the Dedicated Build Panel and Renderer Wiring

**Files:**
- Create: `src/components/BuildPanel.tsx`
- Create: `src/components/BuildPanel.test.tsx`
- Modify: `src/App.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing build-panel tests**

Cover these cases in `src/components/BuildPanel.test.tsx`:
- idle panel shows the configured step list and enabled "Start Build"
- running panel disables "Start Build" and enables "Stop Build"
- failed panel shows "Analyze Failure"
- analyze button stays disabled when there is no active main AICLI session

Run: `node .\\node_modules\\vitest\\vitest.mjs run src/components/BuildPanel.test.tsx`

Expected: FAIL because the panel does not exist.

- [ ] **Step 2: Implement `BuildPanel.tsx` as a dedicated drawer**

Render:
- project name / repo summary
- overall build status
- steps list with per-step status badges
- `Start Build`
- `Stop Build`
- `Analyze Failure`
- read-only log area (`pre` or virtualized list is not required in v1)

Panel behavior:
- dock it inside the main app as a dedicated build drawer/panel, separate from the terminal
- keep it closable without destroying the last run snapshot
- auto-scroll logs only while the user is already at the bottom

- [ ] **Step 3: Wire the build panel into `App.tsx`**

Add renderer state for:
- `buildConfig`
- `buildPanelOpen`
- `buildSnapshot`
- `buildLog`

Add main-app actions:
- open panel
- start build with current project id
- stop build
- subscribe/unsubscribe to `build:data` and `build:status`

Open-state recommendation:
- keep a top-level `Build` button visible near existing task actions
- when no project is selected, disable it with a tooltip/hint

- [ ] **Step 4: Re-run panel tests and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run src/components/BuildPanel.test.tsx`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 5: Commit the build panel**

```bash
git add src/components/BuildPanel.tsx src/components/BuildPanel.test.tsx src/App.tsx src/styles.css
git commit -m "feat: add dedicated project build panel"
```

## Task 7: Wire Manual Failure Analysis to the Main AICLI Session

**Files:**
- Modify: `src/App.tsx`
- Modify: `src/components/BuildPanel.tsx`
- Modify: `src/components/BuildPanel.test.tsx`

- [ ] **Step 1: Add the failing AICLI-forwarding tests**

Cover these cases in `src/components/BuildPanel.test.tsx` or a small `App`-level test:
- failed run + active `sessionId` enables "Analyze Failure"
- clicking "Analyze Failure" requests the prompt from `window.api.build.getFailureAnalysisPrompt()`
- the returned prompt is sent with `window.api.cc.sendUser(sessionId, prompt)`
- no active `sessionId` keeps the button disabled and shows a hint

Run: `node .\\node_modules\\vitest\\vitest.mjs run src/components/BuildPanel.test.tsx`

Expected: FAIL until the forwarding path is implemented.

- [ ] **Step 2: Implement the renderer-side forwarding path**

In `App.tsx`:
- require a live main `sessionId`
- on analyze click:
  1. call `window.api.build.getFailureAnalysisPrompt()`
  2. if successful, forward the returned prompt with `window.api.cc.sendUser(sessionId, prompt)`
  3. surface success/failure notice to the user

The build panel must not call `cc.write`; use `cc.sendUser` so the main session gets the message as a user request rather than raw terminal input.

- [ ] **Step 3: Keep the diagnosis-only guardrails visible in UI**

Add a small helper note near the button, for example:
- "This sends the failed build context to the current AICLI session for diagnosis only."

This mirrors the prompt contract and reduces accidental expectations that the AI will start editing code.

- [ ] **Step 4: Re-run the build-panel tests and typecheck**

Run:
- `node .\\node_modules\\vitest\\vitest.mjs run src/components/BuildPanel.test.tsx`
- `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 5: Commit the analysis wiring**

```bash
git add src/App.tsx src/components/BuildPanel.tsx src/components/BuildPanel.test.tsx
git commit -m "feat: send build failures to AICLI for diagnosis"
```

## Task 8: Final Verification on Realistic Pipelines

**Files:**
- Modify: none

- [ ] **Step 1: Run the focused automated suite**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/build/config.test.ts electron/build/visualStudio.test.ts electron/build/runner.test.ts electron/build/analysisPrompt.test.ts src/components/ProjectBuildSettingsSection.test.tsx src/components/BuildPanel.test.tsx
```

Expected: PASS

- [ ] **Step 2: Run full typecheck**

Run:

```bash
npm.cmd run typecheck
```

Expected: PASS

- [ ] **Step 3: Manual MSYS pipeline verification**

Configure one test project with:

```text
Step 1
- name: Build SDK
- envType: msys
- cwd: sdk
- command: ./build_sdk.sh
```

Expected:
- build panel enters `running`
- log stream appears live
- success marks step `succeeded`

- [ ] **Step 4: Manual mixed MSYS -> Visual Studio verification**

Configure one test project with:

```text
Step 1
- name: Build SDK
- envType: msys
- cwd: sdk
- command: ./build_sdk.sh

Step 2
- name: Build Demo
- envType: visual-studio
- cwd: demo
- command: msbuild demo.sln /p:Configuration=Debug /p:Platform=x64
```

Expected:
- steps run in order
- Visual Studio environment is initialized before the second command
- a failure in Step 2 leaves Step 3+ untouched

- [ ] **Step 5: Manual failure-analysis verification**

Trigger a failing build command intentionally, then verify:
- overall build status becomes `failed`
- `Analyze Failure` appears
- button is disabled when no AICLI session is active
- once a main AICLI session is running, clicking the button sends one diagnosis prompt into that session
- the prompt asks for diagnosis only and not code changes

- [ ] **Step 6: Final working-tree check**

Run:

```bash
git status --short
```

Expected: clean working tree after all commits in this plan.

## Self-Review Checklist

- Spec coverage:
  - project-level single default pipeline with multiple sequential steps is covered in Tasks 1 and 5
  - per-step `msys` / `visual-studio` execution is covered in Tasks 2 and 3
  - dedicated build panel is covered in Task 6
  - failure-stop behavior is covered in Task 3
  - manual AICLI diagnosis-only handoff is covered in Tasks 4 and 7
- Scope control:
  - no multi-pipeline support
  - no parallel build steps
  - no `xcodebuild`
  - no AI auto-fix flow
- Integration sanity:
  - `project.json.msys_enabled` remains untouched for AI sessions
  - build execution does not require PTY or terminal input
  - no diagnosis action is available without a running main AICLI session
