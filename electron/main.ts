import { app, BrowserWindow, ipcMain, shell, dialog } from 'electron'
import { join, isAbsolute, dirname } from 'path'
import { tmpdir } from 'os'
import { randomBytes } from 'crypto'
import { execFile } from 'child_process'
import { promisify } from 'util'
const execFileAsync = promisify(execFile)
import {
  initDb,
  closeDb,
  getProject,
  createProject,
  listProjects,
  deleteProject as dbDeleteProject,
  updateProjectName,
  touchProject,
  listArtifacts,
  listEvents,
  recordEvent
} from './store/db.js'
import {
  ensureRootDir,
  createProjectLayout,
  projectDir as projectDirFn,
  artifactsDir,
  rootDir
} from './store/paths.js'
import { registerPtyIpc, killAllSessions } from './cc/ptyManager.js'
import { listPlans, registerExternalPlan } from './orchestrator/plans.js'
import { detectMsys, buildOpenMsysTerminalCommand } from './util/msys.js'
import { spawn as spawnChild } from 'child_process'
import { promises as fs } from 'fs'
import { snapshotArtifact } from './store/snapshot.js'
import { resolvePlanArtifactAbs } from './orchestrator/prompts.js'

const isDev = !app.isPackaged

function createWindow(): void {
  const win = new BrowserWindow({
    width: 1400,
    height: 900,
    minWidth: 1100,
    minHeight: 700,
    show: false,
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(__dirname, '../preload/preload.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  win.on('ready-to-show', () => win.show())

  win.webContents.setWindowOpenHandler((details) => {
    shell.openExternal(details.url)
    return { action: 'deny' }
  })

  if (isDev && process.env['ELECTRON_RENDERER_URL']) {
    win.loadURL(process.env['ELECTRON_RENDERER_URL'])
    win.webContents.openDevTools({ mode: 'detach' })
  } else {
    win.loadFile(join(__dirname, '../renderer/index.html'))
  }
}

app.whenReady().then(async () => {
  await ensureRootDir()
  initDb()

  if (process.env.MULTI_AI_CODE_PTY_DUMP === '1') {
    console.log(
      `[pty-dump] enabled. Raw PTY chunks for every session will be written to ${join(
        rootDir(),
        'logs'
      )}\\pty-*.jsonl`
    )
  }

  ipcMain.handle('app:ping', () => 'pong')
  ipcMain.handle('app:version', () => app.getVersion())

  ipcMain.handle(
    'plan:list',
    async (_e, { projectDir }: { projectDir: string }) => {
      try {
        const items = await listPlans(projectDir)
        return { ok: true as const, items }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message, items: [] }
      }
    }
  )

  ipcMain.handle(
    'plan:registerExternal',
    async (
      _e,
      { projectDir, externalPath }: { projectDir: string; externalPath: string }
    ) => {
      try {
        return await registerExternalPlan(projectDir, externalPath)
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  // Migration: if an old hardcoded "demo" project dir exists but no DB row,
  // register it so users don't lose historical data.
  const legacyDemoDir = projectDirFn('demo')
  try {
    await fs.access(legacyDemoDir)
    if (!getProject('demo')) {
      createProject({ id: 'demo', name: 'Demo', target_repo: legacyDemoDir })
    }
  } catch {
    /* no legacy data */
  }

  async function readProjectMeta(pdir: string): Promise<{ target_repo?: string; name?: string }> {
    try {
      return JSON.parse(await fs.readFile(join(pdir, 'project.json'), 'utf8'))
    } catch {
      return {}
    }
  }

  ipcMain.handle('project:list', async () => {
    const rows = listProjects()
    const out = []
    for (const r of rows) {
      const pdir = projectDirFn(r.id)
      const meta = await readProjectMeta(pdir)
      out.push({
        id: r.id,
        name: meta.name || r.name,
        target_repo: meta.target_repo || r.target_repo,
        dir: pdir,
        created_at: r.created_at,
        updated_at: r.updated_at
      })
    }
    return out
  })

  ipcMain.handle(
    'project:create',
    async (_e, { name, target_repo }: { name: string; target_repo: string }) => {
      try {
        const st = await fs.stat(target_repo)
        if (!st.isDirectory()) return { ok: false, error: '选择的目标仓库不是目录' }
      } catch {
        return { ok: false, error: '目标仓库目录不存在' }
      }
      const id = `p_${Date.now().toString(36)}_${randomBytes(3).toString('hex')}`
      try {
        await createProjectLayout(id, target_repo)
        createProject({ id, name: name.trim() || id, target_repo })
        await fs.mkdir(artifactsDir(id), { recursive: true })
        const metaPath = join(projectDirFn(id), 'project.json')
        let meta: Record<string, unknown> = {}
        try {
          meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
        } catch {
          /* ignore */
        }
        meta.name = name.trim() || id
        await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
        return {
          ok: true,
          id,
          name: name.trim() || id,
          target_repo,
          dir: projectDirFn(id)
        }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle('project:delete', async (_e, { id }: { id: string }) => {
    try {
      killAllSessions()
      const row = getProject(id)
      const pdir = projectDirFn(id)
      // Move to .trash instead of hard rm so it can be undone
      const trashRoot = join(rootDir(), '.trash')
      await fs.mkdir(trashRoot, { recursive: true })
      const trashPath = join(trashRoot, `${id}_${Date.now()}`)
      try {
        await fs.rename(pdir, trashPath)
      } catch {
        /* dir may not exist */
      }
      dbDeleteProject(id)
      return {
        ok: true,
        trashPath,
        snapshot: row
          ? { id: row.id, name: row.name, target_repo: row.target_repo }
          : null
      }
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
  })

  ipcMain.handle(
    'project:undelete',
    async (
      _e,
      {
        trashPath,
        snapshot
      }: {
        trashPath: string
        snapshot: { id: string; name: string; target_repo: string } | null
      }
    ) => {
      if (!snapshot) return { ok: false, error: 'no snapshot to restore' }
      try {
        const pdir = projectDirFn(snapshot.id)
        await fs.rename(trashPath, pdir)
        if (!getProject(snapshot.id)) {
          createProject({ id: snapshot.id, name: snapshot.name, target_repo: snapshot.target_repo })
        }
        return { ok: true }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle('project:purge-trash', async (_e, { trashPath }: { trashPath: string }) => {
    try {
      await fs.rm(trashPath, { recursive: true, force: true })
      return { ok: true }
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
  })

  ipcMain.handle('project:rename', (_e, { id, name }: { id: string; name: string }) => {
    try {
      updateProjectName(id, name)
      return { ok: true }
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
  })

  ipcMain.handle('project:touch', (_e, { id }: { id: string }) => {
    touchProject(id)
    return { ok: true }
  })

  ipcMain.handle('project:get-stage-configs', async (_e, { id }: { id: string }) => {
    const meta = await readProjectMeta(projectDirFn(id))
    const m = meta as any
    return (m.stage_configs ?? {}) as Record<
      string,
      { command?: string; args?: string[]; env?: Record<string, string> }
    >
  })

  ipcMain.handle('project:get-msys-enabled', async (_e, { id }: { id: string }) => {
    const meta = await readProjectMeta(projectDirFn(id))
    return !!(meta as { msys_enabled?: boolean }).msys_enabled
  })

  ipcMain.handle(
    'project:set-msys-enabled',
    async (_e, { id, enabled }: { id: string; enabled: boolean }) => {
      const metaPath = join(projectDirFn(id), 'project.json')
      let meta: Record<string, unknown> = {}
      try {
        meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
      } catch {
        /* empty */
      }
      meta.msys_enabled = !!enabled
      meta.updated_at = new Date().toISOString()
      await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
      return { ok: true }
    }
  )

  ipcMain.handle('env:detect-msys', async () => detectMsys())

  // ---------- Git IPC: log + diff for Stage 3 Diff Review ----------

  ipcMain.handle(
    'git:log',
    async (_e, { cwd, limit }: { cwd: string; limit?: number }) => {
      try {
        const args = [
          'log',
          `--max-count=${Math.max(1, Math.min(limit ?? 50, 500))}`,
          '--pretty=format:%H%x00%h%x00%an%x00%ad%x00%s',
          '--date=iso'
        ]
        const { stdout } = await execFileAsync('git', args, {
          cwd,
          maxBuffer: 10 * 1024 * 1024
        })
        const entries = stdout
          .split('\n')
          .filter(Boolean)
          .map((line) => {
            const [hash, short, author, date, subject] = line.split('\x00')
            return { hash, short, author, date, subject }
          })
        return { ok: true, entries }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'git:diff',
    async (
      _e,
      {
        cwd,
        mode,
        refs
      }: {
        cwd: string
        mode: 'working' | 'head1' | 'commit' | 'range' | 'working_range'
        refs?: string[]
      }
    ) => {
      try {
        // Always emit full-file context so the user can see the entire file
        // with changed lines highlighted (not just 3-line windows). Git caps
        // -U at file length automatically — 99999 is effectively "whole file".
        const fullCtx = '-U99999'
        let args: string[]
        switch (mode) {
          case 'working':
            args = ['diff', fullCtx, 'HEAD']
            break
          case 'head1':
            args = ['diff', fullCtx, 'HEAD~1', 'HEAD']
            break
          case 'commit': {
            const ref = refs?.[0]
            if (!ref) return { ok: false, error: '缺少 commit 引用' }
            args = ['show', fullCtx, '--format=', ref]
            break
          }
          case 'range': {
            const [from, to] = refs ?? []
            if (!from || !to)
              return { ok: false, error: '缺少 commit 范围（需要两个引用）' }
            args = ['diff', fullCtx, `${from}..${to}`]
            break
          }
          case 'working_range': {
            // diff working tree vs a past commit ("from") — shows all
            // uncommitted changes AND everything committed since <from>.
            const from = refs?.[0]
            if (!from) return { ok: false, error: '缺少起始 commit' }
            args = ['diff', fullCtx, from]
            break
          }
          default:
            return { ok: false, error: `invalid mode: ${mode}` }
        }
        const { stdout } = await execFileAsync('git', args, {
          cwd,
          maxBuffer: 200 * 1024 * 1024
        })
        return { ok: true, diff: stdout }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle('shell:open-msys-terminal', async (_e, { cwd }: { cwd: string }) => {
    const info = await detectMsys()
    const cmd = buildOpenMsysTerminalCommand(info, cwd)
    if (!cmd) return { ok: false, error: '未检测到 MSYS 环境（或非 Windows 平台）' }
    try {
      const child = spawnChild(cmd.command, cmd.args, {
        cwd,
        detached: true,
        stdio: 'ignore',
        windowsHide: false
      })
      child.unref()
      return { ok: true, variant: info.variant }
    } catch (err) {
      return { ok: false, error: (err as Error).message }
    }
  })

  ipcMain.handle(
    'project:set-stage-configs',
    async (
      _e,
      {
        id,
        configs
      }: {
        id: string
        configs: Record<
          string,
          { command?: string; args?: string[]; env?: Record<string, string> }
        >
      }
    ) => {
      const metaPath = join(projectDirFn(id), 'project.json')
      let meta: Record<string, unknown> = {}
      try {
        meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
      } catch {
        /* ignore */
      }
      meta.stage_configs = configs
      meta.updated_at = new Date().toISOString()
      await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
      return { ok: true }
    }
  )

  interface AiSettings {
    ai_cli: 'claude' | 'codex'
    command?: string
    args?: string[]
    env?: Record<string, string>
  }

  ipcMain.handle('project:get-ai-settings', async (_e, { id }: { id: string }) => {
    const pdir = projectDirFn(id)
    try {
      const raw = await fs.readFile(join(pdir, 'project.json'), 'utf8')
      const meta = JSON.parse(raw) as { ai_settings?: AiSettings }
      return meta.ai_settings ?? { ai_cli: 'claude' as const }
    } catch {
      return { ai_cli: 'claude' as const }
    }
  })

  ipcMain.handle(
    'project:set-ai-settings',
    async (
      _e,
      { id, settings }: { id: string; settings: AiSettings }
    ): Promise<{ ok: boolean; error?: string }> => {
      const pdir = projectDirFn(id)
      const metaPath = join(pdir, 'project.json')
      try {
        const raw = await fs.readFile(metaPath, 'utf8')
        const meta = JSON.parse(raw) as Record<string, unknown>
        meta.ai_settings = settings as unknown as Record<string, unknown>
        await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
        return { ok: true }
      } catch (err: unknown) {
        return {
          ok: false,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle('project:pick-dir', async () => {
    const res = await dialog.showOpenDialog({
      title: '选择项目仓库目录',
      properties: ['openDirectory', 'createDirectory']
    })
    if (res.canceled || res.filePaths.length === 0) return { canceled: true }
    return { canceled: false, path: res.filePaths[0] }
  })

  ipcMain.handle(
    'project:set-target-repo',
    async (_e, { id, path }: { id: string; path: string }) => {
      try {
        const st = await fs.stat(path)
        if (!st.isDirectory()) return { ok: false, error: '选择的路径不是目录' }
      } catch {
        return { ok: false, error: '目录不存在或无法访问' }
      }
      killAllSessions()
      const pdir = projectDirFn(id)
      const metaPath = join(pdir, 'project.json')
      let meta: Record<string, unknown> = {}
      try {
        meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
      } catch {
        /* ignore */
      }
      meta.target_repo = path
      meta.updated_at = new Date().toISOString()
      await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))
      touchProject(id)
      return { ok: true, target_repo: path, name: meta.name as string }
    }
  )

  // Save a clipboard-pasted image (incl. screenshots) to a temp file so the
  // CLI can reference it by path.
  ipcMain.handle(
    'clipboard:save-image',
    async (_e, { data, ext }: { data: Uint8Array | ArrayBuffer; ext: string }) => {
      try {
        const dir = join(tmpdir(), 'multi-ai-code', 'pasted')
        await fs.mkdir(dir, { recursive: true })
        const safeExt = /^[a-z0-9]{1,6}$/i.test(ext) ? ext.toLowerCase() : 'png'
        const ts = new Date().toISOString().replace(/[:.]/g, '-')
        const name = `paste-${ts}-${randomBytes(3).toString('hex')}.${safeExt}`
        const full = join(dir, name)
        const buf = Buffer.from(data as ArrayBuffer)
        await fs.writeFile(full, buf)
        return { ok: true, path: full }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  /** Write a text string to a temp file and return its absolute path. */
  ipcMain.handle(
    'file:write-temp',
    async (_e, { content, ext }: { content: string; ext?: string }) => {
      try {
        const dir = join(tmpdir(), 'multi-ai-code', 'temp')
        await fs.mkdir(dir, { recursive: true })
        const safeExt = /^[a-z0-9]{1,6}$/i.test(ext ?? '') ? ext!.toLowerCase() : 'md'
        const ts = new Date().toISOString().replace(/[:.]/g, '-')
        const name = `merge-${ts}-${randomBytes(3).toString('hex')}.${safeExt}`
        const full = join(dir, name)
        await fs.writeFile(full, content, 'utf8')
        return { ok: true, path: full }
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'file:save-as',
    async (_e, { defaultName, content }: { defaultName: string; content: string }) => {
      const res = await dialog.showSaveDialog({
        title: '导出为 Markdown',
        defaultPath: defaultName,
        filters: [
          { name: 'Markdown', extensions: ['md'] },
          { name: 'All Files', extensions: ['*'] }
        ]
      })
      if (res.canceled || !res.filePath) return { ok: false, canceled: true }
      await fs.writeFile(res.filePath, content, 'utf8')
      return { ok: true, path: res.filePath }
    }
  )

  // ---------- Global search ----------
  ipcMain.handle(
    'search:artifacts',
    async (_e, { projectId, query }: { projectId: string; query: string }) => {
      if (!query.trim()) return { ok: true, results: [] }
      const q = query.toLowerCase()
      const pdir = projectDirFn(projectId)
      const historyRoot = join(pdir, 'artifacts', 'history')
      const results: {
        path: string
        stageId: number
        line: number
        snippet: string
      }[] = []
      const MAX_DEPTH = 8
      const MAX_RESULTS = 200
      const DEADLINE_MS = 5000
      const deadline = Date.now() + DEADLINE_MS
      const { relative } = await import('path')
      let truncated: 'limit' | 'deadline' | null = null
      async function walk(dir: string, depth: number): Promise<void> {
        if (depth > MAX_DEPTH) return
        if (results.length >= MAX_RESULTS) { truncated = 'limit'; return }
        if (Date.now() > deadline) { truncated = 'deadline'; return }
        let entries: import('fs').Dirent[]
        try {
          entries = await fs.readdir(dir, { withFileTypes: true })
        } catch {
          return
        }
        for (const ent of entries) {
          if (results.length >= MAX_RESULTS) { truncated = 'limit'; return }
          if (Date.now() > deadline) { truncated = 'deadline'; return }
          // Skip symlinks to avoid loops
          if (ent.isSymbolicLink()) continue
          const p = join(dir, ent.name)
          if (ent.isDirectory()) {
            await walk(p, depth + 1)
          } else if (ent.isFile() && ent.name.endsWith('.md')) {
            try {
              const content = await fs.readFile(p, 'utf8')
              const lines = content.split(/\r?\n/)
              const stageMatch = p.match(/stage(\d+)/)
              const stageId = stageMatch ? Number(stageMatch[1]) : 0
              for (let i = 0; i < lines.length; i++) {
                if (lines[i].toLowerCase().includes(q)) {
                  results.push({
                    path: relative(pdir, p),
                    stageId,
                    line: i + 1,
                    snippet: lines[i].trim().slice(0, 200)
                  })
                  if (results.length >= MAX_RESULTS) { truncated = 'limit'; return }
                }
              }
            } catch {
              /* skip */
            }
          }
        }
      }
      await walk(historyRoot, 0)
      return { ok: true, results, truncated }
    }
  )

  // ---------- CLI Doctor ----------
  ipcMain.handle('doctor:check', async () => {
    const tools: {
      name: string
      required: boolean
      cmd: string
      args: string[]
      install: string
    }[] = [
      {
        name: 'node',
        required: true,
        cmd: 'node',
        args: ['--version'],
        install: '安装 Node.js 22 LTS: https://nodejs.org/'
      },
      {
        name: 'git',
        required: true,
        cmd: 'git',
        args: ['--version'],
        install: '安装 Git: https://git-scm.com/downloads'
      },
      {
        name: 'claude',
        required: true,
        cmd: 'claude',
        args: ['--version'],
        install:
          '安装 Claude Code CLI: `npm i -g @anthropic-ai/claude-code` （需 Node 18+）'
      },
      {
        name: 'codex',
        required: true,
        cmd: 'codex',
        args: ['--version'],
        install: '安装 OpenAI Codex CLI: `npm i -g @openai/codex`'
      }
    ]
    const results: {
      name: string
      required: boolean
      ok: boolean
      version?: string
      error?: string
      install: string
    }[] = []
    for (const t of tools) {
      try {
        const { stdout, stderr } = await execFileAsync(t.cmd, t.args, {
          shell: process.platform === 'win32',
          timeout: 5000
        })
        const version = (stdout || stderr || '').trim().split(/\r?\n/)[0]
        results.push({ name: t.name, required: t.required, ok: true, version, install: t.install })
      } catch (err: any) {
        // ETIMEDOUT or SIGTERM signal indicates the CLI hung past timeout
        const msg = err.killed
          ? `调用 ${t.cmd} 超过 5s 未响应（可能正等待交互式输入）`
          : (err.message || String(err)).toString().trim()
        results.push({
          name: t.name,
          required: t.required,
          ok: false,
          error: msg,
          install: t.install
        })
      }
    }

    // Optional: MSYS bash for running .sh scripts on Windows.
    if (process.platform === 'win32') {
      const msys = await detectMsys()
      results.push({
        name: 'msys',
        required: false,
        ok: msys.available,
        version: msys.available ? `${msys.variant} · ${msys.bashPath}` : undefined,
        error: msys.available ? undefined : '未检测到 MSYS bash',
        install:
          '安装 MSYS2 (https://www.msys2.org/) 或 Git for Windows (https://git-scm.com/downloads)，然后在项目配置里启用 MSYS。'
      })
    }
    return results
  })

  // ---------- Git integration ----------
  async function runGit(cwd: string, args: string[]): Promise<{ ok: true; stdout: string } | { ok: false; error: string }> {
    try {
      const { stdout } = await execFileAsync('git', args, {
        cwd,
        maxBuffer: 16 * 1024 * 1024,
        timeout: 30000
      })
      return { ok: true, stdout }
    } catch (err: any) {
      const msg = err.killed
        ? `git ${args[0] ?? ''} 执行超过 30s 未完成`
        : (err.stderr || err.message || String(err)).toString().trim()
      return { ok: false, error: msg }
    }
  }

  ipcMain.handle('git:status', async (_e, { cwd }: { cwd: string }) => {
    const res = await runGit(cwd, ['status', '--porcelain=v1', '-b'])
    if (!res.ok) return res
    const lines = res.stdout.split(/\r?\n/).filter(Boolean)
    let branch = ''
    const files: { status: string; path: string }[] = []
    for (const ln of lines) {
      if (ln.startsWith('##')) {
        const m = ln.match(/^##\s+([^.]+?)(?:\.\.\.|$)/)
        branch = m?.[1] ?? ''
      } else {
        const status = ln.slice(0, 2).trim()
        const path = ln.slice(3)
        files.push({ status, path })
      }
    }
    return { ok: true, branch, files }
  })

  ipcMain.handle(
    'git:commit',
    async (_e, { cwd, message }: { cwd: string; message: string }) => {
      if (!message.trim()) return { ok: false, error: 'commit message is empty' }
      const add = await runGit(cwd, ['add', '-A'])
      if (!add.ok) return add
      const commit = await runGit(cwd, ['commit', '-m', message])
      if (!commit.ok) return commit
      return { ok: true, output: commit.stdout }
    }
  )

  ipcMain.handle(
    'git:checkout-branch',
    async (_e, { cwd, name }: { cwd: string; name: string }) => {
      const r1 = await runGit(cwd, ['checkout', name])
      if (r1.ok) return { ok: true, created: false }
      const r2 = await runGit(cwd, ['checkout', '-b', name])
      if (!r2.ok) return r2
      return { ok: true, created: true }
    }
  )

  // ---------- Artifact handlers (single-stage) ----------

  ipcMain.handle('event:list', (_e, { projectId, limit }: { projectId: string; limit?: number }) => {
    return listEvents(projectId, limit ?? 500)
  })

  /** List snapshotted artifacts for a project, optionally filtered by stage. */
  ipcMain.handle(
    'artifact:list',
    (_e, { projectId, stageId }: { projectId: string; stageId?: number }) => {
      return listArtifacts(projectId, stageId)
    }
  )

  /** Shared helper: materialize a content blob as the plan's design artifact,
   *  optionally firing a synthesized stage:done. */
  async function materializeArtifact(req: {
    projectId: string
    projectDir: string
    content: string
    sessionId?: string
    kind: string
    summary: string
    /** When true, skip the stage:done broadcast (e.g. seeding for further refinement). */
    skipBroadcast?: boolean
    /** Human-readable label for the snapshot filename (e.g. plan name). */
    label?: string
    /** Override target path (absolute). When provided, content is written there
     *  instead of the planArtifactPath default. */
    externalArtifactPath?: string | null
  }): Promise<{ ok: true; artifactPath: string; artifactAbs: string; snapshotPath: string | null } | { ok: false; error: string }> {
    let artifactAbs: string
    let artifactPathForEvent: string
    if (req.externalArtifactPath && isAbsolute(req.externalArtifactPath)) {
      artifactAbs = req.externalArtifactPath
      artifactPathForEvent = artifactAbs
    } else {
      try {
        artifactAbs = await resolvePlanArtifactAbs(req.projectDir, req.label)
      } catch (err) {
        return { ok: false, error: (err as Error).message }
      }
      artifactPathForEvent = artifactAbs
    }
    await fs.mkdir(dirname(artifactAbs), { recursive: true })
    await fs.writeFile(artifactAbs, req.content, 'utf8')
    const snapshotPath = await snapshotArtifact({
      projectId: req.projectId,
      projectDir: req.projectDir,
      stageId: 1,
      content: req.content,
      kind: req.kind,
      label: req.label
    })
    if (!req.skipBroadcast) {
      for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) {
          win.webContents.send('stage:done', {
            sessionId: req.sessionId ?? `${req.projectId}:stage1`,
            projectId: req.projectId,
            stageId: 1,
            raw: `<<${req.kind}>>`,
            params: { summary: req.summary },
            artifactPath: artifactPathForEvent,
            artifactContent: req.content,
            snapshotPath
          })
        }
      }
    }
    recordEvent({
      project_id: req.projectId,
      from_stage: 1,
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
   * Restore a historical snapshot into the plan's design artifact path, then
   * fire a synthesized `stage:done` so the completion drawer pops.
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
          : join(projectDirFn(req.projectId), req.snapshotPath)
        const content = await fs.readFile(snapAbs, 'utf8')
        return materializeArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
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
   * Seed the plan's design artifact with either a historical snapshot or an
   * external file, WITHOUT firing the completion drawer.
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
            : join(projectDirFn(req.projectId), req.snapshotPath)
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

  /** Import an external file as the plan's design artifact. Opens a native
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

  /** Read the CURRENT working artifact for the plan's design.
   *  Returns absolute path + content so the renderer can preview the plan. */
  ipcMain.handle(
    'artifact:read-current',
    async (
      _e,
      {
        projectDir,
        stageId: _stageId,
        label
      }: { projectDir: string; stageId: number; label?: string }
    ) => {
      // Check for external plan first
      let abs: string | null = null
      if (label?.trim()) {
        const sources = await listPlans(projectDir)
        const ext = sources.find((p) => p.source === 'external' && p.name === label.trim())
        if (ext && isAbsolute(ext.abs)) abs = ext.abs
      }
      if (!abs) {
        try {
          abs = await resolvePlanArtifactAbs(projectDir, label)
        } catch (err) {
          return { ok: false, path: null, relPath: null, error: (err as Error).message }
        }
      }
      try {
        const content = await fs.readFile(abs, 'utf8')
        return { ok: true, path: abs, relPath: abs, content }
      } catch (err) {
        return {
          ok: false,
          path: abs,
          relPath: abs,
          error: (err as Error).message
        }
      }
    }
  )

  /** Open a text file and return its content WITHOUT materializing an artifact.
   *  Used by the plan preview-before-commit flow. */
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

  /** Commit caller-provided content as the plan artifact (post user preview). */
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
      // Import from file flow: archive back to the original file (not default location)
      // and remember the mapping so subsequent spawns keep writing to the same source path.
      const useExternal = !!req.sourcePath && isAbsolute(req.sourcePath)
      if (useExternal && req.label) {
        try {
          await registerExternalPlan(req.projectDir, req.sourcePath)
        } catch {
          /* best-effort; mapping will simply not persist across restarts */
        }
      }
      return materializeArtifact({
        projectId: req.projectId,
        projectDir: req.projectDir,
        content: req.content,
        sessionId: req.sessionId,
        kind: 'imported',
        summary: `导入文件: ${req.sourcePath}`,
        label: req.label,
        externalArtifactPath: useExternal ? req.sourcePath : null
      })
    }
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
      let artifactAbs: string | null = null
      let artifactRel: string | null = null
      if (req.artifactPath) {
        artifactRel = req.artifactPath
        artifactAbs = isAbsolute(req.artifactPath)
          ? req.artifactPath
          : join(req.projectDir, req.artifactPath)
      } else {
        try {
          artifactAbs = await resolvePlanArtifactAbs(req.projectDir, undefined)
          artifactRel = artifactAbs
        } catch {
          artifactAbs = null
          artifactRel = null
        }
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
      if (artifactContent) {
        snapshotPath = await snapshotArtifact({
          projectId: req.projectId,
          projectDir: req.projectDir,
          stageId: req.stageId,
          content: artifactContent,
          kind: 'manual'
        })
      }
      for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) {
          win.webContents.send('stage:done', {
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
        }
      }
      return { ok: true, artifactFound: !!artifactContent, snapshotPath }
    }
  )

  registerPtyIpc()

  createWindow()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

app.on('window-all-closed', () => {
  killAllSessions()
  closeDb()
  if (process.platform !== 'darwin') app.quit()
})

app.on('before-quit', () => {
  killAllSessions()
})
