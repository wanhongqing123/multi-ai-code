import { describe, expect, it } from 'vitest'
import type { RemoteImConfig, RemoteImMessage } from '../../../electron/remote-im/types.js'
import {
  createRemoteImRouter,
  type RemoteImAicliOutputRoute
} from '../../../electron/remote-im/router.js'
import {
  createRemoteImAicliOutputText
} from '../../../electron/remote-im/outputForwarding.js'
import { REMOTE_IM_REPLY_CLOSE_TAG, REMOTE_IM_REPLY_OPEN_TAG } from '../../../electron/remote-im/replyProtocol.js'

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
,
  remoteDesktopMode: 'disabled',
  remoteDesktopControl: false
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
    },
    findByRemoteMessageId(provider: RemoteImMessage['provider'], remoteMessageId: string) {
      return (
        messages.find(
          (message) =>
            message.provider === provider && message.remoteMessageId === remoteMessageId
        ) ?? null
      )
    }
  }
}

describe('remote IM router', () => {
  it('consumes remote desktop protocol frames before machine collaboration routing', async () => {
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
      remoteMessageId: 'remote-desktop-signal-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '\u2063\u200B[remote-desktop]{"v":1,"type":"stop","sessionId":"s1"}',
      origin: 'machine',
      createdAt: 100
    })

    expect(result).toEqual({ ok: true })
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages).toEqual([])
  })

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
      replyId: string | undefined
      taskId: string | undefined
    }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text, options) => {
        sentToAicli.push({
          sessionId,
          text,
          displayText: options?.displayText,
          replyId: options?.replyId,
          taskId: options?.taskId
        })
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
      origin: 'human',
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
    expect(sentToAicli[0]?.replyId).toBe('reply-fixed')
    expect(sentToAicli[0]?.taskId).toBe('remote-im-task-reply-fixed')
    expect(sentToIm).toEqual([])
    expect(store.messages.map((message) => message.status)).toEqual(['sent-to-aicli'])
  })

  it.each(['codex', 'opencode'] as const)(
    'uses explicit task correlation for %s without adding reply markers to the prompt',
    async (sourceKind) => {
      const store = createMessageStore()
      const starts: Array<{ replyId?: string; taskId: string }> = []
      const submissions: Array<{
        text: string
        inputOrigin?: 'remote-im' | 'remote-im-machine' | 'local'
        replyId?: string
        taskId?: string
      }> = []
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => ({
          sessionId: 'session-main',
          targetRepo: 'repo',
          sourceKind
        }),
        onAicliOutputStart: ({ replyId, taskId }) => starts.push({ replyId, taskId }),
        sendUser: async (_sessionId, text, options) => {
          submissions.push({
            text,
            inputOrigin: options?.inputOrigin,
            replyId: options?.replyId,
            taskId: options?.taskId
          })
          return { ok: true }
        },
        sendImText: async () => ({ ok: true }),
        createReplyId: () => 'source-reply-id',
        store
      })

      const result = await router.handleIncomingText({
        projectId: 'project-1',
        remoteMessageId: `remote-${sourceKind}`,
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: '检查构建',
        origin: 'human',
        createdAt: 100
      })

      expect(result).toMatchObject({ ok: true, aicliSessionId: 'session-main' })
      expect(result.replyId).toBe('source-reply-id')
      expect(starts).toHaveLength(1)
      expect(starts[0]?.replyId).toBe('source-reply-id')
      expect(starts[0]?.taskId).toMatch(/^remote-im-route-/)
      expect(submissions).toEqual([
        {
          text: expect.stringContaining('检查构建'),
          inputOrigin: 'remote-im',
          replyId: 'source-reply-id',
          taskId: starts[0]?.taskId
        }
      ])
      expect(submissions[0]?.text).not.toContain('[IM_REPLY]')
      expect(submissions[0]?.text).not.toContain('<remote-im-reply')
    }
  )

  it('does not submit a second source task when route admission rejects it', async () => {
    const store = createMessageStore()
    const routes: RemoteImAicliOutputRoute[] = []
    const submissions: Array<{ replyId?: string; taskId?: string }> = []
    const sentToIm: string[] = []
    let replySequence = 0
    const multiUserConfig: RemoteImConfig = {
      ...config,
      friendUserIds: ['phone_b'],
      allowedUserIds: ['phone_admin', 'phone_b']
    }
    const router = createRemoteImRouter({
      getConfig: () => multiUserConfig,
      resolveSession: () => ({
        sessionId: 'session-main',
        targetRepo: 'repo',
        sourceKind: 'codex'
      }),
      createReplyId: () => `source-reply-${++replySequence}`,
      authorizeAicliOutputStart: () =>
        routes.length === 0
          ? { ok: true }
          : { ok: false, error: '当前 AICLI 会话正在处理其他远程任务。' },
      onAicliOutputStart: (route) => routes.push(route),
      sendUser: async (_sessionId, _text, options) => {
        submissions.push({ replyId: options?.replyId, taskId: options?.taskId })
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      store
    })

    const first = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-a',
      fromUserId: 'phone_admin',
      text: '任务 A',
      origin: 'human',
      createdAt: 100
    })
    const second = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-b',
      fromUserId: 'phone_b',
      text: '任务 B',
      origin: 'human',
      createdAt: 101
    })

    expect(first.ok).toBe(true)
    expect(second).toEqual({
      ok: false,
      error: '当前 AICLI 会话正在处理其他远程任务。'
    })
    expect(routes).toHaveLength(1)
    expect(routes[0]).toMatchObject({ toUserId: 'phone_admin', replyId: 'source-reply-1' })
    expect(submissions).toEqual([
      { replyId: routes[0]?.replyId, taskId: routes[0]?.taskId }
    ])
    expect(sentToIm.at(-1)).toContain('发送给 AICLI 失败：')
  })

  it('submits a machine input immediately without creating an output route', async () => {
    const store = createMessageStore()
    const routes: RemoteImAicliOutputRoute[] = []
    const submissions: Array<{
      inputOrigin?: 'remote-im' | 'remote-im-machine' | 'local'
      text: string
    }> = []
    let admissionCalls = 0
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({
        sessionId: 'session-main',
        targetRepo: 'repo',
        sourceKind: 'codex'
      }),
      authorizeAicliOutputStart: () => {
        admissionCalls += 1
        return { ok: true }
      },
      onAicliOutputStart: (route) => routes.push(route),
      sendUser: async (_sessionId, text, options) => {
        submissions.push({ text, inputOrigin: options?.inputOrigin })
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'machine-queued-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '继续协作',
      origin: 'machine',
      createdAt: 100
    })

    expect(result).toMatchObject({ ok: true, aicliSessionId: 'session-main' })
    expect(result).not.toHaveProperty('queued')
    expect(result).not.toHaveProperty('replyId')
    expect(admissionCalls).toBe(0)
    expect(routes).toEqual([])
    expect(submissions).toHaveLength(1)
    expect(submissions[0]?.inputOrigin).toBe('remote-im-machine')
    expect(submissions[0]?.text).toContain('继续协作')
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      status: 'sent-to-aicli',
      sessionId: 'session-main'
    })
  })

  it('submits a human input immediately with an automatic output route', async () => {
    const store = createMessageStore()
    const routes: RemoteImAicliOutputRoute[] = []
    const submissions: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({
        sessionId: 'session-main',
        targetRepo: 'repo',
        sourceKind: 'claude'
      }),
      authorizeAicliOutputStart: () => ({ ok: true }),
      onAicliOutputStart: (route) => routes.push(route),
      sendUser: async (_sessionId, text) => {
        submissions.push(text)
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      createReplyId: () => 'human-reply',
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'human-queued-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '人工远程任务',
      origin: 'human',
      createdAt: 100
    })

    expect(result).toMatchObject({ ok: true, replyId: 'human-reply' })
    expect(result).not.toHaveProperty('queued')
    expect(routes).toHaveLength(1)
    expect(routes[0]).toMatchObject({ autoReplyToIm: true })
    expect(submissions).toHaveLength(1)
    expect(submissions[0]).not.toContain('本轮 AICLI 的普通输出不会自动发送到 IM')
    expect(store.messages[0]).toMatchObject({ status: 'sent-to-aicli' })
  })

  it('keeps a fresh reply identity for each human continuation', async () => {
    const store = createMessageStore()
    const submitted: Array<{ text: string; replyId?: string; taskId?: string }> = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo', sourceKind: 'claude' }),
      authorizeAicliOutputStart: (route) => {
        route.taskId = 'active-task'
        route.continuation = true
        return { ok: true }
      },
      sendUser: async (_sessionId, text, options) => {
        submitted.push({ text, replyId: options?.replyId, taskId: options?.taskId })
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      createReplyId: () => 'fresh-continuation-reply',
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'human-steer-identity',
      fromUserId: 'phone_admin',
      text: '中途补充',
      origin: 'human'
    })

    expect(result).toMatchObject({ ok: true, replyId: 'fresh-continuation-reply' })
    expect(submitted[0]).toMatchObject({
      replyId: 'fresh-continuation-reply',
      taskId: 'active-task'
    })
    expect(submitted[0]?.text).toContain(
      '<remote-im-reply id="fresh-continuation-reply">'
    )
  })

  it('rolls back only the rejected continuation reservation without cancelling its owner', async () => {
    const store = createMessageStore()
    const cancelled: RemoteImAicliOutputRoute[] = []
    const rejected: RemoteImAicliOutputRoute[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo', sourceKind: 'codex' }),
      authorizeAicliOutputStart: (route) => {
        route.replyId = 'active-reply'
        route.taskId = 'active-task'
        route.continuation = true
        return { ok: true }
      },
      sendUser: async () => ({ ok: false, error: 'steer rejected' }),
      onAicliOutputCancel: (route) => cancelled.push(route),
      onAicliInputRejected: (route) => rejected.push(route),
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'human-steer-rejected',
      fromUserId: 'phone_admin',
      text: '中途补充',
      origin: 'human'
    })

    expect(result).toEqual({ ok: false, error: 'steer rejected' })
    expect(cancelled).toEqual([])
    expect(rejected).toHaveLength(1)
    expect(rejected[0]).toMatchObject({ continuation: true })
  })

  it('defaults missing origin to a silent machine route', async () => {
    const store = createMessageStore()
    const routes: RemoteImAicliOutputRoute[] = []
    const inputs: Array<{
      inputOrigin?: 'remote-im' | 'remote-im-machine' | 'local'
      text: string
    }> = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      authorizeAicliOutputStart: () => ({ ok: true }),
      onAicliOutputStart: (route) => routes.push(route),
      sendUser: async (_sessionId, text, options) => {
        inputs.push({ text, inputOrigin: options?.inputOrigin })
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'legacy-without-origin',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '旧版未标记消息'
    })

    expect(result.ok).toBe(true)
    expect(routes).toEqual([])
    expect(inputs[0]).toMatchObject({ inputOrigin: 'remote-im-machine' })
    expect(inputs[0]?.text).toContain('旧版未标记消息')
    expect(inputs[0]?.text).not.toContain('如需继续与对方协作，请显式调用 imcli')
    expect(store.messages[0]).toMatchObject({ role: 'aicli', status: 'sent-to-aicli' })
  })

  it('starts output forwarding before submitting the AICLI prompt', async () => {
    const store = createMessageStore()
    const events: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      onAicliOutputStart: ({ sessionId, replyId }) => {
        events.push(`start:${sessionId}:${replyId}`)
      },
      sendUser: async () => {
        events.push('submit')
        return { ok: true }
      },
      sendImText: async () => {
        events.push('ack')
        return { ok: true }
      },
      createReplyId: () => 'reply-race',
      store
    })

    await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-race-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '立即失败也必须能回传',
      origin: 'human',
      createdAt: 100
    })

    expect(events).toEqual(['start:session-main:reply-race', 'submit'])
  })

  it('cancels the matching output route when AICLI rejects the prompt', async () => {
    const store = createMessageStore()
    const events: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      onAicliOutputStart: ({ replyId }) => events.push(`start:${replyId}`),
      onAicliOutputCancel: ({ replyId }) => events.push(`cancel:${replyId}`),
      sendUser: async () => {
        events.push('submit')
        return { ok: false, error: 'session not ready' }
      },
      sendImText: async () => ({ ok: true }),
      createReplyId: () => 'reply-rejected',
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-rejected-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '不会留下悬挂监听',
      origin: 'human',
      createdAt: 100
    })

    expect(result).toEqual({ ok: false, error: 'session not ready' })
    expect(events).toEqual(['start:reply-rejected', 'submit', 'cancel:reply-rejected'])
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
    expect(sentToIm).toEqual([])
    expect(store.messages.filter((item) => item.direction === 'incoming')).toHaveLength(1)
  })

  it('reserves the incoming row before awaiting sendUser to close the redelivery race', async () => {
    const store = createMessageStore()
    let sendCount = 0
    let releaseSend: (() => void) | null = null
    const firstSend = new Promise<void>((resolve) => {
      releaseSend = resolve
    })
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async () => {
        sendCount += 1
        await firstSend
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      store
    })
    const message = {
      projectId: 'project-1',
      remoteMessageId: 'remote-in-flight-dup',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '只能执行一次',
      origin: 'human' as const
    }

    const first = router.handleIncomingText(message)
    const duplicate = await router.handleIncomingText(message)

    expect(duplicate).toMatchObject({ ok: true, aicliSessionId: 'session-main' })
    expect(sendCount).toBe(1)
    expect(store.messages).toHaveLength(1)
    expect(store.messages[0]).toMatchObject({
      status: 'received',
      sessionId: 'session-main'
    })

    releaseSend!()
    await expect(first).resolves.toMatchObject({ ok: true })
    expect(store.messages[0]).toMatchObject({ status: 'sent-to-aicli' })
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
      origin: 'human',
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

  it('consumes approval commands before slash parsing or AICLI task routing', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const approvalInputs: Array<{ projectId: string; fromUserId: string; text: string }> = []
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
      handleApprovalCommand: async (input) => {
        approvalInputs.push(input)
        return { handled: true, ok: true, text: '已批准这一次命令执行。' }
      },
      handleControlCommand: async () => {
        throw new Error('approval must not reach general control commands')
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-approval-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/approve approval-public-a',
      origin: 'human',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(approvalInputs).toEqual([
      {
        projectId: 'project-1',
        fromUserId: 'phone_admin',
        text: '/approve approval-public-a'
      }
    ])
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toHaveLength(1)
    expect(sentToIm[0]).toBe('已批准这一次命令执行。')
    expect(store.messages[0]).toMatchObject({ role: 'remote-user', status: 'received' })
    expect(store.messages[1]).toMatchObject({
      role: 'system',
      direction: 'outgoing',
      content: '已批准这一次命令执行。'
    })
  })

  it('consumes an approval command sent by another AICLI through imcli', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const approvalInputs: Array<{ projectId: string; fromUserId: string; text: string }> = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        sentToAicli.push(text)
        return { ok: true }
      },
      sendImText: async () => ({ ok: true }),
      handleApprovalCommand: async (input) => {
        approvalInputs.push(input)
        return { handled: true, ok: true, text: '已批准这一次命令执行。' }
      },
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-machine-approval-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/approve approval-public-a',
      origin: 'machine',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(approvalInputs).toEqual([
      {
        projectId: 'project-1',
        fromUserId: 'phone_admin',
        text: '/approve approval-public-a'
      }
    ])
    expect(sentToAicli).toEqual([])
    expect(store.messages[0]).toMatchObject({ role: 'aicli', status: 'received' })
  })

  it('routes a transported approval result into the receiving AICLI without automatic IM output', async () => {
    const senderStore = createMessageStore()
    const wireTexts: string[] = []
    const sender = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-sender', targetRepo: 'repo' }),
      sendUser: async () => ({ ok: true }),
      sendImText: async (_projectId, _toUserId, text) => {
        wireTexts.push(text)
        return { ok: true }
      },
      handleApprovalCommand: async () => ({
        handled: true,
        ok: true,
        text: '已拒绝这一次命令执行。'
      }),
      store: senderStore
    })
    await sender.handleIncomingText({
      projectId: 'project-sender',
      remoteMessageId: 'approval-reply',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/reject approval-public-a',
      origin: 'human'
    })

    const receiverStore = createMessageStore()
    const receivedAicliInputs: string[] = []
    const receiverRoutes: RemoteImAicliOutputRoute[] = []
    const receiver = createRemoteImRouter({
      getConfig: () => ({
        ...config,
        desktopUserId: 'receiver_bot',
        masterUserIds: ['desktop_bot'],
        allowedUserIds: ['desktop_bot']
      }),
      resolveSession: () => ({ sessionId: 'session-receiver', targetRepo: 'repo' }),
      sendUser: async (_sessionId, text) => {
        receivedAicliInputs.push(text)
        return { ok: true }
      },
      onAicliOutputStart: (route) => receiverRoutes.push(route),
      sendImText: async () => ({ ok: true }),
      store: receiverStore
    })
    const result = await receiver.handleIncomingText({
      projectId: 'project-receiver',
      remoteMessageId: 'approval-result-wire',
      fromUserId: 'desktop_bot',
      toUserId: 'receiver_bot',
      text: wireTexts[0]!,
      origin: 'machine'
    })

    expect(result.ok).toBe(true)
    expect(receivedAicliInputs).toHaveLength(1)
    expect(receivedAicliInputs[0]).toContain('已拒绝这一次命令执行。')
    expect(receivedAicliInputs[0]).not.toContain('如需继续与对方协作，请显式调用 imcli')
    expect(receiverRoutes).toEqual([])
    expect(receiverStore.messages[0]).toMatchObject({
      role: 'aicli',
      status: 'sent-to-aicli',
      content: '已拒绝这一次命令执行。'
    })
    expect(senderStore.messages[1]).toMatchObject({
      role: 'system',
      content: '已拒绝这一次命令执行。'
    })
  })

  it('sends a generated /diff report as an IM file attachment', async () => {
    const store = createMessageStore()
    const sentToIm: string[] = []
    const sentFiles: Array<{ projectId: string; toUserId: string; localPath: string }> = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: '/repo' }),
      sendUser: async () => ({ ok: true }),
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      sendImFile: async (projectId, toUserId, localPath) => {
        sentFiles.push({ projectId, toUserId, localPath })
        return { ok: true }
      },
      handleControlCommand: async () => ({
        ok: true,
        text: '完整 Diff 已生成。',
        attachmentPath: '/tmp/remote-im-diff.md'
      }),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      remoteMessageId: 'remote-diff-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      text: '/diff',
      origin: 'human',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(sentToIm).toEqual(['完整 Diff 已生成。'])
    expect(sentFiles).toEqual([
      {
        projectId: 'project-1',
        toUserId: 'phone_admin',
        localPath: '/tmp/remote-im-diff.md'
      }
    ])
  })

  it('returns forwarding ids for /btw control commands', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const handled: Array<{
      command: string
      args: string
      replyId?: string
      taskId?: string
    }> = []
    const events: string[] = []
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
      onAicliOutputStart: ({ replyId }) => events.push(`start:${replyId}`),
      handleControlCommand: async ({ command, args, replyId, taskId }) => {
        events.push('submit')
        handled.push({ command, args, replyId, taskId })
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
      origin: 'human',
      createdAt: 100
    })

    expect(result).toEqual({
      ok: true,
      aicliSessionId: 'session-main',
      replyId: 'reply-btw-fixed'
    })
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual(['已提交 /btw 子任务，完成后会通过 IM 回传。'])
    expect(events).toEqual(['start:reply-btw-fixed', 'submit'])
    expect(handled).toEqual([
      {
        command: 'btw',
        args: '检查最近一次失败日志',
        replyId: 'reply-btw-fixed',
        taskId: 'remote-im-task-reply-btw-fixed'
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
      origin: 'human',
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
      origin: 'human',
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
    expect(sentToIm).toEqual([])
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
      origin: 'human',
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

  it('routes trusted file messages of any type to AICLI with the cached local path', async () => {
    const store = createMessageStore()
    const sentToAicli: Array<{
      sessionId: string
      text: string
      displayText: string | undefined
      inputOrigin: string | undefined
    }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text, options) => {
        sentToAicli.push({
          sessionId,
          text,
          displayText: options?.displayText,
          inputOrigin: options?.inputOrigin
        })
        return { ok: true }
      },
      sendImText: async (_projectId, _toUserId, text) => {
        sentToIm.push(text)
        return { ok: true }
      },
      cacheFile: async () => ({
        ok: true,
        attachment: {
          type: 'file',
          localPath: '/tmp/remote-im/files/spec.pdf',
          remoteUrl: 'https://example.test/spec.pdf',
          sizeBytes: 2_097_152,
          fileName: 'spec.pdf',
          mimeType: 'application/pdf',
          sdkFileId: 'file-1'
        }
      }),
      store
    })

    const result = await router.handleIncomingFile({
      projectId: 'project-1',
      remoteMessageId: 'file-remote-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      fileUrl: 'https://example.test/spec.pdf',
      sizeBytes: 2_097_152,
      fileName: 'spec.pdf',
      mimeType: 'application/pdf',
      uuid: 'file-1',
      caption: '按这个文档改',
      createdAt: 100
    })

    // 以前收到文件只入库供界面显示，**从不转给 AICLI**——用户发来的文件在对话里
    // 毫无反应，看起来像没收到。现在必须与图片走同一条路。
    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]?.text).toContain('本地路径: /tmp/remote-im/files/spec.pdf')
    expect(sentToAicli[0]?.text).toContain('文件名: spec.pdf')
    expect(sentToAicli[0]?.text).toContain('类型: application/pdf')
    // 大小要能让 AICLI 先判断值不值得读，而不是闷头打开一个几十 MB 的二进制。
    expect(sentToAicli[0]?.text).toContain('大小: 2.0 MB')
    expect(sentToAicli[0]?.text).toContain('配文: 按这个文档改')
    expect(sentToAicli[0]?.inputOrigin).toBe('remote-im-machine')
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      kind: 'file',
      status: 'sent-to-aicli',
      attachment: { type: 'file', localPath: '/tmp/remote-im/files/spec.pdf' }
    })
  })

  it('tells AICLI a video is a video, not a file to be read as text', async () => {
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
      cacheFile: async () => ({
        ok: true,
        attachment: {
          type: 'file',
          localPath: '/tmp/remote-im/files/clip.mp4',
          remoteUrl: 'https://example.test/clip.mp4',
          sizeBytes: 8_388_608,
          fileName: 'clip.mp4',
          mimeType: 'video/mp4',
          sdkFileId: 'video-1'
        }
      }),
      store
    })

    const result = await router.handleIncomingFile({
      projectId: 'project-1',
      remoteMessageId: 'video-remote-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      fileUrl: 'https://example.test/clip.mp4',
      sizeBytes: 8_388_608,
      fileName: 'clip.mp4',
      mimeType: 'video/mp4',
      uuid: 'video-1',
      caption: '看下这段录屏',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    // 抬头写「文件消息」的话，AICLI 很可能直接去读这个 8MB 的二进制当文本。
    expect(sentToAicli[0]).toContain('[视频消息]')
    expect(sentToAicli[0]).not.toContain('[文件消息]')
    expect(sentToAicli[0]).toContain('本地路径: /tmp/remote-im/files/clip.mp4')
    expect(sentToAicli[0]).toContain('类型: video/mp4')
    expect(sentToAicli[0]).toContain('配文: 看下这段录屏')
    expect(sentToAicli[0]).toContain('请结合配文与这段视频继续处理。')
  })

  it('reports to the sender when a file arrives with no running AICLI', async () => {
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
      cacheFile: async () => ({
        ok: true,
        attachment: {
          type: 'file',
          localPath: '/tmp/remote-im/files/spec.pdf',
          remoteUrl: 'https://example.test/spec.pdf',
          sizeBytes: 10,
          fileName: 'spec.pdf',
          mimeType: 'application/pdf',
          sdkFileId: null
        }
      }),
      store
    })

    const result = await router.handleIncomingFile({
      projectId: 'project-1',
      remoteMessageId: 'file-remote-2',
      fromUserId: 'phone_admin',
      origin: 'human',
      fileUrl: 'https://example.test/spec.pdf',
      fileName: 'spec.pdf'
    })

    // 没有会话时也必须回一句，否则发送方只看到文件发出去了、然后石沉大海。
    expect(result.ok).toBe(false)
    expect(sentToIm[0]).toContain('当前没有运行中的 AICLI')
    expect(store.messages[0]).toMatchObject({ kind: 'file', status: 'failed' })
  })

  it('routes trusted image messages to AICLI with the cached local image path', async () => {
    const store = createMessageStore()
    const sentToAicli: Array<{
      sessionId: string
      text: string
      displayText: string | undefined
      attachments: unknown
      inputOrigin: string | undefined
    }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text, options) => {
        sentToAicli.push({
          sessionId,
          text,
          displayText: options?.displayText,
          attachments: options?.attachments,
          inputOrigin: options?.inputOrigin
        })
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
    expect(sentToAicli[0]?.inputOrigin).toBe('remote-im-machine')
    expect(sentToAicli[0]?.attachments).toEqual([
      {
        type: 'image',
        localPath: '/tmp/remote-im/images/photo.png',
        mimeType: 'image/png',
        fileName: 'photo.png'
      }
    ])
    expect(sentToIm).toEqual([])
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

  it('merges an image and its caption into a single AICLI input with one receipt', async () => {
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
          thumbnailUrl: null,
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
      fileName: 'photo.png',
      mimeType: 'image/png',
      caption: '帮我看看这张报错截图',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    // 合并成「一次」输入：只调用一次 sendUser，配文与图片路径同在一个 prompt 里。
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]?.text).toContain('本地路径: /tmp/remote-im/images/photo.png')
    expect(sentToAicli[0]?.text).toContain('配文: 帮我看看这张报错截图')
    // 成功投递保持静默，图片与配文不会产生额外回执。
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      kind: 'image',
      status: 'sent-to-aicli',
      content: '[图片消息] photo.png\n帮我看看这张报错截图'
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
      origin: 'human',
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
      origin: 'human',
      createdAt: 100
    })

    expect(result.ok).toBe(true)
    expect(result.aicliSessionId).toBe('session-main')
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('hello from friend')
    expect(sentToIm).toEqual([])
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
    expect(sentToIm).toEqual([])
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
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      status: 'sent-to-aicli',
      error: null
    })
  })

  it('routes legacy marked slave output as a silent machine collaboration input', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const routes: RemoteImAicliOutputRoute[] = []
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
      onAicliOutputStart: (route) => routes.push(route),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'slave-b',
      toUserId: 'desktop_bot',
      text: createRemoteImAicliOutputText('处理完成')
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('处理完成')
    expect(sentToAicli[0]).not.toContain(createRemoteImAicliOutputText(''))
    expect(sentToIm).toEqual([])
    expect(routes).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      status: 'sent-to-aicli',
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
    expect(sentToIm).toEqual([])
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
      origin: 'human',
      text: '检查构建'
    })

    expect(result.ok).toBe(false)
    expect(result.error).toContain('No running AICLI')
    expect(sentToIm[0]).toContain('当前没有运行中的 AICLI')
    expect(store.messages[0]).toMatchObject({ status: 'failed' })
  })

  it('routes machine system replies to AICLI without an output route', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const routes: RemoteImAicliOutputRoute[] = []
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
      onAicliOutputStart: (route) => routes.push(route),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: '当前没有运行中的 AICLI。',
      origin: 'machine'
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('当前没有运行中的 AICLI。')
    expect(sentToIm).toEqual([])
    expect(routes).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      status: 'sent-to-aicli',
      content: '当前没有运行中的 AICLI。'
    })
  })

  it('routes legacy marked remote output to the local AICLI without automatic IM output', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const routes: RemoteImAicliOutputRoute[] = []
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
      onAicliOutputStart: (route) => routes.push(route),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: createRemoteImAicliOutputText('build passed')
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('build passed')
    expect(sentToIm).toEqual([])
    expect(routes).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      direction: 'incoming',
      status: 'sent-to-aicli',
      content: 'build passed'
    })
  })

  it('does not use nested remote text heuristics to block machine AICLI input', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const routes: RemoteImAicliOutputRoute[] = []
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
      onAicliOutputStart: (route) => routes.push(route),
      store
    })

    const nestedOutput = '[来自远程 IM：desktop_bot]\nClaude Code output'
    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: nestedOutput,
      origin: 'machine'
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain(nestedOutput)
    expect(sentToIm).toEqual([])
    expect(routes).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      direction: 'incoming',
      status: 'sent-to-aicli',
      content: nestedOutput
    })
  })

  it('routes operation completion notifications to AICLI without automatic IM output', async () => {
    const store = createMessageStore()
    const sentToAicli: string[] = []
    const sentToIm: string[] = []
    const routes: RemoteImAicliOutputRoute[] = []
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
      onAicliOutputStart: (route) => routes.push(route),
      store
    })

    const result = await router.handleIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: '操作已完成。',
      origin: 'machine'
    })

    expect(result.ok).toBe(true)
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]).toContain('操作已完成。')
    expect(sentToIm).toEqual([])
    expect(routes).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      status: 'sent-to-aicli',
      content: '操作已完成。'
    })
  })

  it('backfills roamed text into the store without routing to AICLI', async () => {
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

    const result = await router.backfillRoamedText('project-1', [
      {
        remoteMessageId: 'roam-1',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: '离线期间发的任务',
        createdAt: 100,
        flow: 'in'
      },
      {
        remoteMessageId: 'roam-2',
        fromUserId: 'desktop_bot',
        toUserId: 'phone_admin',
        text: '我发出的历史回复',
        createdAt: 200,
        flow: 'out'
      }
    ])

    expect(result).toEqual({ ok: true, inserted: 2 })
    // 关键：漫游是历史，绝不重新路由 AICLI，也不触发回执发送。
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      remoteMessageId: 'roam-1',
      role: 'remote-user',
      direction: 'incoming',
      status: 'received',
      content: '离线期间发的任务',
      createdAt: 100
    })
    expect(store.messages[1]).toMatchObject({
      remoteMessageId: 'roam-2',
      role: 'remote-user',
      direction: 'outgoing',
      status: 'sent-to-im',
      content: '我发出的历史回复',
      sentToImAt: 200
    })
  })

  it('skips roamed messages that already exist and disallowed senders', async () => {
    const store = createMessageStore()
    store.create({
      projectId: 'project-1',
      sessionId: null,
      provider: 'tencent-im',
      remoteMessageId: 'roam-known',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      role: 'remote-user',
      direction: 'incoming',
      content: '实时链路已经入库过',
      kind: 'text',
      attachment: null,
      status: 'received',
      error: null,
      createdAt: 50,
      sentToAicliAt: null,
      sentToImAt: null
    })
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => null,
      sendUser: async () => ({ ok: true }),
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.backfillRoamedText('project-1', [
      {
        remoteMessageId: 'roam-known',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: '实时链路已经入库过',
        createdAt: 50,
        flow: 'in'
      },
      {
        remoteMessageId: 'roam-intruder',
        fromUserId: 'intruder',
        toUserId: 'desktop_bot',
        text: '陌生人的漫游消息',
        createdAt: 60,
        flow: 'in'
      },
      {
        remoteMessageId: 'roam-remote-desktop',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: '\u2063\u200B[remote-desktop]{"v":1,"type":"invite","sessionId":"s1"}',
        origin: 'machine',
        createdAt: 70,
        flow: 'in'
      }
    ])

    expect(result).toEqual({ ok: true, inserted: 0 })
    expect(store.messages).toHaveLength(1)
  })

  it('classifies roamed AICLI output text as aicli role for display', async () => {
    const store = createMessageStore()
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => null,
      sendUser: async () => ({ ok: true }),
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.backfillRoamedText('project-1', [
      {
        remoteMessageId: 'roam-aicli',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: createRemoteImAicliOutputText('AICLI 的历史输出'),
        createdAt: 300,
        flow: 'in'
      }
    ])

    expect(result).toEqual({ ok: true, inserted: 1 })
    expect(store.messages[0]).toMatchObject({
      role: 'aicli',
      content: 'AICLI 的历史输出',
      status: 'received'
    })
  })

  it('classifies structured machine roaming messages without a legacy text marker', async () => {
    const store = createMessageStore()
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => null,
      sendUser: async () => ({ ok: true }),
      sendImText: async () => ({ ok: true }),
      store
    })

    const result = await router.backfillRoamedText('project-1', [
      {
        remoteMessageId: 'roam-machine-in',
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        text: '协作输入',
        origin: 'machine',
        createdAt: 400,
        flow: 'in'
      },
      {
        remoteMessageId: 'roam-machine-out',
        fromUserId: 'desktop_bot',
        toUserId: 'phone_admin',
        text: '协作输出',
        origin: 'machine',
        createdAt: 401,
        flow: 'out'
      }
    ])

    expect(result).toEqual({ ok: true, inserted: 2 })
    expect(store.messages).toEqual([
      expect.objectContaining({
        remoteMessageId: 'roam-machine-in',
        role: 'aicli',
        direction: 'incoming',
        content: '协作输入'
      }),
      expect.objectContaining({
        remoteMessageId: 'roam-machine-out',
        role: 'aicli',
        direction: 'outgoing',
        content: '协作输出'
      })
    ])
  })
})
