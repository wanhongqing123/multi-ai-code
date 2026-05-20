import { describe, expect, it, vi } from 'vitest'
import { scheduleTerminalMeasurementRecovery } from './terminalLayoutRecovery.js'

function createDeferred<T = void>(): {
  promise: Promise<T>
  resolve: (value: T | PromiseLike<T>) => void
} {
  let resolve!: (value: T | PromiseLike<T>) => void
  const promise = new Promise<T>((res) => {
    resolve = res
  })
  return { promise, resolve }
}

describe('scheduleTerminalMeasurementRecovery', () => {
  it('re-runs terminal sync after the next frame, after a short settle delay, and after fonts are ready', async () => {
    vi.useFakeTimers()
    const calls: string[] = []
    const frameQueue: FrameRequestCallback[] = []
    const fonts = createDeferred<void>()

    const cleanup = scheduleTerminalMeasurementRecovery(
      () => {
        calls.push('sync')
      },
      {
        fonts: { ready: fonts.promise },
        requestAnimationFrame: (cb) => {
          frameQueue.push(cb)
          return frameQueue.length
        },
        cancelAnimationFrame: () => {}
      }
    )

    expect(calls).toEqual([])

    frameQueue.shift()?.(0)
    expect(calls).toEqual(['sync'])

    await vi.advanceTimersByTimeAsync(120)
    expect(calls).toEqual(['sync', 'sync'])

    fonts.resolve()
    await Promise.resolve()
    frameQueue.shift()?.(0)
    expect(calls).toEqual(['sync', 'sync', 'sync'])

    cleanup()
    vi.useRealTimers()
  })

  it('stops pending callbacks after cleanup', async () => {
    vi.useFakeTimers()
    const sync = vi.fn()
    const frameQueue: FrameRequestCallback[] = []
    const fonts = createDeferred<void>()

    const cleanup = scheduleTerminalMeasurementRecovery(sync, {
      fonts: { ready: fonts.promise },
      requestAnimationFrame: (cb) => {
        frameQueue.push(cb)
        return frameQueue.length
      },
      cancelAnimationFrame: () => {}
    })

    cleanup()
    frameQueue.shift()?.(0)
    await vi.advanceTimersByTimeAsync(120)
    fonts.resolve()
    await Promise.resolve()

    expect(sync).not.toHaveBeenCalled()
    vi.useRealTimers()
  })
})
