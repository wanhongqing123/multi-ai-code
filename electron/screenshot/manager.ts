import {
  BrowserWindow,
  desktopCapturer,
  ipcMain,
  screen,
  globalShortcut
} from 'electron'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { randomBytes } from 'crypto'

/**
 * Screenshot pipeline:
 *   global hotkey
 *     → capture primary display
 *     → open overlay window (region drag)
 *     → on commit: crop image
 *     → open editor window (canvas annotation)
 *     → on send: save final PNG, broadcast {path, prompt} to main window
 *     → main window's App.tsx forwards to current cc.sendUser session
 *
 * Sessions are tracked by a short opaque token so overlay/editor windows
 * can fetch only their own payload — useful in the (rare) case the user
 * triggers multiple screenshots in quick succession.
 */

const isDev = !!process.env['ELECTRON_RENDERER_URL']

interface OverlayPayload {
  imageDataUrl: string
  logicalSize: { w: number; h: number }
  physicalSize: { w: number; h: number }
  /** Source NativeImage retained so we can crop without re-decoding. */
  source: Electron.NativeImage
}

interface EditorPayload {
  imageDataUrl: string
  size: { w: number; h: number }
}

interface ScreenshotSession {
  token: string
  overlayWin: BrowserWindow | null
  editorWin: BrowserWindow | null
  overlayPayload: OverlayPayload | null
  editorPayload: EditorPayload | null
}

const sessions = new Map<string, ScreenshotSession>()

function newToken(): string {
  return randomBytes(8).toString('hex')
}

function findMainWindow(): BrowserWindow | null {
  for (const win of BrowserWindow.getAllWindows()) {
    if (win.isDestroyed()) continue
    const url = win.webContents.getURL()
    // Main window: index.html with no ?window=... query. Repo-view / screenshot
    // windows all carry a `window=` param.
    if (!/[?&]window=/.test(url)) return win
  }
  return null
}

function abandonSession(token: string): void {
  const s = sessions.get(token)
  if (!s) return
  try {
    if (s.overlayWin && !s.overlayWin.isDestroyed()) s.overlayWin.close()
  } catch {
    /* ignore */
  }
  try {
    if (s.editorWin && !s.editorWin.isDestroyed()) s.editorWin.close()
  } catch {
    /* ignore */
  }
  sessions.delete(token)
}

async function captureMainDisplay(): Promise<OverlayPayload> {
  const primary = screen.getPrimaryDisplay()
  const { width, height } = primary.size
  const scaleFactor = primary.scaleFactor || 1
  // Ask the capture for physical pixels so the crop is sharp on hi-DPI.
  const sources = await desktopCapturer.getSources({
    types: ['screen'],
    thumbnailSize: {
      width: Math.round(width * scaleFactor),
      height: Math.round(height * scaleFactor)
    }
  })
  if (sources.length === 0) {
    throw new Error('No display sources available')
  }
  // Prefer the source whose id contains the primary display id; else first.
  const primaryId = String(primary.id)
  const picked =
    sources.find((s) => s.display_id === primaryId) ?? sources[0]
  const img = picked.thumbnail
  const sz = img.getSize()
  return {
    imageDataUrl: img.toDataURL(),
    logicalSize: { w: width, h: height },
    physicalSize: { w: sz.width, h: sz.height },
    source: img
  }
}

function buildRendererSearch(params: Record<string, string>): string {
  return '?' + new URLSearchParams(params).toString()
}

function loadRendererPage(win: BrowserWindow, search: string): void {
  if (isDev) {
    const base = process.env['ELECTRON_RENDERER_URL']!
    const root = base.endsWith('/') ? base : `${base}/`
    void win.loadURL(`${root}${search}`)
  } else {
    void win.loadFile(join(__dirname, '../renderer/index.html'), { search })
  }
}

function openOverlayWindow(session: ScreenshotSession): void {
  const primary = screen.getPrimaryDisplay()
  const win = new BrowserWindow({
    x: primary.bounds.x,
    y: primary.bounds.y,
    width: primary.size.width,
    height: primary.size.height,
    frame: false,
    transparent: true,
    fullscreen: false,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    skipTaskbar: true,
    alwaysOnTop: true,
    hasShadow: false,
    show: false,
    webPreferences: {
      preload: join(__dirname, '../preload/preload.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })
  win.setAlwaysOnTop(true, 'screen-saver')
  session.overlayWin = win
  win.on('ready-to-show', () => {
    win.show()
    win.focus()
  })
  win.on('closed', () => {
    if (session.overlayWin === win) session.overlayWin = null
  })
  loadRendererPage(
    win,
    buildRendererSearch({ window: 'screenshot-overlay', token: session.token })
  )
}

function openEditorWindow(session: ScreenshotSession): void {
  const { w, h } = session.editorPayload!.size
  // Editor window: a normal frame, sized to fit the cropped image but capped.
  const maxW = Math.min(1200, w + 80)
  const maxH = Math.min(900, h + 220)
  const win = new BrowserWindow({
    width: Math.max(640, maxW),
    height: Math.max(480, maxH),
    minWidth: 480,
    minHeight: 360,
    title: '截图标注',
    autoHideMenuBar: true,
    show: false,
    webPreferences: {
      preload: join(__dirname, '../preload/preload.mjs'),
      sandbox: false,
      contextIsolation: true,
      nodeIntegration: false
    }
  })
  session.editorWin = win
  win.on('ready-to-show', () => {
    win.show()
    win.focus()
  })
  win.on('closed', () => {
    if (session.editorWin === win) session.editorWin = null
    // If the editor closes without a successful send, drop the session.
    if (session.editorPayload && !session.overlayWin) {
      sessions.delete(session.token)
    }
  })
  loadRendererPage(
    win,
    buildRendererSearch({ window: 'screenshot-editor', token: session.token })
  )
}

async function saveAnnotatedImage(bytes: Uint8Array): Promise<string> {
  const dir = join(tmpdir(), 'multi-ai-code', 'screenshots')
  await fs.mkdir(dir, { recursive: true })
  const ts = new Date().toISOString().replace(/[:.]/g, '-')
  const name = `shot-${ts}-${randomBytes(3).toString('hex')}.png`
  const full = join(dir, name)
  await fs.writeFile(full, Buffer.from(bytes))
  return full
}

function deliverToMainWindow(path: string, prompt: string): void {
  const win = findMainWindow()
  if (!win) return
  if (!win.isVisible() || win.isMinimized()) {
    win.show()
    win.restore()
  }
  win.focus()
  win.webContents.send('screenshot:deliver', { path, prompt })
}

/** Starts a new screenshot session. Idempotent if a session is already live. */
async function beginScreenshotSession(): Promise<void> {
  // If a session is already in progress, ignore so the hotkey isn't a footgun.
  for (const s of sessions.values()) {
    if (s.overlayWin || s.editorWin) return
  }
  const token = newToken()
  const session: ScreenshotSession = {
    token,
    overlayWin: null,
    editorWin: null,
    overlayPayload: null,
    editorPayload: null
  }
  sessions.set(token, session)
  try {
    session.overlayPayload = await captureMainDisplay()
  } catch (err) {
    sessions.delete(token)
    const main = findMainWindow()
    main?.webContents.send('screenshot:error', {
      message: `截图失败：${(err as Error).message}`
    })
    return
  }
  openOverlayWindow(session)
}

export function registerScreenshotIpc(): void {
  ipcMain.handle('screenshot:overlay-load', async (_e, token: string) => {
    const s = sessions.get(token)
    if (!s || !s.overlayPayload) return { ok: false as const, error: 'no session' }
    return {
      ok: true as const,
      payload: {
        imageDataUrl: s.overlayPayload.imageDataUrl,
        logicalSize: s.overlayPayload.logicalSize,
        physicalSize: s.overlayPayload.physicalSize
      }
    }
  })

  ipcMain.handle(
    'screenshot:overlay-commit',
    async (
      _e,
      req: {
        token: string
        logicalRect: { x: number; y: number; w: number; h: number }
        logicalSize: { w: number; h: number }
        physicalSize: { w: number; h: number }
      }
    ) => {
      const s = sessions.get(req.token)
      if (!s || !s.overlayPayload) return { ok: false as const, error: 'no session' }
      const scaleX = req.physicalSize.w / Math.max(1, req.logicalSize.w)
      const scaleY = req.physicalSize.h / Math.max(1, req.logicalSize.h)
      const physRect = {
        x: Math.round(req.logicalRect.x * scaleX),
        y: Math.round(req.logicalRect.y * scaleY),
        width: Math.round(req.logicalRect.w * scaleX),
        height: Math.round(req.logicalRect.h * scaleY)
      }
      if (physRect.width <= 0 || physRect.height <= 0) {
        return { ok: false as const, error: 'empty crop region' }
      }
      const cropped = s.overlayPayload.source.crop(physRect)
      s.editorPayload = {
        imageDataUrl: cropped.toDataURL(),
        size: { w: physRect.width, h: physRect.height }
      }
      // Drop the overlay window now; we're done with the full screen.
      try {
        if (s.overlayWin && !s.overlayWin.isDestroyed()) s.overlayWin.close()
      } catch {
        /* ignore */
      }
      s.overlayWin = null
      openEditorWindow(s)
      return { ok: true as const }
    }
  )

  ipcMain.handle('screenshot:overlay-cancel', async (_e, token: string) => {
    abandonSession(token)
    return { ok: true as const }
  })

  ipcMain.handle('screenshot:editor-load', async (_e, token: string) => {
    const s = sessions.get(token)
    if (!s || !s.editorPayload) return { ok: false as const, error: 'no session' }
    return {
      ok: true as const,
      payload: s.editorPayload
    }
  })

  ipcMain.handle('screenshot:editor-cancel', async (_e, token: string) => {
    abandonSession(token)
    return { ok: true as const }
  })

  ipcMain.handle(
    'screenshot:editor-send',
    async (
      _e,
      req: { token: string; pngBytes: Uint8Array | ArrayBuffer; prompt: string }
    ) => {
      const s = sessions.get(req.token)
      if (!s) return { ok: false as const, error: 'no session' }
      try {
        const bytes =
          req.pngBytes instanceof Uint8Array
            ? req.pngBytes
            : new Uint8Array(req.pngBytes)
        const path = await saveAnnotatedImage(bytes)
        deliverToMainWindow(path, req.prompt ?? '')
        // Cleanly close the editor window.
        try {
          if (s.editorWin && !s.editorWin.isDestroyed()) s.editorWin.close()
        } catch {
          /* ignore */
        }
        sessions.delete(s.token)
        return { ok: true as const, path }
      } catch (err) {
        return { ok: false as const, error: (err as Error).message }
      }
    }
  )

  // Manual trigger (e.g. from a UI button); same flow as the global hotkey.
  ipcMain.handle('screenshot:start', async () => {
    await beginScreenshotSession()
    return { ok: true as const }
  })
}

const HOTKEY = 'CommandOrControl+Shift+A'

export function registerScreenshotHotkey(): void {
  try {
    globalShortcut.register(HOTKEY, () => {
      void beginScreenshotSession()
    })
  } catch {
    /* tolerate registration failure — manual trigger still works */
  }
}

export function unregisterScreenshotHotkey(): void {
  try {
    globalShortcut.unregister(HOTKEY)
  } catch {
    /* ignore */
  }
}
