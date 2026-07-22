import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  deliverRemoteImOutgoingImage,
  deliverRemoteImOutgoingFile,
  deliverRemoteImOutgoingText,
  type RemoteImOutgoingFileEvent,
  type RemoteImOutgoingImageEvent,
  type RemoteImOutgoingTextEvent
} from '../../../src/remote-im/outgoingDelivery.js'
import type { TencentImRuntime } from '../../../src/remote-im/tencentImClient.js'

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
    expect(markSent).toHaveBeenCalledWith(42, null)
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
    expect(markFailed).toHaveBeenCalledWith(42, 'IM 运行时未连接')
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
    expect(markFailed).toHaveBeenCalledWith(42, 'IM 发送超时')
  })

  it('marks an outgoing image as sent after SDK delivery succeeds', async () => {
    const imageEvent: RemoteImOutgoingImageEvent = {
      projectId: 'project-1',
      messageId: 88,
      toUserId: 'desktop-b',
      fileToken: 'token-1'
    }
    const file = new File([new Uint8Array([1])], 'photo.png', { type: 'image/png' })
    const sendImage = vi.fn<NonNullable<TencentImRuntime['sendImage']>>(async () => undefined)
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(async () => undefined),
      sendImage
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingImage({
      runtime,
      event: imageEvent,
      resolveFile: () => file,
      markSent,
      markFailed
    })

    expect(sendImage).toHaveBeenCalledWith('desktop-b', file, { messageId: 88 })
    expect(markSent).toHaveBeenCalledWith(88, null)
    expect(markFailed).not.toHaveBeenCalled()
  })

  it('delivers an outgoing image from an inline IPC file payload', async () => {
    const imageEvent: RemoteImOutgoingImageEvent = {
      projectId: 'project-1',
      messageId: 88,
      toUserId: 'desktop-b',
      fileName: 'desktop_shot.png',
      mimeType: 'image/png',
      fileBytes: new Uint8Array([1, 2, 3])
    }
    const sendImage = vi.fn<NonNullable<TencentImRuntime['sendImage']>>(async () => undefined)
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(async () => undefined),
      sendImage
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingImage({
      runtime,
      event: imageEvent,
      resolveFile: () => null,
      markSent,
      markFailed
    })

    expect(sendImage).toHaveBeenCalledTimes(1)
    const sentFile = sendImage.mock.calls[0]![1]
    expect(sentFile).toBeInstanceOf(File)
    expect(sentFile.name).toBe('desktop_shot.png')
    expect(sentFile.type).toBe('image/png')
    expect(sendImage).toHaveBeenCalledWith('desktop-b', sentFile, { messageId: 88 })
    expect(markSent).toHaveBeenCalledWith(88, null)
    expect(markFailed).not.toHaveBeenCalled()
  })

  it('marks an outgoing image as failed when its file token is missing', async () => {
    const imageEvent: RemoteImOutgoingImageEvent = {
      projectId: 'project-1',
      messageId: 88,
      toUserId: 'desktop-b',
      fileToken: 'missing-token'
    }
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(async () => undefined),
      sendImage: vi.fn(async () => undefined)
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingImage({
      runtime,
      event: imageEvent,
      resolveFile: () => null,
      markSent,
      markFailed
    })

    expect(runtime.sendImage).not.toHaveBeenCalled()
    expect(markSent).not.toHaveBeenCalled()
    expect(markFailed).toHaveBeenCalledWith(88, '图片文件已失效，请重新选择')
  })

  it('marks an outgoing image as failed when no runtime is connected', async () => {
    const imageEvent: RemoteImOutgoingImageEvent = {
      projectId: 'project-1',
      messageId: 88,
      toUserId: 'desktop-b',
      fileToken: 'token-1'
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingImage({
      runtime: null,
      event: imageEvent,
      resolveFile: vi.fn(),
      markSent,
      markFailed
    })

    expect(markSent).not.toHaveBeenCalled()
    expect(markFailed).toHaveBeenCalledWith(88, 'IM 运行时未连接')
  })

  it('delivers an outgoing markdown/html file from an inline IPC file payload', async () => {
    const fileEvent: RemoteImOutgoingFileEvent = {
      projectId: 'project-1',
      messageId: 89,
      toUserId: 'desktop-b',
      fileName: 'report.md',
      mimeType: 'text/markdown',
      fileBytes: new TextEncoder().encode('# Report')
    }
    const sendFile = vi.fn<NonNullable<TencentImRuntime['sendFile']>>(async () => undefined)
    const runtime: TencentImRuntime = {
      disconnect: vi.fn(),
      sendText: vi.fn(async () => undefined),
      sendFile
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingFile({
      runtime,
      event: fileEvent,
      markSent,
      markFailed
    })

    expect(sendFile).toHaveBeenCalledTimes(1)
    const sentFile = sendFile.mock.calls[0]![1]
    expect(sentFile).toBeInstanceOf(File)
    expect(sentFile.name).toBe('report.md')
    expect(sentFile.type).toBe('text/markdown')
    expect(sendFile).toHaveBeenCalledWith('desktop-b', sentFile, { messageId: 89 })
    expect(markSent).toHaveBeenCalledWith(89, null)
    expect(markFailed).not.toHaveBeenCalled()
  })

  it('marks an outgoing file as failed when no runtime is connected', async () => {
    const fileEvent: RemoteImOutgoingFileEvent = {
      projectId: 'project-1',
      messageId: 89,
      toUserId: 'desktop-b',
      fileName: 'report.md',
      mimeType: 'text/markdown',
      fileBytes: new TextEncoder().encode('# Report')
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingFile({
      runtime: null,
      event: fileEvent,
      markSent,
      markFailed
    })

    expect(markSent).not.toHaveBeenCalled()
    expect(markFailed).toHaveBeenCalledWith(89, 'IM 运行时未连接')
  })

  it('passes the SDK-confirmed remote message id to markSent for dedup backfill', async () => {
    const runtime = {
      disconnect: vi.fn(async () => undefined),
      sendText: vi.fn(async () => ({ remoteMessageId: 'tim-msg-42' }))
    }
    const markSent = vi.fn()
    const markFailed = vi.fn()

    await deliverRemoteImOutgoingText({
      runtime,
      event: { projectId: 'p1', toUserId: 'desktop-b', text: 'hello', messageId: 42 },
      markSent,
      markFailed
    })

    expect(markSent).toHaveBeenCalledWith(42, 'tim-msg-42')
    expect(markFailed).not.toHaveBeenCalled()
  })
})
