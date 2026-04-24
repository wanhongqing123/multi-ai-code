import { BrowserWindow } from 'electron'
import { PtyCCProcess } from '../cc/PtyCCProcess.js'
import {
  normalizeTerminalText,
  shouldAutoAcceptCodexTrustPrompt,
  isCodexReadyForPromptInjection,
  isClaudeReadyForPromptInjection
} from '../cc/codexTrust.js'

interface RepoAnalysisSession {
  winId: number
  proc: PtyCCProcess
  projectId: string
  targetRepo: string
  command: string
  startedAt: number
  hasSeenOutput?: boolean
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
  claudePromptReady?: boolean
  claudeBootText?: string
  /** Last time we auto-pressed "2\r" on a permission prompt — debounce so
   *  the same prompt streaming in across multiple chunks isn't answered
   *  twice, but a *new* prompt later in the session still gets handled. */
  lastPermissionRespondAt?: number
  permissionPromptActive?: boolean
}

const INTERACTIVE_ASSUMPTION_MS = 1200

/** Minimum time to wait for the CLI TUI to take over the PTY before
 *  we start typing into it — mirrors ptyManager's PRIMING_DELAY_MS. */
const READY_TIMEOUT_MS_CLAUDE = 15000
const READY_TIMEOUT_MS_CODEX = 10000

const sessions = new Map<number, RepoAnalysisSession>()

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

export function shouldWaitForRepoCliReady(
  session: Pick<
    RepoAnalysisSession,
    'command' | 'startedAt' | 'hasSeenOutput' | 'codexPromptReady' | 'claudePromptReady'
  >,
  now = Date.now()
): boolean {
  if (session.command === 'codex' && session.codexPromptReady) return false
  if (session.command === 'claude' && session.claudePromptReady) return false
  if (session.hasSeenOutput && now - session.startedAt >= INTERACTIVE_ASSUMPTION_MS) {
    return false
  }
  return true
}

export function shouldAutoRespondToRepoPermissionPrompt(
  session: Pick<
    RepoAnalysisSession,
    'permissionPromptActive' | 'lastPermissionRespondAt'
  >,
  combined: string,
  now = Date.now()
): {
  shouldRespond: boolean
  promptActive: boolean
  lastRespondAt: number | undefined
} {
  const text = normalizeTerminalText(combined).toLowerCase()
  const promptVisible =
    text.includes('allow all edits during this session') ||
    (text.includes('do you want to proceed') &&
      text.includes('and always allow') &&
      (text.includes('ensure analyses cache directory exists') ||
        text.includes('analyses/ from this project') ||
        text.includes('.multi-ai-code/repo-view/analyses')))
  if (!promptVisible) {
    return {
      shouldRespond: false,
      promptActive: false,
      lastRespondAt: session.lastPermissionRespondAt
    }
  }
  if (session.permissionPromptActive) {
    return {
      shouldRespond: false,
      promptActive: true,
      lastRespondAt: session.lastPermissionRespondAt
    }
  }
  return {
    shouldRespond: true,
    promptActive: true,
    lastRespondAt: now
  }
}

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
    command: input.command,
    startedAt: Date.now()
  }
  proc.on('data', (chunk) => {
    if (chunk.length > 0) session.hasSeenOutput = true
    const combined = (((session.command === 'codex'
      ? session.codexBootText
      : session.claudeBootText) ?? '') + chunk).slice(-16000)
    const permission = shouldAutoRespondToRepoPermissionPrompt(
      session,
      combined,
      Date.now()
    )
    session.permissionPromptActive = permission.promptActive
    session.lastPermissionRespondAt = permission.lastRespondAt
    if (permission.shouldRespond) {
      proc.write('2\r')
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
  if (shouldWaitForRepoCliReady(session)) {
    if (session.command === 'codex') {
      await waitForCodexReady(input.winId, READY_TIMEOUT_MS_CODEX)
    } else if (session.command === 'claude') {
      await waitForClaudeReady(input.winId, READY_TIMEOUT_MS_CLAUDE)
    }
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
