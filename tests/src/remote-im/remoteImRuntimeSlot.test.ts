import { describe, expect, it, vi } from 'vitest'
import { createRemoteImRuntimeSlot } from '../../../src/remote-im/remoteImRuntimeSlot.js'

function runtime() {
  return {
    disconnect: vi.fn(async () => undefined)
  }
}

describe('remote IM runtime slot', () => {
  it('does not let an old cleanup disconnect a newer runtime', async () => {
    const slot = createRemoteImRuntimeSlot()
    const oldRuntime = runtime()
    const newRuntime = runtime()

    slot.setCurrent(oldRuntime)
    slot.setCurrent(newRuntime)

    await slot.disconnectOwned(oldRuntime)

    expect(oldRuntime.disconnect).toHaveBeenCalledTimes(1)
    expect(newRuntime.disconnect).not.toHaveBeenCalled()
    expect(slot.getCurrent()).toBe(newRuntime)
  })

  it('clears the current runtime before awaiting its async disconnect', async () => {
    const slot = createRemoteImRuntimeSlot()
    let finishDisconnect!: () => void
    const currentRuntime = {
      disconnect: vi.fn(
        () =>
          new Promise<void>((resolve) => {
            finishDisconnect = resolve
          })
      )
    }

    slot.setCurrent(currentRuntime)
    const disconnecting = slot.disconnectCurrent()

    expect(slot.getCurrent()).toBeNull()
    finishDisconnect()
    await disconnecting
  })

  it('resolves waiters when a runtime becomes current', async () => {
    vi.useFakeTimers()
    try {
      const slot = createRemoteImRuntimeSlot()
      const nextRuntime = runtime()

      const waiting = slot.waitForCurrent(1000)
      await vi.advanceTimersByTimeAsync(250)
      slot.setCurrent(nextRuntime)

      await expect(waiting).resolves.toBe(nextRuntime)
    } finally {
      vi.useRealTimers()
    }
  })

  it('rejects waiters when no runtime becomes current before the timeout', async () => {
    vi.useFakeTimers()
    try {
      const slot = createRemoteImRuntimeSlot()
      const waiting = slot.waitForCurrent(1000)
      const assertion = expect(waiting).rejects.toThrow('IM 运行时未连接')

      await vi.advanceTimersByTimeAsync(1000)

      await assertion
    } finally {
      vi.useRealTimers()
    }
  })
})
