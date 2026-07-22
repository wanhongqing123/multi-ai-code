import { execFile } from 'node:child_process'
import { promisify } from 'node:util'
import { join } from 'node:path'
import { chmod, copyFile, mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { afterEach, describe, expect, it, vi } from 'vitest'
import type { RemoteImConfig, RemoteImStatus } from '../../../electron/remote-im/types.js'
import { startRemoteImCliServer } from '../../../electron/remote-im/imcliServer.js'

const execFileAsync = promisify(execFile)
const imcliWrapperPath = join(process.cwd(), 'bin', 'imcli')
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
    expect(stdout).toContain('Command details:')
    expect(stdout).toContain('imcli send <user> <text>')
    expect(stdout).toContain('imcli send-image <user> <imagePath>')
    expect(stdout).toContain('imcli send-file <user> <filePath>')
    expect(stdout).toContain('imcli history')
    expect(stdout).toContain('Output format: #<id> <role>/<direction> <from> -> <to>: <content>')
    expect(stdout).toContain('Use one of these user IDs as the <user> argument')
    expect(stdout).toContain('The receiver can tap the file card in iOS, Android, or Desktop IM to preview it.')
    expect(stdout).toContain('This is text-only. Use send-image or send-file separately for attachments.')
    expect(stdout).toContain('Requirements:')
    expect(stdout).toContain('MULTI_AI_CODE_PROJECT_ID')
    expect(stdout).toContain('Markdown and HTML files')
    expect(stdout).toContain('Use send-image for png/jpg/jpeg/gif/webp image files.')
    expect(stdout).toContain('Examples:')
    expect(stdout).toContain('imcli send-image phone-user C:\\temp\\screenshot.png --project project-1')
    expect(stdout).toContain('imcli send-file phone-user ./report.md --project project-1')
  })

  it('prefers the packaged Electron runtime before falling back to host node', async () => {
    const rootDir = await createTempDir()
    const binDir = join(
      rootDir,
      'Multi-AI Code.app',
      'Contents',
      'Resources',
      'app.asar.unpacked',
      'bin'
    )
    const macosDir = join(rootDir, 'Multi-AI Code.app', 'Contents', 'MacOS')
    const wrapperPath = join(binDir, 'imcli')
    const fakeElectronPath = join(macosDir, 'Multi-AI Code')

    await mkdir(binDir, { recursive: true })
    await mkdir(macosDir, { recursive: true })
    await copyFile(imcliWrapperPath, wrapperPath)
    await chmod(wrapperPath, 0o755)
    await writeFile(join(binDir, 'imcli.mjs'), 'throw new Error("fake electron should receive this path")\n')
    await writeFile(
      fakeElectronPath,
      [
        '#!/usr/bin/env sh',
        'echo "run_as_node=$ELECTRON_RUN_AS_NODE"',
        'echo "script=$1"',
        'shift',
        'echo "args=$*"'
      ].join('\n')
    )
    await chmod(fakeElectronPath, 0o755)

    const { stdout } = await execFileAsync(wrapperPath, ['help'])
    const wrapper = await readFile(imcliWrapperPath, 'utf8')
    const packagedRuntimeIndex = wrapper.indexOf('ELECTRON_RUN_AS_NODE=1')
    const hostNodeFallbackIndex = wrapper.lastIndexOf('exec node')

    expect(wrapper).toContain('../../../MacOS/Multi-AI Code')
    expect(packagedRuntimeIndex).toBeGreaterThan(-1)
    expect(hostNodeFallbackIndex).toBeGreaterThan(-1)
    expect(packagedRuntimeIndex).toBeLessThan(hostNodeFallbackIndex)
    expect(stdout).toContain('run_as_node=1')
    expect(stdout).toContain(`script=${join(binDir, 'imcli.mjs')}`)
    expect(stdout).toContain('args=help')
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

  it('sends a markdown/html file path through the local app bridge using project environment', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'report.md')
    await writeFile(filePath, '# Report\n')
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
      const { stdout } = await execFileAsync(
        process.execPath,
        [imcliPath, 'send-file', 'agent-b', filePath],
        {
          env: {
            ...process.env,
            MULTI_AI_CODE_IMCLI_URL: bridge.url,
            MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
            MULTI_AI_CODE_PROJECT_ID: 'project-1'
          }
        }
      )

      expect(stdout).toContain('sent file to agent-b')
      expect(sendPeerFile).toHaveBeenCalledWith('project-1', filePath, 'agent-b')
    } finally {
      await bridge.close()
    }
  })
})
