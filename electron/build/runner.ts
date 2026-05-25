import { execFile, spawn } from 'child_process'
import iconv from 'iconv-lite'
import { isAbsolute, relative, resolve, sep } from 'path'
import { StringDecoder } from 'string_decoder'
import { detectMsys, type MsysInfo } from '../util/msys.js'
import { resolveVisualStudioEnvironment } from './visualStudio.js'
import type {
  BuildDataEvent,
  BuildFailureContext,
  BuildRuntimeState,
  BuildStartResult,
  BuildStepRuntime,
  BuildStopResult,
  ProjectBuildConfig,
  StartBuildRequest,
} from './types.js'

export type { BuildRuntimeState, BuildDataEvent, BuildFailureContext, StartBuildRequest, BuildStartResult } from './types.js'

export interface SpawnedBuildProcess {
  pid?: number
  stdout: NodeJS.ReadableStream | null
  stderr: NodeJS.ReadableStream | null
  kill(signal?: NodeJS.Signals | number): boolean
  on(event: 'close', listener: (code: number | null, signal: NodeJS.Signals | null) => void): this
  on(event: 'error', listener: (error: Error) => void): this
}

type SpawnLike = (
  command: string,
  args?: readonly string[],
  options?: {
    cwd?: string
    env?: NodeJS.ProcessEnv
    shell?: boolean
    windowsHide?: boolean
    windowsVerbatimArguments?: boolean
    stdio?: 'pipe'
  }
) => SpawnedBuildProcess

interface BuildRunnerDeps {
  platform: NodeJS.Platform
  spawn: SpawnLike
  detectMsys: () => Promise<MsysInfo>
  resolveVisualStudioEnvironment: typeof resolveVisualStudioEnvironment
  killProcessTree: (pid: number) => void
  logLimit: number
  now: () => string
}

interface ChunkDecoder {
  write(chunk: Buffer): string
  end(): string
}

export interface BuildRunner {
  start(request: StartBuildRequest): Promise<BuildStartResult>
  stop(): BuildStopResult
  getState(): BuildRuntimeState
  onData(listener: (event: BuildDataEvent) => void): () => void
  onStatus(listener: (state: BuildRuntimeState) => void): () => void
}

const LOG_TAIL_LIMIT = 8000
const ACTIVE_LOG_LIMIT = 200_000
const TRUNCATED_LOG_MARKER = '[build] earlier log truncated...\n'

function initialState(): BuildRuntimeState {
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
}

function cloneState(state: BuildRuntimeState): BuildRuntimeState {
  return {
    ...state,
    steps: state.steps.map((step) => ({ ...step })),
    lastFailure: state.lastFailure ? { ...state.lastFailure } : null,
  }
}

function appendTail(current: string, chunk: string): string {
  const next = `${current}${chunk}`
  return next.length > LOG_TAIL_LIMIT ? next.slice(-LOG_TAIL_LIMIT) : next
}

function appendWindowedLog(
  current: string,
  chunk: string,
  limit: number,
  truncatedBefore: boolean
): { nextLog: string; truncated: boolean } {
  const base = truncatedBefore && current.startsWith(TRUNCATED_LOG_MARKER)
    ? current.slice(TRUNCATED_LOG_MARKER.length)
    : current
  const next = `${base}${chunk}`
  if (!truncatedBefore && next.length <= limit) {
    return { nextLog: next, truncated: false }
  }

  const tailLimit = Math.max(0, limit - TRUNCATED_LOG_MARKER.length)
  return {
    nextLog: `${TRUNCATED_LOG_MARKER}${next.slice(-tailLimit)}`,
    truncated: true,
  }
}

function resolveStepCwd(targetRepo: string, cwd: string): string {
  if (isAbsolute(cwd)) {
    throw new Error('cwd must be a relative path within target_repo')
  }

  const resolvedTargetRepo = resolve(targetRepo)
  const resolvedCwd = resolve(resolvedTargetRepo, cwd)
  const rel = relative(resolvedTargetRepo, resolvedCwd)
  if (rel === '..' || rel.startsWith(`..${sep}`) || isAbsolute(rel)) {
    throw new Error('cwd must stay within target_repo')
  }

  return resolvedCwd
}

function prependPath(env: NodeJS.ProcessEnv, segment: string): NodeJS.ProcessEnv {
  const next = { ...env }
  const pathKey = Object.keys(next).find((key) => key.toLowerCase() === 'path') ?? 'PATH'
  const existing = next[pathKey]
  const sep = process.platform === 'win32' ? ';' : ':'
  next[pathKey] = existing ? `${segment}${sep}${existing}` : segment
  return next
}

function buildMsysEnv(baseEnv: NodeJS.ProcessEnv, usrBinDir: string | null): NodeJS.ProcessEnv {
  return usrBinDir ? prependPath(baseEnv, usrBinDir) : { ...baseEnv }
}

function toMsysPath(absPath: string): string {
  const normalized = absPath.replace(/\\/g, '/')
  const driveMatch = /^([A-Za-z]):(\/.*)?$/.exec(normalized)
  if (!driveMatch) return normalized
  const [, drive, rest = ''] = driveMatch
  return `/${drive.toLowerCase()}${rest}`
}

function quoteForBash(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`
}

function markPendingStepsSkipped(steps: BuildStepRuntime[]): void {
  for (const step of steps) {
    if (step.status === 'pending' || step.status === 'running') {
      step.status = 'skipped'
      step.finishedAt = step.finishedAt ?? step.startedAt ?? new Date().toISOString()
    }
  }
}

function resolveOutputEncoding(step: BuildStepRuntime): 'utf8' | 'gbk' {
  if (step.outputEncoding === 'utf8' || step.outputEncoding === 'gbk') {
    return step.outputEncoding
  }

  return step.envType === 'visual-studio' ? 'gbk' : 'utf8'
}

function createChunkDecoder(step: BuildStepRuntime): ChunkDecoder {
  if (resolveOutputEncoding(step) === 'gbk') {
    const decoder = iconv.getDecoder('gbk')
    return {
      write(chunk: Buffer): string {
        return decoder.write(chunk)
      },
      end(): string {
        return decoder.end() ?? ''
      },
    }
  }

  const decoder = new StringDecoder('utf8')
  return {
    write(chunk: Buffer): string {
      return decoder.write(chunk)
    },
    end(): string {
      return decoder.end()
    },
  }
}

export function createBuildRunner(partialDeps?: Partial<BuildRunnerDeps>): BuildRunner {
  const deps: BuildRunnerDeps = {
    platform: partialDeps?.platform ?? process.platform,
    spawn: partialDeps?.spawn ?? spawn,
    detectMsys: partialDeps?.detectMsys ?? detectMsys,
    resolveVisualStudioEnvironment:
      partialDeps?.resolveVisualStudioEnvironment ?? resolveVisualStudioEnvironment,
    killProcessTree:
      partialDeps?.killProcessTree ??
      ((pid: number) => {
        execFile('taskkill', ['/PID', String(pid), '/T', '/F'], { windowsHide: true }, () => {
          // Best-effort tree termination for Windows builds.
        })
      }),
    logLimit: partialDeps?.logLimit ?? ACTIVE_LOG_LIMIT,
    now: partialDeps?.now ?? (() => new Date().toISOString()),
  }

  let state = initialState()
  let activeChild: SpawnedBuildProcess | null = null
  let stopRequested = false
  let stopCurrentStepRequested = false
  let logTruncated = false
  let currentStepControl: { hasSettled: boolean } | null = null
  const dataListeners = new Set<(event: BuildDataEvent) => void>()
  const statusListeners = new Set<(nextState: BuildRuntimeState) => void>()

  function emitStatus(): void {
    const snapshot = cloneState(state)
    for (const listener of statusListeners) listener(snapshot)
  }

  function emitData(event: BuildDataEvent): void {
    for (const listener of dataListeners) listener(event)
  }

  function subscribeStatus(listener: (nextState: BuildRuntimeState) => void): () => void {
    statusListeners.add(listener)
    return () => statusListeners.delete(listener)
  }

  function subscribeData(listener: (event: BuildDataEvent) => void): () => void {
    dataListeners.add(listener)
    return () => dataListeners.delete(listener)
  }

  function appendLog(stepId: string | null, stream: BuildDataEvent['stream'], chunk: string): void {
    const next = appendWindowedLog(state.log, chunk, deps.logLimit, logTruncated)
    state.log = next.nextLog
    logTruncated = next.truncated
    emitData({
      at: deps.now(),
      projectId: state.projectId,
      stepId,
      stream,
      chunk,
    })
  }

  function markFailure(step: BuildStepRuntime, reason: string, logTail: string): void {
    step.status = 'failed'
    step.finishedAt = deps.now()
    state.status = 'failed'
    state.finishedAt = step.finishedAt
    state.activeStepId = null
    state.lastFailure = {
      projectId: state.projectId ?? '',
      projectName: state.projectName,
      targetRepo: state.targetRepo ?? '',
      stepId: step.id,
      stepName: step.name,
      envType: step.envType,
      visualStudioInstanceId: step.envType === 'visual-studio' ? step.visualStudioInstanceId : null,
      visualStudioDisplayName:
        step.envType === 'visual-studio' ? step.visualStudioDisplayName : null,
      outputEncoding: step.outputEncoding,
      cwd: step.resolvedCwd ?? step.cwd,
      command: step.command,
      exitCode: step.exitCode,
      signal: step.signal,
      reason,
      logTail,
    }
    emitStatus()
  }

  async function spawnForStep(step: BuildStepRuntime): Promise<{
    child: SpawnedBuildProcess
    resolvedCwd: string
  }> {
    const resolvedCwd = resolveStepCwd(state.targetRepo ?? '', step.cwd)
    step.resolvedCwd = resolvedCwd

    if (step.envType === 'msys') {
      step.visualStudioDisplayName = null
      const info = await deps.detectMsys()
      if (!info.available || !info.bashPath) {
        throw new Error('msys environment is unavailable')
      }

      const command = `cd ${quoteForBash(toMsysPath(resolvedCwd))} && ${step.command}`
      const child = deps.spawn(info.bashPath, ['-lc', command], {
        cwd: resolvedCwd,
        env: buildMsysEnv(process.env, info.usrBinDir),
        shell: false,
        windowsHide: true,
        stdio: 'pipe',
      }) as SpawnedBuildProcess
      return { child, resolvedCwd }
    }

    const result = await deps.resolveVisualStudioEnvironment({
      instanceId: step.visualStudioInstanceId,
      platform: deps.platform,
      baseEnv: process.env,
    })
    if (!result.ok) {
      throw new Error(result.error)
    }
    step.visualStudioDisplayName = result.displayName

    const child = deps.spawn('cmd.exe', ['/d', '/s', '/c', step.command], {
      cwd: resolvedCwd,
      env: result.env,
      shell: false,
      windowsHide: true,
      windowsVerbatimArguments: true,
      stdio: 'pipe',
    }) as SpawnedBuildProcess
    return { child, resolvedCwd }
  }

  async function runBuild(request: StartBuildRequest): Promise<void> {
    const scope = request.scope ?? 'all'
    if (!request.config.enabled) {
      markPendingStepsSkipped(state.steps)
      state.status = 'stopped'
      state.finishedAt = deps.now()
      emitStatus()
      return
    }

    const runnableSteps =
      scope === 'single-step'
        ? state.steps.filter((step) => step.id === request.stepId && step.enabled)
        : state.steps.filter((step) => step.enabled)
    if (runnableSteps.length === 0) {
      state.status = 'succeeded'
      state.finishedAt = deps.now()
      emitStatus()
      return
    }

    for (const step of runnableSteps) {
      if (stopRequested) break

      step.status = 'running'
      step.startedAt = deps.now()
      state.activeStepId = step.id
      emitStatus()
      appendLog(step.id, 'system', `[build] starting step ${step.name}\n`)

      let logTail = ''

      try {
        const { child } = await spawnForStep(step)
        const stdoutDecoder = createChunkDecoder(step)
        const stderrDecoder = createChunkDecoder(step)
        activeChild = child
        currentStepControl = { hasSettled: false }
        stopCurrentStepRequested = false
        if (stopRequested && !currentStepControl.hasSettled) {
          stopCurrentStepRequested = true
          activeChild.kill('SIGTERM')
        }

        child.stdout?.on('data', (chunk: Buffer | string) => {
          const text = typeof chunk === 'string' ? chunk : stdoutDecoder.write(chunk)
          logTail = appendTail(logTail, text)
          appendLog(step.id, 'stdout', text)
        })
        child.stderr?.on('data', (chunk: Buffer | string) => {
          const text = typeof chunk === 'string' ? chunk : stderrDecoder.write(chunk)
          logTail = appendTail(logTail, text)
          appendLog(step.id, 'stderr', text)
        })

        const result = await new Promise<{ code: number | null; signal: NodeJS.Signals | null }>(
          (resolvePromise, rejectPromise) => {
            child.on('error', rejectPromise)
            child.on('close', (code, signal) => {
              const stdoutRemainder = stdoutDecoder.end()
              if (stdoutRemainder) {
                logTail = appendTail(logTail, stdoutRemainder)
                appendLog(step.id, 'stdout', stdoutRemainder)
              }
              const stderrRemainder = stderrDecoder.end()
              if (stderrRemainder) {
                logTail = appendTail(logTail, stderrRemainder)
                appendLog(step.id, 'stderr', stderrRemainder)
              }
              if (currentStepControl) currentStepControl.hasSettled = true
              resolvePromise({ code, signal })
            })
          }
        )

        activeChild = null
        const shouldStopCurrentStep = stopCurrentStepRequested
        currentStepControl = null
        stopCurrentStepRequested = false
        step.exitCode = result.code
        step.signal = result.signal

        if (shouldStopCurrentStep) {
          step.status = 'skipped'
          step.finishedAt = deps.now()
          markPendingStepsSkipped(state.steps)
          state.status = 'stopped'
          state.finishedAt = step.finishedAt
          state.activeStepId = null
          emitStatus()
          return
        }

        if (result.code === 0) {
          step.status = 'succeeded'
          step.finishedAt = deps.now()
          state.activeStepId = null
          emitStatus()
          continue
        }

        const reason =
          result.code !== null
            ? `process exited with code ${result.code}`
            : `process terminated by signal ${result.signal ?? 'unknown'}`
        markFailure(step, reason, logTail)
        return
      } catch (error: unknown) {
        activeChild = null
        currentStepControl = null
        stopCurrentStepRequested = false
        step.exitCode = null
        step.signal = null
        markFailure(
          step,
          error instanceof Error ? error.message : String(error),
          logTail
        )
        return
      }
    }

    if (stopRequested && state.status === 'running') {
      markPendingStepsSkipped(state.steps)
      state.status = 'stopped'
      state.finishedAt = deps.now()
      state.activeStepId = null
      emitStatus()
      return
    }

    if (state.status === 'running') {
      state.status = 'succeeded'
      state.finishedAt = deps.now()
      state.activeStepId = null
      emitStatus()
    }
  }

  return {
    async start(request: StartBuildRequest): Promise<BuildStartResult> {
      if (state.status === 'running') {
        return { ok: false, error: 'build already running', state: cloneState(state) }
      }

      let resolveStarted: (() => void) | null = null
      const started = new Promise<void>((resolve) => {
        resolveStarted = resolve
      })
      const markStarted = () => {
        if (resolveStarted) resolveStarted()
      }

      stopRequested = false
      stopCurrentStepRequested = false
      logTruncated = false
      currentStepControl = null
      const scope = request.scope ?? 'all'
      state = {
        status: 'running',
        scope,
        requestedStepId: request.stepId ?? null,
        projectId: request.projectId,
        projectName: request.projectName,
        targetRepo: request.targetRepo,
        startedAt: deps.now(),
        finishedAt: null,
        activeStepId: null,
        steps: request.config.steps.map((step) => ({
          ...step,
          visualStudioDisplayName: null,
          status:
            scope === 'all'
              ? step.enabled ? 'pending' : 'skipped'
              : step.enabled && step.id === request.stepId ? 'pending' : 'not-run',
          resolvedCwd: null,
          startedAt: null,
          finishedAt: null,
          exitCode: null,
          signal: null,
        })),
        log: '',
        lastFailure: null,
      }
      emitStatus()
      void runBuild(request).finally(() => {
        markStarted()
      })
      if (state.status !== 'running' || state.activeStepId !== null || !state.steps.some((step) => step.enabled)) {
        markStarted()
      } else {
        const off = subscribeStatus(listener)
        function listener(nextState: BuildRuntimeState): void {
          if (nextState.status !== 'running' || nextState.activeStepId !== null) {
            off()
            markStarted()
          }
        }
      }
      await started
      return { ok: true, state: cloneState(state) }
    },

    stop(): BuildStopResult {
      if (state.status !== 'running') {
        return { ok: false, error: 'no build running' }
      }
      stopRequested = true
      appendLog(state.activeStepId, 'system', '[build] stop requested\n')
      if (activeChild && currentStepControl && !currentStepControl.hasSettled) {
        stopCurrentStepRequested = true
        if (deps.platform === 'win32' && typeof activeChild.pid === 'number') {
          deps.killProcessTree(activeChild.pid)
        } else {
          activeChild.kill('SIGTERM')
        }
      }
      return { ok: true }
    },

    getState(): BuildRuntimeState {
      return cloneState(state)
    },

    onData(listener: (event: BuildDataEvent) => void): () => void {
      return subscribeData(listener)
    },

    onStatus(listener: (nextState: BuildRuntimeState) => void): () => void {
      return subscribeStatus(listener)
    },
  }
}
