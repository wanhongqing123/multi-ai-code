import { readFileSync } from 'node:fs'
import { inflateSync } from 'node:zlib'
import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  connectTencentImClient,
  extractTencentImAudioMessages,
  extractTencentImTextMessages,
  extractUserSig,
  generateTencentUserSig
} from './tencentImClient.js'

const sdkMock = vi.hoisted(() => {
  const handlers = new Map<string, (...args: unknown[]) => void>()
  let loginUser = ''
  const chat = {
    setLogLevel: vi.fn(),
    on: vi.fn((event: string, handler: (...args: unknown[]) => void) => {
      handlers.set(event, handler)
    }),
    off: vi.fn(),
    isReady: vi.fn(() => false),
    getLoginUser: vi.fn(() => loginUser),
    login: vi.fn(async ({ userID }: { userID: string }) => {
      loginUser = userID
      return { code: 0, message: 'OK' }
    }),
    logout: vi.fn(async () => {
      loginUser = ''
    }),
    destroy: vi.fn(async () => undefined),
    createTextMessage: vi.fn((message: unknown) => message),
    sendMessage: vi.fn(async () => ({ code: 0, message: 'OK' }))
  }
  const sdk = {
    create: vi.fn(() => chat),
    EVENT: {
      MESSAGE_RECEIVED: 'messageReceived',
      SDK_READY: 'sdkReady',
      SDK_NOT_READY: 'sdkNotReady'
    },
    TYPES: {
      CONV_C2C: 'C2C'
    }
  }
  return {
    chat,
    handlers,
    sdk,
    setLoginUser: (userID: string) => {
      loginUser = userID
    }
  }
})

vi.mock('@tencentcloud/lite-chat', () => ({
  default: sdkMock.sdk
}))

describe('tencent IM client helpers', () => {
  afterEach(() => {
    vi.restoreAllMocks()
    sdkMock.handlers.clear()
    sdkMock.setLoginUser('')
    sdkMock.chat.isReady.mockReturnValue(false)
    sdkMock.chat.login.mockImplementation(async ({ userID }: { userID: string }) => {
      sdkMock.setLoginUser(userID)
      return { code: 0, message: 'OK' }
    })
    sdkMock.chat.sendMessage.mockResolvedValue({ code: 0, message: 'OK' })
  })

  it('extracts UserSig from supported endpoint response shapes', () => {
    expect(extractUserSig({ userSig: 'sig-1' })).toBe('sig-1')
    expect(extractUserSig({ ok: true, userSig: 'sig-2' })).toBe('sig-2')
    expect(() => extractUserSig({ ok: false })).toThrow('凭证接口响应缺少有效凭证')
  })

  it('extracts C2C text messages from Tencent message events', () => {
    const messages = extractTencentImTextMessages({
      data: [
        {
          ID: 'msg-1',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMTextElem',
          payload: { text: 'hello' },
          time: 1782238800
        },
        {
          ID: 'msg-2',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMImageElem',
          payload: {}
        }
      ]
    })

    expect(messages).toEqual([
      {
        remoteMessageId: 'msg-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: 'hello',
        createdAt: 1782238800000
      }
    ])
  })

  it('extracts C2C audio messages from Tencent message events', () => {
    const messages = extractTencentImAudioMessages({
      data: [
        {
          ID: 'voice-1',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMSoundElem',
          payload: {
            url: 'https://cos.example.test/voice.amr',
            uuid: 'sound-uuid-1',
            duration: 4,
            size: 2048
          },
          time: 1782238800
        },
        {
          ID: 'msg-2',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMTextElem',
          payload: { text: 'hello' }
        }
      ]
    })

    expect(messages).toEqual([
      {
        remoteMessageId: 'voice-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        audioUrl: 'https://cos.example.test/voice.amr',
        durationSeconds: 4,
        sizeBytes: 2048,
        uuid: 'sound-uuid-1',
        createdAt: 1782238800000
      }
    ])
  })

  it('generates a Tencent UserSig locally from SDKAppID, UserID, and SecretKey', async () => {
    const userSig = await generateTencentUserSig({
      sdkAppId: 1400704311,
      userId: 'desktop-a',
      secretKey: 'local-test-secret',
      nowSeconds: 1782238800
    })
    const base64 = userSig.replace(/\*/g, '+').replace(/-/g, '/').replace(/_/g, '=')
    const payload = JSON.parse(inflateSync(Buffer.from(base64, 'base64')).toString('utf8'))

    expect(payload).toMatchObject({
      'TLS.ver': '2.0',
      'TLS.identifier': 'desktop-a',
      'TLS.sdkappid': 1400704311,
      'TLS.expire': 604800,
      'TLS.time': 1782238800
    })
    expect(typeof payload['TLS.sig']).toBe('string')
    expect(payload['TLS.sig'].length).toBeGreaterThan(10)
  })

  it('uses a Vite-statically analyzable Tencent IM SDK import', () => {
    const source = readFileSync(new URL('./tencentImClient.ts', import.meta.url), 'utf8')

    expect(source).toContain("import('@tencentcloud/lite-chat')")
    expect(source).not.toContain('@vite-ignore')
  })

  it('waits for SDK_READY before reporting the Tencent IM runtime as connected', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )

    let resolved = false
    const runtimePromise = connectTencentImClient({
      projectId: 'project-1',
      config: {
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: 1400704311,
        desktopUserId: 'desktop-a',
        desktopRole: 'master',
        userSigMode: 'endpoint',
        userSigEndpoint: 'https://example.test/sig',
        userSigSecretKey: '',
        friendUserIds: [],
        masterUserIds: ['desktop-b'],
        slaveUserIds: [],
        allowedUserIds: ['desktop-b'],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn()
    }).then((runtime) => {
      resolved = true
      return runtime
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalled())

    expect(sdkMock.chat.login).toHaveBeenCalledWith({
      userID: 'desktop-a',
      userSig: 'sig-1'
    })
    expect(resolved).toBe(false)

    sdkMock.handlers.get('sdkReady')?.()
    await runtimePromise

    expect(resolved).toBe(true)
  })

  it('rejects sendText when Tencent IM SDK returns a non-zero send code', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )

    const runtimePromise = connectTencentImClient({
      projectId: 'project-1',
      config: {
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: 1400704311,
        desktopUserId: 'desktop-a',
        desktopRole: 'master',
        userSigMode: 'endpoint',
        userSigEndpoint: 'https://example.test/sig',
        userSigSecretKey: '',
        friendUserIds: [],
        masterUserIds: ['desktop-b'],
        slaveUserIds: [],
        allowedUserIds: ['desktop-b'],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn()
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalled())
    sdkMock.chat.isReady.mockReturnValue(true)
    sdkMock.handlers.get('sdkReady')?.()
    const runtime = await runtimePromise
    sdkMock.chat.sendMessage.mockResolvedValueOnce({ code: 10017, message: 'not friends' })

    await expect(runtime.sendText('desktop-b', 'hello')).rejects.toThrow(
      'IM 发送失败 (10017): not friends'
    )
  })

  it('does not relogin before sending after SDK_READY only because isReady later reports false', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )

    const runtimePromise = connectTencentImClient({
      projectId: 'project-1',
      config: {
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: 1400704311,
        desktopUserId: 'desktop-a',
        desktopRole: 'master',
        userSigMode: 'endpoint',
        userSigEndpoint: 'https://example.test/sig',
        userSigSecretKey: '',
        friendUserIds: [],
        masterUserIds: ['desktop-b'],
        slaveUserIds: [],
        allowedUserIds: ['desktop-b'],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn()
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalledTimes(1))
    sdkMock.handlers.get('sdkReady')?.()
    const runtime = await runtimePromise
    sdkMock.chat.isReady.mockReturnValue(false)
    sdkMock.chat.login.mockRejectedValueOnce(new Error('login failed. error: {"code":2025}'))

    await runtime.sendText('desktop-b', 'hello')

    expect(sdkMock.chat.login).toHaveBeenCalledTimes(1)
    expect(sdkMock.chat.sendMessage).toHaveBeenCalledTimes(1)
  })

  it('relogs in before sending after the Tencent IM SDK emits SDK_NOT_READY', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )

    const runtimePromise = connectTencentImClient({
      projectId: 'project-1',
      config: {
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: 1400704311,
        desktopUserId: 'desktop-a',
        desktopRole: 'master',
        userSigMode: 'endpoint',
        userSigEndpoint: 'https://example.test/sig',
        userSigSecretKey: '',
        friendUserIds: [],
        masterUserIds: ['desktop-b'],
        slaveUserIds: [],
        allowedUserIds: ['desktop-b'],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn()
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalledTimes(1))
    sdkMock.handlers.get('sdkReady')?.()
    const runtime = await runtimePromise
    sdkMock.handlers.get('sdkNotReady')?.()

    const sendPromise = runtime.sendText('desktop-b', 'hello')
    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalledTimes(2))
    sdkMock.handlers.get('sdkReady')?.()
    await sendPromise

    expect(sdkMock.chat.sendMessage).toHaveBeenCalledTimes(1)
  })

  it('rejects connect when Tencent IM login returns a non-zero code', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )
    sdkMock.chat.login.mockResolvedValueOnce({ code: 2024, message: '用户未登录' })

    await expect(
      connectTencentImClient({
        projectId: 'project-1',
        config: {
          enabled: true,
          provider: 'tencent-im',
          sdkAppId: 1400704311,
          desktopUserId: 'desktop-a',
          desktopRole: 'master',
          userSigMode: 'endpoint',
          userSigEndpoint: 'https://example.test/sig',
          userSigSecretKey: '',
          friendUserIds: [],
          masterUserIds: ['desktop-b'],
          slaveUserIds: [],
          allowedUserIds: ['desktop-b'],
          outputFlushIntervalMs: 1000,
          outputMaxChunkChars: 1200
        },
        onIncomingText: vi.fn()
      })
    ).rejects.toThrow('IM 登录失败 (2024): 用户未登录')
  })

  it('logs in with a locally generated UserSig when SecretKey mode is selected', async () => {
    const fetch = vi.fn()
    vi.stubGlobal('fetch', fetch)

    const runtimePromise = connectTencentImClient({
      projectId: 'project-1',
      config: {
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: 1400704311,
        desktopUserId: 'desktop-a',
        desktopRole: 'master',
        userSigMode: 'secret-key',
        userSigEndpoint: '',
        userSigSecretKey: 'local-test-secret',
        friendUserIds: [],
        masterUserIds: ['desktop-b'],
        slaveUserIds: [],
        allowedUserIds: ['desktop-b'],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn()
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalled())
    sdkMock.handlers.get('sdkReady')?.()
    await runtimePromise

    expect(fetch).not.toHaveBeenCalled()
    const loginInput = (sdkMock.chat.login.mock.calls.at(-1) as
      | Array<{ userID: string; userSig: string }>
      | undefined)?.[0]
    expect(loginInput).toMatchObject({
      userID: 'desktop-a'
    })
    expect(loginInput?.userSig).toEqual(expect.any(String))
  })

  it('emits runtime diagnostics around Tencent IM send attempts', async () => {
    const onRuntimeLog = vi.fn()
    const runtimePromise = connectTencentImClient({
      projectId: 'project-1',
      config: {
        enabled: true,
        provider: 'tencent-im',
        sdkAppId: 1600148979,
        desktopUserId: 'desktop-a',
        desktopRole: 'master',
        userSigMode: 'secret-key',
        userSigEndpoint: '',
        userSigSecretKey: 'local-test-secret',
        friendUserIds: [],
        masterUserIds: [],
        slaveUserIds: ['desktop-b'],
        allowedUserIds: ['desktop-b'],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn(),
      onRuntimeLog
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalled())
    sdkMock.handlers.get('sdkReady')?.()
    const runtime = await runtimePromise

    await runtime.sendText('desktop-b', 'hello', { messageId: 42 })

    expect(onRuntimeLog).toHaveBeenCalledWith(
      expect.objectContaining({
        event: 'send:start',
        projectId: 'project-1',
        sdkAppId: 1600148979,
        desktopUserId: 'desktop-a',
        peerUserId: 'desktop-b',
        messageId: 42
      })
    )
    expect(onRuntimeLog).toHaveBeenCalledWith(
      expect.objectContaining({
        event: 'send:resolved',
        projectId: 'project-1',
        sdkAppId: 1600148979,
        desktopUserId: 'desktop-a',
        peerUserId: 'desktop-b',
        messageId: 42,
        detail: {
          code: 0,
          message: 'OK'
        }
      })
    )
    expect(JSON.stringify(onRuntimeLog.mock.calls)).not.toContain('local-test-secret')
  })
})
