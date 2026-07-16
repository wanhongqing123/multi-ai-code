import { describe, expect, it } from 'vitest'
import type { RemoteImConfig, RemoteImMessage } from './types.js'
import { createRemoteImRouter } from './router.js'
import { createRemoteImAicliOutputText } from './outputForwarding.js'
import { REMOTE_IM_REPLY_CLOSE_TAG, REMOTE_IM_REPLY_OPEN_TAG } from './replyProtocol.js'

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1400000000,
  desktopUserId: 'desktop_bot',
  desktopRole: 'master',
  userSigMode: 'endpoint',
  userSigEndpoint: 'https://example.test/sig',
  userSigSecretKey: '',
  friendUserIds: [],
  masterUserIds: ['phone_admin'],
  slaveUserIds: [],
  allowedUserIds: ['phone_admin'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

function createMessageStore() {
  const messages: RemoteImMessage[] = []
  let nextId = 1
  return {
    messages,
    create(input: Omit<RemoteImMessage, 'id'>): RemoteImMessage {
      if (input.remoteMessageId !== null) {
        const existing = messages.find(
          (message) =>
            message.provider === input.provider &&
            message.remoteMessageId === input.remoteMessageId
        )
        if (existing) return existing
      }
      const message = { ...input, id: nextId++ }
      messages.push(message)
      return message
    },
    updateStatus(id: number, patch: Partial<RemoteImMessage>) {
      const message = messages.find((item) => item.id === id)
      if (message) Object.assign(message, patch)
      return message ?? null
    }
  }
}

describe('remote IM router', () => {
  it('rejects non-whitelisted users without sending to AICLI', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-1',
      fromUserId: 'intruder',
      toUserId: 'desktop_bot',
      text: 'hello',
      createdAt: 100
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('not allowed')
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({ status: 'rejected', role: 'remote-user' })
  })

  it('wraps whitelisted text and sends it to the current AICLI session', async () => {
    const store = createMessageStore()
    const sentToAicli: Array<{
      sessionId: string
      text: string
      displayText: string | undefined
    }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text, options) => {
        sentToAicli.push({ sessionId, text, displayText: options?.displayText })
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      createReplyId: () => 'reply-fixed',
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-2',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '检查构建',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(result.replyId).toBe('reply-fixed')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]?.sessionId).toBe('session-main')
    expect(sentToAicli[0]?.text).toContain('phone_admin')
    expect(sentToAicli[0]?.text).toContain('<remote-im-reply id="reply-fixed">')
    expect(sentToAicli[0]?.text).toContain('</remote-im-reply id="reply-fixed">')
    expect(sentToAicli[0]?.displayText).toBe('[来自远程 IM：phone_admin]\n检查构建')
    expect(sentToAicli[0]?.displayText).not.toContain('[IM_REPLY]')
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages.map((message) => message.status)).toEqual([
      'sent-to-aicli',
      'streaming'
    ])
  })

  it('does not route the same remote IM message to AICLI twice', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      createReplyId: () => 'reply-fixed',
      store
    })

    const message = {
      projectId: 'project-1',
      remoteMessageId: 'remote-dup-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '同一条 SDK 消息重放',
      createdAt: 100
    }

    const first = await router.handleIncomingText(message)
    const second = await router.handleIncomingText(message)

    expect(first.ok).toBe(true)
    expect(second.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToIm).toEqual(['已发送给当前 AICLI，开始处理。'])
    expect(store.messages.filter((item) => item.direction === 'incoming')).toHaveLength(1)
  })

  it('handles supported slash commands without sending text to AICLI', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: Array<{
      projectId: string
      toUserId: string
      text: string
      messageId: number | undefined
    }> = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (projectId, toUserId, text, options) => {
        sentToIm.push({ projectId, toUserId, text, messageId: options?.messageId })
        return { ok: true }
      },
      handleControlCommand: async ({ command }) => ({
        ok: true,
        text: `handled ${command}`
      }),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-command-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/status',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([
      {
        projectId: 'project-1',
        toUserId: 'phone_admin',
        text: 'handled status',
        messageId: 2
      }
    ])
    expect(store.messages.map((message) => message.content)).toEqual(['/status', 'handled status'])
    expect(store.messages[1]).toMatchObject({
      direction: 'outgoing',
      status: 'streaming',
      sentToImAt: null
    })
  })

  it('returns forwarding ids for /btw control commands', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const handled: Array<{ command: string; args: string; replyId?: string }> = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      createReplyId: () => 'reply-btw-fixed',
      handleControlCommand: async ({ command, args, replyId }) => {
        handled.push({ command, args, replyId })
        return {
          ok: true,
          text: '已提交 /btw 子任务，完成后会通过 IM 回传。'
        }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-btw-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/btw 检查最近一次失败日志',
      createdAt: 100
    })

    expect(result).toEqual({
      ok: true,
      aicliSessionId: 'session-main',
      replyId: 'reply-btw-fixed'
    })
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual(['已提交 /btw 子任务，完成后会通过 IM 回传。'])
    expect(handled).toEqual([
      {
        command: 'btw',
        args: '检查最近一次失败日志',
        replyId: 'reply-btw-fixed'
      }
    ])
  })

  it('keeps unknown slash commands out of the normal AICLI task channel', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-command-2',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/review',
      createdAt: 100
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('unsupported remote IM control command')
    expect(sentToAicli).toEqual([])
    expect(sentToIm[0]).toContain('不支持的 IM 控制命令：/review')
    expect(sentToIm[0]).toContain('/status')
  })

  it('routes slash-leading path messages to the AICLI as normal tasks', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-path-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/etc/hosts 这个文件怎么改',
      createdAt: 100
    })

    // 路径开头的消息是普通任务，不能被未知命令分支拒收。
    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('/etc/hosts 这个文件怎么改')
  })

  it('transcribes trusted audio messages and sends the transcript to the current AICLI session', async () => {
    const store = createMessageStore()
    const sentToAicli: Array<{
      sessionId: string
      text: string
      displayText: string | undefined
    }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text, options) => {
        sentToAicli.push({ sessionId, text, displayText: options?.displayText })
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      transcribeAudio: async () => ({ ok: true, text: '检查一下构建失败原因' }),
      store
    })

    const result = await router.handleIncomingAudio({
      projectId: 'project-1',
      remoteMessageId: 'voice-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      audioUrl: 'https://cos.example.test/voice.amr',
      durationSeconds: 4,
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]?.text).toContain('[语音转文字]')
    expect(sentToAicli[0]?.text).toContain('检查一下构建失败原因')
    expect(sentToAicli[0]?.displayText).toBe(
      '[来自远程 IM：phone_admin]\n[语音转文字]\n检查一下构建失败原因'
    )
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages[0]).toMatchObject({
      role: 'remote-user',
      direction: 'incoming',
      status: 'sent-to-aicli',
      content: '[语音消息 4s]\n[语音转文字]\n检查一下构建失败原因'
    })
  })

  it('records audio transcription failures without sending an empty task to AICLI', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      transcribeAudio: async () => ({
        ok: false,
        error: '本地 Whisper 未配置'
      }),
      store
    })

    const result = await router.handleIncomingAudio({
      projectId: 'project-1',
      remoteMessageId: 'voice-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      audioUrl: 'https://cos.example.test/voice.amr',
      durationSeconds: 4
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('本地 Whisper 未配置')
    expect(sentToAicli).toEqual([])
    expect(sentToIm[0]).toContain('语音转文字失败')
    expect(store.messages[0]).toMatchObject({
      status: 'failed',
      error: '本地 Whisper 未配置',
      content: '[语音消息 4s]'
    })
  })

  it('routes trusted image messages to AICLI with the cached local image path', async () => {
    const store = createMessageStore()
    const sentToAicli: Array<{ sessionId: string; text: string; displayText: string | undefined }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text, options) => {
        sentToAicli.push({ sessionId, text, displayText: options?.displayText })
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      cacheImage: async () => ({
        ok: true,
        attachment: {
          type: 'image',
          localPath: '/tmp/remote-im/images/photo.png',
          remoteUrl: 'https://example.test/photo.png',
          thumbnailUrl: 'https://example.test/thumb.png',
          width: 640,
          height: 480,
          sizeBytes: 4096,
          fileName: 'photo.png',
          mimeType: 'image/png',
          sdkImageId: 'image-1'
        }
      }),
      store
    })

    const result = await router.handleIncomingImage({
      projectId: 'project-1',
      remoteMessageId: 'image-remote-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      imageUrl: 'https://example.test/photo.png',
      thumbnailUrl: 'https://example.test/thumb.png',
      width: 640,
      height: 480,
      fileName: 'photo.png',
      mimeType: 'image/png',
      uuid: 'image-1',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]?.text).toContain('本地路径: /tmp/remote-im/images/photo.png')
    expect(sentToAicli[0]?.displayText).toContain('本地路径: /tmp/remote-im/images/photo.png')
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages[0]).toMatchObject({
      kind: 'image',
      status: 'sent-to-aicli',
      content: '[图片消息] photo.png',
      attachment: {
        type: 'image',
        localPath: '/tmp/remote-im/images/photo.png'
      }
    })
  })

  it('records image download failures without sending AICLI input', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      cacheImage: async () => ({
        ok: false,
        error: 'HTTP 404'
      }),
      store
    })

    const result = await router.handleIncomingImage({
      projectId: 'project-1',
      remoteMessageId: 'image-remote-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      imageUrl: 'https://example.test/missing.png',
      fileName: 'missing.png',
      createdAt: 100
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('HTTP 404')
    expect(sentToAicli).toEqual([])
    expect(sentToIm[0]).toContain('图片下载失败')
    expect(store.messages[0]).toMatchObject({
      kind: 'image',
      status: 'failed',
      error: 'HTTP 404',
      attachment: {
        type: 'image',
        localPath: null,
        remoteUrl: 'https://example.test/missing.png'
      }
    })
  })

  it('rejects image messages from unknown senders before downloading', async () => {
    const store = createMessageStore()
    let downloadAttempted = false
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async () => ({ ok: true }),
      sendImText: async () => ({ ok: true }),
      cacheImage: async () => {
        downloadAttempted = true
        return { ok: false, error: 'should not download' }
      },
      store
    })

    const result = await router.handleIncomingImage({
      projectId: 'project-1',
      remoteMessageId: 'image-remote-1',
      fromUserId: 'intruder',
      toUserId: 'desktop_bot',
      imageUrl: 'https://example.test/photo.png',
      fileName: 'photo.png',
      createdAt: 100
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('not allowed')
    expect(downloadAttempted).toBe(false)
    expect(store.messages[0]).toMatchObject({
      kind: 'image',
      status: 'rejected',
      error: 'sender not allowed'
    })
  })

  it('routes configured friend messages to the current AICLI session', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => ({
        ...config,
        friendUserIds: ['friend-a'],
        masterUserIds: [],
        slaveUserIds: [],
        allowedUserIds: ['friend-a']
      }),
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-friend-1',
      fromUserId: 'friend-a',
      toUserId: 'desktop_bot',
      text: 'hello from friend',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('hello from friend')
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages[0]).toMatchObject({
      role: 'remote-user',
      status: 'sent-to-aicli',
      content: 'hello from friend'
    })
  })

  it('lets a slave route tasks received from a configured master', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => ({
        ...config,
        desktopRole: 'slave',
        masterUserIds: ['master-a'],
        slaveUserIds: ['slave-c'],
        allowedUserIds: ['master-a', 'slave-c']
      }),
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'master-a',
      toUserId: 'desktop_bot',
      text: '执行构建'
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages[0]).toMatchObject({ status: 'sent-to-aicli' })
  })

  it('routes legacy slave-to-master messages as trusted friend tasks', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => ({
        ...config,
        masterUserIds: ['master-a'],
        slaveUserIds: ['slave-b'],
        allowedUserIds: ['master-a', 'slave-b']
      }),
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'slave-b',
      toUserId: 'desktop_bot',
      text: '主动发起一个任务'
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages[0]).toMatchObject({
      status: 'sent-to-aicli',
      error: null
    })
  })

  it('records marked slave AICLI output on the master without executing it as a task', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => ({
        ...config,
        masterUserIds: ['master-a'],
        slaveUserIds: ['slave-b'],
        allowedUserIds: ['master-a', 'slave-b']
      }),
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'slave-b',
      toUserId: 'desktop_bot',
      text: createRemoteImAicliOutputText('处理完成')
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      status: 'received',
      content: '处理完成'
    })
  })

  it('routes legacy slave-to-slave messages as trusted friend tasks', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => ({
        ...config,
        desktopRole: 'slave',
        masterUserIds: ['master-a'],
        slaveUserIds: ['slave-c'],
        allowedUserIds: ['master-a', 'slave-c']
      }),
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'slave-c',
      toUserId: 'desktop_bot',
      text: '互相处理一下'
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages[0]).toMatchObject({ status: 'sent-to-aicli', error: null })
  })

  it('reports missing AICLI session to the phone', async () => {
    const store = createMessageStore()
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => null,
      sendUser: async () => ({ ok: true }),
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: '检查构建'
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('No running AICLI')
    expect(sentToIm[0]).toContain('当前没有运行中的 AICLI')
    expect(store.messages[0]).toMatchObject({ status: 'failed' })
  })

  it('records remote IM system replies without sending another reply', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => null,
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: '当前没有运行中的 AICLI。'
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      status: 'received',
      content: '当前没有运行中的 AICLI。'
    })
  })

  it('records marked remote AICLI output without sending it to the local AICLI', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: createRemoteImAicliOutputText('build passed')
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      direction: 'incoming',
      status: 'received',
      content: 'build passed'
    })
  })

  it('records nested remote IM output without routing it back into AICLI', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const nestedOutput = '[来自远程 IM：desktop_bot]\nClaude Code output'
    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: nestedOutput
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      direction: 'incoming',
      status: 'received',
      content: nestedOutput
    })
  })

  it('records operation completion notifications without forwarding them to AICLI', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: '操作已完成。'
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      status: 'received',
      content: '操作已完成。'
    })
  })
})
