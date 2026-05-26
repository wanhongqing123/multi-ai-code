/**
 * Main-process service that wires `screenSampler` to:
 *   - active-win    → L1 window probes
 *   - desktopCapturer → L2 thumbnails
 *   - habit_events  → persisted samples
 *   - habit settings → live pause flag + blocklist
 *
 * Owns the singleton sampler handle. Started from main.ts, stopped on
 * app `before-quit`.
 */

import { promises as fs } from 'fs'
import { createRequire } from 'module'
import {
  desktopCapturer,
  screen as electronScreen,
  type Display,
  type NativeImage
} from 'electron'
import { applyRetentionSweep } from './screenSampleRetention.js'
import {
  screenSampleRoot,
  startScreenSampler,
  type ScreenSamplerHandle
} from './screenSampler.js'
import { insertHabitEvent } from './db.js'
import { loadHabitSettings, updateHabitSettings } from './settings.js'

const require = createRequire(import.meta.url)

interface ActiveWinResult {
  title: string
  owner: { name: string; processId: number; bundleId?: string; path?: string }
}

type ActiveWinFn = (opts?: {
  accessibilityPermission?: boolean
  screenRecordingPermission?: boolean
}) => Promise<ActiveWinResult | undefined>

let activeWinModule: ActiveWinFn | null = null
let activeWinLoadError: string | null = null

function loadActiveWin(): ActiveWinFn | null {
  if (activeWinModule) return activeWinModule
  if (activeWinLoadError) return null
  try {
    const mod = require('active-win') as ActiveWinFn | { default?: ActiveWinFn }
    activeWinModule = (typeof mod === 'function' ? mod : mod.default) ?? null
    if (!activeWinModule) {
      activeWinLoadError = 'active-win module loaded but export is not callable'
    }
  } catch (err) {
    activeWinLoadError = err instanceof Error ? err.message : String(err)
  }
  return activeWinModule
}

let handle: ScreenSamplerHandle | null = null

/** Module-local state — single source of truth for the live flags. */
const liveState = {
  enabled: true,
  paused: false,
  appBlocklist: [] as string[]
}

/**
 * Snapshot exposed via IPC so the topbar can render the indicator.
 */
export interface ScreenSamplerStatus {
  enabled: boolean
  paused: boolean
  /** Runtime status only present after the sampler has actually started. */
  runtime: {
    running: boolean
    lastL1At: number
    lastL2At: number
    lastError: { label: string; message: string; at: number } | null
    lastWindowTitle: string | null
    lastWindowApp: string | null
  } | null
  /** If active-win failed to load (e.g. native binding missing) — UI shows. */
  activeWinLoadError: string | null
}

export function getScreenSamplerStatus(): ScreenSamplerStatus {
  if (!handle) {
    return {
      enabled: liveState.enabled,
      paused: liveState.paused,
      runtime: null,
      activeWinLoadError
    }
  }
  const rt = handle.getStatus()
  const lastWin = handle.getLastWindow()
  return {
    enabled: liveState.enabled,
    paused: liveState.paused,
    runtime: {
      running: rt.running,
      lastL1At: rt.lastL1At,
      lastL2At: rt.lastL2At,
      lastError: rt.lastError,
      lastWindowTitle: lastWin?.sample.title ?? null,
      lastWindowApp: lastWin?.sample.appName ?? null
    },
    activeWinLoadError
  }
}

/** Toggles the persisted pause flag; takes effect on the very next tick. */
export async function toggleScreenSamplerPause(): Promise<ScreenSamplerStatus> {
  return setScreenSamplerPaused(!liveState.paused)
}

export async function setScreenSamplerPaused(
  paused: boolean
): Promise<ScreenSamplerStatus> {
  const cur = await loadHabitSettings()
  await updateHabitSettings({
    screenSampler: { ...cur.screenSampler, paused }
  })
  liveState.paused = paused
  return getScreenSamplerStatus()
}

/**
 * Captures the primary display at the requested thumbnail size. Returns
 * JPEG-encoded bytes (q=85 — visually lossless for UI text, ~5× smaller
 * than equivalent PNG). Null on any failure (permission denied, no
 * display, etc.). Uses desktopCapturer.thumbnailSize so Chromium does
 * the resize server-side, which is far cheaper than scaling a
 * full-resolution image in JS each tick.
 */
const JPEG_QUALITY = 92

export function resolveCaptureRequestSize(
  display: Pick<Display, 'size' | 'scaleFactor'>,
  targetW: number,
  targetH: number
): { width: number; height: number } {
  const scaleFactor = Math.max(display.scaleFactor || 1, 1)
  return {
    width: Math.max(Math.round(display.size.width * scaleFactor), targetW),
    height: Math.max(Math.round(display.size.height * scaleFactor), targetH)
  }
}

async function capturePrimaryThumbnail(
  targetW: number,
  targetH: number
): Promise<{ buf: Buffer; w: number; h: number } | null> {
  try {
    const primary = electronScreen.getPrimaryDisplay()
    const requestSize = resolveCaptureRequestSize(primary, targetW, targetH)
    const sources = await desktopCapturer.getSources({
      types: ['screen'],
      thumbnailSize: requestSize
    })
    if (sources.length === 0) return null
    const primaryId = String(primary.id)
    const picked =
      sources.find((s) => s.display_id === primaryId) ?? sources[0]
    const img: NativeImage = picked.thumbnail
    if (!img || img.isEmpty()) return null
    const size = img.getSize()
    return { buf: img.toJPEG(JPEG_QUALITY), w: size.width, h: size.height }
  } catch {
    return null
  }
}

/** Best-effort active-window probe; never throws. */
async function probeActiveWindow(): Promise<{
  title: string
  appName: string
  bundleId?: string
  processId?: number
} | null> {
  const fn = loadActiveWin()
  if (!fn) return null
  try {
    const r = await fn({
      accessibilityPermission: false,
      screenRecordingPermission: false
    })
    if (!r) return null
    return {
      title: r.title || '',
      appName: r.owner?.name || '',
      bundleId: r.owner?.bundleId,
      processId: r.owner?.processId
    }
  } catch {
    return null
  }
}

let retentionTimer: NodeJS.Timeout | null = null

async function runRetentionOnce(): Promise<void> {
  try {
    const settings = await loadHabitSettings()
    await applyRetentionSweep(screenSampleRoot(), settings.retentionDays)
  } catch {
    /* sweep is best-effort */
  }
}

export async function startScreenSamplerService(): Promise<void> {
  if (handle) return
  const settings = await loadHabitSettings()
  liveState.enabled = settings.screenSampler.enabled
  liveState.paused = settings.screenSampler.paused
  liveState.appBlocklist = [...settings.screenSampler.appBlocklist]
  if (!settings.screenSampler.enabled) return
  try {
    await fs.mkdir(screenSampleRoot(), { recursive: true })
  } catch {
    /* tolerate */
  }
  handle = startScreenSampler(
    {
      getActiveWindow: probeActiveWindow,
      captureThumbnail: capturePrimaryThumbnail
    },
    {
      paused: () => liveState.paused,
      appBlocklist: () => liveState.appBlocklist,
      onWindow: (info, ts) => {
        insertHabitEvent({
          ts,
          kind: 'screen_window',
          payload: {
            title: info.title,
            appName: info.appName,
            bundleId: info.bundleId,
            processId: info.processId
          },
          sourceWindow: 'screen-sampler'
        })
      },
      onFrame: (info, ts) => {
        insertHabitEvent({
          ts,
          kind: 'screen_frame',
          payload: {
            framePath: info.framePath,
            w: info.w,
            h: info.h,
            app: info.app
          },
          sourceWindow: 'screen-sampler'
        })
      },
      onError: () => {
        /* status already records lastError; avoid console spam */
      }
    }
  )

  if (!retentionTimer) {
    setTimeout(() => void runRetentionOnce(), 30_000).unref()
    retentionTimer = setInterval(
      () => void runRetentionOnce(),
      24 * 60 * 60 * 1000
    )
    retentionTimer.unref()
  }
}

export function stopScreenSamplerService(): void {
  if (handle) {
    handle.stop()
    handle = null
  }
  if (retentionTimer) {
    clearInterval(retentionTimer)
    retentionTimer = null
  }
}

/**
 * Pulls the latest persisted settings into the runtime cache. Call this
 * after any IPC mutation of habit settings so the live samplers see the
 * change on the next tick.
 */
export async function refreshScreenSamplerLiveState(): Promise<void> {
  const s = await loadHabitSettings()
  liveState.enabled = s.screenSampler.enabled
  liveState.paused = s.screenSampler.paused
  liveState.appBlocklist = [...s.screenSampler.appBlocklist]
}
