import { mkdtemp, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig, RemoteImMessage, RemoteImStatus } from '../../../electron/remote-im/types.js'
import { startRemoteImCliServer } from '../../../electron/remote-im/imcliServer.js'

const config: RemoteImConfig = {
  provider: 'tencent-im',
  sdkAppId: 1600148979,
  desktopUserId: 'agent-a',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: ['agent-b', 'phone-user'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
,
  remoteDesktopMode: 'disabled' as const,
  remoteDesktopControl: false
}

const status: RemoteImStatus = {
  projectId: 'project-1',
  state: 'connected',
  detail: null,
  updatedAt: 1
}

function message(overrides: Partial<RemoteImMessage>): RemoteImMessage {
  return {
    id: 1,
    projectId: 'project-1',
    sessionId: null,
    provider: 'tencent-im',
    remoteMessageId: null,
    fromUserId: 'phone-user',
    toUserId: 'agent-a',
    role: 'remote-user',
    direction: 'incoming',
    kind: 'text',
    attachment: null,
    content: 'hello',
    status: 'received',
    error: null,
    createdAt: 100,
    sentToAicliAt: null,
    sentToImAt: null,
    ...overrides
  }
}

let tempDir: string | null = null

async function createTempDir(): Promise<string> {
  tempDir = await mkdtemp(join(tmpdir(), 'remote-im-cli-'))
  return tempDir
}

describe('remote IM CLI bridge server', () => {
  afterEach(async () => {
    if (tempDir) await rm(tempDir, { recursive: true, force: true })
    tempDir = null
  })

  it('serves account, contact, and history data behind a local bearer token', async () => {
    const rootDir = await createTempDir()
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [
        message({ id: 1, content: 'hello', createdAt: 100 }),
        message({
          id: 2,
          role: 'aicli',
          direction: 'outgoing',
          fromUserId: 'agent-a',
          toUserId: 'phone-user',
          content: 'reply',
          status: 'sent-to-im',
          createdAt: 200
        })
      ],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' })
    })

    try {
      const headers = { authorization: `Bearer ${bridge.token}` }
      await expect(fetch(`${bridge.url}/whoami?projectId=project-1`, { headers }).then((res) => res.json())).resolves.toMatchObject({
        ok: true,
        value: {
          projectId: 'project-1',
          userId: 'agent-a',
          sdkAppId: 1600148979,
          status: 'connected'
        }
      })
      await expect(fetch(`${bridge.url}/contacts?projectId=project-1`, { headers }).then((res) => res.json())).resolves.toMatchObject({
        ok: true,
        value: {
          contacts: [
            { userId: 'agent-b' },
            { userId: 'phone-user' }
          ]
        }
      })
      await expect(fetch(`${bridge.url}/history?projectId=project-1&peer=phone-user&limit=5`, { headers }).then((res) => res.json())).resolves.toMatchObject({
        ok: true,
        value: {
          messages: [
            { id: 1, content: 'hello' },
            { id: 2, content: 'reply' }
          ]
        }
      })
    } finally {
      await bridge.close()
    }
  })

  it('sends peer messages through the app runtime instead of writing the database directly', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage
    })

    try {
      const response = await fetch(`${bridge.url}/send`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json'
        },
        body: JSON.stringify({
          projectId: 'project-1',
          toUserId: 'agent-b',
          text: 'forwarded reply'
        })
      })

      await expect(response.json()).resolves.toMatchObject({
        ok: true,
        value: { toUserId: 'agent-b' }
      })
      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', 'forwarded reply', 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('sends peer markdown/html files through the app runtime', async () => {
    const rootDir = await createTempDir()
    const sendPeerFile = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      sendPeerFile
    })

    try {
      const response = await fetch(`${bridge.url}/send-file`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json'
        },
        body: JSON.stringify({
          projectId: 'project-1',
          toUserId: 'agent-b',
          localPath: '/tmp/report.md'
        })
      })

      await expect(response.json()).resolves.toMatchObject({
        ok: true,
        value: { toUserId: 'agent-b' }
      })
      expect(sendPeerFile).toHaveBeenCalledWith('project-1', '/tmp/report.md', 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('sends a structured Diff request bound to the authorized AICLI session', async () => {
    const rootDir = await createTempDir()
    const sendPeerDiff = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const authorizeCaller = vi.fn(() => ({ ok: true as const }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      sendPeerDiff,
      authorizeCaller
    })

    try {
      const response = await fetch(`${bridge.url}/send-diff`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json',
          'x-multi-ai-code-session-id': 'session-diff'
        },
        body: JSON.stringify({
          projectId: 'project-1',
          toUserId: 'agent-b',
          args: '--commit HEAD src'
        })
      })

      await expect(response.json()).resolves.toMatchObject({
        ok: true,
        value: { toUserId: 'agent-b' }
      })
      expect(authorizeCaller).toHaveBeenCalledWith('project-1', 'session-diff')
      expect(sendPeerDiff).toHaveBeenCalledWith(
        'project-1',
        '--commit HEAD src',
        'agent-b',
        'session-diff'
      )
    } finally {
      await bridge.close()
    }
  })

  it('sends peer videos through the app runtime', async () => {
    const rootDir = await createTempDir()
    const sendPeerVideo = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      sendPeerVideo
    })

    try {
      const response = await fetch(`${bridge.url}/send-video`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json'
        },
        body: JSON.stringify({
          projectId: 'project-1',
          toUserId: 'agent-b',
          localPath: '/tmp/screen-record.mp4'
        })
      })

      await expect(response.json()).resolves.toMatchObject({
        ok: true,
        value: { toUserId: 'agent-b' }
      })
      expect(sendPeerVideo).toHaveBeenCalledWith('project-1', '/tmp/screen-record.mp4', 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('reports a clear error when the running app build cannot send video', async () => {
    const rootDir = await createTempDir()
    // 旧版本主进程没接 sendPeerVideo：必须 400 说明白，不能装作发出去了。
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' })
    })

    try {
      const response = await fetch(`${bridge.url}/send-video`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json'
        },
        body: JSON.stringify({
          projectId: 'project-1',
          toUserId: 'agent-b',
          localPath: '/tmp/screen-record.mp4'
        })
      })

      expect(response.status).toBe(400)
      await expect(response.json()).resolves.toMatchObject({
        ok: false,
        error: 'video sending is not available'
      })
    } finally {
      await bridge.close()
    }
  })

  it('adds a contact through the app runtime', async () => {
    const rootDir = await createTempDir()
    const addContact = vi.fn(async () => ({ ok: true as const, userId: 'agent-c' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      addContact
    })

    try {
      const response = await fetch(`${bridge.url}/add-contact`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json'
        },
        body: JSON.stringify({ projectId: 'project-1', userId: 'agent-c' })
      })

      await expect(response.json()).resolves.toMatchObject({
        ok: true,
        value: { userId: 'agent-c' }
      })
      // projectId 必须透传下去：账号配置的落盘 profile 是按它挑的。
      expect(addContact).toHaveBeenCalledWith('project-1', 'agent-c')
    } finally {
      await bridge.close()
    }
  })

  it('rejects an add-contact request without a user id', async () => {
    const rootDir = await createTempDir()
    const addContact = vi.fn(async () => ({ ok: true as const, userId: '' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      addContact
    })

    try {
      const response = await fetch(`${bridge.url}/add-contact`, {
        method: 'POST',
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'content-type': 'application/json'
        },
        body: JSON.stringify({ projectId: 'project-1', userId: '   ' })
      })

      expect(response.status).toBe(400)
      // 空 ID 必须在到达主进程之前就被挡掉，别把空联系人写进账号配置。
      expect(addContact).not.toHaveBeenCalled()
    } finally {
      await bridge.close()
    }
  })

  it('binds production bridge requests to the originating AICLI session', async () => {
    const rootDir = await createTempDir()
    const authorizeCaller = vi.fn((projectId: string, sessionId: string) =>
      projectId === 'project-1' && sessionId === 'session-a'
        ? ({ ok: true } as const)
        : ({ ok: false, error: 'stale AICLI session' } as const)
    )
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      authorizeCaller
    })

    try {
      const unauthorized = await fetch(`${bridge.url}/whoami?projectId=project-1`, {
        headers: { authorization: `Bearer ${bridge.token}` }
      })
      await expect(unauthorized.json()).resolves.toMatchObject({
        ok: false,
        error: 'AICLI session identity is required'
      })

      const authorized = await fetch(`${bridge.url}/whoami?projectId=project-1`, {
        headers: {
          authorization: `Bearer ${bridge.token}`,
          'x-multi-ai-code-session-id': 'session-a'
        }
      })
      await expect(authorized.json()).resolves.toMatchObject({
        ok: true,
        value: { projectId: 'project-1', userId: 'agent-a' }
      })
      expect(authorizeCaller).toHaveBeenCalledWith('project-1', 'session-a')
    } finally {
      await bridge.close()
    }
  })

  it('keeps reads and sends inside the authorized account operation', async () => {
    const rootDir = await createTempDir()
    let insideAuthorizedOperation = false
    let authorizedOperationCount = 0
    const withAuthorizedCaller = async <T>(
      projectId: string,
      sessionId: string,
      operation: () => Promise<T>
    ): Promise<T> => {
      expect(projectId).toBe('project-1')
      expect(sessionId).toBe('session-a')
      authorizedOperationCount += 1
      insideAuthorizedOperation = true
      try {
        return await operation()
      } finally {
        insideAuthorizedOperation = false
      }
    }
    const getConfig = vi.fn(async () => {
      expect(insideAuthorizedOperation).toBe(true)
      return config
    })
    const sendPeerMessage = vi.fn(async () => {
      expect(insideAuthorizedOperation).toBe(true)
      return { ok: true as const, toUserId: 'agent-b' }
    })
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage,
      withAuthorizedCaller
    })
    const headers = {
      authorization: `Bearer ${bridge.token}`,
      'x-multi-ai-code-session-id': 'session-a'
    }

    try {
      const contacts = await fetch(`${bridge.url}/contacts?projectId=project-1`, { headers })
      await expect(contacts.json()).resolves.toMatchObject({ ok: true })

      const sent = await fetch(`${bridge.url}/send`, {
        method: 'POST',
        headers: { ...headers, 'content-type': 'application/json' },
        body: JSON.stringify({
          projectId: 'project-1',
          toUserId: 'agent-b',
          text: 'delegated work'
        })
      })
      await expect(sent.json()).resolves.toMatchObject({ ok: true })
      expect(authorizedOperationCount).toBe(2)
      expect(getConfig).toHaveBeenCalledTimes(1)
      expect(sendPeerMessage).toHaveBeenCalledTimes(1)
      expect(insideAuthorizedOperation).toBe(false)
    } finally {
      await bridge.close()
    }
  })

})
