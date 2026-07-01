import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { join } from 'node:path'
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig, RemoteImStatus } from './types.js'
import { startRemoteImCliServer } from './imcliServer.js'

const execFileAsync = promisify(execFile)
const imcliPath = join(process.cwd(), 'bin', 'imcli.mjs')

const config: RemoteImConfig = {
  enabled: true,
  provider: 'tencent-im',
  sdkAppId: 1600148979,
  desktopUserId: 'agent-a',
  desktopRole: 'master',
  userSigMode: 'secret-key',
  userSigEndpoint: '',
  userSigSecretKey: 'secret',
  friendUserIds: ['agent-b'],
  masterUserIds: [],
  slaveUserIds: [],
  allowedUserIds: ['agent-b'],
  outputFlushIntervalMs: 2000,
  outputMaxChunkChars: 1200
}

const status: RemoteImStatus = {
  projectId: 'project-1',
  state: 'connected',
  detail: null,
  updatedAt: 1
}

let tempDir: string | null = null

async function createTempDir(): Promise<string> {
  tempDir = await mkdtemp(join(tmpdir(), 'remote-im-command-'))
  return tempDir
}

describe('imcli command', () => {
  afterEach(async () => {
    if (tempDir) await rm(tempDir, { recursive: true, force: true })
    tempDir = null
  })

  it('prints help that AICLI can inspect before using IM operations', async () => {
    const { stdout } = await execFileAsync(process.execPath, [imcliPath, 'help'])

    expect(stdout).toContain('imcli help')
    expect(stdout).toContain('imcli send <user> <text>')
    expect(stdout).toContain('imcli history')
  })

  it('sends a message through the local app bridge using project environment', async () => {
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
      const { stdout } = await execFileAsync(
        process.execPath,
        [imcliPath, 'send', 'agent-b', 'hello from cli'],
        {
          env: {
            ...process.env,
            MULTI_AI_CODE_IMCLI_URL: bridge.url,
            MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
            MULTI_AI_CODE_PROJECT_ID: 'project-1'
          }
        }
      )

      expect(stdout).toContain('sent to agent-b')
      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', 'hello from cli', 'agent-b')
    } finally {
      await bridge.close()
    }
  })
})
