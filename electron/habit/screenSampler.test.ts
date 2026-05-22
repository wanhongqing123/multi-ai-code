import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { startScreenSampler, type SamplerDeps } from './screenSampler.js'

let tempRoot: string
let handles: { stop: () => void }[] = []

/** Drain the microtask queue so the synchronous kickoff promise(s) settle. */
async function flush(cycles = 5): Promise<void> {
  for (let i = 0; i < cycles; i++) await Promise.resolve()
}

/** Wait for at least N real-time ticks of the fastest interval to fire. */
async function realWait(ms: number): Promise<void> {
  await new Promise((r) => setTimeout(r, ms))
}

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'screen-sampler-'))
  handles = []
})

afterEach(async () => {
  for (const h of handles) h.stop()
  handles = []
  await fs.rm(tempRoot, { recursive: true, force: true })
})

function makeDeps(
  over: {
    activeWin?: { title: string; appName: string } | null
    capture?: { buf: Buffer; w: number; h: number } | null
    getActiveWindowOverride?: SamplerDeps['getActiveWindow']
  } = {}
): SamplerDeps & {
  getActiveWindow: ReturnType<typeof vi.fn>
  captureThumbnail: ReturnType<typeof vi.fn>
} {
  const getActiveWindow =
    over.getActiveWindowOverride
      ? vi.fn(over.getActiveWindowOverride)
      : vi.fn(async () =>
          over.activeWin === undefined
            ? { title: 'VSCode — App.tsx', appName: 'VSCode' }
            : over.activeWin
        )
  const captureThumbnail = vi.fn(async () =>
    over.capture === undefined
      ? { buf: Buffer.from([0x89, 0x50, 0x4e, 0x47]), w: 256, h: 144 }
      : over.capture
  )
  return {
    getActiveWindow,
    captureThumbnail,
    sampleRoot: () => tempRoot
  }
}

describe('startScreenSampler L1', () => {
  it('fires onWindow on the immediate kickoff sample', async () => {
    const onWindow = vi.fn()
    const onFrame = vi.fn()
    const deps = makeDeps()
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow,
        onFrame,
        l1IntervalMs: 10_000,
        l2IntervalMs: 10_000
      })
    )
    await flush()
    expect(deps.getActiveWindow).toHaveBeenCalled()
    expect(onWindow).toHaveBeenCalled()
    expect(onWindow.mock.calls[0][0]).toMatchObject({
      title: expect.stringContaining('VSCode'),
      appName: 'VSCode'
    })
  })

  it('skips when paused() returns true', async () => {
    const onWindow = vi.fn()
    const onFrame = vi.fn()
    const deps = makeDeps()
    handles.push(
      startScreenSampler(deps, {
        paused: () => true,
        onWindow,
        onFrame,
        l1IntervalMs: 10_000,
        l2IntervalMs: 10_000
      })
    )
    await flush()
    expect(onWindow).not.toHaveBeenCalled()
    expect(onFrame).not.toHaveBeenCalled()
  })

  it('drops samples whose app is on the blocklist', async () => {
    const onWindow = vi.fn()
    const deps = makeDeps({ activeWin: { title: 'Vault', appName: '1Password' } })
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow,
        onFrame: vi.fn(),
        l1IntervalMs: 10_000,
        l2IntervalMs: 10_000
      })
    )
    await flush()
    expect(onWindow).not.toHaveBeenCalled()
  })

  it('honors a custom blocklist (callable each tick)', async () => {
    const onWindow = vi.fn()
    const deps = makeDeps({ activeWin: { title: 'banking', appName: 'Edge' } })
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        appBlocklist: () => ['banking'],
        onWindow,
        onFrame: vi.fn(),
        l1IntervalMs: 10_000,
        l2IntervalMs: 10_000
      })
    )
    await flush()
    expect(onWindow).not.toHaveBeenCalled()
  })

  it('dedupes identical title within 24h (real-timer interval)', async () => {
    const onWindow = vi.fn()
    const deps = makeDeps()
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow,
        onFrame: vi.fn(),
        l1IntervalMs: 30,
        l2IntervalMs: 60_000
      })
    )
    // Allow multiple intervals to fire on the real clock
    await realWait(120)
    // Should have called onWindow exactly once (the kickoff), even though
    // the interval fired several times with the same title.
    expect(onWindow).toHaveBeenCalledTimes(1)
  })

  it('forwards errors to onError but keeps running', async () => {
    const onWindow = vi.fn()
    const onError = vi.fn()
    let calls = 0
    const deps = makeDeps({
      getActiveWindowOverride: async () => {
        calls++
        if (calls === 1) throw new Error('oh no')
        return { title: `Window-${calls}`, appName: 'A' }
      }
    })
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow,
        onFrame: vi.fn(),
        onError,
        l1IntervalMs: 20,
        l2IntervalMs: 60_000
      })
    )
    await realWait(80)
    expect(onError).toHaveBeenCalled()
    expect(onError.mock.calls[0][0]).toBe('l1')
    expect(onWindow).toHaveBeenCalled()
  })
})

describe('startScreenSampler L2', () => {
  it('writes thumbnail PNG under the date folder and emits onFrame', async () => {
    const onFrame = vi.fn()
    const deps = makeDeps()
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow: vi.fn(),
        onFrame,
        l1IntervalMs: 60_000,
        l2IntervalMs: 60_000
      })
    )
    await flush()
    // The frame write is async (fs.mkdir + fs.writeFile) so give it real ticks.
    await realWait(60)
    expect(onFrame).toHaveBeenCalled()
    const arg = onFrame.mock.calls[0][0] as {
      framePath: string
      absPath: string
      w: number
      h: number
    }
    expect(arg.framePath).toMatch(
      /^screen-samples\/\d{4}-\d{2}-\d{2}\/.+\.png$/
    )
    expect(arg.w).toBe(256)
    expect(arg.h).toBe(144)
    const written = await fs.readFile(arg.absPath)
    expect(written.length).toBeGreaterThan(0)
  })

  it('does NOT write or call onFrame when capture returns null', async () => {
    const onFrame = vi.fn()
    const deps = makeDeps({ capture: null })
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow: vi.fn(),
        onFrame,
        l1IntervalMs: 60_000,
        l2IntervalMs: 60_000
      })
    )
    await realWait(60)
    expect(onFrame).not.toHaveBeenCalled()
  })

  it('attaches the last-known app name to the frame payload', async () => {
    const onFrame = vi.fn()
    const deps = makeDeps()
    handles.push(
      startScreenSampler(deps, {
        paused: () => false,
        onWindow: vi.fn(),
        onFrame,
        l1IntervalMs: 60_000,
        l2IntervalMs: 60_000
      })
    )
    await realWait(60)
    expect(onFrame).toHaveBeenCalled()
    const arg = onFrame.mock.calls[0][0] as { app?: string }
    expect(arg.app).toBe('VSCode')
  })
})

describe('startScreenSampler handle', () => {
  it('stop() prevents further ticks', async () => {
    const onWindow = vi.fn()
    const deps = makeDeps()
    const h = startScreenSampler(deps, {
      paused: () => false,
      onWindow,
      onFrame: vi.fn(),
      l1IntervalMs: 20,
      l2IntervalMs: 60_000
    })
    handles.push(h)
    await realWait(40)
    const callsBeforeStop = onWindow.mock.calls.length
    h.stop()
    await realWait(80)
    // After stop, count must not increase.
    expect(onWindow.mock.calls.length).toBe(callsBeforeStop)
  })

  it('getLastWindow exposes the most recent observation', async () => {
    const onWindow = vi.fn()
    const deps = makeDeps()
    const h = startScreenSampler(deps, {
      paused: () => false,
      onWindow,
      onFrame: vi.fn(),
      l1IntervalMs: 60_000,
      l2IntervalMs: 60_000
    })
    handles.push(h)
    await flush()
    const last = h.getLastWindow()
    expect(last).not.toBeNull()
    expect(last!.sample.appName).toBe('VSCode')
  })

  it('getStatus reports the lastError without throwing the sampler', async () => {
    const onError = vi.fn()
    const deps = makeDeps({
      getActiveWindowOverride: async () => {
        throw new Error('permission denied')
      }
    })
    const h = startScreenSampler(deps, {
      paused: () => false,
      onWindow: vi.fn(),
      onFrame: vi.fn(),
      onError,
      l1IntervalMs: 60_000,
      l2IntervalMs: 60_000
    })
    handles.push(h)
    await flush()
    const status = h.getStatus()
    expect(status.lastError?.label).toBe('l1')
    expect(status.lastError?.message).toContain('permission denied')
  })
})
