import { promises as fs } from 'fs'
import { join } from 'path'
import { rootDir } from '../store/paths.js'
import {
  DEFAULT_APP_BLOCKLIST,
  SampleDedupe,
  applyBlocklist,
  dateFolderFor,
  redactSecrets,
  safeIsoStamp,
  type BlocklistRule,
  type WindowSample
} from './screenSamplerLogic.js'

/**
 * Lightweight, always-on screen sampler that feeds habit_events.
 *
 *   L1 (every 5s by default)  — active foreground window: title + appName.
 *   L2 (every 30s by default) — primary-display thumbnail 256×144, written
 *                                to `<rootDir>/screen-samples/<date>/*.png`.
 *
 * Designed to be **completely best-effort**: every IO and IPC call is
 * wrapped so the sampler never throws into the main process. If the user
 * pauses (`paused()` returns true) or the platform refuses (e.g. macOS
 * screen-recording permission denied), the affected layer simply skips
 * the tick.
 *
 * The sampler is intentionally decoupled from the writer: it emits via
 * callbacks (`onWindow`, `onFrame`) so wiring into habit_events lives in
 * `electron/main.ts` where the DB is already open.
 */

export interface ScreenSamplerOptions {
  /** Defaults to 5000 ms. */
  l1IntervalMs?: number
  /** Defaults to 30000 ms. */
  l2IntervalMs?: number
  /** Live read each tick — returning true skips the tick. */
  paused: () => boolean
  /** Live read each tick — used as override for DEFAULT_APP_BLOCKLIST. */
  appBlocklist?: () => BlocklistRule[]
  /** Receives a filtered L1 sample to persist. */
  onWindow: (info: WindowSample, capturedAt: number) => void
  /**
   * Receives an L2 thumbnail. `framePath` is repo-rootDir-relative posix
   * style (so it survives moves of the data dir).
   */
  onFrame: (
    info: {
      framePath: string
      absPath: string
      w: number
      h: number
      app?: string
    },
    capturedAt: number
  ) => void
  /**
   * Errors that would otherwise be swallowed are forwarded here for the
   * UI's "last error" line. The sampler will keep ticking regardless.
   */
  onError?: (label: 'l1' | 'l2' | 'frame-write', err: Error) => void
}

/** Returned handle for stopping / inspecting the running sampler. */
export interface ScreenSamplerHandle {
  stop: () => void
  /** Last successful L1 sample (for the topbar tooltip / UI). */
  getLastWindow: () => { sample: WindowSample; capturedAt: number } | null
  /** Sampler state for IPC. */
  getStatus: () => {
    running: boolean
    lastL1At: number
    lastL2At: number
    lastError: { label: string; message: string; at: number } | null
  }
}

/** Resolved path of the screen-sample root directory (created lazily). */
export function screenSampleRoot(): string {
  return join(rootDir(), 'screen-samples')
}

/**
 * Inject points for the dependencies we don't want to import statically
 * (active-win is CJS native, desktopCapturer is Electron-only). Tests
 * pass mocks; production wires the real modules from electron/main.ts.
 */
export interface SamplerDeps {
  /** Returns the active window info, or null when unavailable. */
  getActiveWindow: () => Promise<WindowSample | null>
  /**
   * Captures the primary display, returning a Buffer of PNG bytes already
   * resized to the requested size. Implementations should fall back to a
   * larger PNG if resize is unavailable — the writer's `fs.writeFile` is
   * agnostic.
   */
  captureThumbnail: (
    targetW: number,
    targetH: number
  ) => Promise<{ buf: Buffer; w: number; h: number } | null>
  /** Returns a monotonic clock — abstracted so tests can fast-forward. */
  now?: () => number
  /** Optional override for the on-disk root (tests use a tmpdir). */
  sampleRoot?: () => string
}

const THUMB_W = 256
const THUMB_H = 144

/**
 * Starts both samplers. Returns a handle for graceful shutdown. Safe to
 * call multiple times concurrently — each call returns an independent
 * handle (we don't enforce a singleton at this level; the caller in
 * main.ts is responsible for that).
 */
export function startScreenSampler(
  deps: SamplerDeps,
  opts: ScreenSamplerOptions
): ScreenSamplerHandle {
  const l1IntervalMs = opts.l1IntervalMs ?? 5_000
  const l2IntervalMs = opts.l2IntervalMs ?? 30_000
  const dedupeL1 = new SampleDedupe()
  const now = deps.now ?? (() => Date.now())
  const root = deps.sampleRoot ?? screenSampleRoot

  let lastWindow: { sample: WindowSample; capturedAt: number } | null = null
  let lastL1At = 0
  let lastL2At = 0
  let lastError: { label: string; message: string; at: number } | null = null
  let stopped = false

  const reportError = (label: 'l1' | 'l2' | 'frame-write', err: unknown) => {
    const e = err instanceof Error ? err : new Error(String(err))
    lastError = { label, message: e.message, at: now() }
    try {
      opts.onError?.(label, e)
    } catch {
      /* user-supplied callback swallows */
    }
  }

  const blocklistOf = (): BlocklistRule[] => {
    try {
      const extra = opts.appBlocklist?.() ?? []
      return [...DEFAULT_APP_BLOCKLIST, ...extra]
    } catch {
      return DEFAULT_APP_BLOCKLIST
    }
  }

  // ---------- L1 tick ----------
  const l1Tick = async (): Promise<void> => {
    if (stopped) return
    try {
      if (opts.paused()) return
    } catch {
      /* pause callback bad — assume paused for safety */
      return
    }
    let raw: WindowSample | null = null
    try {
      raw = await deps.getActiveWindow()
    } catch (err) {
      reportError('l1', err)
      return
    }
    if (!raw) return
    const filtered = applyBlocklist(raw, blocklistOf())
    if (!filtered) return
    const dedupeKey = `${filtered.appName}|${filtered.title}`
    if (!dedupeL1.shouldKeep(dedupeKey, now())) {
      lastL1At = now()
      lastWindow = { sample: filtered, capturedAt: now() }
      return
    }
    lastL1At = now()
    lastWindow = { sample: filtered, capturedAt: lastL1At }
    try {
      opts.onWindow(filtered, lastL1At)
    } catch (err) {
      reportError('l1', err)
    }
  }

  // ---------- L2 tick ----------
  const l2Tick = async (): Promise<void> => {
    if (stopped) return
    try {
      if (opts.paused()) return
    } catch {
      return
    }
    let cap: { buf: Buffer; w: number; h: number } | null = null
    try {
      cap = await deps.captureThumbnail(THUMB_W, THUMB_H)
    } catch (err) {
      reportError('l2', err)
      return
    }
    if (!cap || !cap.buf || cap.buf.length === 0) return

    const capturedAt = now()
    const folder = dateFolderFor(capturedAt)
    const stamp = safeIsoStamp(capturedAt)
    const dir = join(root(), folder)
    const absPath = join(dir, `${stamp}.png`)
    const framePath = `screen-samples/${folder}/${stamp}.png`

    try {
      await fs.mkdir(dir, { recursive: true })
      await fs.writeFile(absPath, cap.buf)
    } catch (err) {
      reportError('frame-write', err)
      return
    }

    lastL2At = capturedAt
    const appName = lastWindow?.sample.appName
    try {
      opts.onFrame(
        { framePath, absPath, w: cap.w, h: cap.h, app: appName },
        capturedAt
      )
    } catch (err) {
      reportError('l2', err)
    }
  }

  // ---------- schedule + handle ----------
  // unref() prevents the timers from holding the event loop open during quit.
  const l1Timer = setInterval(() => void l1Tick(), l1IntervalMs)
  const l2Timer = setInterval(() => void l2Tick(), l2IntervalMs)
  if (typeof l1Timer.unref === 'function') l1Timer.unref()
  if (typeof l2Timer.unref === 'function') l2Timer.unref()
  // Fire one of each immediately so we don't wait a full interval for
  // first signals on app start.
  void l1Tick()
  void l2Tick()

  return {
    stop(): void {
      if (stopped) return
      stopped = true
      clearInterval(l1Timer)
      clearInterval(l2Timer)
    },
    getLastWindow(): { sample: WindowSample; capturedAt: number } | null {
      return lastWindow
    },
    getStatus(): ScreenSamplerHandle['getStatus'] extends () => infer T
      ? T
      : never {
      return {
        running: !stopped,
        lastL1At,
        lastL2At,
        lastError
      }
    }
  }
}

// Re-export the helpers so callers don't need to import from logic.ts too.
export { redactSecrets, DEFAULT_APP_BLOCKLIST }
