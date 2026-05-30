import { promises as fs } from 'fs'
import { join } from 'path'
import { rootDir } from '../store/paths.js'
import {
  dateFolderFor,
  safeIsoStamp,
  type BlocklistRule,
  type WindowSample
} from './screenSamplerLogic.js'

/**
 * Lightweight, always-on screenshot sampler that feeds habit_events.
 *
 *   Every 30s by default — primary-display thumbnail, written
 *                                to `<rootDir>/screen-samples/<date>/*.jpg`.
 *
 * Designed to be **completely best-effort**: every IO and IPC call is
 * wrapped so the sampler never throws into the main process. If the user
 * pauses (`paused()` returns true) or the platform refuses (e.g. macOS
 * screen-recording permission denied), the affected layer simply skips
 * the tick.
 *
 * The sampler is intentionally decoupled from the writer: it emits via
 * callbacks (`onFrame`) so wiring into habit_events lives in
 * `electron/main.ts` where the DB is already open.
 */

export interface ScreenSamplerOptions {
  /** Legacy option retained for callers/tests; active-window sampling is disabled. */
  l1IntervalMs?: number
  /** Defaults to 30000 ms. */
  l2IntervalMs?: number
  /** Live read each tick — returning true skips the tick. */
  paused: () => boolean
  /** Legacy option retained for settings compatibility; screenshots are not app-filtered. */
  appBlocklist?: () => BlocklistRule[]
  /** Legacy callback retained for compatibility; never called. */
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
  /** Always null because active-window sampling is disabled. */
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
 * Inject points for Electron-only screenshot dependencies. Tests pass mocks;
 * production wires the real modules from electron/main.ts.
 */
export interface SamplerDeps {
  /** Legacy dependency retained for compatibility; never called. */
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

// L2 thumbnail dimensions. Sized for multi-modal AI vision (Claude /
// GPT-4V) — readable for window titles, toolbar text, and medium-size
// code; smaller than this and the model can identify the app but not
// the on-screen content. Combined with JPEG q≈85 the per-frame size is
// ~80–200 KB, which against the 1 GB hard cap gives ~2–3 days of
// retention at the default 30 s sampling interval.
const THUMB_W = 1280
const THUMB_H = 720
/** Filename extension used for new L2 frames. Older PNGs on disk are
 *  still recognized by the retention sweeper for cleanup. */
const FRAME_EXT = 'jpg'

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
  const l2IntervalMs = opts.l2IntervalMs ?? 30_000
  const now = deps.now ?? (() => Date.now())
  const root = deps.sampleRoot ?? screenSampleRoot

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
    const absPath = join(dir, `${stamp}.${FRAME_EXT}`)
    const framePath = `screen-samples/${folder}/${stamp}.${FRAME_EXT}`

    try {
      await fs.mkdir(dir, { recursive: true })
      await fs.writeFile(absPath, cap.buf)
    } catch (err) {
      reportError('frame-write', err)
      return
    }

    lastL2At = capturedAt
    try {
      opts.onFrame(
        { framePath, absPath, w: cap.w, h: cap.h },
        capturedAt
      )
    } catch (err) {
      reportError('l2', err)
    }
  }

  // ---------- schedule + handle ----------
  // unref() prevents the timers from holding the event loop open during quit.
  const l2Timer = setInterval(() => void l2Tick(), l2IntervalMs)
  if (typeof l2Timer.unref === 'function') l2Timer.unref()
  // Fire immediately so we don't wait a full interval for first signals on app start.
  void l2Tick()

  return {
    stop(): void {
      if (stopped) return
      stopped = true
      clearInterval(l2Timer)
    },
    getLastWindow(): { sample: WindowSample; capturedAt: number } | null {
      return null
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
