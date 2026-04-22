import { BrowserWindow } from 'electron'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { randomBytes } from 'crypto'
import { PtyCCProcess } from '../cc/PtyCCProcess.js'
import { shouldAutoAcceptCodexTrustPrompt, isCodexReadyForPromptInjection } from '../cc/codexTrust.js'
import { buildRepoAnalysisPrompt } from './analysisPrompt.js'

interface RepoAnalysisSession {
  winId: number
  proc: PtyCCProcess
  projectId: string
  targetRepo: string
  command: string
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
}

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
  repoRoot: string
  filePath: string
  selection: string
  question: string
  projectSummary: string
  fileNote: string
}): Promise<void> {
  const session = sessions.get(input.winId)
  if (!session) throw new Error('repo analysis session not started')
  if (session.command === 'codex') {
    await waitForCodexReady(input.winId, 10000)
  }
  const text = buildRepoAnalysisPrompt(input)
  const dir = join(tmpdir(), 'multi-ai-code', 'repo-view')
  await fs.mkdir(dir, { recursive: true })
  const file = join(dir, `analysis-${randomBytes(4).toString('hex')}.md`)
  await fs.writeFile(file, text, 'utf8')
  await sendMessage(session.proc, `请先完整读取 ${file}，然后严格按要求输出分析结果。`)
}

export function stopRepoAnalysisSession(winId: number): void {
  const session = sessions.get(winId)
  if (!session) return
  session.proc.kill()
  sessions.delete(winId)
}
