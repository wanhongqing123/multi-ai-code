import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { join } from 'node:path'
import { mkdtemp, rm, writeFile } from 'node:fs/promises'
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
    expect(stdout).toContain('imcli send-image <user> <imagePath>')
    expect(stdout).toContain('imcli history')
    expect(stdout).toContain('Requirements:')
    expect(stdout).toContain('MULTI_AI_CODE_PROJECT_ID')
    expect(stdout).toContain('Examples:')
    expect(stdout).toContain('imcli send-image phone-user C:\\temp\\screenshot.png --project project-1')
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

  it('repairs text that was decoded as GBK before imcli receives it', async () => {
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
      const garbled =
        '銆怉pollo win/mac 姣忓懆 Crash 鏍瑰洜鍒嗘瀽 + 淇鏂规銆?鐗堟湰: 6.9.8.899 | 绐楀彛: 07-07~07-15'
      const expected =
        '【Apollo win/mac 每周 Crash 根因分析 + 修复方案】版本: 6.9.8.899 | 窗口: 07-07~07-15'
      const { stdout } = await execFileAsync(
        process.execPath,
        [imcliPath, 'send', 'agent-b', garbled],
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
      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', expected, 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('sends an image path through the local app bridge using project environment', async () => {
    const rootDir = await createTempDir()
    const imagePath = join(rootDir, 'photo.png')
    await writeFile(imagePath, new Uint8Array([1, 2, 3]))
    const sendPeerImage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage: async () => ({ ok: true as const, toUserId: 'agent-b' }),
      sendPeerImage
    })

    try {
      const { stdout } = await execFileAsync(
        process.execPath,
        [imcliPath, 'send-image', 'agent-b', imagePath],
        {
          env: {
            ...process.env,
            MULTI_AI_CODE_IMCLI_URL: bridge.url,
            MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
            MULTI_AI_CODE_PROJECT_ID: 'project-1'
          }
        }
      )

      expect(stdout).toContain('sent image to agent-b')
      expect(sendPeerImage).toHaveBeenCalledWith('project-1', imagePath, 'agent-b')
    } finally {
      await bridge.close()
    }
  })
})
