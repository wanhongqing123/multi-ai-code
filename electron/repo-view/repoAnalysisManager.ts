import { BrowserWindow } from 'electron'
import { PtyCCProcess } from '../cc/PtyCCProcess.js'
import {
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
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
  claudePromptReady?: boolean
  claudeBootText?: string
}

/** Minimum time to wait for the CLI TUI to take over the PTY before
 *  we start typing into it — mirrors ptyManager's PRIMING_DELAY_MS. */
const PRIMING_DELAY_MS_CLAUDE = 1200
const READY_TIMEOUT_MS_CLAUDE = 15000
const READY_TIMEOUT_MS_CODEX = 10000

const sessions = new Map<number, RepoAnalysisSession>()

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))

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
    } else if (session.command === 'claude') {
      const boot = ((session.claudeBootText ?? '') + chunk).slice(-16000)
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
  if (session.command === 'codex') {
    await waitForCodexReady(input.winId, READY_TIMEOUT_MS_CODEX)
  } else if (session.command === 'claude') {
    await sleep(PRIMING_DELAY_MS_CLAUDE)
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
