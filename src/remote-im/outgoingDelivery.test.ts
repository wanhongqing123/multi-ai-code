import { afterEach, describe, expect, it, vi } from 'vitest'
import { deliverRemoteImOutgoingText, type RemoteImOutgoingTextEvent } from './outgoingDelivery.js'
import type { TencentImRuntime } from './tencentImClient.js'

const event: RemoteImOutgoingTextEvent = {
  projectId: 'project-1',
  messageId: 42,
  toUserId: 'desktop-b',
  text: 'hello'
}

describe('remote IM outgoing delivery', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  it('marks an outgoing IM message as sent after Tencent SDK delivery succeeds', async () => {
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(async () => undefined)
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingText({ runtime, event, markSent, markFailed })

    expect(runtime.sendText).toHaveBeenCalledWith('desktop-b', 'hello', { messageId: 42 })
    expect(markSent).toHaveBeenCalledWith(42)
    expect(markFailed).not.toHaveBeenCalled()
  })

  it('marks an outgoing IM message as failed when Tencent SDK delivery fails', async () => {
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(async () => {
        throw new Error('SDK send failed')
      })
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingText({ runtime, event, markSent, markFailed })

    expect(markSent).not.toHaveBeenCalled()
    expect(markFailed).toHaveBeenCalledWith(42, 'SDK send failed')
  })

  it('marks an outgoing IM message as failed when no Tencent runtime is connected', async () => {
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingText({ runtime: null, event, markSent, markFailed })

    expect(markSent).not.toHaveBeenCalled()
    expect(markFailed).toHaveBeenCalledWith(42, 'Tencent IM runtime is not connected')
  })

  it('marks an outgoing IM message as failed when Tencent SDK delivery does not settle', async () => {
    vi.useFakeTimers()
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(() => new Promise<void>(() => undefined))
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    const delivery = deliverRemoteImOutgoingText({
      runtime,
      event,
      markSent,
      markFailed,
      sendTimeoutMs: 1000
    })
    await vi.advanceTimersByTimeAsync(1000)
    await delivery

    expect(markSent).not.toHaveBeenCalled()
    expect(markFailed).toHaveBeenCalledWith(42, 'Tencent IM send timed out')
  })
})
