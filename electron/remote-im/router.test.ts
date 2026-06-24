import { describe, expect, it } from 'vitest'
import type { RemoteImConfig, RemoteImMessage } from './types.js'
import { createRemoteImRouter } from './router.js'

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1400000000,
  desktopUserId: 'desktop_bot',
  userSigEndpoint: 'https://example.test/sig',
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
    expect(sentToIm[0]).toContain('没有远程控制权限')
    expect(store.messages[0]).toMatchObject({ status: 'rejected', role: 'remote-user' })
  })

  it('wraps whitelisted text and sends it to the current AICLI session', async () => {
    const store = createMessageStore()
    const sentToAicli: Array<{ sessionId: string; text: string }> = []
    const sentToIm: string[] = []
    const router = createRemoteImRouter({
      getConfig: () => config,
      resolveSession: () => ({ sessionId: 'session-main', targetRepo: 'repo' }),
      sendUser: async (sessionId, text) => {
        sentToAicli.push({ sessionId, text })
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
    expect(sentToAicli).toEqual([
      {
        sessionId: 'session-main',
        text: '[来自远程 IM：phone_admin]\n检查构建'
      }
    ])
    expect(sentToIm[0]).toContain('已发送给当前 AICLI')
    expect(store.messages.map((message) => message.status)).toEqual([
      'sent-to-aicli',
      'sent-to-im'
    ])
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
})
