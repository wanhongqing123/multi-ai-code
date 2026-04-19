import { ipcMain, BrowserWindow, dialog } from 'electron'
import { promises as fs, createWriteStream, WriteStream } from 'fs'
import { join, isAbsolute, dirname } from 'path'
import { PtyCCProcess } from './PtyCCProcess.js'
import { StageDoneScanner, StageDoneMeta } from './StageDoneScanner.js'
import { snapshotArtifact } from '../store/snapshot.js'
import { listArtifacts, listEvents, recordEvent } from '../store/db.js'
import {
  STAGE_ARTIFACTS,
  stageArtifactPath,
  resolveStageArtifactAbs,
  STAGE_CLI_ARGS,
  STAGE_COMMAND,
  STAGE_CWD,
  buildSystemPrompt,
  buildForwardHandoff,
  buildFeedbackHandoff
} from '../orchestrator/prompts.js'
import { detectMsys } from '../util/msys.js'
import { rootDir } from '../store/paths.js'

/**
 * PTY chunk debug dumper. Enable by setting env var MULTI_AI_CODE_PTY_DUMP=1
 * before launching the app. Writes one JSONL record per chunk to
 * <rootDir>/logs/pty-stageN-<sessionId>-<ts>.jsonl so we can inspect the raw
 * bytes (ANSI escapes included) and design accurate noise filters.
 */
const PTY_DUMP_ENABLED = process.env.MULTI_AI_CODE_PTY_DUMP === '1'

async function openPtyDumpStream(
  sessionId: string,
  stageId: number,
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
  const file = join(dir, `pty-stage${stageId}-${safeSid}-${ts}.jsonl`)
  const stream = createWriteStream(file, { flags: 'a', encoding: 'utf8' })
  stream.write(
    JSON.stringify({
      t: Date.now(),
      event: 'spawn',
      stage: stageId,
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

const MSYS_STAGES = new Set([2, 3, 4])

async function readProjectMsysEnabled(projectDir: string): Promise<boolean> {
  try {
    const meta = JSON.parse(
      await fs.readFile(join(projectDir, 'project.json'), 'utf8')
    ) as { msys_enabled?: boolean }
    return !!meta.msys_enabled
  } catch {
    return false
  }
}

/**
 * Per-project mapping of Stage 1 plan-name → original absolute file path,
 * stored in `project.json` under `plan_sources`. When a user imports a
 * plan from an external markdown file, we keep writing back to that file
 * on subsequent Stage 1 sessions — "归档 = 原文件" rather than copying.
 */
async function readPlanSources(projectDir: string): Promise<Record<string, string>> {
  try {
    const meta = JSON.parse(
      await fs.readFile(join(projectDir, 'project.json'), 'utf8')
    ) as { plan_sources?: Record<string, string> }
    return meta.plan_sources ?? {}
  } catch {
    return {}
  }
}

async function writePlanSource(
  projectDir: string,
  label: string,
  absPath: string
): Promise<void> {
  const metaPath = join(projectDir, 'project.json')
  let meta: Record<string, unknown> = {}
  try {
    meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
  } catch {
    /* new project or missing file — start empty */
  }
  const prev = (meta.plan_sources as Record<string, string> | undefined) ?? {}
  meta.plan_sources = { ...prev, [label]: absPath }
  meta.updated_at = new Date().toISOString()
  await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
}

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
  skipSystemPrompt?: boolean
  /** Human-readable label for snapshot filenames (plan name). */
  label?: string
}

interface Session {
  proc: PtyCCProcess
  scanner: StageDoneScanner
  projectId: string
  stageId: number
  projectDir: string
  cwd: string
  sessionId: string
  /** Human-readable label for snapshot filenames (e.g. plan name). */
  label?: string
  /** Resolves to true once we've injected initial system prompt. */
  primed: boolean
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

    // If this project opted into MSYS and this is a stage that runs scripts
    // against target_repo, wire MSYS bash + usr/bin into the CLI's env so
    // `bash build.sh` etc. resolve against MSYS instead of failing.
    let enableMsys = false
    let msysBashPath: string | undefined
    let msysUsrBinDir: string | undefined
    if (
      process.platform === 'win32' &&
      MSYS_STAGES.has(req.stageId) &&
      (await readProjectMsysEnabled(req.projectDir))
    ) {
      const info = await detectMsys()
      if (info.available && info.bashPath && info.usrBinDir) {
        enableMsys = true
        msysBashPath = info.bashPath
        msysUsrBinDir = info.usrBinDir
      }
    }

    const proc = new PtyCCProcess({
      cwd: finalCwd,
      command: finalCommand,
      args: finalArgs,
      cols: req.cols,
      rows: req.rows,
      env: req.env,
      enableMsys,
      msysBashPath,
      msysUsrBinDir
    })
    const scanner = new StageDoneScanner()
    const dumpStream = await openPtyDumpStream(
      req.sessionId,
      req.stageId,
      req.projectId
    )
    const session: Session = {
      proc,
      scanner,
      projectId: req.projectId,
      stageId: req.stageId,
      projectDir: req.projectDir,
      cwd: finalCwd,
      sessionId: req.sessionId,
      label: req.label,
      primed: false,
      dumpStream
    }

    proc.on('data', (chunk: string) => {
      writePtyDump(dumpStream, chunk)
      broadcast('cc:data', { sessionId: req.sessionId, chunk })
      scanner.push(chunk)
    })
    proc.on('exit', (info: { exitCode: number; signal?: number }) => {
      closePtyDump(dumpStream, `exit(code=${info.exitCode})`)
      broadcast('cc:exit', { sessionId: req.sessionId, ...info })
      sessions.delete(req.sessionId)
    })

    scanner.on('done', async (meta: StageDoneMeta) => {
      // Stage 3 is user-annotation-driven code review; it does NOT produce an
      // artifact file, so don't fall back to stageArtifactPath — leaving it
      // null lets the CompletionDrawer show "本阶段未声明产物文件" cleanly
      // instead of "(未找到产物文件)".
      let artifactAbs: string | null = null
      let artifactRel: string | null = null
      if (meta.params.artifact) {
        artifactRel = meta.params.artifact
        artifactAbs = isAbsolute(artifactRel)
          ? artifactRel
          : join(req.projectDir, artifactRel)
      } else if (req.stageId !== 3) {
        artifactAbs = await resolveStageArtifactAbs(
          req.projectDir,
          req.stageId,
          req.label
        )
        // Report absolute path for stage 1 (target_repo-based); for stages
        // 2/4 keep the project-dir-relative form for legacy UI/event
        // consumers.
        artifactRel =
          req.stageId === 1
            ? artifactAbs
            : stageArtifactPath(req.stageId, req.label)
      }
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
          kind: 'auto',
          label: session.label
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
      recordEvent({
        project_id: req.projectId,
        from_stage: req.stageId,
        kind: 'stage:done',
        payload: {
          summary: meta.params.summary,
          verdict: meta.params.verdict,
          artifactPath: artifactRel,
          label: session.label
        }
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

          // Stage 1: plan name may be empty at spawn time (user hasn't named
          // the plan yet). In that case we tell the CLI to ask for a name at
          // archive time and not to hardcode a path now.
          const planPending = req.stageId === 1 && !req.label?.trim()

          // If this Stage 1 plan was previously imported from an external
          // file, archive back to that file (not the default location). The
          // mapping is persisted in project.json by the import flow.
          let externalArtifactAbs: string | null = null
          if (req.stageId === 1 && req.label?.trim()) {
            const sources = await readPlanSources(req.projectDir)
            const s = sources[req.label.trim()]
            if (s && isAbsolute(s)) externalArtifactAbs = s
          }

          const defaultAbs = await resolveStageArtifactAbs(
            req.projectDir,
            req.stageId,
            req.label
          )
          const artifactAbs = externalArtifactAbs ?? defaultAbs
          // For stage 1 we pass the absolute path to the prompt (since the
          // canonical location is outside the cwd). Stages 2-4 keep their
          // existing relative-form behavior so legacy prompt text stays
          // valid.
          const artifactPathForPrompt =
            req.stageId === 1
              ? artifactAbs
              : externalArtifactAbs ??
                stageArtifactPath(req.stageId, req.label) ??
                ''

          const sysPrompt = await buildSystemPrompt(req.stageId, {
            projectDir: req.projectDir,
            artifactPath: artifactPathForPrompt,
            projectName,
            targetRepo,
            stageCwd: finalCwd,
            planPending
          })

          // Stage 1 & Stage 3 use claude with auto-loaded `CLAUDE.md` — no
          // explicit Read tool call, no permission prompt, no user-facing
          // noise. Stage 2 / Stage 4 fall back to the legacy .injections
          // flow (codex doesn't auto-load CLAUDE.md the same way).
          if (req.stageId === 1) {
            const mdPath = join(finalCwd, 'CLAUDE.md')
            await fs.writeFile(
              mdPath,
              `<!-- This file is auto-generated by Multi-AI Code. Do not edit; it is rewritten on every Stage 1 spawn. -->\n\n${sysPrompt}`,
              'utf8'
            )
            const kickoffLines: string[] = [
              `你好，本阶段是「方案设计」，你的角色与约束都写在 cwd 的 \`CLAUDE.md\` 里（启动时你会自动加载）。`,
              ``
            ]
            if (planPending) {
              kickoffLines.push(
                `【重要】用户还**没有**输入方案名称。请先跟用户对话澄清需求；`,
                `在即将开始写设计文档之前，**必须先问用户**："你希望把这份方案归档成什么名字？"`,
                `拿到名字后，把方案写到 \`${targetRepo ?? req.projectDir}/.multi-ai-code/designs/<用户给的名字>.md\` 这个绝对路径，`,
                `并在 STAGE_DONE 标记里用这个完整的绝对路径作为 artifact= 的值。`,
                ``,
                `准备好后，请用 brainstorming 与我开始澄清这次方案的需求与目标。`
              )
            } else {
              kickoffLines.push(
                `【本次方案的归档路径】${artifactAbs}`,
                ``,
                `准备好后，请用 brainstorming 与我开始澄清这次方案的需求与目标。`
              )
            }
            await sendMessage(proc, kickoffLines.join('\n'))
          } else if (req.stageId === 3) {
            // Stage 3 = code review + user-annotation-driven edits. No
            // artifact, no forced handoff read. Just load the role via
            // CLAUDE.md and tell the CLI to wait for user annotations.
            const mdPath = join(finalCwd, 'CLAUDE.md')
            await fs.writeFile(
              mdPath,
              `<!-- This file is auto-generated by Multi-AI Code. Do not edit; it is rewritten on every Stage 3 spawn. -->\n\n${sysPrompt}`,
              'utf8'
            )
            await sendMessage(
              proc,
              [
                `你好，本阶段是「方案验收 · 代码审查」。你的角色与约束都写在 cwd 的 \`CLAUDE.md\` 里（启动时你会自动加载）。`,
                ``,
                `请先回复一句话表示已就绪，然后**等待**我通过"Diff 审查"窗口把具体的代码标注发给你。`,
                `收到标注后，按每条批注修改对应代码；全部处理完单独一行打印 \`<<STAGE_DONE summary="...">>\`（无需 artifact 参数）。`
              ].join('\n')
            )
          } else {
            // Legacy: write to .injections and instruct the CLI to read it.
            const refPath = await writePromptFile(
              finalCwd,
              req.stageId,
              'system',
              sysPrompt
            )
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
          }
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
          const designAbs = await resolveStageArtifactAbs(pdir, 1, s.label)
          designSpec = await fs.readFile(designAbs, 'utf8')
        } catch {
          designSpec = null
        }
      }
      // Auto-load acceptance report when entering the test stage
      let acceptanceReport: string | null = null
      if (req.toStage === 4) {
        try {
          const accAbs = await resolveStageArtifactAbs(pdir, 3, s.label)
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
      const artifactAbs = await resolveStageArtifactAbs(pdir, req.toStage, s.label)
      await sendMessage(
        s.proc,
        [
          `请读取 ${refPath}，这是来自上一阶段的 handoff（含上一阶段产物 + Stage 1 原始设计文档 + 本阶段约束），读完后继续本阶段工作。`,
          ``,
          `【硬性约定】完成本阶段时：把产物写到 ${artifactAbs}，并在终端单独一行打印 \`<<STAGE_DONE artifact=${artifactAbs} summary="一句话概括">>\`。不打印该标记平台就识别不到，流程会卡住。`
        ].join('\n')
      )
      recordEvent({
        project_id: s.projectId,
        from_stage: req.fromStage,
        to_stage: req.toStage,
        kind: 'handoff',
        payload: { summary: req.summary, verdict: req.verdict, label: s.label }
      })
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
      recordEvent({
        project_id: s.projectId,
        from_stage: req.fromStage,
        to_stage: req.toStage,
        kind: 'feedback',
        payload: { note: req.note.slice(0, 400), label: s.label }
      })
      return { ok: true }
    }
  )

  ipcMain.handle('event:list', (_e, { projectId, limit }: { projectId: string; limit?: number }) => {
    return listEvents(projectId, limit ?? 500)
  })

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
      const sessLabel = sessions.get(req.sessionId)?.label
      let artifactRel: string | null
      let artifactAbs: string | null
      if (req.artifactPath) {
        artifactRel = req.artifactPath
        artifactAbs = isAbsolute(req.artifactPath)
          ? req.artifactPath
          : join(req.projectDir, req.artifactPath)
      } else {
        artifactAbs = await resolveStageArtifactAbs(
          req.projectDir,
          req.stageId,
          sessLabel
        )
        // Stage 1 path is outside the project dir, so surface the absolute
        // form to the UI/event layer; stages 2-4 keep the relative form.
        artifactRel =
          req.stageId === 1
            ? artifactAbs
            : stageArtifactPath(req.stageId, sessLabel)
      }
      let artifactContent: string | null = null
      if (artifactAbs) {
        try {
          artifactContent = await fs.readFile(artifactAbs, 'utf8')
        } catch {
          artifactContent = null
        }
      }
      let snapshotPath: string | null = null
      const triggerSession = sessions.get(req.sessionId)
      if (artifactContent) {
        snapshotPath = await snapshotArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content: artifactContent,
          kind: 'manual',
          label: triggerSession?.label
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
    /** Human-readable label for the snapshot filename (e.g. plan name). */
    label?: string
    /** Override target path (absolute). When provided (e.g. externally-
     *  imported plan that should archive back to its origin file), content
     *  is written there instead of the stageArtifactPath default.
     *  A snapshot is still recorded under the platform's snapshot dir for
     *  history tracking. */
    externalArtifactPath?: string | null
  }): Promise<{ ok: true; artifactPath: string; artifactAbs: string; snapshotPath: string | null } | { ok: false; error: string }> {
    let artifactAbs: string
    let artifactPathForEvent: string
    if (req.externalArtifactPath && isAbsolute(req.externalArtifactPath)) {
      artifactAbs = req.externalArtifactPath
      // For external files we report the absolute path in stage:done so
      // handoff / UI can open it directly. There is no project-relative form.
      artifactPathForEvent = artifactAbs
    } else {
      const abs = await resolveStageArtifactAbs(
        req.projectDir,
        req.stageId,
        req.label
      )
      artifactAbs = abs
      // For stage 1 we now report absolute path (target_repo-based);
      // stages 2-4 keep the legacy project-dir-relative form.
      artifactPathForEvent =
        req.stageId === 1
          ? abs
          : stageArtifactPath(req.stageId, req.label)
    }
    await fs.mkdir(dirname(artifactAbs), { recursive: true })
    await fs.writeFile(artifactAbs, req.content, 'utf8')
    const snapshotPath = await snapshotArtifact({
      projectId: req.projectId,
      projectDir: req.projectDir,
      stageId: req.stageId,
      content: req.content,
      kind: req.kind,
      label: req.label
    })
    if (!req.skipBroadcast) {
      broadcast('stage:done', {
        sessionId: req.sessionId ?? `${req.projectId}:stage${req.stageId}`,
        projectId: req.projectId,
        stageId: req.stageId,
        raw: `<<${req.kind}>>`,
        params: { summary: req.summary },
        artifactPath: artifactPathForEvent,
        artifactContent: req.content,
        snapshotPath
      })
    }
    recordEvent({
      project_id: req.projectId,
      from_stage: req.stageId,
      kind: `artifact:${req.kind}`,
      payload: {
        summary: req.summary,
        artifactPath: artifactPathForEvent,
        label: req.label
      }
    })
    return { ok: true, artifactPath: artifactPathForEvent, artifactAbs, snapshotPath }
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
        snapshotPath: string
        sessionId?: string
        label?: string
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
          summary: '选用历史方案',
          label: req.label
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
        snapshotPath?: string
        pickFile?: boolean
        label?: string
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
          skipBroadcast: true,
          label: req.label
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
        label?: string
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
          summary: `导入文件: ${res.filePaths[0]}`,
          label: req.label
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

  /** Read the CURRENT working artifact for a given stage, using the same
   *  stageArtifactPath logic the CLI writes to. Returns absolute path +
   *  content so the renderer can preview the in-progress plan.
   *  For Stage 1 plans imported from an external file, reads that file
   *  instead of the workspaces/ default. */
  ipcMain.handle(
    'artifact:read-current',
    async (
      _e,
      {
        projectDir,
        stageId,
        label
      }: { projectDir: string; stageId: number; label?: string }
    ) => {
      let abs: string | null = null
      let rel: string | null = null
      if (stageId === 1 && label?.trim()) {
        const sources = await readPlanSources(projectDir)
        const s = sources[label.trim()]
        if (s && isAbsolute(s)) abs = s
      }
      if (!abs) {
        abs = await resolveStageArtifactAbs(projectDir, stageId, label)
        // Legacy relative form for display; for stage 1 (absolute) just
        // echo abs so the renderer has something to show.
        rel =
          stageId === 1 ? abs : stageArtifactPath(stageId, label)
      }
      try {
        const content = await fs.readFile(abs, 'utf8')
        return { ok: true, path: abs, relPath: rel ?? abs, content }
      } catch (err) {
        return {
          ok: false,
          path: abs,
          relPath: rel ?? abs,
          error: (err as Error).message
        }
      }
    }
  )

  /** Open a text file and return its content WITHOUT materializing an artifact.
   *  Used by the Stage-1 preview-before-commit flow. */
  ipcMain.handle(
    'dialog:pick-text-file',
    async (_e, opts: { title?: string } = {}) => {
      try {
        const res = await dialog.showOpenDialog({
          title: opts.title ?? '选择要导入的文件',
          properties: ['openFile'],
          filters: [
            { name: 'Markdown / Text', extensions: ['md', 'markdown', 'txt'] },
            { name: 'All Files', extensions: ['*'] }
          ]
        })
        if (res.canceled || res.filePaths.length === 0) {
          return { canceled: true as const }
        }
        const path = res.filePaths[0]
        const content = await fs.readFile(path, 'utf8')
        return { canceled: false as const, path, content }
      } catch (err) {
        return { canceled: false as const, error: (err as Error).message }
      }
    }
  )

  /** Commit caller-provided content as the stage artifact (post user preview). */
  ipcMain.handle(
    'artifact:commit-content',
    async (
      _e,
      req: {
        projectId: string
        projectDir: string
        stageId: number
        content: string
        sourcePath: string
        sessionId?: string
        label?: string
      }
    ) => {
      // Stage 1 "import from file" flow: archive back to the original file
      // (not workspaces/) and remember the mapping so subsequent Stage 1
      // spawns for this plan-name keep writing to the same source path.
      const useExternal =
        req.stageId === 1 && !!req.sourcePath && isAbsolute(req.sourcePath)
      if (useExternal && req.label) {
        try {
          await writePlanSource(req.projectDir, req.label, req.sourcePath)
        } catch {
          /* best-effort; mapping will simply not persist across restarts */
        }
      }
      return materializeArtifact({
        projectId: req.projectId,
        projectDir: req.projectDir,
        stageId: req.stageId,
        content: req.content,
        sessionId: req.sessionId,
        kind: 'imported',
        summary: `导入文件: ${req.sourcePath}`,
        label: req.label,
        externalArtifactPath: useExternal ? req.sourcePath : null
      })
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
