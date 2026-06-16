import { execFile, spawn } from 'child_process'
import iconv from 'iconv-lite'
import { EventEmitter } from 'events'
import { createRequire } from 'module'
import { isAbsolute, relative, resolve, sep } from 'path'
import { PassThrough } from 'stream'
import { StringDecoder } from 'string_decoder'
import { detectMsys, type MsysInfo } from '../util/msys.js'
import { resolveVisualStudioEnvironment } from '../build/visualStudio.js'
import type {
  RuntimeDataEvent,
  RuntimeOutputEncoding,
  RuntimeStartResult,
  RuntimeState,
  RuntimeStopResult,
  StartRuntimeRequest,
} from './types.js'

export type {
  RuntimeDataEvent,
  RuntimeStartResult,
  RuntimeState,
  RuntimeStopResult,
  StartRuntimeRequest,
} from './types.js'

export interface SpawnedRuntimeProcess {
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
) => SpawnedRuntimeProcess

interface RuntimePty {
  pid: number
  write(data: string): void
  kill(signal?: string): void
  onData(listener: (data: string) => void): void
  onExit(listener: (event: { exitCode: number; signal?: number | string }) => void): void
}

type NodePtySpawn = (
  command: string,
  args: string[],
  options: Record<string, unknown>
) => RuntimePty

interface RuntimeRunnerDeps {
  platform: NodeJS.Platform
  spawn: SpawnLike
  spawnPty: SpawnLike | null
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

export interface RuntimeRunner {
  start(request: StartRuntimeRequest): Promise<RuntimeStartResult>
  stop(): RuntimeStopResult
  getState(): RuntimeState
  onData(listener: (event: RuntimeDataEvent) => void): () => void
  onStatus(listener: (state: RuntimeState) => void): () => void
}

const ACTIVE_LOG_LIMIT = 200_000
const TRUNCATED_LOG_MARKER = '[runtime] earlier log truncated...\n'
const require = createRequire(import.meta.url)

class PtyRuntimeProcess extends EventEmitter implements SpawnedRuntimeProcess {
  readonly pid?: number
  readonly stdout = new PassThrough()
  readonly stderr = null
  private readonly pty: RuntimePty

  constructor(pty: RuntimePty) {
    super()
    this.pty = pty
    this.pid = pty.pid
    pty.onData((chunk) => this.stdout.write(chunk))
    pty.onExit(({ exitCode, signal }) => {
      this.stdout.end()
      this.emit(
        'close',
        exitCode,
        typeof signal === 'string' ? (signal as NodeJS.Signals) : null
      )
    })
  }

  kill(signal?: NodeJS.Signals | number): boolean {
    try {
      this.pty.kill(typeof signal === 'string' ? signal : undefined)
      return true
    } catch {
      return false
    }
  }
}

function normalizePtyEnv(env: NodeJS.ProcessEnv | undefined): Record<string, string> {
  const next: Record<string, string> = {}
  for (const [key, value] of Object.entries(env ?? process.env)) {
    if (typeof value === 'string') next[key] = value
  }
  next.TERM = next.TERM || 'xterm-256color'
  next.COLORTERM = next.COLORTERM || 'truecolor'
  return next
}

function loadNodePtySpawn(): NodePtySpawn {
  const mod = require('node-pty') as { spawn?: NodePtySpawn }
  if (typeof mod.spawn !== 'function') {
    throw new Error('node-pty module loaded but `spawn` is missing')
  }
  return mod.spawn
}

function createPtyRuntimeProcess(
  command: string,
  args: readonly string[] = [],
  options: Parameters<SpawnLike>[2] = {}
): SpawnedRuntimeProcess {
  const pty = loadNodePtySpawn()(command, [...args], {
    name: 'xterm-256color',
    cols: 120,
    rows: 30,
    cwd: options.cwd,
    env: normalizePtyEnv(options.env),
  })
  return new PtyRuntimeProcess(pty)
}

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
    log: '',
  }
}

function cloneState(state: RuntimeState): RuntimeState {
  return { ...state }
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

function resolveRuntimeCwd(targetRepo: string, cwd: string): string {
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
  const pathSep = process.platform === 'win32' ? ';' : ':'
  next[pathKey] = existing ? `${segment}${pathSep}${existing}` : segment
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

function resolveOutputEncoding(state: RuntimeState): 'utf8' | 'gbk' {
  if (state.outputEncoding === 'utf8' || state.outputEncoding === 'gbk') {
    return state.outputEncoding
  }

  return state.envType === 'visual-studio' ? 'gbk' : 'utf8'
}

function createChunkDecoder(state: RuntimeState): ChunkDecoder {
  if (resolveOutputEncoding(state) === 'gbk') {
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

function validateStartRequest(request: StartRuntimeRequest): string | null {
  if (!request.config.enabled) return 'runtime is disabled'
  if (!request.config.command.trim()) return 'runtime command is empty'
  if (request.config.envType === 'visual-studio' && !request.config.visualStudioInstanceId.trim()) {
    return 'visual studio instance is required'
  }

  try {
    resolveRuntimeCwd(request.targetRepo, request.config.cwd)
  } catch (error: unknown) {
    return error instanceof Error ? error.message : String(error)
  }

  return null
}

export function createRuntimeRunner(partialDeps?: Partial<RuntimeRunnerDeps>): RuntimeRunner {
  const customSpawnProvided = Object.prototype.hasOwnProperty.call(partialDeps ?? {}, 'spawn')
  const deps: RuntimeRunnerDeps = {
    platform: partialDeps?.platform ?? process.platform,
    spawn: partialDeps?.spawn ?? spawn,
    spawnPty: partialDeps?.spawnPty ?? (customSpawnProvided ? null : createPtyRuntimeProcess),
    detectMsys: partialDeps?.detectMsys ?? detectMsys,
    resolveVisualStudioEnvironment:
      partialDeps?.resolveVisualStudioEnvironment ?? resolveVisualStudioEnvironment,
    killProcessTree:
      partialDeps?.killProcessTree ??
      ((pid: number) => {
        execFile('taskkill', ['/PID', String(pid), '/T', '/F'], { windowsHide: true }, () => {
          // Best-effort tree termination for Windows runtimes.
        })
      }),
    logLimit: partialDeps?.logLimit ?? ACTIVE_LOG_LIMIT,
    now: partialDeps?.now ?? (() => new Date().toISOString()),
  }

  let state = initialState()
  let activeChild: SpawnedRuntimeProcess | null = null
  let stopRequested = false
  let childSettled = false
  let logTruncated = false
  const dataListeners = new Set<(event: RuntimeDataEvent) => void>()
  const statusListeners = new Set<(nextState: RuntimeState) => void>()

  function emitStatus(): void {
    const snapshot = cloneState(state)
    for (const listener of statusListeners) listener(snapshot)
  }

  function emitData(event: RuntimeDataEvent): void {
    for (const listener of dataListeners) listener(event)
  }

  function subscribeStatus(listener: (nextState: RuntimeState) => void): () => void {
    statusListeners.add(listener)
    return () => statusListeners.delete(listener)
  }

  function subscribeData(listener: (event: RuntimeDataEvent) => void): () => void {
    dataListeners.add(listener)
    return () => dataListeners.delete(listener)
  }

  function appendLog(stream: RuntimeDataEvent['stream'], chunk: string): void {
    const next = appendWindowedLog(state.log, chunk, deps.logLimit, logTruncated)
    state.log = next.nextLog
    logTruncated = next.truncated
    emitData({
      at: deps.now(),
      projectId: state.projectId,
      stream,
      chunk,
    })
  }

  function finish(status: RuntimeState['status'], code: number | null, signal: NodeJS.Signals | null): void {
    childSettled = true
    activeChild = null
    state.status = status
    state.finishedAt = deps.now()
    state.exitCode = code
    state.signal = signal
    emitStatus()
  }

  async function spawnRuntime(request: StartRuntimeRequest): Promise<SpawnedRuntimeProcess> {
    const resolvedCwd = resolveRuntimeCwd(request.targetRepo, request.config.cwd)
    state.cwd = resolvedCwd

    if (request.config.envType === 'msys') {
      state.visualStudioDisplayName = null
      const info = await deps.detectMsys()
      if (!info.available || !info.bashPath) {
        throw new Error('msys environment is unavailable')
      }

      const command = `cd ${quoteForBash(toMsysPath(resolvedCwd))} && ${request.config.command}`
      const runtimeSpawn = deps.spawnPty ?? deps.spawn
      return runtimeSpawn(info.bashPath, ['-lc', command], {
        cwd: resolvedCwd,
        env: buildMsysEnv(process.env, info.usrBinDir),
        shell: false,
        windowsHide: true,
        stdio: 'pipe',
      })
    }

    const result = await deps.resolveVisualStudioEnvironment({
      instanceId: request.config.visualStudioInstanceId,
      platform: deps.platform,
      baseEnv: process.env,
    })
    if (!result.ok) {
      throw new Error(result.error)
    }

    state.visualStudioDisplayName = result.displayName
    const runtimeSpawn = deps.spawnPty ?? deps.spawn
    return runtimeSpawn('cmd.exe', ['/d', '/s', '/c', request.config.command], {
      cwd: resolvedCwd,
      env: result.env,
      shell: false,
      windowsHide: true,
      windowsVerbatimArguments: true,
      stdio: 'pipe',
    })
  }

  function attachChild(child: SpawnedRuntimeProcess): void {
    const stdoutDecoder = createChunkDecoder(state)
    const stderrDecoder = createChunkDecoder(state)

    child.stdout?.on('data', (chunk: Buffer | string) => {
      const text = typeof chunk === 'string' ? chunk : stdoutDecoder.write(chunk)
      appendLog('stdout', text)
    })
    child.stderr?.on('data', (chunk: Buffer | string) => {
      const text = typeof chunk === 'string' ? chunk : stderrDecoder.write(chunk)
      appendLog('stderr', text)
    })
    child.on('error', (error) => {
      appendLog('system', `[runtime] ${error.message}\n`)
      finish('failed', null, null)
    })
    child.on('close', (code, signal) => {
      const stdoutRemainder = stdoutDecoder.end()
      if (stdoutRemainder) appendLog('stdout', stdoutRemainder)
      const stderrRemainder = stderrDecoder.end()
      if (stderrRemainder) appendLog('stderr', stderrRemainder)

      if (stopRequested) {
        finish('stopped', code, signal)
        return
      }

      finish(code === 0 ? 'exited' : 'failed', code, signal)
    })
  }

  return {
    async start(request: StartRuntimeRequest): Promise<RuntimeStartResult> {
      if (state.status === 'running') {
        return { ok: false, error: 'runtime already running', state: cloneState(state) }
      }

      const validationError = validateStartRequest(request)
      if (validationError) {
        return { ok: false, error: validationError, state: cloneState(state) }
      }

      stopRequested = false
      childSettled = false
      logTruncated = false
      state = {
        status: 'running',
        projectId: request.projectId,
        projectName: request.projectName,
        targetRepo: request.targetRepo,
        cwd: null,
        command: request.config.command.trim(),
        envType: request.config.envType,
        visualStudioInstanceId:
          request.config.envType === 'visual-studio'
            ? request.config.visualStudioInstanceId
            : null,
        visualStudioDisplayName: null,
        outputEncoding: request.config.outputEncoding as RuntimeOutputEncoding,
        startedAt: deps.now(),
        finishedAt: null,
        exitCode: null,
        signal: null,
        log: '',
      }

      try {
        activeChild = await spawnRuntime(request)
        attachChild(activeChild)
        appendLog('system', `[runtime] started: ${request.config.command.trim()}\n`)
        emitStatus()
        return { ok: true, state: cloneState(state) }
      } catch (error: unknown) {
        const message = error instanceof Error ? error.message : String(error)
        appendLog('system', `[runtime] failed to start: ${message}\n`)
        finish('failed', null, null)
        return { ok: false, error: message, state: cloneState(state) }
      }
    },

    stop(): RuntimeStopResult {
      if (state.status !== 'running') {
        return { ok: false, error: 'no runtime running' }
      }

      stopRequested = true
      appendLog('system', '[runtime] stop requested\n')
      if (activeChild && !childSettled) {
        if (deps.platform === 'win32' && typeof activeChild.pid === 'number') {
          deps.killProcessTree(activeChild.pid)
        } else {
          activeChild.kill('SIGTERM')
        }
      }
      return { ok: true }
    },

    getState(): RuntimeState {
      return cloneState(state)
    },

    onData(listener: (event: RuntimeDataEvent) => void): () => void {
      return subscribeData(listener)
    },

    onStatus(listener: (nextState: RuntimeState) => void): () => void {
      return subscribeStatus(listener)
    },
  }
}
