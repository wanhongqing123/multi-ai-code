import { execFile, spawn } from 'node:child_process'
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

function runImcliWithStdin(
  args: string[],
  stdinText: string,
  env: NodeJS.ProcessEnv
): Promise<{ stdout: string; code: number | null }> {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [imcliPath, ...args], { env })
    let output = ''
    child.stdout.on('data', (data) => {
      output += String(data)
    })
    child.stderr.on('data', (data) => {
      output += String(data)
    })
    child.on('error', reject)
    child.on('close', (code) => resolve({ stdout: output, code }))
    child.stdin.end(stdinText, 'utf8')
  })
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
    expect(stdout).toContain('imcli send <user> <text | ->')
    expect(stdout).toContain('imcli send-image <user> <imagePath>')
    expect(stdout).toContain('imcli send-file <user> <filePath>')
    expect(stdout).toContain('imcli history')
    expect(stdout).toContain('Output format: #<id> <role>/<direction> <from> -> <to>: <content>')
    expect(stdout).toContain('Use one of these user IDs as the <user> argument')
    expect(stdout).toContain('Send a local file of any type to one user (up to 100MB).')
    expect(stdout).toContain('in iOS, Android, or Desktop IM to preview it.')
    expect(stdout).toContain('the receiver taps to save locally')
    expect(stdout).toContain('This is text-only. Use send-image or send-file separately for attachments.')
    expect(stdout).toContain('Requirements:')
    expect(stdout).toContain('MULTI_AI_CODE_PROJECT_ID')
    expect(stdout).toContain('Markdown and HTML files')
    expect(stdout).toContain('Use send-image for png/jpg/jpeg/gif/webp image files')
    expect(stdout).toContain('Examples:')
    expect(stdout).toContain('imcli send-image phone-user C:\\temp\\screenshot.png --project project-1')
    expect(stdout).toContain('imcli send-file phone-user ./report.md --project project-1')
  })

  // bin/imcli 是 sh 包装，只在 mac/Linux 上可执行；Windows 上 spawn 它必然 ENOENT。
  // Windows 的对应覆盖见下面两条（imcli.cmd / imcli.ps1）。
  it.runIf(process.platform !== 'win32')(
    'prefers the packaged Electron runtime before falling back to host node',
    async () => {
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

  it('windows wrappers prefer the packaged Electron runtime before host node', async () => {
    const cmdWrapper = await readFile(join(process.cwd(), 'bin', 'imcli.cmd'), 'utf8')
    const ps1Wrapper = await readFile(join(process.cwd(), 'bin', 'imcli.ps1'), 'utf8')

    // 安装到没有装 Node 的机器上时，随包 Electron 是唯一的运行时来源。
    // macOS 的 bin/imcli 早就这么做了，Windows 这两个包装器长期只会调裸 node。
    for (const [name, wrapper] of [
      ['imcli.cmd', cmdWrapper],
      ['imcli.ps1', ps1Wrapper]
    ] as const) {
      expect(wrapper, name).toContain('..\\..\\..\\Multi-AI Code.exe')
      const packagedIndex = wrapper.indexOf('ELECTRON_RUN_AS_NODE')
      const hostNodeIndex = wrapper.lastIndexOf('node ')
      expect(packagedIndex, name).toBeGreaterThan(-1)
      expect(hostNodeIndex, name).toBeGreaterThan(-1)
      expect(packagedIndex, name).toBeLessThan(hostNodeIndex)

      // cmd.exe 与 PowerShell 5.1 都按系统 ANSI 代码页读取无 BOM 文件：
      // 中文注释会被错解码，字节落到反引号上还会吞掉后续行（实测踩过）。
      // eslint-disable-next-line no-control-regex
      expect(/^[\x00-\x7F]*$/.test(wrapper), `${name} must stay ASCII-only`).toBe(true)
    }

    // PowerShell 把管道输入绑定到脚本自己的 $input，不会转发给子进程；
    // 少了这一步，`imcli send <user> -` 的 stdin 模式永远读到空。
    expect(ps1Wrapper).toContain('$input |')
    // 随包 Electron 是 GUI 子系统程序，PowerShell 不等它结束：直接调用会出现
    // 输出捕获为空、$LASTEXITCODE 不可信。接一段管道才能强制同步。
    expect(ps1Wrapper).toContain('| Write-Output')
  })

  it.runIf(process.platform === 'win32')(
    'runs imcli through the packaged runtime and forwards stdin on windows',
    async () => {
      const rootDir = await createTempDir()
      const binDir = join(rootDir, 'resources', 'app.asar.unpacked', 'bin')
      await mkdir(binDir, { recursive: true })
      // 用真实的 node.exe 冒充随包 Electron：它同样会执行传入的脚本，
      // 而脚本回报 process.execPath，能直接证明包装器选的是哪个运行时。
      await copyFile(process.execPath, join(rootDir, 'Multi-AI Code.exe'))
      await copyFile(join(process.cwd(), 'bin', 'imcli.cmd'), join(binDir, 'imcli.cmd'))
      await writeFile(
        join(binDir, 'imcli.mjs'),
        [
          'let body = ""',
          'process.stdin.on("data", (chunk) => { body += chunk })',
          'process.stdin.on("end", () => {',
          '  console.log("RUNTIME=" + process.execPath)',
          '  console.log("ARGV=" + JSON.stringify(process.argv.slice(2)))',
          '  console.log("STDIN=" + body.trim())',
          '})'
        ].join('\n')
      )

      const { stdout } = await new Promise<{ stdout: string }>((resolve, reject) => {
        // Node 在 Windows 上不允许直接 spawn .cmd/.bat（CVE-2024-27980 之后的限制），
        // 经 cmd.exe /c 调用，同时避开 shell:true 的引号陷阱（路径含空格）。
        const child = spawn(
          process.env.ComSpec ?? 'cmd.exe',
          ['/c', join(binDir, 'imcli.cmd'), 'send', 'agent-b', '-'],
          { shell: false }
        )
        let output = ''
        child.stdout.on('data', (data) => {
          output += String(data)
        })
        child.on('error', reject)
        child.on('close', () => resolve({ stdout: output }))
        child.stdin.end('多行\n正文', 'utf8')
      })

      expect(stdout).toContain(`RUNTIME=${join(rootDir, 'Multi-AI Code.exe')}`)
      expect(stdout).toContain('ARGV=["send","agent-b","-"]')
      expect(stdout).toContain('STDIN=多行\n正文')
    }
  )

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

  it('documents the truncation pitfall and safe multi-line patterns in help', async () => {
    const { stdout } = await execFileAsync(process.execPath, [imcliPath, 'help'])

    expect(stdout).toContain('imcli send <user> <text | ->')
    expect(stdout).toContain('Multi-line text — READ THIS FIRST (prevents silent truncation):')
    expect(stdout).toContain('stops parsing arguments at the first real newline')
    expect(stdout).toContain('SILENTLY cut down to its first line')
    expect(stdout).toContain('imcli expands it to newlines')
    expect(stdout).toContain('imcli send phone-user "标题\\n第二行\\n第三行"')
    expect(stdout).toContain('pipe the body via stdin')
    expect(stdout).toContain('type msg.txt | imcli send phone-user -')
    expect(stdout).toContain('Get-Content msg.txt -Raw | imcli send phone-user -')
    expect(stdout).toContain('use imcli send-file instead of send')
    expect(stdout).toContain('does not guarantee integrity')
  })

  it('sends multi-line text read from stdin when the text argument is -', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage
    })

    // cmd.exe 的批处理包装无法在 argv 里携带换行（多行文本会被截断成首行）；
    // stdin 管道是多行文本的可靠通道，这里验证 `-` 模式端到端不丢内容。
    const multiline = '【Code Review 巡查】\n- 第一条结论\n- 第二条结论\n\n结尾行'
    try {
      const { stdout, code } = await runImcliWithStdin(['send', 'agent-b', '-'], multiline, {
        ...process.env,
        MULTI_AI_CODE_IMCLI_URL: bridge.url,
        MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
        MULTI_AI_CODE_PROJECT_ID: 'project-1'
      })

      expect(code).toBe(0)
      expect(stdout).toContain('sent to agent-b')
      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', multiline, 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('expands literal \\n escapes in a single-line text argument into newlines', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage
    })

    // cmd.exe 通道无法在 argv 携带真实换行；调用方以字面量 \n 表达换行，imcli 展开。
    try {
      const { stdout } = await execFileAsync(
        process.execPath,
        [imcliPath, 'send', 'agent-b', '【标题】\\n第二行\\n第三行'],
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
      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', '【标题】\n第二行\n第三行', 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('keeps literal \\n untouched when the text already contains real newlines', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage
    })

    // 真实换行能到达说明通道无截断，此时 \n 视为字面内容（如 Windows 路径 C:\new）。
    const text = '报告在 C:\\new\\report.md\n第二行'
    try {
      await execFileAsync(process.execPath, [imcliPath, 'send', 'agent-b', text], {
        env: {
          ...process.env,
          MULTI_AI_CODE_IMCLI_URL: bridge.url,
          MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
          MULTI_AI_CODE_PROJECT_ID: 'project-1'
        }
      })

      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', text, 'agent-b')
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

  it('sends a generic file path through the local app bridge using project environment', async () => {
    const rootDir = await createTempDir()
    const filePath = join(rootDir, 'bundle.zip')
    await writeFile(filePath, new Uint8Array([0x50, 0x4b, 3, 4]))
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
