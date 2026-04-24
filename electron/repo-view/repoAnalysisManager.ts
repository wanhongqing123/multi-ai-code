import { BrowserWindow } from 'electron'
import { PtyCCProcess } from '../cc/PtyCCProcess.js'
import {
  shouldAutoAcceptCodexTrustPrompt,
  shouldAutoAcceptSessionEditPrompt,
  isCodexReadyForPromptInjection,
  isClaudeReadyForPromptInjection
} from '../cc/codexTrust.js'

interface RepoAnalysisSession {
  winId: number
  proc: PtyCCProcess
  projectId: string
  targetRepo: string
  command: string
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
  claudePromptReady?: boolean
  claudeBootText?: string
  /** Last time we auto-pressed "2\r" on a permission prompt — debounce so
   *  the same prompt streaming in across multiple chunks isn't answered
   *  twice, but a *new* prompt later in the session still gets handled. */
  lastPermissionRespondAt?: number
}

/** Minimum ms between consecutive auto-accepts. Claude's TUI clears the
 *  prompt within ~100ms of receiving "2\r", so a debounce a few times
 *  larger than that is enough to avoid double-fire on streaming chunks
 *  while still letting a *different* later prompt through. */
const PERMISSION_RESPOND_DEBOUNCE_MS = 1500

/** Minimum time to wait for the CLI TUI to take over the PTY before
 *  we start typing into it — mirrors ptyManager's PRIMING_DELAY_MS. */
const READY_TIMEOUT_MS_CLAUDE = 15000
const READY_TIMEOUT_MS_CODEX = 10000

const sessions = new Map<number, RepoAnalysisSession>()

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

/** Type + submit text into the PTY: chunked-write to avoid the TUI's
 *  bracketed-paste detection, then a CR (twice for safety) to submit. */
async function sendMessage(proc: PtyCCProcess, text: string): Promise<void> {
  const chunk = 64
  for (let i = 0; i < text.length; i += chunk) {
    proc.write(text.slice(i, i + chunk))
    await sleep(6)
  }
  await sleep(350)
  proc.write('\r')
  await sleep(120)
  proc.write('\r')
}

function emitTo(winId: number, channel: string, payload: unknown): void {
  const win = BrowserWindow.fromId(winId)
  if (win && !win.isDestroyed()) {
    win.webContents.send(channel, payload)
  }
}

async function waitForCodexReady(winId: number, timeoutMs: number): Promise<void> {
  const startedAt = Date.now()
  while (Date.now() - startedAt < timeoutMs) {
    const s = sessions.get(winId)
    if (!s) return
    if (s.codexPromptReady) return
    await sleep(120)
  }
}

async function waitForClaudeReady(winId: number, timeoutMs: number): Promise<void> {
  const startedAt = Date.now()
  while (Date.now() - startedAt < timeoutMs) {
    const s = sessions.get(winId)
    if (!s) return
    if (s.claudePromptReady) return
    await sleep(120)
  }
}

export async function startRepoAnalysisSession(input: {
  winId: number
  projectId: string
  targetRepo: string
  command: string
  args: string[]
  env?: Record<string, string>
}): Promise<void> {
  if (sessions.has(input.winId)) return
  const proc = new PtyCCProcess({
    cwd: input.targetRepo,
    command: input.command,
    args: input.args,
    env: input.env
  })
  const session: RepoAnalysisSession = {
    winId: input.winId,
    proc,
    projectId: input.projectId,
    targetRepo: input.targetRepo,
    command: input.command
  }
  proc.on('data', (chunk) => {
    const combined = (((session.command === 'codex'
      ? session.codexBootText
      : session.claudeBootText) ?? '') + chunk).slice(-16000)
    if (shouldAutoAcceptSessionEditPrompt(combined)) {
      const now = Date.now()
      if (
        !session.lastPermissionRespondAt ||
        now - session.lastPermissionRespondAt > PERMISSION_RESPOND_DEBOUNCE_MS
      ) {
        session.lastPermissionRespondAt = now
        proc.write('2\r')
      }
    }
    if (session.command === 'codex') {
      const boot = combined
      session.codexBootText = boot
      if (!session.codexTrustAccepted && shouldAutoAcceptCodexTrustPrompt(boot)) {
        session.codexTrustAccepted = true
        proc.write('\r')
      }
      if (!session.codexPromptReady && isCodexReadyForPromptInjection(boot)) {
        session.codexPromptReady = true
      }
    } else if (session.command === 'claude') {
      const boot = combined
      session.claudeBootText = boot
      if (!session.claudePromptReady && isClaudeReadyForPromptInjection(boot)) {
        session.claudePromptReady = true
      }
    }
    emitTo(input.winId, 'repo-view:analysis-data', { chunk })
  })
  proc.on('exit', ({ exitCode, signal }) => {
    emitTo(input.winId, 'repo-view:analysis-status', {
      status: 'exited',
      exitCode,
      signal
    })
    sessions.delete(input.winId)
  })
  proc.start()
  sessions.set(input.winId, session)
  emitTo(input.winId, 'repo-view:analysis-status', { status: 'running' })
}

export async function sendRepoAnalysisPrompt(input: {
  winId: number
  text: string
}): Promise<void> {
  const session = sessions.get(input.winId)
  if (!session) throw new Error('repo analysis session not started')
  // Only pay the readiness cost on the first send for each CLI — once the
  // TUI has been seen interactive, subsequent sends are immediate.
  if (session.command === 'codex' && !session.codexPromptReady) {
    await waitForCodexReady(input.winId, READY_TIMEOUT_MS_CODEX)
  } else if (session.command === 'claude' && !session.claudePromptReady) {
    await waitForClaudeReady(input.winId, READY_TIMEOUT_MS_CLAUDE)
  }
  await sendMessage(session.proc, input.text)
}

export function stopRepoAnalysisSession(winId: number): void {
  const session = sessions.get(winId)
  if (!session) return
  session.proc.kill()
  sessions.delete(winId)
}

export function writeRepoAnalysisInput(winId: number, data: string): void {
  const session = sessions.get(winId)
  if (!session) return
  session.proc.write(data)
}

export function resizeRepoAnalysisSession(
  winId: number,
  cols: number,
  rows: number
): void {
  const session = sessions.get(winId)
  if (!session) return
  session.proc.resize(cols, rows)
}

export function hasRepoAnalysisSession(winId: number): boolean {
  return sessions.has(winId)
}
