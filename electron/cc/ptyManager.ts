import { ipcMain, BrowserWindow } from 'electron'
import { promises as fs, createWriteStream, WriteStream } from 'fs'
import { join } from 'path'
import { PtyCCProcess } from './PtyCCProcess.js'
import {
  shouldAutoAcceptCodexTrustPrompt,
  isCodexReadyForPromptInjection
} from './codexTrust.js'

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
  console.log(`[pty-dump] capturing raw chunks → ${file}`)
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
}

interface Session {
  proc: PtyCCProcess
  projectId: string
  projectDir: string
  sessionId: string
  planName: string
  command: string
  codexTrustAccepted?: boolean
  codexPromptReady?: boolean
  codexBootText?: string
  /** Raw-chunk dump stream (only when PTY_DUMP_ENABLED). */
  dumpStream?: WriteStream | null
}

const sessions = new Map<string, Session>()

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
  const CHUNK = 64
  for (let i = 0; i < text.length; i += CHUNK) {
    proc.write(text.slice(i, i + CHUNK))
    await sleep(6)
  }
  await sleep(500)
  proc.write('\r')
  await sleep(150)
  proc.write('\r')
}

export function registerPtyIpc(): void {
  ipcMain.handle('cc:spawn', async (_e, req: SpawnRequest) => {
    if (!req.targetRepo || !req.planAbsPath || typeof req.initialUserMessage !== 'string') {
      return {
        ok: false,
        error:
          'cc:spawn missing required fields (targetRepo, planAbsPath, initialUserMessage). ' +
          'Task 9 UI rewrite must supply these — see StagePanel.tsx TODO markers.'
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

    const proc = new PtyCCProcess({
      cwd: finalCwd,
      command: req.command,
      args: req.args,
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
      sessionId: req.sessionId,
      planName: req.planName,
      command: req.command,
      dumpStream
    }

    proc.on('data', (chunk: string) => {
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
      writePtyDump(dumpStream, chunk)
      broadcast('cc:data', { sessionId: req.sessionId, chunk })
    })
    proc.on('exit', (info: { exitCode: number; signal?: number }) => {
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

        if (req.command === 'claude') {
          // Write CLAUDE.md so claude auto-loads it
          const mdPath = join(finalCwd, 'CLAUDE.md')
          await fs.writeFile(
            mdPath,
            `<!-- This file is auto-generated by Multi-AI Code. Do not edit; it is rewritten on every spawn. -->\n\n${sysPrompt}`,
            'utf8'
          )
          await sendMessage(proc, req.initialUserMessage)
        } else {
          // codex: write to .injections and instruct the CLI to read it
          const injDir = join(finalCwd, '.injections')
          await fs.mkdir(injDir, { recursive: true })
          const ts = new Date().toISOString().replace(/[:.]/g, '-')
          const refPath = join(injDir, `system-${ts}.md`)
          await fs.writeFile(refPath, sysPrompt, 'utf8')
          await sendMessage(
            proc,
            [
              `请先完整读取 ${refPath} 作为本次任务的系统角色与约束说明，逐字遵守后再开始工作。`,
              ``,
              req.initialUserMessage
            ].join('\n')
          )
        }
      } catch (err) {
        broadcast('cc:notice', {
          sessionId: req.sessionId,
          level: 'warn',
          message: `系统 prompt 注入失败: ${(err as Error).message}`
        })
      }
    }, req.command === 'codex' ? PRIMING_DELAY_MS_CODEX : PRIMING_DELAY_MS)

    return { ok: true }
  })

  ipcMain.on('cc:input', (_e, { sessionId, data }: { sessionId: string; data: string }) => {
    sessions.get(sessionId)?.proc.write(data)
  })

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
    closePtyDump(s.dumpStream, 'kill')
    sessions.delete(sessionId)
    return { ok: true }
  })

  ipcMain.handle('cc:kill-all', () => {
    const killed = Array.from(sessions.keys())
    for (const [, s] of sessions) s.proc.kill()
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

}

export function killAllSessions(): void {
  for (const [, s] of sessions) {
    s.proc.kill()
    closePtyDump(s.dumpStream, 'killAll')
  }
  sessions.clear()
}
