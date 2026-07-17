import { ipcMain, BrowserWindow } from 'electron'
import { promises as fs, createWriteStream, existsSync, WriteStream } from 'fs'
import { delimiter, join } from 'path'
import { PtyCCProcess } from './PtyCCProcess.js'
import {
  shouldAutoAcceptCodexTrustPrompt,
  isCodexReadyForPromptInjection,
  isClaudeReadyForPromptInjection
} from './codexTrust.js'
import {
  buildExternalReviewPrompt,
  extractTaggedJsonReply,
  type ExternalReviewDecision,
  type ExternalReviewSuggestion
} from './structuredReply.js'
import { planSystemPromptInjection } from './systemPromptInjection.js'
import { buildResumeArgs, type ResumeCommand } from './resumeArgs.js'
import { withEmbeddedClaudeSettings } from './claudeLaunchSettings.js'
import { buildEnvWithPath, resolveCliSpawn } from '../util/cliSpawn.js'
import {
  isOpenCodeCommand,
  withOpenCodeLspEnv,
  type OpenCodeProviderProfile
} from '../aicli/opencodeConfig.js'
import {
  withCodexTerminalEnv,
  type TerminalThemeMode
} from '../aicli/codexConfig.js'
import {
  createAicliStructuredOutputBridge,
  type AicliControlMode,
  type AicliControlCommandResult,
  type AicliStructuredOutputBridge,
  type AicliStructuredOutputProvider
} from '../aicli/structuredOutputBridge.js'

import {
  buildSystemPrompt
} from '../orchestrator/prompts.js'
import { detectMsys } from '../util/msys.js'
import { rootDir } from '../store/paths.js'

/**
 * PTY chunk debug dumper. Enable by setting env var MULTI_AI_CODE_PTY_DUMP=1
 * before launching the app. Writes one JSONL record per chunk to
 * <rootDir>/logs/pty-<sessionId>-<ts>.jsonl.
 */
const PTY_DUMP_ENABLED = process.env.MULTI_AI_CODE_PTY_DUMP === '1'

function systemPromptInjectionCommand(command: string): 'claude' | 'codex' | 'opencode' {
  if (command === 'codex') return 'codex'
  if (command === 'opencode') return 'opencode'
  return 'claude'
}

function structuredOutputProvider(command: string): AicliStructuredOutputProvider | null {
  if (command === 'codex') return 'codex'
  if (isOpenCodeCommand(command)) return 'opencode'
  return null
}

async function openPtyDumpStream(
  sessionId: string,
  projectId: string
): Promise<WriteStream | null> {
  if (!PTY_DUMP_ENABLED) return null
  const dir = join(rootDir(), 'logs')
  try {
    await fs.mkdir(dir, { recursive: true })
  } catch {
    /* ignore */
  }
  const ts = new Date().toISOString().replace(/[:.]/g, '-')
  const safeSid = sessionId.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 32)
  const file = join(dir, `pty-${safeSid}-${ts}.jsonl`)
  const stream = createWriteStream(file, { flags: 'a', encoding: 'utf8' })
  stream.write(
    JSON.stringify({
      t: Date.now(),
      event: 'spawn',
      projectId,
      sessionId
    }) + '\n'
  )
  console.log(`[pty-dump] capturing raw chunks 鈫?${file}`)
  return stream
}

function writePtyDump(stream: WriteStream | null | undefined, chunk: string): void {
  if (!stream) return
  try {
    stream.write(
      JSON.stringify({ t: Date.now(), len: chunk.length, text: chunk }) + '\n'
    )
  } catch {
    /* swallow: dump is best-effort */
  }
}

function closePtyDump(
  stream: WriteStream | null | undefined,
  reason: string
): void {
  if (!stream) return
  try {
    stream.write(JSON.stringify({ t: Date.now(), event: 'close', reason }) + '\n')
    stream.end()
  } catch {
    /* ignore */
  }
}

export interface SpawnRequest {
  sessionId: string
  projectId: string
  projectDir: string
  targetRepo: string
  planName?: string
  /** 'none' starts a raw project CLI session without plan prompt injection. */
  planMode?: 'plan' | 'none'
  /** Absolute path resolved via resolvePlanArtifactAbs. */
  planAbsPath?: string
  /** true when plan file does not yet exist on disk. */
  planPending?: boolean
  /** True when this session is allowed to receive scheduled task prompts. */
  allowScheduledTasks?: boolean
  /** First user message to feed after kickoff. */
  initialUserMessage?: string
  /** CLI binary (claude | codex). */
  command: string
  /** CLI args. */
  args: string[]
  env?: Record<string, string>
  opencode?: OpenCodeProviderProfile
  /**
   * 宿主终端当前的明暗主题。用于给 codex 注入 CODEX_DEFAULT_TERMINAL_BG/FG，
   * 让 codex 的明暗判定与我们实际的终端背景一致（Windows ConPTY 无法探测）。
   */
  terminalTheme?: TerminalThemeMode
  cols?: number
  rows?: number
  /**
   * 'new' (default) spawns a fresh CLI session and injects the system prompt.
   * 'resume' rewrites args to the CLI's native continue form and skips
   * system-prompt injection so the CLI's saved conversation stays clean.
   */
  mode?: 'new' | 'resume'
}

export interface ResolveLaunchRequest {
  command: string
  env?: Record<string, string>
}

export interface ResolveLaunchResponse {
  ok: boolean
  notice?: string
  error?: string
}

function resolveLaunchNotice(
  command: string,
  envOverrides: Record<string, string> | undefined
): ResolveLaunchResponse {
  const env = buildEnvWithPath({
    ...process.env,
    ...(envOverrides ?? {})
  })
  const resolution = resolveCliSpawn(command, [], env)
  if (!resolution.ok) {
    return { ok: false, error: resolution.error }
  }
  return {
    ok: true,
    notice: resolution.resolved.launchNotice
  }
}

function spawnOkResponse(launchNotice: string | undefined): { ok: true; launchNotice?: string } {
  return launchNotice ? { ok: true, launchNotice } : { ok: true }
}

interface Session {
  proc: PtyCCProcess
  projectId: string
  projectDir: string
  /** Absolute path of the target repo used as the process cwd. */
  targetRepo: string
  sessionId: string
  planName: string
  command: string
  allowScheduledTasks: boolean
  startedAtMs: number
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
  claudePromptReady?: boolean
  claudeBootText?: string
  /** Raw-chunk dump stream (only when PTY_DUMP_ENABLED). */
  dumpStream?: WriteStream | null
  structuredOutputBridge?: AicliStructuredOutputBridge | null
  inputQueue?: Promise<void>
}

interface ExternalReviewJudgeRequest {
  sessionId: string
  planAbsPath: string
  suggestion: ExternalReviewSuggestion
}

interface PendingExternalReview {
  buffer: string
  resolve: (value: ExternalReviewDecision) => void
  reject: (error: Error) => void
  timeout: NodeJS.Timeout
}

type SessionDataListener = (evt: { sessionId: string; chunk: string }) => void
type SessionExitListener = (evt: {
  sessionId: string
  exitCode: number | null
  signal?: number | string | null
}) => void

const sessions = new Map<string, Session>()
const pendingExternalReviews = new Map<string, PendingExternalReview>()
const sessionDataListeners = new Set<SessionDataListener>()
const sessionExitListeners = new Set<SessionExitListener>()

const EXTERNAL_REVIEW_TIMEOUT_MS = 90_000
const RESUME_BOOT_TAIL_LIMIT = 16_384
const CLI_BOOT_TEXT_LIMIT = 65_536
const EXTERNAL_REVIEW_BUFFER_LIMIT = 524_288

function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload)
  }
}

export function addSessionDataListener(listener: SessionDataListener): () => void {
  sessionDataListeners.add(listener)
  return () => sessionDataListeners.delete(listener)
}

export function addSessionExitListener(listener: SessionExitListener): () => void {
  sessionExitListeners.add(listener)
  return () => sessionExitListeners.delete(listener)
}

function emitSessionData(evt: { sessionId: string; chunk: string }): void {
  for (const listener of sessionDataListeners) {
    try {
      listener(evt)
    } catch {
      /* keep PTY data delivery isolated from observers */
    }
  }
}

function emitSessionExit(evt: {
  sessionId: string
  exitCode: number | null
  signal?: number | string | null
}): void {
  for (const listener of sessionExitListeners) {
    try {
      listener(evt)
    } catch {
      /* keep PTY exit observers isolated */
    }
  }
}

/** Delay (ms) after spawn before injecting the system prompt, so CC's TUI is ready. */
const PRIMING_DELAY_MS = 1200
/** Extra delay for codex which boots slower than claude. */
const PRIMING_DELAY_MS_CODEX = 2500

const sleep = (ms: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, ms))

function getPathKey(env: Record<string, string>): string {
  return process.platform === 'win32'
    ? Object.keys(env).find((key) => key.toLowerCase() === 'path') ?? 'Path'
    : 'PATH'
}

function remoteImCliBinCandidates(): string[] {
  const processWithResources = process as NodeJS.Process & { resourcesPath?: string }
  return [
    process.env.MULTI_AI_CODE_IMCLI_BIN,
    join(process.cwd(), 'bin'),
    join(__dirname, '..', '..', '..', 'bin'),
    join(__dirname, '..', '..', 'bin'),
    processWithResources.resourcesPath
      ? join(processWithResources.resourcesPath, 'app.asar.unpacked', 'bin')
      : '',
    processWithResources.resourcesPath ? join(processWithResources.resourcesPath, 'bin') : ''
  ].filter((item): item is string => Boolean(item && existsSync(item)))
}

function withRemoteImCliEnv(
  env: Record<string, string> | undefined,
  projectId: string
): Record<string, string> {
  const next: Record<string, string> = {
    ...(env ?? {}),
    MULTI_AI_CODE_PROJECT_ID: projectId,
    MULTI_AI_CODE_ROOT_DIR: rootDir()
  }
  const pathKey = getPathKey(next)
  const pathParts = (next[pathKey] ?? process.env[pathKey] ?? '')
    .split(delimiter)
    .filter(Boolean)
  for (const candidate of remoteImCliBinCandidates().reverse()) {
    if (!pathParts.includes(candidate)) pathParts.unshift(candidate)
  }
  next[pathKey] = pathParts.join(delimiter)
  return next
}

export interface SendUserMessageOptions {
  displayText?: string
}

async function waitForCodexReady(
  sessionId: string,
  timeoutMs: number
): Promise<boolean> {
  const startedAt = Date.now()
  while (Date.now() - startedAt < timeoutMs) {
    const s = sessions.get(sessionId)
    if (!s) return false
    if (s.codexPromptReady || s.structuredOutputBridge?.isReady()) {
      s.codexPromptReady = true
      return true
    }
    await sleep(120)
  }
  const s = sessions.get(sessionId)
  if (s?.structuredOutputBridge?.isReady()) {
    s.codexPromptReady = true
    return true
  }
  return s?.codexPromptReady === true
}

async function waitForOpenCodeReady(
  sessionId: string,
  timeoutMs: number
): Promise<boolean> {
  const session = sessions.get(sessionId)
  if (!session) return false
  if (!session.structuredOutputBridge) return true

  // OpenCode historically accepted input immediately. Prefer the source-level
  // bridge readiness when available, but keep old binaries usable if they do
  // not emit control_ready.
  const ready = await session.structuredOutputBridge.waitUntilReady(timeoutMs)
  return ready || sessions.has(sessionId)
}

async function waitForClaudeReady(
  sessionId: string,
  timeoutMs: number
): Promise<boolean> {
  const startedAt = Date.now()
  while (Date.now() - startedAt < timeoutMs) {
    const s = sessions.get(sessionId)
    if (!s) return false
    if (s.claudePromptReady) return true
    await sleep(120)
  }
  return sessions.get(sessionId)?.claudePromptReady === true
}

/**
 * Write one "message" into the CC TTY: text + CR to submit.
 *
 * NOTE: TUIs like codex/claude detect bracketed-paste when a large chunk
 * arrives in one PTY read, and stash it as `[Pasted Content N chars]`
 * without auto-submitting. To look like real typing we stream in small
 * chunks with a tiny delay between them.
 */
async function sendMessage(
  proc: PtyCCProcess,
  text: string,
  options: { singleSubmit?: boolean } = {}
): Promise<void> {
  await streamInput(proc, text)
  await sleep(500)
  proc.write('\r')
  // 双回车是 claude/codex 的提交兜底；opencode 首个回车即提交，
  // 第二个回车会把空编辑器再提交一次，在会话里留下一条空消息。
  if (options.singleSubmit) return
  await sleep(150)
  proc.write('\r')
}

function createLocalTerminalDisplayChunk(text: string): string {
  const clean = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n').trim()
  if (!clean) return ''
  return `\r\n${clean.replace(/\n/g, '\r\n')}\r\n`
}

function displayLocalTerminalText(sessionId: string, text: string | undefined): void {
  if (!text) return
  const chunk = createLocalTerminalDisplayChunk(text)
  if (!chunk) return
  broadcast('cc:data', { sessionId, chunk })
}

export function getSessionForProject(projectId: string): {
  sessionId: string
  targetRepo: string
} | null {
  for (const session of sessions.values()) {
    if (session.projectId === projectId) {
      return {
        sessionId: session.sessionId,
        targetRepo: session.targetRepo
      }
    }
  }
  return null
}

export function getScheduledTaskSessionForProject(projectId: string): {
  sessionId: string
  targetRepo: string
} | null {
  for (const session of sessions.values()) {
    if (session.projectId === projectId && session.allowScheduledTasks) {
      return {
        sessionId: session.sessionId,
        targetRepo: session.targetRepo
      }
    }
  }
  return null
}

export function getActiveSessionForProject(projectId: string): {
  sessionId: string
  targetRepo: string
} | null {
  for (const session of sessions.values()) {
    if (session.projectId === projectId) {
      return {
        sessionId: session.sessionId,
        targetRepo: session.targetRepo
      }
    }
  }
  return null
}

export function getSessionRuntimeInfo(sessionId: string): {
  command: string
  targetRepo: string
  startedAtMs: number
} | null {
  const session = sessions.get(sessionId)
  if (!session) return null
  return {
    command: session.command,
    targetRepo: session.targetRepo,
    startedAtMs: session.startedAtMs
  }
}

async function enqueueSessionInput<T>(
  sessionId: string,
  task: (session: Session) => Promise<T>
): Promise<T> {
  const session = sessions.get(sessionId)
  if (!session) throw new Error('no session')

  const previous = session.inputQueue ?? Promise.resolve()
  const next = previous
    .catch(() => {
      /* keep later input delivery from inheriting a previous failure */
    })
    .then(async () => {
      const current = sessions.get(sessionId)
      if (!current) throw new Error('no session')
      return task(current)
    })

  session.inputQueue = next.then(
    () => undefined,
    () => undefined
  )
  return next
}

export async function sendUserMessageToSession(
  sessionId: string,
  text: string,
  options: SendUserMessageOptions = {}
): Promise<{ ok: boolean; error?: string }> {
  let session = sessions.get(sessionId)
  if (!session) return { ok: false, error: 'no session' }
  const ready =
    session.command === 'codex'
      ? await waitForCodexReady(sessionId, 10_000)
      : session.command === 'claude'
        ? await waitForClaudeReady(sessionId, 15_000)
        : isOpenCodeCommand(session.command)
          ? await waitForOpenCodeReady(sessionId, 10_000)
          : true
  if (!ready) return { ok: false, error: 'session not ready for input' }
  session = sessions.get(sessionId)
  if (!session) return { ok: false, error: 'no session' }
  try {
    await enqueueSessionInput(sessionId, async (current) => {
      const openCodeSession = isOpenCodeCommand(current.command)
      await sendMessage(current.proc, text, { singleSubmit: openCodeSession })
      // opencode 的全屏 TUI 会在会话里展示已提交消息；合成的本地回显 chunk 会直接
      // 画在 TUI 的编辑器/状态栏上，opentui 不感知这些格子被改过，形成永久残留。
      if (!openCodeSession) {
        displayLocalTerminalText(sessionId, options.displayText)
      }
    })
    return { ok: true }
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  }
}

export function switchAicliModeForSession(
  sessionId: string,
  mode: AicliControlMode
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  // 等 AICLI 回 control_result 再定成败：TUI 侧可能拒绝（协作模式未启用、
  // 任务执行中等），fire-and-forget 会把这些失败误报成“已切换”。
  return session.structuredOutputBridge.requestControlCommand({ command: 'switch_mode', mode })
}

export function requestAicliStatusForSession(
  sessionId: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({ command: 'status' })
}

export function requestAicliModelForSession(
  sessionId: string,
  model?: string,
  reasoning?: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({ command: 'model', model, reasoning })
}

export function requestAicliGoalForSession(
  sessionId: string,
  goal?: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({ command: 'goal', goal })
}

export function requestAicliBtwForSession(
  sessionId: string,
  task: string,
  replyId?: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({
    command: 'btw',
    task,
    ...(replyId ? { replyId } : {})
  })
}

export function requestAicliInterruptForSession(
  sessionId: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({ command: 'interrupt' })
}

export function requestAicliCompactForSession(
  sessionId: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({ command: 'compact' })
}

export function requestAicliClearForSession(
  sessionId: string
): Promise<AicliControlCommandResult> {
  const session = sessions.get(sessionId)
  if (!session) return Promise.resolve({ ok: false, error: 'no session' })
  if (!session.structuredOutputBridge) {
    return Promise.resolve({ ok: false, error: 'AICLI control bridge is not available' })
  }
  return session.structuredOutputBridge.requestControlCommand({ command: 'clear' })
}

/** Stream raw input into PTY in small chunks to avoid large-paste truncation. */
async function streamInput(proc: PtyCCProcess, text: string): Promise<void> {
  const CHUNK = 64
  for (let i = 0; i < text.length; i += CHUNK) {
    proc.write(text.slice(i, i + CHUNK))
    await sleep(6)
  }
}

function settlePendingExternalReview(
  sessionId: string,
  result:
    | { kind: 'resolve'; value: ExternalReviewDecision }
    | { kind: 'reject'; error: Error }
): void {
  const pending = pendingExternalReviews.get(sessionId)
  if (!pending) return
  clearTimeout(pending.timeout)
  pendingExternalReviews.delete(sessionId)
  if (result.kind === 'resolve') {
    pending.resolve(result.value)
    return
  }
  pending.reject(result.error)
}

function rejectPendingExternalReview(sessionId: string, message: string): void {
  settlePendingExternalReview(sessionId, {
    kind: 'reject',
    error: new Error(message)
  })
}

export function registerPtyIpc(): void {
  ipcMain.handle('cc:resolve-launch', async (_e, req: ResolveLaunchRequest) => {
    return resolveLaunchNotice(req.command, req.env)
  })

  ipcMain.handle('cc:spawn', async (_e, req: SpawnRequest) => {
    const launchResolution = resolveLaunchNotice(req.command, req.env)
    const planMode = req.planMode ?? 'plan'
    const hasPlan = planMode !== 'none'
    if (
      !req.targetRepo ||
      (hasPlan && (!req.planAbsPath || typeof req.initialUserMessage !== 'string'))
    ) {
      return {
        ok: false,
        error:
          'cc:spawn missing required fields (targetRepo, planAbsPath, initialUserMessage). ' +
          'Task 9 UI rewrite must supply these 鈥?see StagePanel.tsx TODO markers.'
      }
    }

    if (sessions.has(req.sessionId)) {
      return { ok: false, error: 'session already exists' }
    }

    // Resolve final cwd = req.targetRepo
    const finalCwd = req.targetRepo

    // MSYS detection for Windows
    let enableMsys = false
    let msysBashPath: string | undefined
    let msysUsrBinDir: string | undefined
    if (process.platform === 'win32') {
      try {
        const meta = JSON.parse(
          await fs.readFile(join(req.projectDir, 'project.json'), 'utf8')
        ) as { msys_enabled?: boolean }
        if (meta.msys_enabled) {
          const info = await detectMsys()
          if (info.available && info.bashPath && info.usrBinDir) {
            enableMsys = true
            msysBashPath = info.bashPath
            msysUsrBinDir = info.usrBinDir
          }
        }
      } catch {
        /* ignore */
      }
    }

    // Resume mode only applies to known CLIs; for anything else we fall back
    // to the regular 'new' path so callers can't accidentally rewrite args
    // for an unsupported binary.
    const isResumeMode =
      req.mode === 'resume' &&
      (req.command === 'claude' || req.command === 'codex' || req.command === 'opencode')
    let effectiveArgs = isResumeMode
      ? buildResumeArgs(req.command as ResumeCommand, req.args)
      : req.args
    effectiveArgs = withEmbeddedClaudeSettings(req.command, effectiveArgs)
    const structuredProvider = structuredOutputProvider(req.command)
    const structuredOutputBridge = structuredProvider
      ? await createAicliStructuredOutputBridge(req.sessionId, structuredProvider)
      : null
    if (structuredOutputBridge) {
      effectiveArgs = [...effectiveArgs, ...structuredOutputBridge.args]
    }

    const proc = new PtyCCProcess({
      cwd: finalCwd,
      command: req.command,
      args: effectiveArgs,
      cols: req.cols,
      rows: req.rows,
      env: withRemoteImCliEnv(
        withCodexTerminalEnv(
          req.command,
          withOpenCodeLspEnv(req.command, req.env, req.opencode),
          req.terminalTheme
        ),
        req.projectId
      ),
      enableMsys,
      msysBashPath,
      msysUsrBinDir
    })

    const dumpStream = await openPtyDumpStream(req.sessionId, req.projectId)

    const session: Session = {
      proc,
      projectId: req.projectId,
      projectDir: req.projectDir,
      targetRepo: req.targetRepo,
      sessionId: req.sessionId,
      planName: req.planName ?? '',
      command: req.command,
      allowScheduledTasks: req.allowScheduledTasks === true,
      startedAtMs: Date.now(),
      dumpStream,
      structuredOutputBridge
    }

    // Resume-failure detection: if the CLI exits with a non-zero code within
    // this window, we treat it as "no conversation to resume" (or similar)
    // and broadcast cc:resume-failed so the renderer can return to the boot
    // gate. `resumeBootTail` keeps the most recent output for the
    // failure event so the UI can show the CLI's own error message.
    const RESUME_FAIL_WINDOW_MS = 5000
    let resumeWindowOpen = isResumeMode
    let resumeBootTail = ''
    const resumeWindowTimer = isResumeMode
      ? setTimeout(() => {
          resumeWindowOpen = false
        }, RESUME_FAIL_WINDOW_MS)
      : null

    proc.on('data', (chunk: string) => {
      if (resumeWindowOpen) {
        resumeBootTail = (resumeBootTail + chunk).slice(-RESUME_BOOT_TAIL_LIMIT)
      }
      if (session.command === 'codex') {
        const boot = ((session.codexBootText ?? '') + chunk).slice(-CLI_BOOT_TEXT_LIMIT)
        session.codexBootText = boot
        if (!session.codexTrustAccepted && shouldAutoAcceptCodexTrustPrompt(boot)) {
          session.codexTrustAccepted = true
          proc.write('\r')
        }
        if (!session.codexPromptReady && isCodexReadyForPromptInjection(boot)) {
          session.codexPromptReady = true
        }
      } else if (session.command === 'claude') {
        const boot = ((session.claudeBootText ?? '') + chunk).slice(-CLI_BOOT_TEXT_LIMIT)
        session.claudeBootText = boot
        if (!session.claudePromptReady && isClaudeReadyForPromptInjection(boot)) {
          session.claudePromptReady = true
        }
      }

      const pending = pendingExternalReviews.get(req.sessionId)
      if (pending) {
        pending.buffer = (pending.buffer + chunk).slice(-EXTERNAL_REVIEW_BUFFER_LIMIT)
        try {
          const parsed = extractTaggedJsonReply(pending.buffer)
          if (parsed) {
            settlePendingExternalReview(req.sessionId, {
              kind: 'resolve',
              value: parsed
            })
          }
        } catch (err) {
          settlePendingExternalReview(req.sessionId, {
            kind: 'reject',
            error: err instanceof Error ? err : new Error(String(err))
          })
        }
      }

      writePtyDump(dumpStream, chunk)
      broadcast('cc:data', { sessionId: req.sessionId, chunk })
      emitSessionData({ sessionId: req.sessionId, chunk })
    })
    proc.on('exit', (info: { exitCode: number; signal?: number }) => {
      if (resumeWindowTimer) clearTimeout(resumeWindowTimer)
      if (resumeWindowOpen && info.exitCode !== 0) {
        broadcast('cc:resume-failed', {
          sessionId: req.sessionId,
          exitCode: info.exitCode,
          signal: info.signal,
          tail: resumeBootTail
        })
      }
      rejectPendingExternalReview(
        req.sessionId,
        `external review session ended before a structured reply arrived (exit ${info.exitCode})`
      )
      closePtyDump(dumpStream, `exit(code=${info.exitCode})`)
      void session.structuredOutputBridge?.close()
      broadcast('cc:exit', { sessionId: req.sessionId, ...info })
      emitSessionExit({ sessionId: req.sessionId, ...info })
      sessions.delete(req.sessionId)
    })

    try {
      proc.start()
    } catch (err) {
      void structuredOutputBridge?.close()
      return { ok: false, error: (err as Error).message }
    }
    sessions.set(req.sessionId, session)

    // In resume mode, skip the system-prompt + initialUserMessage injection
    // entirely. The CLI is loading its own saved conversation; injecting a
    // fresh system prompt would pollute the resumed context.
    if (isResumeMode) {
      return spawnOkResponse(launchResolution.notice)
    }

    if (!hasPlan) {
      return spawnOkResponse(launchResolution.notice)
    }

    // Inject system prompt after CC TUI boots.
    setTimeout(async () => {
      try {
        if (req.command === 'codex') {
          // Startup time varies heavily (trust gate / update banner / MCP boot).
          // Prefer the source-level control_ready bridge; keep TUI text as a
          // fallback for older Codex builds.
          await waitForCodexReady(req.sessionId, 10000)
        } else if (isOpenCodeCommand(req.command)) {
          await waitForOpenCodeReady(req.sessionId, 10000)
        } else if (req.command === 'claude') {
          // Claude can redraw its TUI after process start. Typing before the
          // input box is interactive can be lost, leaving the prompt file
          // written but never read by the session.
          await waitForClaudeReady(req.sessionId, 15000)
        }
        // Pull project metadata for the prompt
        let projectName: string | undefined
        try {
          const meta = JSON.parse(
            await fs.readFile(join(req.projectDir, 'project.json'), 'utf8')
          ) as { name?: string }
          projectName = meta.name
        } catch {
          /* ignore */
        }

        const sysPrompt = await buildSystemPrompt({
          projectDir: req.projectDir,
          artifactPath: req.planAbsPath ?? '',
          projectName,
          targetRepo: req.targetRepo,
          stageCwd: finalCwd,
          planPending: req.planPending ?? false
        })

        const injection = planSystemPromptInjection({
          command: systemPromptInjectionCommand(req.command),
          cwd: finalCwd,
          systemPrompt: sysPrompt,
          initialUserMessage: req.initialUserMessage ?? ''
        })
        await fs.mkdir(injection.writeDir, { recursive: true })
        await fs.writeFile(injection.writePath, injection.fileContents, 'utf8')
        await enqueueSessionInput(req.sessionId, async (current) => {
          await sendMessage(current.proc, injection.bootstrapMessage, {
            singleSubmit: isOpenCodeCommand(current.command)
          })
        })
      } catch (err) {
        broadcast('cc:notice', {
          sessionId: req.sessionId,
          level: 'warn',
          message: `绯荤粺 prompt 娉ㄥ叆澶辫触: ${(err as Error).message}`
        })
      }
    }, req.command === 'codex' ? PRIMING_DELAY_MS_CODEX : PRIMING_DELAY_MS)

    return spawnOkResponse(launchResolution.notice)
  })

  ipcMain.on('cc:input', (_e, { sessionId, data }: { sessionId: string; data: string }) => {
    sessions.get(sessionId)?.proc.write(data)
  })

  ipcMain.handle(
    'cc:paste',
    async (_e, { sessionId, data }: { sessionId: string; data: string }) => {
      const s = sessions.get(sessionId)
      if (!s) return { ok: false as const, error: 'no session' }
      try {
        await enqueueSessionInput(sessionId, async (current) => {
          await streamInput(current.proc, data)
        })
        return { ok: true as const }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.on(
    'cc:resize',
    (_e, { sessionId, cols, rows }: { sessionId: string; cols: number; rows: number }) => {
      sessions.get(sessionId)?.proc.resize(cols, rows)
    }
  )

  ipcMain.handle('cc:kill', (_e, { sessionId }: { sessionId: string }) => {
    const s = sessions.get(sessionId)
    if (!s) return { ok: false, error: 'no session' }
    s.proc.kill()
    rejectPendingExternalReview(
      sessionId,
      'external review session was terminated before a structured reply arrived'
    )
    closePtyDump(s.dumpStream, 'kill')
    void s.structuredOutputBridge?.close()
    emitSessionExit({ sessionId, exitCode: null, signal: 'kill' })
    sessions.delete(sessionId)
    return { ok: true }
  })

  ipcMain.handle('cc:kill-all', () => {
    const killed = Array.from(sessions.keys())
    for (const sessionId of pendingExternalReviews.keys()) {
      rejectPendingExternalReview(
        sessionId,
        'external review session was terminated before a structured reply arrived'
      )
    }
    for (const [sessionId, s] of sessions) {
      s.proc.kill()
      void s.structuredOutputBridge?.close()
      emitSessionExit({ sessionId, exitCode: null, signal: 'kill-all' })
    }
    pendingExternalReviews.clear()
    sessions.clear()
    return { ok: true, killed }
  })

  ipcMain.handle('cc:list', () => Array.from(sessions.keys()))

  ipcMain.handle('cc:has', (_e, { sessionId }: { sessionId: string }) =>
    sessions.has(sessionId)
  )

  /** Send an arbitrary user-typed message to a running session. */
  ipcMain.handle(
    'cc:send-user',
    async (_e, { sessionId, text }: { sessionId: string; text: string }) => {
      return sendUserMessageToSession(sessionId, text)
    }
  )

  ipcMain.handle(
    'cc:judge-external-review',
    async (_e, req: ExternalReviewJudgeRequest) => {
      const s = sessions.get(req.sessionId)
      if (!s) return { ok: false, error: 'no session' }
      if (pendingExternalReviews.has(req.sessionId)) {
        return { ok: false, error: 'external review already pending for this session' }
      }

      try {
        const result = await new Promise<ExternalReviewDecision>(async (resolve, reject) => {
          const timeout = setTimeout(() => {
            settlePendingExternalReview(req.sessionId, {
              kind: 'reject',
              error: new Error('Timed out waiting for external review decision.')
            })
          }, EXTERNAL_REVIEW_TIMEOUT_MS)

          pendingExternalReviews.set(req.sessionId, {
            buffer: '',
            resolve,
            reject,
            timeout
          })

          try {
            await enqueueSessionInput(req.sessionId, async (current) => {
              await sendMessage(
                current.proc,
                buildExternalReviewPrompt({
                  planAbsPath: req.planAbsPath,
                  suggestion: req.suggestion
                }),
                { singleSubmit: isOpenCodeCommand(current.command) }
              )
            })
          } catch (err) {
            settlePendingExternalReview(req.sessionId, {
              kind: 'reject',
              error: err instanceof Error ? err : new Error(String(err))
            })
          }
        })

        return { ok: true, result }
      } catch (err) {
        return {
          ok: false,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

}

export function killAllSessions(): void {
  for (const sessionId of pendingExternalReviews.keys()) {
    rejectPendingExternalReview(
      sessionId,
      'external review session was terminated before a structured reply arrived'
    )
  }
  for (const [, s] of sessions) {
    s.proc.kill()
    closePtyDump(s.dumpStream, 'killAll')
  }
  pendingExternalReviews.clear()
  sessions.clear()
}
