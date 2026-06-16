# Runtime Runner Log Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a project-level Run workflow that starts a separate runtime process, captures runtime logs, and sends those logs to the active AI CLI for diagnosis.

**Architecture:** Build and runtime stay separate. The new `electron/runtime/*` module owns runtime config, process lifecycle, log buffering, and analysis prompt generation, while renderer state in `App.tsx` wires runtime IPC into the existing build panel surface. Project settings get a new runtime config section stored as `runtime_config` beside `build_config`.

**Tech Stack:** Electron main process, React renderer, TypeScript, Vitest, Node child_process, existing MSYS2 and Visual Studio environment helpers.

---

## File Structure

- Create `electron/runtime/types.ts`
  - Shared runtime config, state, IPC result, and event types for main/preload.
- Create `electron/runtime/config.ts`
  - Normalize, validate, read, and write `runtime_config` in `project.json`.
- Create `electron/runtime/config.test.ts`
  - Tests for defaulting, trimming, validation, metadata repair, and persistence.
- Create `electron/runtime/analysisPrompt.ts`
  - Builds a bounded, diagnostic-only AI prompt from runtime state.
- Create `electron/runtime/analysisPrompt.test.ts`
  - Tests prompt gating, context inclusion, and tail truncation.
- Create `electron/runtime/runner.ts`
  - Starts/stops one runtime process, captures stdout/stderr, broadcasts state/data.
- Create `electron/runtime/runner.test.ts`
  - Tests lifecycle transitions, MSYS/Visual Studio spawning, output decoding, log caps.
- Modify `electron/main.ts`
  - Instantiate runtime runner, broadcast runtime events, add project/runtime IPC.
- Modify `electron/preload.ts`
  - Add runtime types and `window.api.runtime`.
- Create `src/components/ProjectRuntimeSettingsSection.tsx`
  - Settings UI for `runtime_config`.
- Create `src/components/ProjectRuntimeSettingsSection.test.tsx`
  - Renderer tests for settings controls and helper formatting.
- Modify `src/components/AiSettingsDialog.tsx`
  - Load/save runtime config beside build config.
- Modify `src/components/AiSettingsDialog.test.tsx`
  - Tests for runtime section rendering and save plumbing.
- Modify `src/components/ProjectBuildPanel.tsx`
  - Add separate runtime section with Run, Stop, Send Log, and runtime log.
- Modify `src/components/ProjectBuildPanel.test.tsx`
  - Tests for runtime UI states and send gating.
- Modify `src/App.tsx`
  - Add runtime config/state, IPC subscriptions, start/stop/send handlers.

---

### Task 1: Runtime Config Model

**Files:**
- Create: `electron/runtime/types.ts`
- Create: `electron/runtime/config.ts`
- Create: `electron/runtime/config.test.ts`

- [ ] **Step 1: Write failing runtime config tests**

Add `electron/runtime/config.test.ts`:

```ts
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  DEFAULT_RUNTIME_CONFIG,
  getProjectRuntimeConfig,
  normalizeRuntimeConfig,
  setProjectRuntimeConfig,
  type ProjectRuntimeConfig
} from './config.js'

let root: string
let projectDir: string
let metaPath: string

beforeEach(async () => {
  root = await fs.mkdtemp(join(tmpdir(), 'runtime-config-'))
  projectDir = join(root, 'project')
  metaPath = join(projectDir, 'project.json')
  await fs.mkdir(projectDir, { recursive: true })
})

afterEach(async () => {
  await fs.rm(root, { recursive: true, force: true })
})

describe('normalizeRuntimeConfig', () => {
  it('returns the default config for invalid input', () => {
    expect(normalizeRuntimeConfig(null)).toEqual<ProjectRuntimeConfig>(
      DEFAULT_RUNTIME_CONFIG
    )
    expect(normalizeRuntimeConfig({ command: 42 })).toEqual<ProjectRuntimeConfig>(
      DEFAULT_RUNTIME_CONFIG
    )
  })

  it('trims fields and defaults environment settings', () => {
    expect(
      normalizeRuntimeConfig({
        enabled: true,
        cwd: ' app ',
        command: ' npm run dev ',
        envType: 'visual-studio'
      })
    ).toEqual<ProjectRuntimeConfig>({
      enabled: true,
      cwd: 'app',
      command: 'npm run dev',
      envType: 'visual-studio',
      visualStudioInstanceId: '',
      outputEncoding: 'auto'
    })
  })
})

describe('getProjectRuntimeConfig', () => {
  it('returns default config when runtime_config is absent', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p1' }, null, 2), 'utf8')

    const result = await getProjectRuntimeConfig(metaPath)

    expect(result).toEqual({ ok: true, value: DEFAULT_RUNTIME_CONFIG })
  })

  it('repairs corrupted project.json and reads runtime_config', async () => {
    await fs.writeFile(
      metaPath,
      '{ "id": "p2", "runtime_config": { "enabled": true, "cwd": ".", "command": "npm run dev" } } trailing',
      'utf8'
    )

    const result = await getProjectRuntimeConfig(metaPath)

    expect(result).toEqual({
      ok: true,
      repaired: true,
      value: {
        enabled: true,
        cwd: '.',
        command: 'npm run dev',
        envType: 'msys',
        visualStudioInstanceId: '',
        outputEncoding: 'auto'
      }
    })
  })
})

describe('setProjectRuntimeConfig', () => {
  it('rejects absolute and parent-traversal working directories', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p3' }, null, 2), 'utf8')

    const absoluteResult = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: 'C:\\outside',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'auto'
    })
    expect(absoluteResult.ok).toBe(false)
    if (absoluteResult.ok) throw new Error('expected validation failure')
    expect(absoluteResult.details?.[0].path).toBe('runtime_config.cwd')

    const traversalResult = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: '..\\outside',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'auto'
    })
    expect(traversalResult.ok).toBe(false)
    if (traversalResult.ok) throw new Error('expected validation failure')
    expect(traversalResult.details?.[0].message).toContain('parent traversal')
  })

  it('rejects empty command and missing Visual Studio instance', async () => {
    await fs.writeFile(metaPath, JSON.stringify({ id: 'p4' }, null, 2), 'utf8')

    const result = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: '.',
      command: '',
      envType: 'visual-studio',
      visualStudioInstanceId: '',
      outputEncoding: 'auto'
    })

    expect(result.ok).toBe(false)
    if (result.ok) throw new Error('expected validation failure')
    expect(result.details?.map((issue) => issue.path)).toEqual([
      'runtime_config.visualStudioInstanceId',
      'runtime_config.command'
    ])
  })

  it('persists normalized runtime_config without dropping other metadata', async () => {
    await fs.writeFile(
      metaPath,
      JSON.stringify({ id: 'p5', name: 'Demo', build_config: { enabled: false, steps: [] } }, null, 2),
      'utf8'
    )

    const result = await setProjectRuntimeConfig(metaPath, {
      enabled: true,
      cwd: ' app ',
      command: ' npm run dev ',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'utf8'
    })

    expect(result).toEqual({ ok: true })
    const saved = JSON.parse(await fs.readFile(metaPath, 'utf8'))
    expect(saved.name).toBe('Demo')
    expect(saved.build_config).toEqual({ enabled: false, steps: [] })
    expect(saved.runtime_config).toEqual({
      enabled: true,
      cwd: 'app',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'utf8'
    })
  })
})
```

- [ ] **Step 2: Run runtime config tests and verify failure**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/config.test.ts
```

Expected: FAIL because `electron/runtime/config.ts` does not exist.

- [ ] **Step 3: Add runtime types**

Create `electron/runtime/types.ts`:

```ts
import type { BuildOutputEncoding, BuildStepEnvType } from '../build/types.js'

export type RuntimeEnvType = BuildStepEnvType
export type RuntimeOutputEncoding = BuildOutputEncoding
export type RuntimeStatus = 'idle' | 'running' | 'exited' | 'failed' | 'stopped'

export interface ProjectRuntimeConfig {
  enabled: boolean
  cwd: string
  command: string
  envType: RuntimeEnvType
  visualStudioInstanceId: string
  outputEncoding: RuntimeOutputEncoding
}

export interface RuntimeState {
  status: RuntimeStatus
  projectId: string | null
  projectName: string | null
  targetRepo: string | null
  cwd: string | null
  command: string | null
  envType: RuntimeEnvType | null
  visualStudioInstanceId: string | null
  visualStudioDisplayName: string | null
  outputEncoding: RuntimeOutputEncoding | null
  startedAt: string | null
  finishedAt: string | null
  exitCode: number | null
  signal: NodeJS.Signals | null
  log: string
}

export interface RuntimeDataEvent {
  at: string
  projectId: string | null
  stream: 'stdout' | 'stderr' | 'system'
  chunk: string
}

export interface StartRuntimeRequest {
  projectId: string
  projectName: string
  targetRepo: string
  config: ProjectRuntimeConfig
}

export type RuntimeStartResult =
  | { ok: true; state: RuntimeState }
  | { ok: false; error: string; state: RuntimeState }

export type RuntimeStopResult = { ok: true } | { ok: false; error: string }

export type RuntimeAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }
```

- [ ] **Step 4: Add runtime config implementation**

Create `electron/runtime/config.ts`:

```ts
import { isAbsolute } from 'path'
import { readProjectMetaFile, writeProjectMetaFile } from '../store/projectMeta.js'
import type {
  ProjectRuntimeConfig,
  RuntimeEnvType,
  RuntimeOutputEncoding
} from './types.js'

export type {
  ProjectRuntimeConfig,
  RuntimeEnvType,
  RuntimeOutputEncoding
} from './types.js'

export interface RuntimeConfigValidationIssue {
  path: string
  message: string
}

export type ProjectRuntimeConfigReadResult =
  | { ok: true; value: ProjectRuntimeConfig; repaired?: true }
  | { ok: false; error: string }

export type ProjectRuntimeConfigWriteResult =
  | { ok: true; repaired?: true }
  | { ok: false; error: string; details?: RuntimeConfigValidationIssue[] }

export const DEFAULT_RUNTIME_CONFIG: ProjectRuntimeConfig = {
  enabled: false,
  cwd: '.',
  command: '',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}

interface NormalizedRuntimeConfig extends ProjectRuntimeConfig {
  rawEnvType?: unknown
  rawOutputEncoding?: unknown
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value)
}

function isRuntimeEnvType(value: unknown): value is RuntimeEnvType {
  return value === 'msys' || value === 'visual-studio'
}

function isRuntimeOutputEncoding(value: unknown): value is RuntimeOutputEncoding {
  return value === 'auto' || value === 'utf8' || value === 'gbk'
}

function hasParentTraversal(cwd: string): boolean {
  return cwd.split(/[\\/]+/).some((segment) => segment === '..')
}

function normalizeRuntimeConfigInternal(value: unknown): NormalizedRuntimeConfig {
  if (!isRecord(value)) return { ...DEFAULT_RUNTIME_CONFIG }
  const envType = isRuntimeEnvType(value.envType) ? value.envType : 'msys'
  const outputEncoding = isRuntimeOutputEncoding(value.outputEncoding)
    ? value.outputEncoding
    : 'auto'

  return {
    enabled: typeof value.enabled === 'boolean' ? value.enabled : false,
    cwd: typeof value.cwd === 'string' && value.cwd.trim() ? value.cwd.trim() : '.',
    command: typeof value.command === 'string' ? value.command.trim() : '',
    envType,
    visualStudioInstanceId:
      typeof value.visualStudioInstanceId === 'string'
        ? value.visualStudioInstanceId.trim()
        : '',
    outputEncoding,
    rawEnvType: isRuntimeEnvType(value.envType) ? undefined : value.envType,
    rawOutputEncoding: isRuntimeOutputEncoding(value.outputEncoding)
      ? undefined
      : value.outputEncoding
  }
}

export function normalizeRuntimeConfig(value: unknown): ProjectRuntimeConfig {
  const {
    rawEnvType: _rawEnvType,
    rawOutputEncoding: _rawOutputEncoding,
    ...config
  } = normalizeRuntimeConfigInternal(value)
  return config
}

function validateRuntimeConfig(config: NormalizedRuntimeConfig): RuntimeConfigValidationIssue[] {
  const issues: RuntimeConfigValidationIssue[] = []
  if (config.rawEnvType !== undefined) {
    issues.push({
      path: 'runtime_config.envType',
      message: 'envType must be one of: msys, visual-studio'
    })
  }
  if (config.envType === 'visual-studio' && !config.visualStudioInstanceId.trim()) {
    issues.push({
      path: 'runtime_config.visualStudioInstanceId',
      message: 'visualStudioInstanceId must be selected for visual-studio runtime'
    })
  }
  if (config.rawOutputEncoding !== undefined) {
    issues.push({
      path: 'runtime_config.outputEncoding',
      message: 'outputEncoding must be one of: auto, utf8, gbk'
    })
  }
  if (!config.cwd.trim()) {
    issues.push({ path: 'runtime_config.cwd', message: 'cwd must be a non-empty string' })
  } else if (isAbsolute(config.cwd)) {
    issues.push({
      path: 'runtime_config.cwd',
      message: 'cwd must be a relative path within target_repo'
    })
  } else if (hasParentTraversal(config.cwd)) {
    issues.push({
      path: 'runtime_config.cwd',
      message: 'cwd must not contain parent traversal segments'
    })
  }
  if (!config.command.trim()) {
    issues.push({
      path: 'runtime_config.command',
      message: 'command must be a non-empty string'
    })
  }
  return issues
}

export async function getProjectRuntimeConfig(
  metaPath: string
): Promise<ProjectRuntimeConfigReadResult> {
  try {
    const readResult = await readProjectMetaFile(metaPath)
    if (!readResult.ok) return { ok: false, error: readResult.error }
    const value = normalizeRuntimeConfig(readResult.meta.runtime_config)
    return readResult.repaired ? { ok: true, repaired: true, value } : { ok: true, value }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}

export async function setProjectRuntimeConfig(
  metaPath: string,
  config: ProjectRuntimeConfig
): Promise<ProjectRuntimeConfigWriteResult> {
  try {
    const readResult = await readProjectMetaFile(metaPath)
    if (!readResult.ok) return { ok: false, error: readResult.error }

    const normalized = normalizeRuntimeConfigInternal(config)
    const issues = validateRuntimeConfig(normalized)
    if (issues.length > 0) {
      return { ok: false, error: 'invalid runtime config', details: issues }
    }

    await writeProjectMetaFile(metaPath, {
      ...readResult.meta,
      runtime_config: normalizeRuntimeConfig(normalized) as unknown as Record<string, unknown>
    })
    return readResult.repaired ? { ok: true, repaired: true } : { ok: true }
  } catch (error: unknown) {
    return { ok: false, error: error instanceof Error ? error.message : String(error) }
  }
}
```

- [ ] **Step 5: Run runtime config tests**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/config.test.ts
```

Expected: PASS.

- [ ] **Step 6: Commit runtime config model**

```bash
git add electron/runtime/types.ts electron/runtime/config.ts electron/runtime/config.test.ts
git commit -m "feat: add runtime config model"
```

---

### Task 2: Runtime Analysis Prompt

**Files:**
- Create: `electron/runtime/analysisPrompt.ts`
- Create: `electron/runtime/analysisPrompt.test.ts`

- [ ] **Step 1: Write failing prompt tests**

Create `electron/runtime/analysisPrompt.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { buildRuntimeAnalysisPrompt, getRuntimeAnalysisPrompt } from './analysisPrompt.js'
import type { RuntimeState } from './types.js'

const baseState: RuntimeState = {
  status: 'running',
  projectId: 'p1',
  projectName: 'Demo',
  targetRepo: 'E:\\repo',
  cwd: 'E:\\repo\\app',
  command: 'npm run dev',
  envType: 'msys',
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: 'auto',
  startedAt: '2026-06-12T10:00:00.000Z',
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: 'server started\nGET /health 200\n'
}

describe('buildRuntimeAnalysisPrompt', () => {
  it('builds a diagnostic-only prompt with runtime context', () => {
    const prompt = buildRuntimeAnalysisPrompt(baseState, { logTailLimit: 200 })

    expect(prompt).toContain('Analyze this runtime log for likely problems.')
    expect(prompt).toContain('Do not modify code.')
    expect(prompt).toContain('Project: Demo (p1)')
    expect(prompt).toContain('Working directory: E:\\repo\\app')
    expect(prompt).toContain('Command: npm run dev')
    expect(prompt).toContain('server started')
    expect(prompt).toContain('What to check first')
  })

  it('caps the runtime log tail', () => {
    const prompt = buildRuntimeAnalysisPrompt(
      { ...baseState, log: `${'x'.repeat(300)}fatal tail\n` },
      { logTailLimit: 20 }
    )

    expect(prompt).not.toContain('x'.repeat(100))
    expect(prompt).toContain('fatal tail')
  })
})

describe('getRuntimeAnalysisPrompt', () => {
  it('rejects empty runtime logs', () => {
    expect(getRuntimeAnalysisPrompt({ ...baseState, log: '   ' })).toEqual({
      ok: false,
      error: 'no runtime log available'
    })
  })

  it('returns a prompt for running, failed, stopped, or exited runtime states', () => {
    for (const status of ['running', 'failed', 'stopped', 'exited'] as const) {
      const result = getRuntimeAnalysisPrompt({ ...baseState, status })
      expect(result.ok).toBe(true)
    }
  })
})
```

- [ ] **Step 2: Run prompt tests and verify failure**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/analysisPrompt.test.ts
```

Expected: FAIL because `analysisPrompt.ts` does not exist.

- [ ] **Step 3: Implement runtime prompt builder**

Create `electron/runtime/analysisPrompt.ts`:

```ts
import type { RuntimeAnalysisPromptResult, RuntimeState } from './types.js'

export const RUNTIME_ANALYSIS_LOG_TAIL_LIMIT = 200_000

function tail(value: string, limit: number): string {
  if (value.length <= limit) return value
  return `[runtime] earlier log truncated...\n${value.slice(-limit)}`
}

export function buildRuntimeAnalysisPrompt(
  state: RuntimeState,
  opts: { logTailLimit?: number } = {}
): string {
  const logTail = tail(state.log.trim(), opts.logTailLimit ?? RUNTIME_ANALYSIS_LOG_TAIL_LIMIT)
  return [
    'Analyze this runtime log for likely problems.',
    'Do not modify code.',
    'Do not provide patches.',
    'Do not execute commands.',
    '',
    `Project: ${state.projectName ?? 'Unknown'} (${state.projectId ?? 'unknown'})`,
    `Target repo: ${state.targetRepo ?? 'unknown'}`,
    `Runtime status: ${state.status}`,
    `Environment: ${state.envType ?? 'unknown'}`,
    `Working directory: ${state.cwd ?? 'unknown'}`,
    `Command: ${state.command ?? 'unknown'}`,
    `Exit code: ${state.exitCode ?? 'n/a'}`,
    `Signal: ${state.signal ?? 'n/a'}`,
    '',
    'Recent runtime log:',
    logTail,
    '',
    'Reply using these sections:',
    'Problem summary',
    'Evidence',
    'Likely cause',
    'What to check first'
  ].join('\n')
}

export function getRuntimeAnalysisPrompt(state: RuntimeState): RuntimeAnalysisPromptResult {
  if (!state.log.trim()) {
    return { ok: false, error: 'no runtime log available' }
  }
  return { ok: true, prompt: buildRuntimeAnalysisPrompt(state) }
}
```

- [ ] **Step 4: Run prompt tests**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/analysisPrompt.test.ts
```

Expected: PASS.

- [ ] **Step 5: Commit runtime analysis prompt**

```bash
git add electron/runtime/analysisPrompt.ts electron/runtime/analysisPrompt.test.ts
git commit -m "feat: add runtime log analysis prompt"
```

---

### Task 3: Runtime Runner

**Files:**
- Create: `electron/runtime/runner.ts`
- Create: `electron/runtime/runner.test.ts`

- [ ] **Step 1: Write failing runner tests**

Create `electron/runtime/runner.test.ts`:

```ts
import { EventEmitter } from 'events'
import { PassThrough } from 'stream'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { createRuntimeRunner, type SpawnedRuntimeProcess } from './runner.js'
import type { ProjectRuntimeConfig } from './types.js'

class FakeChild extends EventEmitter implements SpawnedRuntimeProcess {
  pid = 9876
  stdout = new PassThrough()
  stderr = new PassThrough()
  kill = vi.fn((signal?: NodeJS.Signals | number) => {
    this.emit('killed', signal)
    return true
  })
}

function flush(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve))
}

const msysConfig: ProjectRuntimeConfig = {
  enabled: true,
  cwd: '.',
  command: 'npm run dev',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}

afterEach(() => {
  vi.restoreAllMocks()
})

describe('createRuntimeRunner', () => {
  it('starts an MSYS runtime, captures logs, and exits cleanly', async () => {
    const child = new FakeChild()
    const spawn = vi.fn(() => child)
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: []
      }),
      now: () => '2026-06-12T10:00:00.000Z'
    })

    const start = await runner.start({
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig
    })

    expect(start.ok).toBe(true)
    expect(spawn).toHaveBeenCalledWith(
      'C:\\msys64\\usr\\bin\\bash.exe',
      ['-lc', "cd '/e/repo' && npm run dev"],
      expect.objectContaining({ cwd: 'E:\\repo', shell: false, windowsHide: true })
    )

    child.stdout.write('server started\n')
    child.stderr.write('warn line\n')
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'exited',
      projectId: 'p1',
      cwd: 'E:\\repo',
      command: 'npm run dev',
      exitCode: 0
    })
    expect(runner.getState().log).toContain('server started')
    expect(runner.getState().log).toContain('warn line')
  })

  it('starts a Visual Studio runtime through cmd.exe', async () => {
    const child = new FakeChild()
    const spawn = vi.fn(() => child)
    const resolveVisualStudioEnvironment = vi.fn().mockResolvedValue({
      ok: true,
      displayName: 'Visual Studio 2022 Community',
      installationPath: 'C:\\VS',
      devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
      env: { Path: 'C:\\VS\\bin' }
    })
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      resolveVisualStudioEnvironment,
      now: () => '2026-06-12T10:00:00.000Z'
    })

    const start = await runner.start({
      projectId: 'p-vs',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        ...msysConfig,
        cwd: 'app',
        command: 'demo.exe',
        envType: 'visual-studio',
        visualStudioInstanceId: 'vs-1',
        outputEncoding: 'gbk'
      }
    })

    expect(start.ok).toBe(true)
    expect(resolveVisualStudioEnvironment).toHaveBeenCalledWith(
      expect.objectContaining({ instanceId: 'vs-1', platform: 'win32' })
    )
    expect(spawn).toHaveBeenCalledWith(
      'cmd.exe',
      ['/d', '/s', '/c', 'demo.exe'],
      expect.objectContaining({
        cwd: 'E:\\repo\\app',
        env: { Path: 'C:\\VS\\bin' },
        windowsVerbatimArguments: true
      })
    )
  })

  it('stops the active process tree on Windows', async () => {
    const child = new FakeChild()
    const killProcessTree = vi.fn()
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn: vi.fn(() => child),
      killProcessTree,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: []
      })
    })

    await runner.start({
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig
    })
    expect(runner.stop()).toEqual({ ok: true })
    child.emit('close', null, 'SIGTERM')
    await flush()

    expect(killProcessTree).toHaveBeenCalledWith(9876)
    expect(runner.getState().status).toBe('stopped')
  })

  it('blocks a second runtime while one is running and truncates active logs', async () => {
    const child = new FakeChild()
    const runner = createRuntimeRunner({
      spawn: vi.fn(() => child),
      logLimit: 30,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: []
      })
    })

    await runner.start({
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig
    })
    child.stdout.write('first line that will be truncated\n')
    child.stdout.write('important tail\n')

    const second = await runner.start({
      projectId: 'p2',
      projectName: 'Other',
      targetRepo: 'E:\\other',
      config: msysConfig
    })

    expect(second.ok).toBe(false)
    expect(second.error).toBe('runtime already running')
    expect(runner.getState().log).toContain('important tail')
    expect(runner.getState().log).toContain('[runtime] earlier log truncated')
  })
})
```

- [ ] **Step 2: Run runner tests and verify failure**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/runner.test.ts
```

Expected: FAIL because `runner.ts` does not exist.

- [ ] **Step 3: Implement runtime runner**

Create `electron/runtime/runner.ts` using the build runner as a pattern. The file must include these exported contracts and behavior:

```ts
import { execFile, spawn } from 'child_process'
import iconv from 'iconv-lite'
import { isAbsolute, relative, resolve, sep } from 'path'
import { StringDecoder } from 'string_decoder'
import { detectMsys, type MsysInfo } from '../util/msys.js'
import { resolveVisualStudioEnvironment } from '../build/visualStudio.js'
import type {
  RuntimeDataEvent,
  RuntimeStartResult,
  RuntimeState,
  RuntimeStopResult,
  StartRuntimeRequest
} from './types.js'

export type {
  RuntimeDataEvent,
  RuntimeStartResult,
  RuntimeState,
  RuntimeStopResult,
  StartRuntimeRequest
} from './types.js'

export interface SpawnedRuntimeProcess {
  pid?: number
  stdout: NodeJS.ReadableStream | null
  stderr: NodeJS.ReadableStream | null
  kill(signal?: NodeJS.Signals | number): boolean
  on(event: 'close', listener: (code: number | null, signal: NodeJS.Signals | null) => void): this
  on(event: 'error', listener: (error: Error) => void): this
}
```

Use these constants and helpers:

```ts
const ACTIVE_LOG_LIMIT = 200_000
const TRUNCATED_LOG_MARKER = '[runtime] earlier log truncated...\n'

function initialState(): RuntimeState {
  return {
    status: 'idle',
    projectId: null,
    projectName: null,
    targetRepo: null,
    cwd: null,
    command: null,
    envType: null,
    visualStudioInstanceId: null,
    visualStudioDisplayName: null,
    outputEncoding: null,
    startedAt: null,
    finishedAt: null,
    exitCode: null,
    signal: null,
    log: ''
  }
}

function appendWindowedLog(current: string, chunk: string, limit: number): string {
  const base = current.startsWith(TRUNCATED_LOG_MARKER)
    ? current.slice(TRUNCATED_LOG_MARKER.length)
    : current
  const next = `${base}${chunk}`
  if (next.length <= limit && !current.startsWith(TRUNCATED_LOG_MARKER)) return next
  const tailLimit = Math.max(0, limit - TRUNCATED_LOG_MARKER.length)
  return `${TRUNCATED_LOG_MARKER}${next.slice(-tailLimit)}`
}
```

Implement `createRuntimeRunner()` with this public shape:

```ts
export interface RuntimeRunner {
  start(request: StartRuntimeRequest): Promise<RuntimeStartResult>
  stop(): RuntimeStopResult
  getState(): RuntimeState
  onData(listener: (event: RuntimeDataEvent) => void): () => void
  onStatus(listener: (state: RuntimeState) => void): () => void
}
```

The implementation should:

- Resolve `config.cwd` under `targetRepo` and reject paths outside the repo.
- For MSYS, spawn `bashPath` with `['-lc', "cd '<msys path>' && <command>"]`.
- For Visual Studio, call `resolveVisualStudioEnvironment` and spawn `cmd.exe /d /s /c <command>`.
- Append stdout/stderr to `state.log`.
- Emit `runtime:data`-compatible events.
- Emit cloned state snapshots.
- On child `close`:
  - `stopped` if stop was requested.
  - `exited` if code is `0`.
  - `failed` otherwise.
- On `stop()`, call `taskkill /T /F` via injected `killProcessTree` on Windows when a pid exists; otherwise call `child.kill('SIGTERM')`.

- [ ] **Step 4: Run runner tests**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/runner.test.ts
```

Expected: PASS.

- [ ] **Step 5: Commit runtime runner**

```bash
git add electron/runtime/runner.ts electron/runtime/runner.test.ts
git commit -m "feat: add runtime process runner"
```

---

### Task 4: Runtime IPC And Preload API

**Files:**
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Add main-process runtime imports and runner**

Modify imports in `electron/main.ts`:

```ts
import {
  getProjectRuntimeConfig,
  setProjectRuntimeConfig,
  type ProjectRuntimeConfig
} from './runtime/config.js'
import { createRuntimeRunner } from './runtime/runner.js'
import { getRuntimeAnalysisPrompt } from './runtime/analysisPrompt.js'
```

Near the existing build runner:

```ts
const buildRunner = createBuildRunner()
const runtimeRunner = createRuntimeRunner()
```

Add broadcasts:

```ts
runtimeRunner.onData((event) => {
  broadcastToAllWindows('runtime:data', event)
})

runtimeRunner.onStatus((nextState) => {
  broadcastToAllWindows('runtime:status', nextState)
})
```

- [ ] **Step 2: Add project runtime config IPC**

In `electron/main.ts`, next to `project:get-build-config` and `project:set-build-config`, add:

```ts
ipcMain.handle('project:get-runtime-config', async (_e, { id }: { id: string }) => {
  const metaPath = join(projectDirFn(id), 'project.json')
  return await getProjectRuntimeConfig(metaPath)
})

ipcMain.handle(
  'project:set-runtime-config',
  async (
    _e,
    { id, config }: { id: string; config: ProjectRuntimeConfig }
  ): Promise<{
    ok: boolean
    repaired?: boolean
    error?: string
    details?: Array<{ path: string; message: string }>
  }> => {
    const metaPath = join(projectDirFn(id), 'project.json')
    return await setProjectRuntimeConfig(metaPath, config)
  }
)
```

- [ ] **Step 3: Add runtime start/stop/prompt IPC**

In `electron/main.ts`, after build IPC handlers, add:

```ts
ipcMain.handle('runtime:start', async (_e, { id }: { id: string }) => {
  const row = getProject(id)
  if (!row) {
    return { ok: false as const, error: 'project not found', state: runtimeRunner.getState() }
  }

  const metaPath = join(projectDirFn(id), 'project.json')
  const metaResult = await readProjectMetaFile(metaPath)
  if (!metaResult.ok) {
    return { ok: false as const, error: metaResult.error, state: runtimeRunner.getState() }
  }

  const configResult = await getProjectRuntimeConfig(metaPath)
  if (!configResult.ok) {
    return { ok: false as const, error: configResult.error, state: runtimeRunner.getState() }
  }
  if (!configResult.value.enabled) {
    return { ok: false as const, error: 'runtime config is disabled', state: runtimeRunner.getState() }
  }

  const metaName =
    typeof metaResult.meta.name === 'string' && metaResult.meta.name.trim()
      ? metaResult.meta.name.trim()
      : row.name
  const targetRepo =
    typeof metaResult.meta.target_repo === 'string' && metaResult.meta.target_repo.trim()
      ? metaResult.meta.target_repo.trim()
      : row.target_repo

  if (!targetRepo) {
    return {
      ok: false as const,
      error: 'project target_repo is not configured',
      state: runtimeRunner.getState()
    }
  }

  try {
    const stat = await fs.stat(targetRepo)
    if (!stat.isDirectory()) {
      return {
        ok: false as const,
        error: 'project target_repo is not a directory',
        state: runtimeRunner.getState()
      }
    }
  } catch (error: unknown) {
    return {
      ok: false as const,
      error: error instanceof Error ? error.message : String(error),
      state: runtimeRunner.getState()
    }
  }

  return await runtimeRunner.start({
    projectId: id,
    projectName: metaName || id,
    targetRepo,
    config: configResult.value
  })
})

ipcMain.handle('runtime:stop', () => runtimeRunner.stop())
ipcMain.handle('runtime:get-state', () => runtimeRunner.getState())
ipcMain.handle('runtime:get-analysis-prompt', () => {
  return getRuntimeAnalysisPrompt(runtimeRunner.getState())
})
```

- [ ] **Step 4: Add preload runtime types**

In `electron/preload.ts`, add runtime interfaces near build interfaces:

```ts
export type RuntimeStatus = 'idle' | 'running' | 'exited' | 'failed' | 'stopped'

export interface ProjectRuntimeConfig {
  enabled: boolean
  cwd: string
  command: string
  envType: BuildStepEnvType
  visualStudioInstanceId: string
  outputEncoding: BuildOutputEncoding
}

export type ProjectRuntimeConfigReadResult =
  | { ok: true; value: ProjectRuntimeConfig; repaired?: true }
  | { ok: false; error: string }

export type ProjectRuntimeConfigWriteResult =
  | { ok: true; repaired?: true }
  | { ok: false; error: string; details?: BuildConfigValidationIssue[] }

export interface RuntimeState {
  status: RuntimeStatus
  projectId: string | null
  projectName: string | null
  targetRepo: string | null
  cwd: string | null
  command: string | null
  envType: BuildStepEnvType | null
  visualStudioInstanceId: string | null
  visualStudioDisplayName: string | null
  outputEncoding: BuildOutputEncoding | null
  startedAt: string | null
  finishedAt: string | null
  exitCode: number | null
  signal: NodeJS.Signals | null
  log: string
}

export interface RuntimeDataEvent {
  at: string
  projectId: string | null
  stream: 'stdout' | 'stderr' | 'system'
  chunk: string
}

export type RuntimeStartResult =
  | { ok: true; state: RuntimeState }
  | { ok: false; error: string; state: RuntimeState }

export type RuntimeStopResult = { ok: true } | { ok: false; error: string }

export type RuntimeAnalysisPromptResult =
  | { ok: true; prompt: string }
  | { ok: false; error: string }
```

- [ ] **Step 5: Expose project and runtime APIs in preload**

In `project`, add:

```ts
getRuntimeConfig: (id: string) =>
  ipcRenderer.invoke('project:get-runtime-config', { id }) as Promise<ProjectRuntimeConfigReadResult>,
setRuntimeConfig: (id: string, config: ProjectRuntimeConfig) =>
  ipcRenderer.invoke('project:set-runtime-config', { id, config }) as Promise<ProjectRuntimeConfigWriteResult>,
```

Add top-level `runtime` beside `build`:

```ts
runtime: {
  start: (projectId: string) =>
    ipcRenderer.invoke('runtime:start', { id: projectId }) as Promise<RuntimeStartResult>,
  stop: () => ipcRenderer.invoke('runtime:stop') as Promise<RuntimeStopResult>,
  getState: () => ipcRenderer.invoke('runtime:get-state') as Promise<RuntimeState>,
  getAnalysisPrompt: () =>
    ipcRenderer.invoke('runtime:get-analysis-prompt') as Promise<RuntimeAnalysisPromptResult>,
  onData: (cb: (evt: RuntimeDataEvent) => void) => {
    const handler = (_event: IpcRendererEvent, evt: RuntimeDataEvent) => cb(evt)
    ipcRenderer.on('runtime:data', handler)
    return () => ipcRenderer.removeListener('runtime:data', handler)
  },
  onStatus: (cb: (state: RuntimeState) => void) => {
    const handler = (_event: IpcRendererEvent, state: RuntimeState) => cb(state)
    ipcRenderer.on('runtime:status', handler)
    return () => ipcRenderer.removeListener('runtime:status', handler)
  }
},
```

- [ ] **Step 6: Run focused backend checks**

Run:

```bash
npm run typecheck
node .\node_modules\vitest\vitest.mjs run electron/runtime/config.test.ts electron/runtime/analysisPrompt.test.ts electron/runtime/runner.test.ts
```

Expected: PASS.

- [ ] **Step 7: Commit runtime IPC**

```bash
git add electron/main.ts electron/preload.ts
git commit -m "feat: wire runtime runner IPC"
```

---

### Task 5: Runtime Settings UI

**Files:**
- Create: `src/components/ProjectRuntimeSettingsSection.tsx`
- Create: `src/components/ProjectRuntimeSettingsSection.test.tsx`
- Modify: `src/components/AiSettingsDialog.tsx`
- Modify: `src/components/AiSettingsDialog.test.tsx`
- Modify: `src/App.tsx`

- [ ] **Step 1: Write failing runtime settings section tests**

Create `src/components/ProjectRuntimeSettingsSection.test.tsx`:

```tsx
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { ProjectRuntimeConfig, VisualStudioInstallation } from '../../electron/preload'
import ProjectRuntimeSettingsSection, {
  formatRuntimeConfigSaveError
} from './ProjectRuntimeSettingsSection.js'

const config: ProjectRuntimeConfig = {
  enabled: true,
  cwd: '.',
  command: 'npm run dev',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}

describe('ProjectRuntimeSettingsSection', () => {
  it('renders no-project and loading states', () => {
    const noProject = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId={null}
        loading={false}
        value={config}
        disabled={false}
        visualStudioInstallations={[]}
        visualStudioInstallationsLoading={false}
        onRefreshVisualStudioInstallations={vi.fn()}
        onChange={vi.fn()}
      />
    )
    expect(noProject).toContain('选择项目后可编辑项目运行配置')

    const loading = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId="p1"
        loading={true}
        value={config}
        disabled={false}
        visualStudioInstallations={[]}
        visualStudioInstallationsLoading={false}
        onRefreshVisualStudioInstallations={vi.fn()}
        onChange={vi.fn()}
      />
    )
    expect(loading).toContain('正在读取项目运行配置')
  })

  it('renders runtime command fields and visual studio selector', () => {
    const installations: VisualStudioInstallation[] = [
      {
        instanceId: 'vs-1',
        displayName: 'Visual Studio 2022 Community',
        installationPath: 'C:\\VS',
        productLineVersion: '2022',
        isPrerelease: false
      }
    ]

    const markup = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId="p1"
        loading={false}
        value={{
          ...config,
          envType: 'visual-studio',
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'gbk'
        }}
        disabled={false}
        visualStudioInstallations={installations}
        visualStudioInstallationsLoading={false}
        onRefreshVisualStudioInstallations={vi.fn()}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('项目运行')
    expect(markup).toContain('npm run dev')
    expect(markup).toContain('Visual Studio 2022 Community')
    expect(markup).toContain('GBK')
  })

  it('formats runtime config save errors with field paths', () => {
    expect(
      formatRuntimeConfigSaveError('invalid runtime config', [
        { path: 'runtime_config.command', message: 'command must be a non-empty string' }
      ])
    ).toContain('runtime_config.command')
  })
})
```

- [ ] **Step 2: Run section test and verify failure**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectRuntimeSettingsSection.test.tsx
```

Expected: FAIL because the component does not exist.

- [ ] **Step 3: Implement runtime settings section**

Create `src/components/ProjectRuntimeSettingsSection.tsx`:

```tsx
import type {
  BuildConfigValidationIssue,
  ProjectRuntimeConfig,
  VisualStudioInstallation
} from '../../electron/preload'

export interface ProjectRuntimeSettingsSectionProps {
  projectId: string | null
  loading: boolean
  value: ProjectRuntimeConfig
  disabled: boolean
  visualStudioInstallations?: VisualStudioInstallation[]
  visualStudioInstallationsLoading?: boolean
  onRefreshVisualStudioInstallations?: () => void
  onChange: (next: ProjectRuntimeConfig) => void
}

function formatRuntimeConfigDetailPath(path: string): string {
  return path
}

export function formatRuntimeConfigSaveError(
  error: string,
  details?: BuildConfigValidationIssue[]
): string {
  if (!details?.length) return `项目运行配置保存失败：${error}`
  return [
    `项目运行配置保存失败：${error}`,
    ...details.map((detail) => `- ${formatRuntimeConfigDetailPath(detail.path)}：${detail.message}`)
  ].join('\n')
}

export default function ProjectRuntimeSettingsSection(
  props: ProjectRuntimeSettingsSectionProps
): JSX.Element {
  const installations = props.visualStudioInstallations ?? []
  const loadingInstallations = props.visualStudioInstallationsLoading ?? false

  if (!props.projectId) {
    return (
      <section className="ai-settings-card">
        <div className="ai-settings-title">项目运行</div>
        <div className="ai-settings-note">选择项目后可编辑项目运行配置</div>
      </section>
    )
  }

  if (props.loading) {
    return (
      <section className="ai-settings-card">
        <div className="ai-settings-title">项目运行</div>
        <div className="ai-settings-note">正在读取项目运行配置...</div>
      </section>
    )
  }

  const update = (patch: Partial<ProjectRuntimeConfig>) =>
    props.onChange({ ...props.value, ...patch })

  return (
    <section className="ai-settings-card">
      <div className="ai-settings-title">项目运行</div>
      <label className="ai-settings-checkbox">
        <input
          type="checkbox"
          checked={props.value.enabled}
          onChange={(event) => update({ enabled: event.target.checked })}
          disabled={props.disabled}
        />
        <span>启用项目运行</span>
      </label>
      <div className="project-build-settings-grid">
        <label>
          环境
          <select
            value={props.value.envType}
            onChange={(event) =>
              update({ envType: event.target.value as ProjectRuntimeConfig['envType'] })
            }
            disabled={props.disabled}
          >
            <option value="msys">MSYS2</option>
            <option value="visual-studio">Visual Studio Developer Command Prompt</option>
          </select>
        </label>
        <label>
          输出编码
          <select
            value={props.value.outputEncoding}
            onChange={(event) =>
              update({
                outputEncoding: event.target.value as ProjectRuntimeConfig['outputEncoding']
              })
            }
            disabled={props.disabled}
          >
            <option value="auto">自动</option>
            <option value="utf8">UTF-8</option>
            <option value="gbk">GBK</option>
          </select>
        </label>
        {props.value.envType === 'visual-studio' ? (
          <label>
            Visual Studio 实例
            <select
              value={props.value.visualStudioInstanceId}
              onChange={(event) => update({ visualStudioInstanceId: event.target.value })}
              disabled={props.disabled}
            >
              <option value="">
                {loadingInstallations ? '正在读取实例...' : '请选择实例'}
              </option>
              {installations.map((item) => (
                <option key={item.instanceId} value={item.instanceId}>
                  {item.displayName}
                </option>
              ))}
            </select>
          </label>
        ) : null}
        <label className="project-build-settings-grid-full">
          工作目录
          <input
            type="text"
            value={props.value.cwd}
            onChange={(event) => update({ cwd: event.target.value })}
            disabled={props.disabled}
            placeholder="."
          />
        </label>
        <label className="project-build-settings-grid-full">
          运行命令
          <textarea
            value={props.value.command}
            onChange={(event) => update({ command: event.target.value })}
            disabled={props.disabled}
            rows={3}
            placeholder="npm run dev"
          />
        </label>
      </div>
      {props.value.envType === 'visual-studio' && props.onRefreshVisualStudioInstallations ? (
        <div className="project-build-settings-toolbar">
          <button
            type="button"
            className="drawer-btn"
            onClick={props.onRefreshVisualStudioInstallations}
            disabled={props.disabled || loadingInstallations}
          >
            刷新 Visual Studio 实例
          </button>
        </div>
      ) : null}
    </section>
  )
}
```

- [ ] **Step 4: Run runtime settings section test**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectRuntimeSettingsSection.test.tsx
```

Expected: PASS.

- [ ] **Step 5: Wire runtime settings into AiSettingsDialog tests**

Modify `src/components/AiSettingsDialog.test.tsx`:

```ts
const defaultRuntimeConfig = {
  enabled: false,
  cwd: '.',
  command: '',
  envType: 'msys' as const,
  visualStudioInstanceId: '',
  outputEncoding: 'auto' as const
}
```

Add `initialRuntimeConfig={defaultRuntimeConfig}`, `runtimeConfigReady={true}`, and
`onSavedRuntimeConfig={vi.fn()}` to every `AiSettingsDialog` render call.

Add a focused save test:

```ts
it('syncs runtime config when saving project-scoped settings', async () => {
  const onRuntimeConfigSaved = vi.fn()
  const setRuntimeConfig = vi.fn().mockResolvedValue({ ok: true })

  await expect(
    saveProjectScopedSettings({
      projectId: 'project-1',
      nextMain: { ai_cli: 'claude' },
      nextRepoView: { ai_cli: 'codex' },
      nextBuildConfig: defaultBuildConfig,
      nextRuntimeConfig: defaultRuntimeConfig,
      setAiSettings: vi.fn().mockResolvedValue({ ok: true }),
      setRepoViewAiSettings: vi.fn().mockResolvedValue({ ok: true }),
      setBuildConfig: vi.fn().mockResolvedValue({ ok: true }),
      setRuntimeConfig,
      onMainSaved: vi.fn(),
      onRepoViewSaved: vi.fn(),
      onBuildConfigSaved: vi.fn(),
      onRuntimeConfigSaved
    })
  ).resolves.toBeNull()

  expect(setRuntimeConfig).toHaveBeenCalledWith('project-1', defaultRuntimeConfig)
  expect(onRuntimeConfigSaved).toHaveBeenCalledWith(defaultRuntimeConfig)
})
```

- [ ] **Step 6: Modify AiSettingsDialog to save runtime config**

In `src/components/AiSettingsDialog.tsx`:

- Import `ProjectRuntimeConfig`.
- Import `ProjectRuntimeSettingsSection` and `formatRuntimeConfigSaveError`.
- Add props:

```ts
initialRuntimeConfig: ProjectRuntimeConfig
runtimeConfigReady: boolean
onSavedRuntimeConfig: (next: ProjectRuntimeConfig) => void
```

- Extend `SaveProjectScopedSettingsParams`:

```ts
nextRuntimeConfig?: ProjectRuntimeConfig
setRuntimeConfig?: (
  projectId: string,
  next: ProjectRuntimeConfig
) => Promise<BuildConfigSaveResponse>
onRuntimeConfigSaved?: (next: ProjectRuntimeConfig) => void
```

- In `saveProjectScopedSettings`, after build save, add:

```ts
let runtimeRes: ProjectSettingsSaveResponse | undefined
if (params.nextRuntimeConfig && params.setRuntimeConfig && params.onRuntimeConfigSaved) {
  const nextRuntimeRes = await params.setRuntimeConfig(params.projectId, params.nextRuntimeConfig)
  if (!nextRuntimeRes.ok) {
    throw new Error(
      formatRuntimeConfigSaveError(
        nextRuntimeRes.error ?? 'save runtime config failed',
        nextRuntimeRes.details
      )
    )
  }
  params.onRuntimeConfigSaved(params.nextRuntimeConfig)
  runtimeRes = nextRuntimeRes
}

return getProjectSettingsRepairToastMessage(mainRes, repoRes, buildRes || runtimeRes)
```

- Add component state:

```ts
const [runtimeConfig, setRuntimeConfig] = useState<ProjectRuntimeConfig>(
  props.initialRuntimeConfig
)
```

- Sync incoming props:

```ts
useEffect(() => {
  if (saving) return
  setRuntimeConfig(props.initialRuntimeConfig)
}, [props.initialRuntimeConfig, saving])
```

- Include in save:

```ts
const nextRuntimeConfig = props.runtimeConfigReady ? runtimeConfig : undefined
```

- Pass to `saveProjectScopedSettings`:

```ts
nextRuntimeConfig,
setRuntimeConfig: props.runtimeConfigReady ? window.api.project.setRuntimeConfig : undefined,
onRuntimeConfigSaved: props.runtimeConfigReady ? props.onSavedRuntimeConfig : undefined
```

- Render the section after build settings:

```tsx
<ProjectRuntimeSettingsSection
  projectId={props.projectId}
  loading={props.projectId !== null && !props.runtimeConfigReady}
  value={runtimeConfig}
  disabled={saving}
  visualStudioInstallations={props.visualStudioInstallations}
  visualStudioInstallationsLoading={props.visualStudioInstallationsLoading}
  onRefreshVisualStudioInstallations={props.onRefreshVisualStudioInstallations}
  onChange={setRuntimeConfig}
/>
```

- [ ] **Step 7: Add App state for runtime config loading**

In `src/App.tsx`, import `ProjectRuntimeConfig` and define:

```ts
const DEFAULT_PROJECT_RUNTIME_CONFIG: ProjectRuntimeConfig = {
  enabled: false,
  cwd: '.',
  command: '',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}
```

Add state:

```ts
const [projectRuntimeConfig, setProjectRuntimeConfig] = useState<ProjectRuntimeConfig>(
  DEFAULT_PROJECT_RUNTIME_CONFIG
)
const [projectRuntimeConfigProjectId, setProjectRuntimeConfigProjectId] = useState<string | null>(
  null
)
```

Add derived values:

```ts
const visibleProjectRuntimeConfig =
  currentProjectId !== null && projectRuntimeConfigProjectId === currentProjectId
    ? projectRuntimeConfig
    : DEFAULT_PROJECT_RUNTIME_CONFIG
const projectRuntimeConfigReady =
  currentProjectId !== null && projectRuntimeConfigProjectId === currentProjectId
```

In the project-load effect:

```ts
setProjectRuntimeConfig(DEFAULT_PROJECT_RUNTIME_CONFIG)
setProjectRuntimeConfigProjectId(null)
```

Load after build config:

```ts
const runtimeResult = await window.api.project.getRuntimeConfig(currentProjectId)
if (cancelled) return
if (!runtimeResult.ok) {
  setProjectRuntimeConfig(DEFAULT_PROJECT_RUNTIME_CONFIG)
  setProjectRuntimeConfigProjectId(currentProjectId)
  showToast(runtimeResult.error ?? '读取项目运行配置失败', { level: 'error' })
} else {
  setProjectRuntimeConfig(runtimeResult.value ?? DEFAULT_PROJECT_RUNTIME_CONFIG)
  setProjectRuntimeConfigProjectId(currentProjectId)
}
```

Pass props to `AiSettingsDialog`:

```tsx
initialRuntimeConfig={visibleProjectRuntimeConfig}
runtimeConfigReady={projectRuntimeConfigReady}
onSavedRuntimeConfig={(next) => {
  setProjectRuntimeConfig(next)
  setProjectRuntimeConfigProjectId(currentProjectId)
}}
```

- [ ] **Step 8: Run settings tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectRuntimeSettingsSection.test.tsx src/components/AiSettingsDialog.test.tsx
npm run typecheck
```

Expected: PASS.

- [ ] **Step 9: Commit runtime settings UI**

```bash
git add src/components/ProjectRuntimeSettingsSection.tsx src/components/ProjectRuntimeSettingsSection.test.tsx src/components/AiSettingsDialog.tsx src/components/AiSettingsDialog.test.tsx src/App.tsx
git commit -m "feat: add project runtime settings"
```

---

### Task 6: Runtime Panel And Log Send UI

**Files:**
- Modify: `src/components/ProjectBuildPanel.tsx`
- Modify: `src/components/ProjectBuildPanel.test.tsx`
- Modify: `src/App.tsx`

- [ ] **Step 1: Add panel tests for runtime controls**

Modify `src/components/ProjectBuildPanel.test.tsx` imports:

```ts
import type {
  BuildRuntimeState,
  ProjectBuildConfig,
  ProjectRuntimeConfig,
  RuntimeState
} from '../../electron/preload'
```

Add constants:

```ts
const enabledRuntimeConfig: ProjectRuntimeConfig = {
  enabled: true,
  cwd: '.',
  command: 'npm run dev',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto'
}

const baseRuntimeState: RuntimeState = {
  status: 'idle',
  projectId: null,
  projectName: null,
  targetRepo: null,
  cwd: null,
  command: null,
  envType: null,
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: null,
  startedAt: null,
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: ''
}
```

Add default runtime props to every `ProjectBuildPanel` render:

```tsx
runtimeConfig={enabledRuntimeConfig}
runtimeConfigReady={true}
runtimeState={baseRuntimeState}
onStartRuntime={vi.fn()}
onStopRuntime={vi.fn()}
onSendRuntimeLog={vi.fn()}
```

Add tests:

```ts
it('renders runtime controls separately from build logs', () => {
  const markup = renderToStaticMarkup(
    <ProjectBuildPanel
      open={true}
      currentProjectId="project-1"
      currentProjectName="Demo"
      buildConfig={enabledBuildConfig}
      buildConfigReady={true}
      runtimeConfig={enabledRuntimeConfig}
      runtimeConfigReady={true}
      state={baseState}
      runtimeState={{
        ...baseRuntimeState,
        status: 'running',
        projectId: 'project-1',
        cwd: 'E:/demo',
        command: 'npm run dev',
        log: 'server started'
      }}
      sessionId="session-1"
      sessionStatus="running"
      onClose={vi.fn()}
      onStartBuild={vi.fn()}
      onStartSingleBuild={vi.fn()}
      onStopBuild={vi.fn()}
      onAnalyzeFailure={vi.fn()}
      onStartRuntime={vi.fn()}
      onStopRuntime={vi.fn()}
      onSendRuntimeLog={vi.fn()}
    />
  )

  expect(markup).toContain('运行')
  expect(markup).toContain('停止运行')
  expect(markup).toContain('发送运行日志')
  expect(markup).toContain('server started')
})

it('warns when runtime log cannot be sent without a running main session', () => {
  const markup = renderToStaticMarkup(
    <ProjectBuildPanel
      open={true}
      currentProjectId="project-1"
      currentProjectName="Demo"
      buildConfig={enabledBuildConfig}
      buildConfigReady={true}
      runtimeConfig={enabledRuntimeConfig}
      runtimeConfigReady={true}
      state={baseState}
      runtimeState={{
        ...baseRuntimeState,
        status: 'failed',
        projectId: 'project-1',
        log: 'fatal runtime error'
      }}
      sessionId={null}
      sessionStatus="idle"
      onClose={vi.fn()}
      onStartBuild={vi.fn()}
      onStartSingleBuild={vi.fn()}
      onStopBuild={vi.fn()}
      onAnalyzeFailure={vi.fn()}
      onStartRuntime={vi.fn()}
      onStopRuntime={vi.fn()}
      onSendRuntimeLog={vi.fn()}
    />
  )

  expect(markup).toContain('主会话未运行')
})
```

- [ ] **Step 2: Run panel tests and verify failure**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildPanel.test.tsx
```

Expected: FAIL because `ProjectBuildPanel` does not accept runtime props.

- [ ] **Step 3: Extend ProjectBuildPanel props and helpers**

In `src/components/ProjectBuildPanel.tsx`, import runtime types:

```ts
import type {
  BuildRuntimeState,
  BuildStepRuntime,
  ProjectBuildConfig,
  ProjectRuntimeConfig,
  RuntimeState
} from '../../electron/preload'
```

Extend props:

```ts
runtimeConfig: ProjectRuntimeConfig
runtimeConfigReady: boolean
runtimeState: RuntimeState
onStartRuntime: () => void
onStopRuntime: () => void
onSendRuntimeLog: () => void
```

Add helpers:

```ts
export function getRuntimeStatusLabel(status: RuntimeState['status']): string {
  switch (status) {
    case 'running':
      return '运行中'
    case 'exited':
      return '已退出'
    case 'failed':
      return '运行失败'
    case 'stopped':
      return '已停止'
    case 'idle':
    default:
      return '空闲'
  }
}

export function getRuntimeStartBlockedReason(
  projectId: string | null,
  runtimeConfigReady: boolean,
  runtimeConfig: ProjectRuntimeConfig,
  runtimeState: RuntimeState
): string | null {
  if (!projectId) return '请先选择项目'
  if (!runtimeConfigReady) return '正在读取项目运行配置，请稍后再试'
  if (!runtimeConfig.enabled) return '当前项目未启用运行配置，请先到设置中开启'
  if (!runtimeConfig.command.trim()) return '运行命令不能为空'
  if (runtimeState.status === 'running') return '当前已有运行进程'
  return null
}

export function canSendRuntimeLog(
  projectId: string | null,
  runtimeState: RuntimeState,
  sessionId: string | null,
  sessionStatus: 'idle' | 'running' | 'exited'
): boolean {
  return (
    !!projectId &&
    runtimeState.projectId === projectId &&
    runtimeState.log.trim().length > 0 &&
    !!sessionId &&
    sessionStatus === 'running'
  )
}
```

- [ ] **Step 4: Add runtime section rendering**

Inside `ProjectBuildPanel`, calculate:

```ts
const runtimeStartBlockedReason = getRuntimeStartBlockedReason(
  props.currentProjectId,
  props.runtimeConfigReady,
  props.runtimeConfig,
  props.runtimeState
)
const runtimeStartEnabled = runtimeStartBlockedReason === null
const runtimeStopEnabled = props.runtimeState.status === 'running'
const sendRuntimeEnabled = canSendRuntimeLog(
  props.currentProjectId,
  props.runtimeState,
  props.sessionId,
  props.sessionStatus
)
const runtimeLogText =
  props.runtimeState.log.trim().length > 0 ? props.runtimeState.log : '暂无运行日志输出'
```

Add a section before the build log section:

```tsx
<section className="build-panel-section">
  <div className="build-panel-section-head">
    <h3>运行</h3>
    <span>{getRuntimeStatusLabel(props.runtimeState.status)}</span>
  </div>
  <div className="build-panel-actions">
    <button
      className="tile-btn"
      onClick={props.onStartRuntime}
      disabled={!runtimeStartEnabled}
      title={runtimeStartEnabled ? '启动项目运行命令' : runtimeStartBlockedReason ?? undefined}
    >
      运行
    </button>
    <button
      className="tile-btn"
      onClick={props.onStopRuntime}
      disabled={!runtimeStopEnabled}
      title={runtimeStopEnabled ? '停止当前运行进程' : '当前没有正在运行的进程'}
    >
      停止运行
    </button>
    <button
      className="tile-btn"
      onClick={props.onSendRuntimeLog}
      disabled={!sendRuntimeEnabled}
      title={sendRuntimeEnabled ? '将最近运行日志发送给主会话 AI CLI' : '需要运行日志和正在运行的主会话'}
    >
      发送运行日志
    </button>
  </div>
  {runtimeStartBlockedReason ? (
    <p className="build-panel-note">{runtimeStartBlockedReason}</p>
  ) : null}
  {props.runtimeState.log.trim() && (!props.sessionId || props.sessionStatus !== 'running') ? (
    <p className="build-panel-note">主会话未运行，无法发送运行日志。</p>
  ) : null}
  <div className="build-step-meta">
    <span>cwd: {props.runtimeState.cwd ?? props.runtimeConfig.cwd}</span>
    <span>命令: {props.runtimeState.command ?? props.runtimeConfig.command}</span>
  </div>
  <pre className="build-panel-log">{runtimeLogText}</pre>
</section>
```

- [ ] **Step 5: Add App runtime state and handlers**

In `src/App.tsx`, import `RuntimeState` and define:

```ts
const DEFAULT_RUNTIME_STATE: RuntimeState = {
  status: 'idle',
  projectId: null,
  projectName: null,
  targetRepo: null,
  cwd: null,
  command: null,
  envType: null,
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: null,
  startedAt: null,
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: ''
}

function appendRuntimeLog(current: string, chunk: string): string {
  const next = current + chunk
  if (next.length <= BUILD_LOG_LIMIT) return next
  return `...[runtime log truncated]...\n${next.slice(-BUILD_LOG_LIMIT)}`
}
```

Add state and current-project filter:

```ts
const [runtimeState, setRuntimeState] = useState<RuntimeState>(DEFAULT_RUNTIME_STATE)
const runtimeStateForCurrentProject =
  currentProjectId !== null && runtimeState.projectId === currentProjectId
    ? runtimeState
    : DEFAULT_RUNTIME_STATE
```

Add subscription effect:

```ts
useEffect(() => {
  let cancelled = false
  void window.api.runtime.getState().then((state) => {
    if (cancelled) return
    setRuntimeState(state)
  })
  const offStatus = window.api.runtime.onStatus((state) => {
    if (cancelled) return
    setRuntimeState(state)
  })
  const offData = window.api.runtime.onData((event) => {
    if (cancelled || !event.chunk) return
    setRuntimeState((prev) => ({
      ...prev,
      projectId: event.projectId ?? prev.projectId,
      log: appendRuntimeLog(prev.log, event.chunk)
    }))
  })
  return () => {
    cancelled = true
    offStatus()
    offData()
  }
}, [])
```

Add handlers:

```ts
const handleStartRuntime = useCallback(async () => {
  if (
    runtimeState.projectId !== null &&
    runtimeState.projectId !== currentProjectId &&
    runtimeState.status === 'running'
  ) {
    showToast('另一个项目的运行进程仍在运行，请先停止后再启动当前项目运行', { level: 'warn' })
    setShowBuildPanel(true)
    return
  }
  if (!currentProjectId) return
  const result = await window.api.runtime.start(currentProjectId)
  setRuntimeState(result.state)
  setShowBuildPanel(true)
  if (!result.ok) {
    showToast(result.error ?? '启动运行失败', { level: 'error' })
  }
}, [currentProjectId, runtimeState.projectId, runtimeState.status])

const handleStopRuntime = useCallback(async () => {
  const result = await window.api.runtime.stop()
  if (!result.ok) {
    showToast(result.error ?? '停止运行失败', { level: 'error' })
  }
}, [])

const handleSendRuntimeLog = useCallback(async () => {
  if (!currentProjectId || runtimeState.projectId !== currentProjectId) {
    showToast('当前运行日志不属于所选项目，请切回对应项目后再发送', { level: 'warn' })
    return
  }
  if (!sessionId || sessionStatus !== 'running') {
    showToast('主会话未运行，无法发送运行日志，请先启动主会话', { level: 'warn' })
    return
  }
  const promptResult = await window.api.runtime.getAnalysisPrompt()
  if (!promptResult.ok) {
    showToast(promptResult.error ?? '获取运行日志分析提示失败', { level: 'error' })
    return
  }
  const sendResult = await window.api.cc.sendUser(sessionId, promptResult.prompt)
  if (!sendResult.ok) {
    showToast(sendResult.error ?? '发送运行日志失败', { level: 'error' })
    return
  }
  showToast('已将运行日志发送到主会话', { level: 'success' })
}, [currentProjectId, runtimeState.projectId, sessionId, sessionStatus])
```

Pass runtime props to `ProjectBuildPanel`:

```tsx
runtimeConfig={visibleProjectRuntimeConfig}
runtimeConfigReady={projectRuntimeConfigReady}
runtimeState={runtimeStateForCurrentProject}
onStartRuntime={() => void handleStartRuntime()}
onStopRuntime={() => void handleStopRuntime()}
onSendRuntimeLog={() => void handleSendRuntimeLog()}
```

- [ ] **Step 6: Run panel tests and typecheck**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run src/components/ProjectBuildPanel.test.tsx
npm run typecheck
```

Expected: PASS.

- [ ] **Step 7: Commit runtime panel UI**

```bash
git add src/components/ProjectBuildPanel.tsx src/components/ProjectBuildPanel.test.tsx src/App.tsx
git commit -m "feat: add runtime panel controls"
```

---

### Task 7: Full Verification

**Files:**
- Verify all changed files.

- [ ] **Step 1: Run focused test suite**

Run:

```bash
node .\node_modules\vitest\vitest.mjs run electron/runtime/config.test.ts electron/runtime/analysisPrompt.test.ts electron/runtime/runner.test.ts src/components/ProjectRuntimeSettingsSection.test.tsx src/components/AiSettingsDialog.test.tsx src/components/ProjectBuildPanel.test.tsx
```

Expected: PASS.

- [ ] **Step 2: Run full test suite**

Run:

```bash
npm test
```

Expected: PASS.

- [ ] **Step 3: Run typecheck**

Run:

```bash
npm run typecheck
```

Expected: PASS.

- [ ] **Step 4: Manual desktop verification**

Run:

```bash
npm run dev
```

Expected: Electron app opens.

Manual checks:

- Open settings.
- Confirm "Project Runtime" appears under project settings.
- Enable runtime with `cwd = .` and `command = node -e "console.log('runtime ok')"` for a safe test project.
- Save settings.
- Open the build/run panel.
- Click Run.
- Confirm runtime log shows `runtime ok`.
- Start the main AI CLI session.
- Click Send Runtime Log.
- Confirm the active AI CLI receives the runtime analysis prompt.

- [ ] **Step 5: Inspect git diff**

Run:

```bash
git status --short
git diff --stat
```

Expected: no unexpected files beyond runtime feature changes.

- [ ] **Step 6: Final commit if needed**

If any verification fixes were made:

```bash
git add <changed-files>
git commit -m "fix: polish runtime runner verification issues"
```

If no changes were needed, do not create an empty commit.
