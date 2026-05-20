export interface FontReadyLike {
  ready: Promise<unknown>
}

export interface TerminalMeasurementRecoveryOptions {
  fonts?: FontReadyLike | null
  settleDelayMs?: number
  requestAnimationFrame?: (callback: FrameRequestCallback) => number
  cancelAnimationFrame?: (handle: number) => void
}

/**
 * Xterm can mis-measure cell width on the first visible frame after mount,
 * especially when a panel was just swapped into view or Chromium is still
 * settling font/render state. Re-run fit/resize a few times so the terminal
 * converges to the real viewport size.
 */
export function scheduleTerminalMeasurementRecovery(
  sync: () => void,
  options: TerminalMeasurementRecoveryOptions = {}
): () => void {
  let disposed = false
  const handles: number[] = []
  const timeoutHandles: Array<ReturnType<typeof globalThis.setTimeout>> = []
  const requestFrame =
    options.requestAnimationFrame ??
    ((callback: FrameRequestCallback) => globalThis.requestAnimationFrame(callback))
  const cancelFrame =
    options.cancelAnimationFrame ??
    ((handle: number) => globalThis.cancelAnimationFrame(handle))

  const run = () => {
    if (disposed) return
    sync()
  }

  handles.push(
    requestFrame(() => {
      run()
    })
  )

  timeoutHandles.push(
    globalThis.setTimeout(() => {
      run()
    }, options.settleDelayMs ?? 120)
  )

  const fontsReady = options.fonts?.ready
  if (fontsReady) {
    void fontsReady
      .then(() => {
        if (disposed) return
        handles.push(
          requestFrame(() => {
            run()
          })
        )
      })
      .catch(() => {
        /* best-effort: skip font-triggered retry */
      })
  }

  return () => {
    disposed = true
    handles.forEach((handle) => cancelFrame(handle))
    timeoutHandles.forEach((handle) => globalThis.clearTimeout(handle))
  }
}
