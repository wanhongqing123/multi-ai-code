import { readFileSync } from 'node:fs'
import { inflateSync } from 'node:zlib'
import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  connectTencentImClient,
  extractTencentImAudioMessages,
  extractTencentImFileMessages,
  extractTencentImImageMessages,
  extractTencentImTextMessages,
  extractUserSig,
  generateTencentUserSig,
  extractTencentImRoamedTextMessages,
  getSentRemoteMessageId
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
    getFriendList: vi.fn(async () => ({
      data: [
        { userID: 'friend-a' },
        { userID: 'friend-b' }
      ]
    })),
    createFileMessage: vi.fn((message: unknown) => message),
    createImageMessage: vi.fn((message: unknown) => message),
    createTextMessage: vi.fn((message: unknown) => message),
    sendMessage: vi.fn(async () => ({ code: 0, message: 'OK' }))
  }
  const sdk = {
    create: vi.fn(() => chat),
    EVENT: {
      MESSAGE_RECEIVED: 'messageReceived',
      SDK_READY: 'sdkReady',
      SDK_NOT_READY: 'sdkNotReady',
      FRIEND_LIST_UPDATED: 'friendListUpdated'
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
    sdkMock.chat.getFriendList.mockResolvedValue({
      data: [
        { userID: 'friend-a' },
        { userID: 'friend-b' }
      ]
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

  it('extracts C2C image messages from Tencent message events', () => {
    const messages = extractTencentImImageMessages({
      data: [
        {
          ID: 'image-1',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMImageElem',
          payload: {
            uuid: 'image-uuid-1',
            imageInfoArray: [
              {
                type: 1,
                url: 'https://cos.example.test/thumb.png',
                width: 160,
                height: 120,
                size: 512
              },
              {
                type: 3,
                url: 'https://cos.example.test/original.png',
                width: 640,
                height: 480,
                size: 4096
              }
            ]
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
        remoteMessageId: 'image-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        imageUrl: 'https://cos.example.test/original.png',
        thumbnailUrl: 'https://cos.example.test/thumb.png',
        width: 640,
        height: 480,
        sizeBytes: 4096,
        uuid: 'image-uuid-1',
        fileName: null,
        mimeType: null,
        createdAt: 1782238800000
      }
    ])
  })

  it('extracts C2C markdown/html file messages from Tencent message events', () => {
    const messages = extractTencentImFileMessages({
      data: [
        {
          ID: 'file-1',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMFileElem',
          payload: {
            url: 'https://cos.example.test/report.md',
            uuid: 'file-uuid-1',
            fileName: 'report.md',
            size: 4096
          },
          time: 1782238800
        },
        {
          ID: 'file-2',
          from: 'phone_admin',
          to: 'desktop_bot',
          type: 'TIMFileElem',
          payload: {
            url: 'https://cos.example.test/report.pdf',
            fileName: 'report.pdf'
          }
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
        remoteMessageId: 'file-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        fileUrl: 'https://cos.example.test/report.md',
        sizeBytes: 4096,
        uuid: 'file-uuid-1',
        fileName: 'report.md',
        mimeType: 'text/markdown',
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

  it('lists friend user ids from Tencent IM SDK after login', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )
    sdkMock.chat.getFriendList.mockResolvedValueOnce({
      data: {
        friendList: [
          { userID: ' whq-iphone ' },
          { profile: { userID: 'whq-android' } },
          { userID: 'whq-iphone' }
        ]
      }
    } as any)

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
        masterUserIds: [],
        slaveUserIds: [],
        allowedUserIds: [],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn()
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalled())
    sdkMock.chat.isReady.mockReturnValue(true)
    sdkMock.handlers.get('sdkReady')?.()
    const runtime = await runtimePromise

    expect(runtime.listFriendUserIds).toBeTypeOf('function')
    await expect(runtime.listFriendUserIds!()).resolves.toEqual(['whq-iphone', 'whq-android'])
    expect(sdkMock.chat.getFriendList).toHaveBeenCalledOnce()
  })

  it('notifies when Tencent IM SDK pushes a friend list update', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => ({
        ok: true,
        json: async () => ({ userSig: 'sig-1' })
      }))
    )
    const onFriendListUpdated = vi.fn()
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
        masterUserIds: [],
        slaveUserIds: [],
        allowedUserIds: [],
        outputFlushIntervalMs: 1000,
        outputMaxChunkChars: 1200
      },
      onIncomingText: vi.fn(),
      onFriendListUpdated
    })

    await vi.waitFor(() => expect(sdkMock.chat.login).toHaveBeenCalled())
    sdkMock.chat.isReady.mockReturnValue(true)
    sdkMock.handlers.get('sdkReady')?.()
    await runtimePromise
    sdkMock.handlers.get('friendListUpdated')?.({
      data: [{ userID: 'whq-iphone' }, { profile: { userID: 'whq-android' } }]
    })

    expect(onFriendListUpdated).toHaveBeenCalledWith(['whq-iphone', 'whq-android'])
  })

  it('sends image files through Tencent image messages', async () => {
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
    const file = new File([new Uint8Array([1, 2, 3])], 'photo.png', { type: 'image/png' })

    expect(runtime.sendImage).toBeTypeOf('function')
    await runtime.sendImage!('desktop-b', file, { messageId: 77 })

    expect(sdkMock.chat.createImageMessage).toHaveBeenCalledWith({
      to: 'desktop-b',
      conversationType: 'C2C',
      payload: { file }
    })
    expect(sdkMock.chat.sendMessage).toHaveBeenCalledTimes(1)
  })

  it('sends markdown/html files through Tencent file messages', async () => {
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
    const file = new File([new TextEncoder().encode('# Report')], 'report.md', {
      type: 'text/markdown'
    })

    expect(runtime.sendFile).toBeTypeOf('function')
    await runtime.sendFile!('desktop-b', file, { messageId: 78 })

    expect(sdkMock.chat.createFileMessage).toHaveBeenCalledWith({
      to: 'desktop-b',
      conversationType: 'C2C',
      payload: { file }
    })
    expect(sdkMock.chat.sendMessage).toHaveBeenCalledTimes(1)
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

describe('tencent IM roamed history helpers', () => {
  it('extracts roamed text messages with flow and drops entries without ID', () => {
    const messages = extractTencentImRoamedTextMessages([
      {
        ID: 'roam-1',
        from: 'phone_admin',
        to: 'desktop_bot',
        time: 1700000000,
        flow: 'in',
        payload: { text: '离线消息' }
      },
      {
        ID: 'roam-2',
        from: 'desktop_bot',
        to: 'phone_admin',
        time: 1700000100,
        flow: 'out',
        payload: { text: '我发的' }
      },
      // 无 ID：无法与库中消息去重，必须丢弃
      { from: 'phone_admin', flow: 'in', payload: { text: 'no id' } },
      // 非文本 payload：忽略
      { ID: 'roam-3', from: 'phone_admin', flow: 'in', payload: {} }
    ])

    expect(messages).toEqual([
      {
        remoteMessageId: 'roam-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: '离线消息',
        createdAt: 1700000000000,
        flow: 'in'
      },
      {
        remoteMessageId: 'roam-2',
        fromUserId: 'desktop_bot',
        toUserId: 'phone_admin',
        text: '我发的',
        createdAt: 1700000100000,
        flow: 'out'
      }
    ])
  })

  it('returns empty for non-array roamed input', () => {
    expect(extractTencentImRoamedTextMessages(null)).toEqual([])
    expect(extractTencentImRoamedTextMessages({})).toEqual([])
  })

  it('reads the sent remote message id from sendMessage result', () => {
    expect(getSentRemoteMessageId({ data: { message: { ID: 'tim-msg-9' } } })).toBe('tim-msg-9')
    expect(getSentRemoteMessageId({ data: { message: {} } })).toBeNull()
    expect(getSentRemoteMessageId({})).toBeNull()
    expect(getSentRemoteMessageId(null)).toBeNull()
  })
})
