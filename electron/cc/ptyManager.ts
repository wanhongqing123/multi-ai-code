import { ipcMain, BrowserWindow } from 'electron'
import { promises as fs, createWriteStream, WriteStream } from 'fs'
import { join } from 'path'
import { PtyCCProcess } from './PtyCCProcess.js'
import {
  shouldAutoAcceptCodexTrustPrompt,
  isCodexReadyForPromptInjection
} from './codexTrust.js'
import {
  buildExternalReviewPrompt,
  extractTaggedJsonReply,
  type ExternalReviewDecision,
  type ExternalReviewSuggestion
} from './structuredReply.js'
import { planSystemPromptInjection } from './systemPromptInjection.js'
import { buildResumeArgs, type ResumeCommand } from './resumeArgs.js'

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
  planName: string
  /** Absolute path resolved via resolvePlanArtifactAbs. */
  planAbsPath: string
  /** true when plan file does not yet exist on disk. */
  planPending: boolean
  /** First user message to feed after kickoff. */
  initialUserMessage: string
  /** CLI binary (claude | codex). */
  command: string
  /** CLI args. */
  args: string[]
  env?: Record<string, string>
  cols?: number
  rows?: number
  /**
   * 'new' (default) spawns a fresh CLI session and injects the system prompt.
   * 'resume' rewrites args to the CLI's native continue form and skips
   * system-prompt injection so the CLI's saved conversation stays clean.
   */
  mode?: 'new' | 'resume'
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
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
  /** Raw-chunk dump stream (only when PTY_DUMP_ENABLED). */
  dumpStream?: WriteStream | null
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

const sessions = new Map<string, Session>()
const pendingExternalReviews = new Map<string, PendingExternalReview>()

const EXTERNAL_REVIEW_TIMEOUT_MS = 90_000

function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload)
  }
}

/** Delay (ms) after spawn before injecting the system prompt, so CC's TUI is ready. */
const PRIMING_DELAY_MS = 1200
/** Extra delay for codex which boots slower than claude. */
const PRIMING_DELAY_MS_CODEX = 2500

const sleep = (ms: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, ms))

async function waitForCodexReady(
  sessionId: string,
  timeoutMs: number
): Promise<void> {
  const startedAt = Date.now()
  while (Date.now() - startedAt < timeoutMs) {
    const s = sessions.get(sessionId)
    if (!s) return
    if (s.codexPromptReady) return
    await sleep(120)
  }
}

/**
 * Write one "message" into the CC TTY: text + CR to submit.
 *
 * NOTE: TUIs like codex/claude detect bracketed-paste when a large chunk
 * arrives in one PTY read, and stash it as `[Pasted Content N chars]`
 * without auto-submitting. To look like real typing we stream in small
 * chunks with a tiny delay between them.
 */
async function sendMessage(proc: PtyCCProcess, text: string): Promise<void> {
  await streamInput(proc, text)
  await sleep(500)
  proc.write('\r')
  await sleep(150)
  proc.write('\r')
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
  ipcMain.handle('cc:spawn', async (_e, req: SpawnRequest) => {
    if (!req.targetRepo || !req.planAbsPath || typeof req.initialUserMessage !== 'string') {
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
      (req.command === 'claude' || req.command === 'codex')
    let effectiveArgs = isResumeMode
      ? buildResumeArgs(req.command as ResumeCommand, req.args)
      : req.args

    const proc = new PtyCCProcess({
      cwd: finalCwd,
      command: req.command,
      args: effectiveArgs,
      cols: req.cols,
      rows: req.rows,
      env: req.env,
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
      planName: req.planName,
      command: req.command,
      dumpStream
    }

    // Resume-failure detection: if the CLI exits with a non-zero code within
    // this window, we treat it as "no conversation to resume" (or similar)
    // and broadcast cc:resume-failed so the renderer can return to the boot
    // gate. `resumeBootTail` keeps the most recent ~2KB of output for the
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
        resumeBootTail = (resumeBootTail + chunk).slice(-2048)
      }
      if (session.command === 'codex') {
        const boot = ((session.codexBootText ?? '') + chunk).slice(-16000)
        session.codexBootText = boot
        if (!session.codexTrustAccepted && shouldAutoAcceptCodexTrustPrompt(boot)) {
          session.codexTrustAccepted = true
          proc.write('\r')
        }
        if (!session.codexPromptReady && isCodexReadyForPromptInjection(boot)) {
          session.codexPromptReady = true
        }
      }

      const pending = pendingExternalReviews.get(req.sessionId)
      if (pending) {
        pending.buffer = (pending.buffer + chunk).slice(-128000)
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
      broadcast('cc:exit', { sessionId: req.sessionId, ...info })
      sessions.delete(req.sessionId)
    })

    try {
      proc.start()
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
    sessions.set(req.sessionId, session)

    // In resume mode, skip the system-prompt + initialUserMessage injection
    // entirely. The CLI is loading its own saved conversation; injecting a
    // fresh system prompt would pollute the resumed context.
    if (isResumeMode) {
      return { ok: true }
    }

    // Inject system prompt after CC TUI boots.
    setTimeout(async () => {
      try {
        if (req.command === 'codex') {
          // Startup time varies heavily (trust gate / update banner / MCP boot).
          // Wait until Codex home UI appears, then inject prompt text.
          await waitForCodexReady(req.sessionId, 10000)
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
          artifactPath: req.planAbsPath,
          projectName,
          targetRepo: req.targetRepo,
          stageCwd: finalCwd,
          planPending: req.planPending
        })

        const injection = planSystemPromptInjection({
          command: req.command === 'codex' ? 'codex' : 'claude',
          cwd: finalCwd,
          systemPrompt: sysPrompt,
          initialUserMessage: req.initialUserMessage
        })
        await fs.mkdir(injection.writeDir, { recursive: true })
        await fs.writeFile(injection.writePath, injection.fileContents, 'utf8')
        await sendMessage(proc, injection.bootstrapMessage)
      } catch (err) {
        broadcast('cc:notice', {
          sessionId: req.sessionId,
          level: 'warn',
          message: `绯荤粺 prompt 娉ㄥ叆澶辫触: ${(err as Error).message}`
        })
      }
    }, req.command === 'codex' ? PRIMING_DELAY_MS_CODEX : PRIMING_DELAY_MS)

    return { ok: true }
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
        await streamInput(s.proc, data)
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
    for (const [, s] of sessions) s.proc.kill()
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
      const s = sessions.get(sessionId)
      if (!s) return { ok: false, error: 'no session' }
      await sendMessage(s.proc, text)
      return { ok: true }
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
            await sendMessage(
              s.proc,
              buildExternalReviewPrompt({
                planAbsPath: req.planAbsPath,
                suggestion: req.suggestion
              })
            )
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
