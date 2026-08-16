import { describe, expect, it, vi } from 'vitest'
import { createRemoteDesktopHost } from '../../../src/remote-desktop/host.js'
import type { RemoteDesktopEngine } from '../../../electron/remote-desktop/engine.js'
import {
  REMOTE_DESKTOP_SIGNAL_PREFIX,
  decodeRemoteDesktopSignal,
  encodeRemoteDesktopSignal
} from '../../../electron/remote-desktop/signal.js'

function setup() {
  const startSharing = vi.fn(async () => {})
  const engine: RemoteDesktopEngine = {
    listScreenSources: async () => [
      { sourceId: 's0', sourceName: 'Screen1', isMainScreen: true, width: 1920, height: 1080 }
    ],
    startSharing,
    stopSharing: async () => {},
    setInputGate: () => {}
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

  it('reads settings afresh per invite so switching off mid-session takes effect', async () => {
    // 改远程桌面开关不会重连 IM（connectionKey 里没有它），所以被控端不能把
    // 建连那一刻的设置闭包住——否则用户在设置里关掉它，直到重启前都还能被连。
    const startSharing = vi.fn(async () => {})
    // 整个换掉而不是改字段：真实的失效是 getSettings 读到过期的 props，
    // 每次都新建对象返回。改字段的话连"建连时快照一次"的坏实现都能骗过去。
    let live: { mode: 'unattended' | 'disabled'; allowedUserIds: string[] } = {
      mode: 'unattended',
      allowedUserIds: ['whq-iphone']
    }
    const sendText = vi.fn(async (_toUserId: string, _text: string) => {})
    const host = createRemoteDesktopHost({
      engine: {
        listScreenSources: async () => [
          { sourceId: 's0', sourceName: 'Screen1', isMainScreen: true, width: 1920, height: 1080 }
        ],
        startSharing,
        stopSharing: async () => {},
        setInputGate: () => {}
      },
      getSettings: () => live,
      getCredentials: async () => ({ sdkAppId: 1, userId: 'host-pc', userSig: 'sig' }),
      sendText
    })

    host.handleIncomingText({
      projectId: 'p',
      fromUserId: 'whq-iphone',
      text: encodeRemoteDesktopSignal({ type: 'invite', sessionId: 's-1', roomId: 'r-1' })
    })
    await vi.waitFor(() => expect(startSharing).toHaveBeenCalledTimes(1))
    await host.stopByLocalUser()
    // 到这里对端已收到 accept 和 stop 两条。
    expect(sendText).toHaveBeenCalledTimes(2)

    live = { mode: 'disabled', allowedUserIds: ['whq-iphone'] }
    host.handleIncomingText({
      projectId: 'p',
      fromUserId: 'whq-iphone',
      text: encodeRemoteDesktopSignal({ type: 'invite', sessionId: 's-2', roomId: 'r-2' })
    })

    // 等"这条邀请已处理完"的确凿证据——回信。等 hostState 还是 idle 没有意义：
    // 异步处理开始前它本来就是 idle，断言会立刻通过而什么都没验证。
    await vi.waitFor(() => expect(sendText).toHaveBeenCalledTimes(3))
    expect(decodeRemoteDesktopSignal(sendText.mock.calls[2][1])?.type).toBe('reject')
    expect(startSharing).toHaveBeenCalledTimes(1)
  })

  it('never throws into the caller when signal handling fails', async () => {
    const logger = vi.fn()
    const engine: RemoteDesktopEngine = {
      listScreenSources: async () => {
        throw new Error('boom')
      },
      startSharing: async () => {},
      stopSharing: async () => {},
      setInputGate: () => {}
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
