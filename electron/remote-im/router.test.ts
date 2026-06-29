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
    expect(sentToAicli).toHaveLength(1)
    expect(sentToAicli[0]?.sessionId).toBe('session-main')
    expect(sentToAicli[0]?.text).toContain('phone_admin')
    expect(sentToAicli[0]?.text).toContain(REMOTE_IM_REPLY_OPEN_TAG)
    expect(sentToAicli[0]?.text).toContain(REMOTE_IM_REPLY_CLOSE_TAG)
    expect(sentToAicli[0]?.displayText).toBe('[来自远程 IM：phone_admin]\n检查构建')
    expect(sentToAicli[0]?.displayText).not.toContain('[IM_REPLY]')
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages.map((message) => message.status)).toEqual([
      'sent-to-aicli',
      'sent-to-im'
    ])
  })

  it('records configured friend messages as normal IM without routing them to AICLI', async () => {
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
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({
      role: 'remote-user',
      status: 'received',
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

  it('rejects normal slave-to-master messages without sending them to AICLI', async () => {
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

    expect(result.ok).toBe(false)
    expect(result.error).toContain('slave nodes cannot initiate tasks')
    expect(sentToAicli).toEqual([])
    expect(sentToIm[0]).toContain('奴隶节点不能主动发起任务')
    expect(store.messages[0]).toMatchObject({
      status: 'rejected',
      error: 'slave cannot initiate task'
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

  it('rejects slave-to-slave task routing without sending it to AICLI', async () => {
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

    expect(result.ok).toBe(false)
    expect(result.error).toContain('slave nodes cannot route tasks to each other')
    expect(sentToAicli).toEqual([])
    expect(sentToIm).toEqual([])
    expect(store.messages[0]).toMatchObject({ status: 'rejected', error: 'slave-to-slave blocked' })
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
