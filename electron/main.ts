import { app, BrowserWindow, ipcMain, shell, dialog, globalShortcut, Menu } from 'electron'
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
  recordEvent
} from './store/db.js'
import {
  ensureRootDir,
  createProjectLayout,
  projectDir as projectDirFn,
  artifactsDir,
  rootDir,
  setActiveAccount
} from './store/paths.js'
import { acquireInstanceLock, releaseInstanceLock } from './store/instanceLock.js'
import { registerPtyIpc, killAllSessions } from './cc/ptyManager.js'
import { registerScheduledTaskIpc } from './scheduledTasks/ipc.js'
import { registerRemoteImIpc, activateRemoteImDataLayer } from './remote-im/ipc.js'
import {
  startScheduledTaskScheduler,
  stopScheduledTaskScheduler
} from './scheduledTasks/scheduler.js'
import { registerScreenshotIpc } from './screenshot/manager.js'
import {
  applyScreenshotHotkeySettings,
  disposeScreenshotHotkey,
  initializeScreenshotHotkey
} from './screenshot/hotkeyService.js'
import {
  DEFAULT_SCREENSHOT_HOTKEY_SETTINGS,
  loadScreenshotHotkeySettings,
  type ScreenshotHotkeySettings
} from './screenshot/hotkeySettings.js'
import {
  DEFAULT_UI_PREFERENCES,
  loadUiPreferences,
  saveUiPreferences,
  type UiPreferences
} from './store/uiPreferences.js'
import type { AppSettings } from './settings/types.js'
import {
  createInternalPlan,
  listPlans,
  registerExternalPlan,
  removeExternalPlan,
  updatePlanDescription,
  updatePlanMetadata
} from './orchestrator/plans.js'
import { detectMsys, buildOpenMsysTerminalCommand } from './util/msys.js'
import { normalizePathForCompare } from './util/pathCompare.js'
import { spawn as spawnChild } from 'child_process'
import { promises as fs } from 'fs'
import { snapshotArtifact } from './store/snapshot.js'
import { resolvePlanArtifactAbs } from './orchestrator/prompts.js'
import { buildRepoViewSearch } from './repo-view/windowMode.js'
import { listRepoTree, readRepoTextFile } from './repo-view/filesystem.js'
import {
  applyRepoMemoryUpdate,
  readRepoConversationHistory,
  readRepoFileNote,
  readRepoMemory,
  writeRepoConversationHistory
} from './repo-view/memory.js'
import { readProjectMetaFile, writeProjectMetaFile } from './store/projectMeta.js'
import {
  getProjectBuildConfig,
  setProjectBuildConfig,
  type ProjectBuildConfig
} from './build/config.js'
import {
  getProjectRuntimeConfig,
  setProjectRuntimeConfig,
  type ProjectRuntimeConfig
} from './runtime/config.js'
import { resolveBuildExecutionScope } from './build/executionScope.js'
import { createBuildRunner } from './build/runner.js'
import { getFailureAnalysisPrompt as getBuildFailureAnalysisPrompt } from './build/analysisPrompt.js'
import { createRuntimeRunner } from './runtime/runner.js'
import { getRuntimeAnalysisPrompt } from './runtime/analysisPrompt.js'
import {
  buildRuntimeAnalysisPromptFileMessage,
  writeRuntimeAnalysisPromptFile
} from './runtime/analysisPromptFile.js'
import { listVisualStudioInstallations } from './build/visualStudio.js'
import {
  hasRepoAnalysisSession,
  resizeRepoAnalysisSession,
  sendRepoAnalysisPrompt,
  startRepoAnalysisSession,
  stopRepoAnalysisSession,
  writeRepoAnalysisInput,
  pasteRepoAnalysisInput
} from './repo-view/repoAnalysisManager.js'
import { ensureAnalysisCacheDir } from './repo-view/analysisCache.js'
import { getRemoteImRuntimeProfileId, resolveRemoteImUserDataPath } from './remote-im/profile.js'

const isDev = !app.isPackaged
const repoViewWindows = new Map<string, BrowserWindow>()
const appIconPath = join(__dirname, '../../build/icon-256.png')
const remoteImRuntimeProfileId = getRemoteImRuntimeProfileId()

app.setPath(
  'userData',
  resolveRemoteImUserDataPath(app.getPath('userData'), remoteImRuntimeProfileId)
)


// AppSettings 由两个独立持久化的部分组合而成：截图快捷键（hotkeySettings）
// + 通用界面偏好（uiPreferences）。分别缓存，get/set 时组合。
let effectiveScreenshotSettings: ScreenshotHotkeySettings | null = null
let effectiveUiPreferences: UiPreferences | null = null
const buildRunner = createBuildRunner()
const runtimeRunner = createRuntimeRunner()

function broadcastToAllWindows(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) {
      win.webContents.send(channel, payload)
    }
  }
}

buildRunner.onData((event) => {
  broadcastToAllWindows('build:data', event)
})

buildRunner.onStatus((nextState) => {
  broadcastToAllWindows('build:status', nextState)
})

runtimeRunner.onData((event) => {
  broadcastToAllWindows('runtime:data', event)
})

runtimeRunner.onStatus((nextState) => {
  broadcastToAllWindows('runtime:status', nextState)
})

function composeAppSettings(): AppSettings {
  const screenshot = effectiveScreenshotSettings ?? DEFAULT_SCREENSHOT_HOTKEY_SETTINGS
  const ui = effectiveUiPreferences ?? DEFAULT_UI_PREFERENCES
  return {
    screenshotShortcutEnabled: screenshot.enabled,
    screenshotShortcut: screenshot.shortcut,
    showDevToolbarButtons: ui.showDevToolbarButtons
  }
}

function fromRendererScreenshot(settings: AppSettings): ScreenshotHotkeySettings {
  return {
    enabled: settings.screenshotShortcutEnabled,
    shortcut: settings.screenshotShortcut
  }
}

let mainWindow: BrowserWindow | null = null

function launchMacAppInstance(): void {
  if (process.platform !== 'darwin') return
  try {
    const child = spawnChild(process.execPath, app.isPackaged ? [] : [app.getAppPath()], {
      detached: true,
      stdio: 'ignore'
    })
    child.unref()
  } catch (error) {
    dialog.showErrorBox(
      '新建应用实例失败',
      error instanceof Error ? error.message : String(error)
    )
  }
}

function createWindow(): void {
  const win = new BrowserWindow({
    // 登录阶段是一个小窗口，只装得下登录表单（钉钉/微信式）。登录成功后由
    // activateAccountDataLayer() 放大成主界面窗口，避免登录内容悬在大窗口里。
    width: 640,
    height: 720,
    resizable: false,
    maximizable: false,
    fullscreenable: false,
    show: false,
    autoHideMenuBar: true,
    icon: appIconPath,
    webPreferences: {
      preload: join(__dirname, '../preload/preload.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  mainWindow = win
  win.on('ready-to-show', () => {
    win.center()
    win.show()
  })
  win.on('closed', () => {
    if (mainWindow === win) mainWindow = null
  })

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

function createRepoViewWindow(projectId: string, title: string): BrowserWindow {
  const existing = repoViewWindows.get(projectId)
  if (existing && !existing.isDestroyed()) {
    if (existing.isMinimized()) existing.restore()
    existing.focus()
    return existing
  }

  const win = new BrowserWindow({
    width: 1600,
    height: 960,
    minWidth: 1100,
    minHeight: 720,
    show: false,
    autoHideMenuBar: true,
    title: `仓库查看 · ${title}`,
    icon: appIconPath,
    webPreferences: {
      preload: join(__dirname, '../preload/preload.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })

  repoViewWindows.set(projectId, win)
  win.on('ready-to-show', () => win.show())
  win.on('closed', () => {
    stopRepoAnalysisSession(win.id)
    repoViewWindows.delete(projectId)
  })

  win.webContents.setWindowOpenHandler((details) => {
    shell.openExternal(details.url)
    return { action: 'deny' }
  })

  const search = buildRepoViewSearch(projectId)
  if (isDev && process.env['ELECTRON_RENDERER_URL']) {
    const base = process.env['ELECTRON_RENDERER_URL']
    const root = base.endsWith('/') ? base : `${base}/`
    win.loadURL(`${root}${search}`)
  } else {
    win.loadFile(join(__dirname, '../renderer/index.html'), { search })
  }
  return win
}

// On Windows, taskbar groups windows by AppUserModelID. If we don't set this
// explicitly, Electron's default id wins and the taskbar keeps showing the
// stock Electron icon / groups our windows under "Electron" — even after we
// set BrowserWindow.icon. Must be set before the first window is created.
// Safe to call as no-op on non-Windows, but we gate it for clarity.
if (process.platform === 'win32') {
  app.setAppUserModelId('com.multiaicode.app')
}

app.whenReady().then(async () => {
  if (process.platform === 'darwin' && app.dock) {
    try {
      app.dock.setIcon(appIconPath)
      app.dock.setMenu(
        Menu.buildFromTemplate([
          {
            label: '新建应用实例',
            click: launchMacAppInstance
          }
        ])
      )
    } catch {
      /* ignore — icon file missing in some dev configurations */
    }
  }

  // 数据层（DB / rootDir / 后台服务）不在启动即初始化——rootDir 现在按账号作用域，
  // 账号要在登录页选定后才知道。这里只注册纯 IPC handler 并显示登录门；真正的数据层
  // 初始化在账号绑定成功后由 activateAccountDataLayer() 触发。
  ipcMain.handle('app:ping', () => 'pong')
  ipcMain.handle('app:version', () => app.getVersion())
  if (process.platform === 'darwin') {
    ipcMain.on('app:launch-new-instance', launchMacAppInstance)
  }
  ipcMain.handle('settings:get-app-settings', async () => {
    if (!effectiveScreenshotSettings) effectiveScreenshotSettings = await loadScreenshotHotkeySettings()
    if (!effectiveUiPreferences) effectiveUiPreferences = await loadUiPreferences()
    return composeAppSettings()
  })
  ipcMain.handle(
    'settings:set-app-settings',
    async (
      _e,
      settings: AppSettings
    ): Promise<{ ok: boolean; value?: AppSettings; error?: string }> => {
      // 界面偏好独立持久化（不受截图快捷键校验影响）。
      effectiveUiPreferences = await saveUiPreferences({
        showDevToolbarButtons: settings.showDevToolbarButtons
      })
      const result = await applyScreenshotHotkeySettings(
        fromRendererScreenshot(settings),
        { registrar: globalShortcut }
      )
      effectiveScreenshotSettings = result.settings
      if (!result.ok) {
        return {
          ok: false,
          value: composeAppSettings(),
          error: result.error
        }
      }
      return {
        ok: true,
        value: composeAppSettings()
      }
    }
  )

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

  ipcMain.handle(
    'plan:createInternal',
    async (_e, { projectDir, name }: { projectDir: string; name: string }) => {
      try {
        return await createInternalPlan(projectDir, name)
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'plan:updateDescription',
    async (
      _e,
      {
        projectDir,
        name,
        description,
        details
      }: { projectDir: string; name: string; description: string; details?: string }
    ) => {
      try {
        return await updatePlanDescription(projectDir, name, description, details)
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'plan:updateMetadata',
    async (
      _e,
      {
        projectDir,
        name,
        description,
        details
      }: { projectDir: string; name: string; description: string; details: string }
    ) => {
      try {
        return await updatePlanMetadata(projectDir, name, { description, details })
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'plan:removeExternal',
    async (
      _e,
      { projectDir, name }: { projectDir: string; name: string }
    ) => {
      try {
        return await removeExternalPlan(projectDir, name)
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )


  async function readProjectMeta(pdir: string): Promise<{ target_repo?: string; name?: string }> {
    try {
      return JSON.parse(await fs.readFile(join(pdir, 'project.json'), 'utf8'))
    } catch {
      return {}
    }
  }

  ipcMain.handle(
    'repo-view:open-window',
    async (_e, { projectId }: { projectId: string }) => {
      const row = getProject(projectId)
      if (!row) return { ok: false, error: 'project not found' }
      const pdir = projectDirFn(projectId)
      const meta = await readProjectMeta(pdir)
      createRepoViewWindow(projectId, meta.name || row.name || projectId)
      return { ok: true }
    }
  )

  ipcMain.handle(
    'repo-view:list-tree',
    async (_e, { root, dir }: { root: string; dir?: string }) => {
      try {
        return { ok: true as const, entries: await listRepoTree(root, dir ?? '') }
      } catch (err) {
        return {
          ok: false as const,
          error: (err as Error).message,
          entries: [] as Awaited<ReturnType<typeof listRepoTree>>
        }
      }
    }
  )

  ipcMain.handle(
    'repo-view:read-file',
    async (_e, { root, path }: { root: string; path: string }) => {
      try {
        return { ok: true as const, ...(await readRepoTextFile(root, path)) }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:memory-load',
    async (_e, { root }: { root: string }) => {
      try {
        return { ok: true as const, ...(await readRepoMemory(root)) }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:memory-file-note',
    async (_e, { root, path }: { root: string; path: string }) => {
      try {
        return { ok: true as const, fileNote: await readRepoFileNote(root, path) }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:memory-apply',
    async (
      _e,
      {
        root,
        path,
        memoryUpdate
      }: {
        root: string
        path: string
        memoryUpdate: string
      }
    ) => {
      try {
        return {
          ok: true as const,
          ...(await applyRepoMemoryUpdate({
            root,
            filePath: path,
            memoryUpdate
          }))
        }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:history-load',
    async (_e, { root }: { root: string }) => {
      try {
        return {
          ok: true as const,
          messages: await readRepoConversationHistory(root)
        }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:history-save',
    async (
      _e,
      {
        root,
        messages
      }: {
        root: string
        messages: Array<{ id: string; role: 'user' | 'assistant'; text: string }>
      }
    ) => {
      try {
        return {
          ok: true as const,
          messages: await writeRepoConversationHistory(root, messages)
        }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:analysis-start',
    async (
      e,
      req: {
        projectId: string
        targetRepo: string
        command: string
        args: string[]
        env?: Record<string, string>
        opencode?: {
          providerId?: string
          name?: string
          baseURL?: string
          apiKey?: string
          mainModel?: string
          smallModel?: string
          timeoutMs?: number
          chunkTimeoutMs?: number
        }
      }
    ) => {
      const win = BrowserWindow.fromWebContents(e.sender)
      if (!win) return { ok: false as const, error: 'window not found' }
      try {
        await startRepoAnalysisSession({ winId: win.id, ...req })
        return { ok: true as const }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle(
    'repo-view:analysis-send',
    async (e, req: { repoRoot: string; text: string }) => {
      const win = BrowserWindow.fromWebContents(e.sender)
      if (!win) return { ok: false as const, error: 'window not found' }
      try {
        await ensureAnalysisCacheDir(req.repoRoot)
      } catch (err) {
        console.warn('[repo-view] ensureAnalysisCacheDir failed:', err)
      }
      try {
        await sendRepoAnalysisPrompt({ winId: win.id, text: req.text })
        return { ok: true as const }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  ipcMain.handle('repo-view:analysis-stop', (e) => {
    const win = BrowserWindow.fromWebContents(e.sender)
    if (!win) return { ok: false as const, error: 'window not found' }
    stopRepoAnalysisSession(win.id)
    return { ok: true as const }
  })

  ipcMain.handle('repo-view:analysis-has', (e) => {
    const win = BrowserWindow.fromWebContents(e.sender)
    if (!win) return { ok: false as const, error: 'window not found' }
    return { ok: true as const, running: hasRepoAnalysisSession(win.id) }
  })

  ipcMain.on(
    'repo-view:analysis-input',
    (e, payload: { data: string }) => {
      const win = BrowserWindow.fromWebContents(e.sender)
      if (!win) return
      writeRepoAnalysisInput(win.id, payload.data)
    }
  )

  ipcMain.handle(
    'repo-view:analysis-paste',
    async (e, payload: { data: string }) => {
      const win = BrowserWindow.fromWebContents(e.sender)
      if (!win) return { ok: false as const, error: 'window not found' }
      try {
        await pasteRepoAnalysisInput(win.id, payload.data)
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
    'repo-view:analysis-resize',
    (e, payload: { cols: number; rows: number }) => {
      const win = BrowserWindow.fromWebContents(e.sender)
      if (!win) return
      resizeRepoAnalysisSession(win.id, payload.cols, payload.rows)
    }
  )

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
      const targetRepoKey = normalizePathForCompare(target_repo)
      const existing = listProjects().find(
        (project) => normalizePathForCompare(project.target_repo) === targetRepoKey
      )
      if (existing) {
        return {
          ok: true,
          id: existing.id,
          name: existing.name,
          target_repo: existing.target_repo,
          dir: projectDirFn(existing.id)
        }
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
        mode: 'working' | 'head1' | 'commit' | 'range'
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

  ipcMain.handle('project:get-build-config', async (_e, { id }: { id: string }) => {
    const metaPath = join(projectDirFn(id), 'project.json')
    return await getProjectBuildConfig(metaPath)
  })

  ipcMain.handle('project:get-runtime-config', async (_e, { id }: { id: string }) => {
    const metaPath = join(projectDirFn(id), 'project.json')
    return await getProjectRuntimeConfig(metaPath)
  })

  ipcMain.handle('project:list-visual-studio-installations', async () => {
    return await listVisualStudioInstallations()
  })

  ipcMain.handle(
    'project:set-build-config',
    async (
      _e,
      { id, config }: { id: string; config: ProjectBuildConfig }
    ): Promise<{
      ok: boolean
      repaired?: boolean
      error?: string
      details?: Array<{ path: string; message: string }>
    }> => {
      const metaPath = join(projectDirFn(id), 'project.json')
      return await setProjectBuildConfig(metaPath, config)
    }
  )

  ipcMain.handle(
    'project:set-runtime-config',
    async (
      _e,
      { id, config }: { id: string; config: ProjectRuntimeConfig }
    ): Promise<{
      ok: boolean
      repaired?: boolean
      error?: string
      details?: Array<{ path: string; message: string }>
    }> => {
      const metaPath = join(projectDirFn(id), 'project.json')
      return await setProjectRuntimeConfig(metaPath, config)
    }
  )

  ipcMain.handle(
    'build:start',
    async (
      _e,
      {
        id,
        scope,
        stepId
      }: {
        id: string
        scope?: 'all' | 'single-step'
        stepId?: string | null
      }
    ) => {
      const row = getProject(id)
      if (!row) {
        return { ok: false as const, error: 'project not found', state: buildRunner.getState() }
      }

      const metaPath = join(projectDirFn(id), 'project.json')
      const metaResult = await readProjectMetaFile(metaPath)
      if (!metaResult.ok) {
        return { ok: false as const, error: metaResult.error, state: buildRunner.getState() }
      }

      const configResult = await getProjectBuildConfig(metaPath)
      if (!configResult.ok) {
        return { ok: false as const, error: configResult.error, state: buildRunner.getState() }
      }

      const metaName =
        typeof metaResult.meta.name === 'string' && metaResult.meta.name.trim()
          ? metaResult.meta.name.trim()
          : row.name
      const targetRepo =
        typeof metaResult.meta.target_repo === 'string' && metaResult.meta.target_repo.trim()
          ? metaResult.meta.target_repo.trim()
          : row.target_repo

      if (!targetRepo) {
        return {
          ok: false as const,
          error: 'project target_repo is not configured',
          state: buildRunner.getState()
        }
      }

      try {
        const stat = await fs.stat(targetRepo)
        if (!stat.isDirectory()) {
          return {
            ok: false as const,
            error: 'project target_repo is not a directory',
            state: buildRunner.getState()
          }
        }
      } catch (error: unknown) {
        return {
          ok: false as const,
          error: error instanceof Error ? error.message : String(error),
          state: buildRunner.getState()
        }
      }

      const selection = resolveBuildExecutionScope(configResult.value, { scope, stepId })
      if (!selection.ok) {
        return { ok: false as const, error: selection.error, state: buildRunner.getState() }
      }

      return await buildRunner.start({
        projectId: id,
        projectName: metaName || id,
        targetRepo,
        config: configResult.value,
        scope: selection.scope,
        stepId: selection.requestedStepId
      })
    }
  )

  ipcMain.handle('build:stop', () => buildRunner.stop())

  ipcMain.handle('build:get-state', () => buildRunner.getState())

  ipcMain.handle('build:get-failure-analysis-prompt', () => {
    return getBuildFailureAnalysisPrompt(buildRunner.getState())
  })

  ipcMain.handle('runtime:start', async (_e, { id }: { id: string }) => {
    const row = getProject(id)
    if (!row) {
      return { ok: false as const, error: 'project not found', state: runtimeRunner.getState() }
    }

    const metaPath = join(projectDirFn(id), 'project.json')
    const metaResult = await readProjectMetaFile(metaPath)
    if (!metaResult.ok) {
      return { ok: false as const, error: metaResult.error, state: runtimeRunner.getState() }
    }

    const configResult = await getProjectRuntimeConfig(metaPath)
    if (!configResult.ok) {
      return { ok: false as const, error: configResult.error, state: runtimeRunner.getState() }
    }

    const metaName =
      typeof metaResult.meta.name === 'string' && metaResult.meta.name.trim()
        ? metaResult.meta.name.trim()
        : row.name
    const targetRepo =
      typeof metaResult.meta.target_repo === 'string' && metaResult.meta.target_repo.trim()
        ? metaResult.meta.target_repo.trim()
        : row.target_repo

    if (!targetRepo) {
      return {
        ok: false as const,
        error: 'project target_repo is not configured',
        state: runtimeRunner.getState()
      }
    }

    try {
      const stat = await fs.stat(targetRepo)
      if (!stat.isDirectory()) {
        return {
          ok: false as const,
          error: 'project target_repo is not a directory',
          state: runtimeRunner.getState()
        }
      }
    } catch (error: unknown) {
      return {
        ok: false as const,
        error: error instanceof Error ? error.message : String(error),
        state: runtimeRunner.getState()
      }
    }

    return await runtimeRunner.start({
      projectId: id,
      projectName: metaName || id,
      targetRepo,
      config: configResult.value
    })
  })

  ipcMain.handle('runtime:stop', () => runtimeRunner.stop())

  ipcMain.handle('runtime:get-state', () => runtimeRunner.getState())

  ipcMain.handle('runtime:get-analysis-prompt', () => {
    return getRuntimeAnalysisPrompt(runtimeRunner.getState())
  })

  ipcMain.handle('runtime:get-analysis-prompt-file', async () => {
    const promptResult = getRuntimeAnalysisPrompt(runtimeRunner.getState())
    if (!promptResult.ok) return promptResult

    try {
      const filePath = await writeRuntimeAnalysisPromptFile(promptResult.prompt)
      return {
        ok: true as const,
        filePath,
        message: buildRuntimeAnalysisPromptFileMessage(filePath)
      }
    } catch (error: unknown) {
      return {
        ok: false as const,
        error: error instanceof Error ? error.message : String(error)
      }
    }
  })

  interface AiSettings {
    ai_cli: 'claude' | 'codex' | 'opencode'
    command?: string
    args?: string[]
    env?: Record<string, string>
    opencode?: {
      providerId?: string
      name?: string
      baseURL?: string
      apiKey?: string
      mainModel?: string
      smallModel?: string
      timeoutMs?: number
      chunkTimeoutMs?: number
    }
  }

  async function readProjectSettingsField(
    id: string,
    field: 'ai_settings' | 'repo_view_ai_settings'
  ): Promise<{ ok: true; value: AiSettings; repaired?: true } | { ok: false; error: string }> {
    const metaPath = join(projectDirFn(id), 'project.json')
    try {
      const readResult = await readProjectMetaFile(metaPath)
      if (!readResult.ok) {
        return { ok: false, error: '项目设置文件已损坏，无法自动修复' }
      }
      const value = readResult.meta[field]
      if (!value || typeof value !== 'object') {
        return readResult.repaired
          ? { ok: true, value: { ai_cli: 'codex' as const }, repaired: true }
          : { ok: true, value: { ai_cli: 'codex' as const } }
      }
      return readResult.repaired
        ? { ok: true, value: value as AiSettings, repaired: true }
        : { ok: true, value: value as AiSettings }
    } catch (err: unknown) {
      return {
        ok: false,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  }

  ipcMain.handle('project:get-ai-settings', async (_e, { id }: { id: string }) => {
    return await readProjectSettingsField(id, 'ai_settings')
  })

  async function updateProjectSettingsField(
    id: string,
    field: 'ai_settings' | 'repo_view_ai_settings',
    settings: AiSettings
  ): Promise<{ ok: true; repaired?: true } | { ok: false; error: string }> {
    try {
      const metaPath = join(projectDirFn(id), 'project.json')
      const readResult = await readProjectMetaFile(metaPath)
      if (!readResult.ok) {
        return { ok: false, error: '项目设置文件已损坏，无法自动修复' }
      }

      await writeProjectMetaFile(metaPath, {
        ...readResult.meta,
        [field]: settings as unknown as Record<string, unknown>
      })
      return readResult.repaired ? { ok: true, repaired: true } : { ok: true }
    } catch (err: unknown) {
      return {
        ok: false,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  }

  ipcMain.handle(
    'project:set-ai-settings',
    async (
      _e,
      { id, settings }: { id: string; settings: AiSettings }
    ): Promise<{ ok: boolean; repaired?: boolean; error?: string }> => {
      return await updateProjectSettingsField(id, 'ai_settings', settings)
    }
  )

  ipcMain.handle(
    'project:get-repo-view-ai-settings',
    async (_e, { id }: { id: string }) => {
      return await readProjectSettingsField(id, 'repo_view_ai_settings')
    }
  )

  ipcMain.handle(
    'project:set-repo-view-ai-settings',
    async (
      _e,
      { id, settings }: { id: string; settings: AiSettings }
    ): Promise<{ ok: boolean; repaired?: boolean; error?: string }> => {
      return await updateProjectSettingsField(id, 'repo_view_ai_settings', settings)
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

  // ---------- fs:read-utf8 — renderer reads arbitrary text files ----------
  ipcMain.handle('fs:read-utf8', async (_e, { path }: { path: string }) => {
    try {
      const content = await fs.readFile(path, 'utf8')
      return { ok: true as const, content }
    } catch (err: unknown) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })

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

  // 纯注册（不碰 DB/rootDir）可在登录前完成；handler 体内触库的那些（project/artifact
  // 等）在登录页阶段不会被调用。
  registerPtyIpc()
  registerRemoteImIpc({ activateDataLayer: activateAccountDataLayer })
  registerScreenshotIpc()

  createWindow()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow()
  })
})

/**
 * 已移除的「习惯监控」功能的遗留数据清理：屏幕采样图片（每账号可达数 GB）与
 * 设置文件。尽力而为、异步后台执行；等所有老账号都清过一轮后可删除本函数。
 */
function cleanupLegacyHabitData(): void {
  const targets = [join(rootDir(), 'screen-samples'), join(rootDir(), 'habit-settings.json')]
  for (const target of targets) {
    void fs.rm(target, { recursive: true, force: true }).catch((err) => {
      console.warn('[habit-cleanup] failed to remove', target, err)
    })
  }
}

/**
 * 账号绑定成功后初始化数据层：解析账号作用域 rootDir、抢每账号单实例锁、开库、起后台
 * 服务。一次性（同一进程只服务一个账号）。返回 alreadyLocked 表示该账号已在别处打开。
 */
let dataLayerActivated = false
async function activateAccountDataLayer(
  userId: string
): Promise<{ ok: true } | { ok: false; alreadyLocked?: boolean; error?: string }> {
  if (dataLayerActivated) return { ok: true }

  setActiveAccount(userId)
  const lock = acquireInstanceLock(rootDir())
  if (!lock.ok) {
    setActiveAccount(null)
    return { ok: false, alreadyLocked: lock.alreadyLocked, error: lock.error }
  }

  if (process.env.MULTI_AI_CODE_PTY_DUMP === '1') {
    console.log(
      `[pty-dump] enabled. Raw PTY chunks will be written to ${join(rootDir(), 'logs')}\\pty-*.jsonl`
    )
  }

  await ensureRootDir()
  initDb()
  cleanupLegacyHabitData()
  registerScheduledTaskIpc()
  const screenshotHotkeyInit = await initializeScreenshotHotkey({ registrar: globalShortcut })
  effectiveScreenshotSettings = screenshotHotkeyInit.settings
  effectiveUiPreferences = await loadUiPreferences()
  if (!screenshotHotkeyInit.ok) {
    console.warn('[screenshot] failed to initialize hotkey:', screenshotHotkeyInit.error)
  }
  startScheduledTaskScheduler()
  activateRemoteImDataLayer()

  // 登录成功：把登录小窗口放大成主界面窗口。
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.setResizable(true)
    mainWindow.setMaximizable(true)
    mainWindow.setFullScreenable(true)
    mainWindow.setMinimumSize(1100, 700)
    mainWindow.setSize(1400, 900)
    mainWindow.center()
  }

  dataLayerActivated = true
  return { ok: true }
}

app.on('window-all-closed', () => {
  killAllSessions()
  closeDb()
  releaseInstanceLock()
  if (process.platform !== 'darwin') app.quit()
})

app.on('before-quit', () => {
  killAllSessions()
  stopScheduledTaskScheduler()
  disposeScreenshotHotkey(globalShortcut)
  releaseInstanceLock()
})
