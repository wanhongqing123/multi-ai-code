import { mkdtemp, rm } from 'node:fs/promises'
import { join } from 'node:path'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig, RemoteImMessage, RemoteImStatus } from '../../../electron/remote-im/types.js'
import { startRemoteImCliServer } from '../../../electron/remote-im/imcliServer.js'

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1600148979,
  desktopUserId: 'agent-a',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: ['agent-b', 'phone-user'],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: ['agent-b', 'phone-user'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
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
})
