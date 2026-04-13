import { ipcMain, BrowserWindow } from 'electron'
import { promises as fs } from 'fs'
import { join, isAbsolute } from 'path'
import { PtyCCProcess } from './PtyCCProcess.js'
import { StageDoneScanner, StageDoneMeta } from './StageDoneScanner.js'
import { snapshotArtifact } from '../store/snapshot.js'
import { listArtifacts } from '../store/db.js'
import {
  STAGE_ARTIFACTS,
  STAGE_CLI_ARGS,
  STAGE_COMMAND,
  STAGE_CWD,
  buildSystemPrompt,
  buildForwardHandoff,
  buildFeedbackHandoff
} from '../orchestrator/prompts.js'

export interface SpawnRequest {
  sessionId: string
  projectId: string
  stageId: number
  projectDir: string
  cwd: string
  command?: string
  args?: string[]
  cols?: number
  rows?: number
  env?: Record<string, string>
  /** If true, skip injecting the system prompt on start. */
  skipSystemPrompt?: boolean
}

interface Session {
  proc: PtyCCProcess
  scanner: StageDoneScanner
  projectId: string
  stageId: number
  projectDir: string
  sessionId: string
  /** Resolves to true once we've injected initial system prompt. */
  primed: boolean
}

const sessions = new Map<string, Session>()

function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload)
  }
}

/** Delay (ms) after spawn before injecting the system prompt, so CC's TUI is ready. */
const PRIMING_DELAY_MS = 1200

/** Write one "message" into the CC TTY: text + CR to submit. */
function sendMessage(proc: PtyCCProcess, text: string): void {
  // For a typical interactive CC TUI, a single CR submits current line.
  // Multi-line text: CC accepts newlines as-is within the input buffer;
  // final CR submits.
  proc.write(text)
  proc.write('\r')
}

export function registerPtyIpc(): void {
  ipcMain.handle('cc:spawn', async (_e, req: SpawnRequest) => {
    if (sessions.has(req.sessionId)) {
      return { ok: false, error: 'session already exists' }
    }
    const stageArgs = STAGE_CLI_ARGS[req.stageId] ?? []
    const finalArgs = req.args && req.args.length > 0 ? req.args : stageArgs
    const finalCommand = req.command ?? STAGE_COMMAND[req.stageId] ?? 'claude'

    // Per-stage cwd: stages 1/2 are isolated empty workspaces (sandbox-safe);
    // stages 3-6 cd into their workspace dir which is a symlink to target_repo.
    // The renderer's `cwd` field is treated as a fallback only.
    const stageCwdRel = STAGE_CWD[req.stageId]
    const finalCwd = stageCwdRel ? join(req.projectDir, stageCwdRel) : req.cwd

    const proc = new PtyCCProcess({
      cwd: finalCwd,
      command: finalCommand,
      args: finalArgs,
      cols: req.cols,
      rows: req.rows,
      env: req.env
    })
    const scanner = new StageDoneScanner()
    const session: Session = {
      proc,
      scanner,
      projectId: req.projectId,
      stageId: req.stageId,
      projectDir: req.projectDir,
      sessionId: req.sessionId,
      primed: false
    }

    proc.on('data', (chunk: string) => {
      broadcast('cc:data', { sessionId: req.sessionId, chunk })
      scanner.push(chunk)
    })
    proc.on('exit', (info: { exitCode: number; signal?: number }) => {
      broadcast('cc:exit', { sessionId: req.sessionId, ...info })
      sessions.delete(req.sessionId)
    })

    scanner.on('done', async (meta: StageDoneMeta) => {
      const artifactRel =
        meta.params.artifact ?? STAGE_ARTIFACTS[req.stageId] ?? null
      const artifactAbs = artifactRel
        ? isAbsolute(artifactRel)
          ? artifactRel
          : join(req.projectDir, artifactRel)
        : null
      let artifactContent: string | null = null
      if (artifactAbs) {
        try {
          artifactContent = await fs.readFile(artifactAbs, 'utf8')
        } catch {
          artifactContent = null
        }
      }
      // Snapshot into artifacts/history/stageN/<timestamp>.md + DB row
      let snapshotPath: string | null = null
      if (artifactContent) {
        snapshotPath = await snapshotArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content: artifactContent,
          kind: 'auto'
        })
      }
      broadcast('stage:done', {
        sessionId: req.sessionId,
        projectId: req.projectId,
        stageId: req.stageId,
        raw: meta.raw,
        params: meta.params,
        artifactPath: artifactRel,
        artifactContent,
        snapshotPath
      })
    })

    scanner.on('feedback', (evt) => {
      broadcast('stage:feedback-emitted', {
        sessionId: req.sessionId,
        projectId: req.projectId,
        fromStage: req.stageId,
        toStage: evt.targetStage,
        raw: evt.raw,
        params: evt.params
      })
    })

    try {
      proc.start()
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
    sessions.set(req.sessionId, session)

    // Inject system prompt after CC TUI boots.
    if (!req.skipSystemPrompt) {
      setTimeout(async () => {
        try {
          // Pull project metadata (name / target_repo) from project.json so the
          // prompt can embed it
          let projectName: string | undefined
          let targetRepo: string | undefined
          try {
            const meta = JSON.parse(
              await fs.readFile(join(req.projectDir, 'project.json'), 'utf8')
            ) as { name?: string; target_repo?: string }
            projectName = meta.name
            targetRepo = meta.target_repo
          } catch {
            /* ignore */
          }

          const sysPrompt = await buildSystemPrompt(req.stageId, {
            projectDir: req.projectDir,
            artifactPath: STAGE_ARTIFACTS[req.stageId] ?? '',
            projectName,
            targetRepo,
            stageCwd: finalCwd
          })
          sendMessage(proc, sysPrompt)
          session.primed = true
        } catch (err) {
          broadcast('cc:notice', {
            sessionId: req.sessionId,
            level: 'warn',
            message: `系统 prompt 注入失败: ${(err as Error).message}`
          })
        }
      }, PRIMING_DELAY_MS)
    }

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
    sessions.delete(sessionId)
    return { ok: true }
  })

  ipcMain.handle('cc:kill-all', () => {
    const killed = Array.from(sessions.keys())
    for (const [, s] of sessions) s.proc.kill()
    sessions.clear()
    return { ok: true, killed }
  })

  /** Inject a forward handoff prompt into an already-running stage. */
  ipcMain.handle(
    'stage:inject-handoff',
    async (
      _e,
      req: {
        sessionId: string
        fromStage: number
        toStage: number
        artifactPath: string | null
        artifactContent: string | null
        summary?: string
        verdict?: string
        /** Optional override; if absent and toStage >= 3 we auto-load from stage 1 artifact. */
        projectDir?: string
      }
    ) => {
      const s = sessions.get(req.sessionId)
      if (!s) return { ok: false, error: 'no session' }

      const pdir = req.projectDir ?? s.projectDir
      // Auto-load design spec for stage >= 3 (authoritative source of truth)
      let designSpec: string | null = null
      if (req.toStage >= 3) {
        try {
          const designAbs = join(pdir, STAGE_ARTIFACTS[1])
          designSpec = await fs.readFile(designAbs, 'utf8')
        } catch {
          designSpec = null
        }
      }
      // Auto-load acceptance report when entering the test stage
      let acceptanceReport: string | null = null
      if (req.toStage === 4) {
        try {
          const accAbs = join(pdir, STAGE_ARTIFACTS[3])
          acceptanceReport = await fs.readFile(accAbs, 'utf8')
        } catch {
          acceptanceReport = null
        }
      }

      const msg = buildForwardHandoff({
        fromStage: req.fromStage,
        toStage: req.toStage,
        artifactPath: req.artifactPath,
        artifactContent: req.artifactContent,
        designSpec,
        acceptanceReport,
        summary: req.summary,
        verdict: req.verdict
      })
      sendMessage(s.proc, msg)
      return { ok: true }
    }
  )

  /** Inject a reverse feedback prompt into an already-running stage. */
  ipcMain.handle(
    'stage:inject-feedback',
    async (
      _e,
      req: {
        sessionId: string
        fromStage: number
        toStage: number
        note: string
        artifactPath?: string
        artifactContent?: string
      }
    ) => {
      const s = sessions.get(req.sessionId)
      if (!s) return { ok: false, error: 'no session' }
      const msg = buildFeedbackHandoff(req)
      sendMessage(s.proc, msg)
      return { ok: true }
    }
  )

  ipcMain.handle('cc:list', () => Array.from(sessions.keys()))

  ipcMain.handle('cc:has', (_e, { sessionId }: { sessionId: string }) =>
    sessions.has(sessionId)
  )

  /** Manually trigger the "stage done" drawer even if the CLI never emitted the marker. */
  ipcMain.handle(
    'stage:trigger-done',
    async (
      _e,
      req: {
        sessionId: string
        projectId: string
        stageId: number
        projectDir: string
        artifactPath?: string
        verdict?: string
        summary?: string
      }
    ) => {
      const artifactRel =
        req.artifactPath ?? STAGE_ARTIFACTS[req.stageId] ?? null
      const artifactAbs = artifactRel
        ? isAbsolute(artifactRel)
          ? artifactRel
          : join(req.projectDir, artifactRel)
        : null
      let artifactContent: string | null = null
      if (artifactAbs) {
        try {
          artifactContent = await fs.readFile(artifactAbs, 'utf8')
        } catch {
          artifactContent = null
        }
      }
      let snapshotPath: string | null = null
      if (artifactContent) {
        snapshotPath = await snapshotArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content: artifactContent,
          kind: 'manual'
        })
      }
      broadcast('stage:done', {
        sessionId: req.sessionId,
        projectId: req.projectId,
        stageId: req.stageId,
        raw: '<<manual>>',
        params: {
          ...(req.verdict ? { verdict: req.verdict } : {}),
          ...(req.summary ? { summary: req.summary } : {})
        },
        artifactPath: artifactRel,
        artifactContent,
        snapshotPath
      })
      return { ok: true, artifactFound: !!artifactContent, snapshotPath }
    }
  )

  /** List snapshotted artifacts for a project, optionally filtered by stage. */
  ipcMain.handle(
    'artifact:list',
    (_e, { projectId, stageId }: { projectId: string; stageId?: number }) => {
      return listArtifacts(projectId, stageId)
    }
  )

  /** Read a snapshot file by its project-dir-relative path. */
  ipcMain.handle(
    'artifact:read',
    async (_e, { projectDir, path }: { projectDir: string; path: string }) => {
      try {
        const abs = isAbsolute(path) ? path : join(projectDir, path)
        const content = await fs.readFile(abs, 'utf8')
        return { ok: true, content }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )
}

export function killAllSessions(): void {
  for (const [, s] of sessions) s.proc.kill()
  sessions.clear()
}
