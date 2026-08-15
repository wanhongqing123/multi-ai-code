import { describe, expect, it, vi } from 'vitest'
import { createRemoteDesktopHost } from '../../../src/remote-desktop/host.js'
import type { RemoteDesktopEngine } from '../../../electron/remote-desktop/engine.js'
import {
  REMOTE_DESKTOP_SIGNAL_PREFIX,
  encodeRemoteDesktopSignal
} from '../../../electron/remote-desktop/signal.js'

function setup() {
  const startSharing = vi.fn(async () => {})
  const engine: RemoteDesktopEngine = {
    listScreenSources: async () => [
      { sourceId: 's0', sourceName: 'Screen1', isMainScreen: true, width: 1920, height: 1080 }
    ],
    startSharing,
    stopSharing: async () => {}
  }
  const sendText = vi.fn(async (_toUserId: string, _text: string) => {})
  const host = createRemoteDesktopHost({
    engine,
    getSettings: () => ({ mode: 'unattended', allowedUserIds: ['whq-iphone'] }),
    getCredentials: async () => ({ sdkAppId: 1, userId: 'host-pc', userSig: 'sig' }),
    sendText
  })
  return { host, sendText, startSharing }
}

describe('renderer remote desktop host', () => {
  it('consumes remote desktop signals so they never reach the AICLI router', async () => {
    const { host } = setup()
    const text = encodeRemoteDesktopSignal({ type: 'invite', sessionId: 's-1', roomId: 'r-1' })

    // 返回 true = 调用方不再往主进程转发。信令若漏进 router，会被当成
    // 普通消息喂给 AICLI，用户会看到一串乱码般的控制文本。
    expect(host.handleIncomingText({ projectId: 'p', fromUserId: 'whq-iphone', text })).toBe(true)
  })

  it('passes ordinary chat through untouched', () => {
    const { host } = setup()
    for (const text of ['你好', '帮我看下构建', '[remote-desktop] 没有前缀的假信令', '']) {
      expect(host.handleIncomingText({ projectId: 'p', fromUserId: 'whq-iphone', text }), text).toBe(
        false
      )
    }
  })

  it('lets a malformed payload fall through as an ordinary message', () => {
    // 前缀对但内容坏：与 MaiChat 的判定一致，当普通消息放行而不是吞掉。
    const { host } = setup()
    const text = REMOTE_DESKTOP_SIGNAL_PREFIX + '{"v":99,"type":"invite"}'
    expect(host.handleIncomingText({ projectId: 'p', fromUserId: 'whq-iphone', text })).toBe(false)
  })

  it('drives the engine and answers the peer for a valid invite', async () => {
    const { host, sendText, startSharing } = setup()
    const text = encodeRemoteDesktopSignal({ type: 'invite', sessionId: 's-1', roomId: 'r-1' })

    host.handleIncomingText({ projectId: 'p', fromUserId: 'whq-iphone', text })
    // handleIncomingText 是同步返回的，副作用在微任务里跑完。
    await vi.waitFor(() => expect(startSharing).toHaveBeenCalledTimes(1))

    expect(sendText).toHaveBeenCalledTimes(1)
    expect(sendText.mock.calls[0][0]).toBe('whq-iphone')
    expect(host.getState().hostState).toBe('sharing')
  })

  it('never throws into the caller when signal handling fails', async () => {
    const logger = vi.fn()
    const engine: RemoteDesktopEngine = {
      listScreenSources: async () => {
        throw new Error('boom')
      },
      startSharing: async () => {},
      stopSharing: async () => {}
    }
    const host = createRemoteDesktopHost({
      engine,
      getSettings: () => ({ mode: 'unattended', allowedUserIds: ['whq-iphone'] }),
      getCredentials: async () => ({ sdkAppId: 1, userId: 'host-pc', userSig: 'sig' }),
      // 回信也失败：构造最坏情况，确认异常不会冒泡到 IM 消息投递路径。
      sendText: async () => {
        throw new Error('send failed')
      },
      logger
    })

    const text = encodeRemoteDesktopSignal({ type: 'invite', sessionId: 's-1', roomId: 'r-1' })
    expect(() =>
      host.handleIncomingText({ projectId: 'p', fromUserId: 'whq-iphone', text })
    ).not.toThrow()
    await vi.waitFor(() => expect(logger).toHaveBeenCalled())
  })
})
