import { app, BrowserWindow, ipcMain, shell, dialog } from 'electron'
import { join } from 'path'
import { initDb, closeDb, getProject, createProject } from './store/db.js'
import {
  ensureRootDir,
  createProjectLayout,
  projectDir as projectDirFn,
  artifactsDir
} from './store/paths.js'
import { registerPtyIpc, killAllSessions } from './cc/ptyManager.js'
import { promises as fs } from 'fs'

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

  ipcMain.handle('app:ping', () => 'pong')
  ipcMain.handle('app:version', () => app.getVersion())

  // Bootstrap a demo project for M3. M4 will add real project management.
  const demoId = 'demo'
  const demoDir = projectDirFn(demoId)
  const demoTargetRepo = demoDir // For demo, target_repo = project dir itself
  await createProjectLayout(demoId, demoTargetRepo)
  // Ensure artifacts dir exists (createProjectLayout already does, but be safe)
  await fs.mkdir(artifactsDir(demoId), { recursive: true })
  if (!getProject(demoId)) {
    createProject({ id: demoId, name: 'Demo', target_repo: demoTargetRepo })
  }
  async function readTargetRepo(): Promise<string> {
    try {
      const meta = JSON.parse(
        await fs.readFile(join(demoDir, 'project.json'), 'utf8')
      ) as { target_repo?: string }
      return meta.target_repo || demoDir
    } catch {
      return demoDir
    }
  }

  ipcMain.handle('app:demo-project', async () => ({
    id: demoId,
    dir: demoDir,
    target_repo: await readTargetRepo()
  }))

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
    async (_e, { path }: { path: string }) => {
      try {
        const st = await fs.stat(path)
        if (!st.isDirectory()) {
          return { ok: false, error: '选择的路径不是目录' }
        }
      } catch {
        return { ok: false, error: '目录不存在或无法访问' }
      }

      // Kill any running CC sessions; their cwd won't survive repo change
      killAllSessions()

      // Re-create symlinks for stages 2-4 → new target_repo
      const stageDirs: Record<number, string> = {
        2: 'stage2_impl',
        3: 'stage3_acceptance',
        4: 'stage4_test'
      }
      for (const [, dir] of Object.entries(stageDirs)) {
        const link = join(demoDir, 'workspaces', dir)
        try {
          await fs.rm(link, { force: true, recursive: false })
        } catch {
          /* ignore */
        }
        try {
          await fs.symlink(path, link, 'dir')
        } catch (err) {
          return { ok: false, error: (err as Error).message }
        }
      }

      // Update project.json + db
      const metaPath = join(demoDir, 'project.json')
      let meta: Record<string, unknown> = {}
      try {
        meta = JSON.parse(await fs.readFile(metaPath, 'utf8'))
      } catch {
        /* ignore */
      }
      meta.target_repo = path
      meta.name = path.split('/').filter(Boolean).pop() || (meta.name as string) || 'demo'
      meta.updated_at = new Date().toISOString()
      await fs.writeFile(metaPath, JSON.stringify(meta, null, 2))

      return { ok: true, target_repo: path, name: meta.name as string }
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
