import { ipcMain, BrowserWindow, dialog } from 'electron'
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
  /** Stage working directory the CLI was spawned in — used as a safe place
   *  to drop inject-files that even a sandboxed CLI (codex --full-auto) can read. */
  cwd: string
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
/** Extra delay for stage 1 (codex) which boots slower than claude. */
const PRIMING_DELAY_MS_STAGE1 = 2500

const sleep = (ms: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, ms))

/**
 * Persist a long prompt to disk and return its absolute path, so we can
 * instruct the AI to `Read` it instead of pasting multi-KB text through the
 * PTY (which triggers bracketed-paste / truncation on Windows conpty).
 */
async function writePromptFile(
  baseDir: string,
  stageId: number,
  kind: 'system' | 'handoff' | 'feedback',
  content: string
): Promise<string> {
  // Place the file under the stage's own cwd so sandboxed CLIs (codex
  // --full-auto on stage 1) can always read it.
  const dir = join(baseDir, '.injections')
  await fs.mkdir(dir, { recursive: true })
  const ts = new Date().toISOString().replace(/[:.]/g, '-')
  const full = join(dir, `stage${stageId}-${kind}-${ts}.md`)
  await fs.writeFile(full, content, 'utf8')
  return full
}

/**
 * Write one "message" into the CC TTY: text + CR to submit.
 *
 * NOTE: TUIs like codex/claude detect bracketed-paste when a large chunk
 * arrives in one PTY read, and stash it as `[Pasted Content N chars]`
 * without auto-submitting. That breaks system-prompt injection on Windows
 * (conpty flushes our whole buffer at once). To look like real typing we
 * stream in small chunks with a tiny delay between them.
 */
async function sendMessage(proc: PtyCCProcess, text: string): Promise<void> {
  const CHUNK = 64
  for (let i = 0; i < text.length; i += CHUNK) {
    proc.write(text.slice(i, i + CHUNK))
    // Yield between chunks so conpty doesn't coalesce them into one read.
    await sleep(6)
  }
  // Let the TUI fully render & exit any transient paste state before we
  // submit. On Windows conpty this must be generous or `\r` gets absorbed
  // into the paste buffer instead of committing it.
  await sleep(500)
  proc.write('\r')
  // Safety net: a second CR after a beat ensures the buffered input is
  // actually dispatched even if the first CR raced with a rendering tick.
  await sleep(150)
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
      cwd: finalCwd,
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
          const refPath = await writePromptFile(finalCwd, req.stageId, 'system', sysPrompt)
          const artifactAbs = join(req.projectDir, STAGE_ARTIFACTS[req.stageId] ?? '')
          await sendMessage(
            proc,
            [
              `请先完整读取 ${refPath} 作为本阶段的系统角色与约束说明，逐字遵守后再开始工作。`,
              ``,
              `【硬性约定，绝不能忘】工作完全结束时，必须：`,
              `1) 把本阶段产物写到 ${artifactAbs}；`,
              `2) 在终端单独一行原样打印形如 \`<<STAGE_DONE artifact=${artifactAbs} summary="一句话概括">>\` 的完成标记（必须是这两个尖括号，任何变体平台都识别不到，流程会卡住无法进入下一阶段）。`
            ].join('\n')
          )
          session.primed = true
        } catch (err) {
          broadcast('cc:notice', {
            sessionId: req.sessionId,
            level: 'warn',
            message: `系统 prompt 注入失败: ${(err as Error).message}`
          })
        }
      }, req.stageId === 1 ? PRIMING_DELAY_MS_STAGE1 : PRIMING_DELAY_MS)
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
      const refPath = await writePromptFile(s.cwd, req.toStage, 'handoff', msg)
      const artifactAbs = join(pdir, STAGE_ARTIFACTS[req.toStage] ?? '')
      await sendMessage(
        s.proc,
        [
          `请读取 ${refPath}，这是来自上一阶段的 handoff（含上一阶段产物 + Stage 1 原始设计文档 + 本阶段约束），读完后继续本阶段工作。`,
          ``,
          `【硬性约定】完成本阶段时：把产物写到 ${artifactAbs}，并在终端单独一行打印 \`<<STAGE_DONE artifact=${artifactAbs} summary="一句话概括">>\`。不打印该标记平台就识别不到，流程会卡住。`
        ].join('\n')
      )
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
      const refPath = await writePromptFile(s.cwd, req.toStage, 'feedback', msg)
      await sendMessage(
        s.proc,
        `请读取 ${refPath}，这是下游阶段发回的 feedback，读完后基于反馈调整并重新产出产物。`
      )
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

  /** Shared helper: materialize a content blob as the stage's default artifact,
   *  optionally firing a synthesized stage:done. */
  async function materializeArtifact(req: {
    projectId: string
    projectDir: string
    stageId: number
    content: string
    sessionId?: string
    kind: string
    summary: string
    /** When true, skip the stage:done broadcast (e.g. seeding for further refinement). */
    skipBroadcast?: boolean
  }): Promise<{ ok: true; artifactPath: string; artifactAbs: string; snapshotPath: string | null } | { ok: false; error: string }> {
    const artifactRel = STAGE_ARTIFACTS[req.stageId]
    if (!artifactRel) return { ok: false, error: `unknown stage ${req.stageId}` }
    const artifactAbs = join(req.projectDir, artifactRel)
    await fs.mkdir(join(artifactAbs, '..'), { recursive: true })
    await fs.writeFile(artifactAbs, req.content, 'utf8')
    const snapshotPath = await snapshotArtifact({
      projectId: req.projectId,
      projectDir: req.projectDir,
      stageId: req.stageId,
      content: req.content,
      kind: req.kind
    })
    if (!req.skipBroadcast) {
      broadcast('stage:done', {
        sessionId: req.sessionId ?? `${req.projectId}:stage${req.stageId}`,
        projectId: req.projectId,
        stageId: req.stageId,
        raw: `<<${req.kind}>>`,
        params: { summary: req.summary },
        artifactPath: artifactRel,
        artifactContent: req.content,
        snapshotPath
      })
    }
    return { ok: true, artifactPath: artifactRel, artifactAbs, snapshotPath }
  }

  /**
   * Restore a historical snapshot into the stage's default artifact path, then
   * fire a synthesized `stage:done` so the completion drawer pops and the
   * user can advance to the next stage without re-running the CLI.
   */
  ipcMain.handle(
    'artifact:restore',
    async (
      _e,
      req: {
        projectId: string
        projectDir: string
        stageId: number
        /** Project-dir-relative path of the snapshot to restore. */
        snapshotPath: string
        sessionId?: string
      }
    ) => {
      try {
        const snapAbs = isAbsolute(req.snapshotPath)
          ? req.snapshotPath
          : join(req.projectDir, req.snapshotPath)
        const content = await fs.readFile(snapAbs, 'utf8')
        return materializeArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content,
          sessionId: req.sessionId,
          kind: 'restored',
          summary: '选用历史方案'
        })
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  /**
   * Seed a stage's default artifact with either a historical snapshot or an
   * external file, WITHOUT firing the completion drawer. Returns the absolute
   * artifact path so the caller can then prompt the AI to refine it.
   */
  ipcMain.handle(
    'artifact:seed',
    async (
      _e,
      req: {
        projectId: string
        projectDir: string
        stageId: number
        /** Exactly one of these must be provided. */
        snapshotPath?: string
        /** Open a native file picker instead (ignored if snapshotPath is set). */
        pickFile?: boolean
      }
    ) => {
      try {
        let content: string
        let sourceLabel: string
        if (req.snapshotPath) {
          const abs = isAbsolute(req.snapshotPath)
            ? req.snapshotPath
            : join(req.projectDir, req.snapshotPath)
          content = await fs.readFile(abs, 'utf8')
          sourceLabel = `历史快照 ${req.snapshotPath}`
        } else if (req.pickFile) {
          const res = await dialog.showOpenDialog({
            title: '选择要继续完善的方案文件',
            properties: ['openFile'],
            filters: [
              { name: 'Markdown / Text', extensions: ['md', 'markdown', 'txt'] },
              { name: 'All Files', extensions: ['*'] }
            ]
          })
          if (res.canceled || res.filePaths.length === 0) {
            return { ok: false, canceled: true }
          }
          content = await fs.readFile(res.filePaths[0], 'utf8')
          sourceLabel = `外部文件 ${res.filePaths[0]}`
        } else {
          return { ok: false, error: 'either snapshotPath or pickFile required' }
        }
        const mat = await materializeArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content,
          kind: 'refine-seed',
          summary: `基于${sourceLabel}继续完善`,
          skipBroadcast: true
        })
        if (!mat.ok) return mat
        return {
          ok: true,
          artifactPath: mat.artifactPath,
          artifactAbs: mat.artifactAbs,
          snapshotPath: mat.snapshotPath,
          sourceLabel
        }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
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

  /** Import an external file as a stage's default artifact. Opens a native
   *  file picker, reads the chosen file, then materializes + fires stage:done. */
  ipcMain.handle(
    'artifact:import-file',
    async (
      _e,
      req: {
        projectId: string
        projectDir: string
        stageId: number
        sessionId?: string
      }
    ) => {
      try {
        const res = await dialog.showOpenDialog({
          title: '选择要作为本阶段产物的文件',
          properties: ['openFile'],
          filters: [
            { name: 'Markdown / Text', extensions: ['md', 'markdown', 'txt'] },
            { name: 'All Files', extensions: ['*'] }
          ]
        })
        if (res.canceled || res.filePaths.length === 0) {
          return { ok: false, canceled: true }
        }
        const content = await fs.readFile(res.filePaths[0], 'utf8')
        return materializeArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content,
          sessionId: req.sessionId,
          kind: 'imported',
          summary: `导入文件: ${res.filePaths[0]}`
        })
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
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
