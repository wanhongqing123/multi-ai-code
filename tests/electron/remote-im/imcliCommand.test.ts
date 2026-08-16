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
,
  remoteDesktopMode: 'disabled',
  remoteDesktopControl: false
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
    expect(stdout).toContain('imcli send <user> --text-b64 <base64>')
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

    // Windows PowerShell 5.1 的 $OutputEncoding 默认是 ASCII，往原生命令灌管道时
    // 每个非 ASCII 字符会被替换成 '?'——静默且不可逆（不像 GBK 乱码还能修回来）。
    // 必须写全局作用域：脚本作用域的赋值管道根本不认（实测 script 域仍出 '?'）。
    expect(ps1Wrapper).toContain('$global:OutputEncoding')
    expect(ps1Wrapper).toContain('UTF8Encoding')
  })

  it.runIf(process.platform === 'win32')(
    'keeps non-ascii intact through the powershell stdin pipe',
    async () => {
      const rootDir = await createTempDir()
      const binDir = join(rootDir, 'resources', 'app.asar.unpacked', 'bin')
      await mkdir(binDir, { recursive: true })
      await copyFile(join(process.cwd(), 'bin', 'imcli.ps1'), join(binDir, 'imcli.ps1'))
      await writeFile(
        join(binDir, 'imcli.mjs'),
        [
          'let body = ""',
          'process.stdin.setEncoding("utf8")',
          'process.stdin.on("data", (chunk) => { body += chunk })',
          'process.stdin.on("end", () => {',
          '  const points = Array.from(body.trim()).map((c) => c.codePointAt(0).toString(16))',
          '  console.log("CP=" + points.join(" "))',
          '})'
        ].join('\n')
      )

      // -NoProfile 让 $OutputEncoding 保持出厂默认（ASCII），正是回归发生的条件。
      const wrapper = join(binDir, 'imcli.ps1')
      const { stdout } = await execFileAsync('powershell.exe', [
        '-NoProfile',
        '-Command',
        `& { '中文' | & '${wrapper}' send agent-b - }`
      ])

      // 中 = U+4E2D, 文 = U+6587。退化成 ASCII 时两者都会变成 3f（'?'）。
      expect(stdout).toContain('CP=4e2d 6587')
    }
  )

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

  it('sends a base64 message through the local app bridge using project environment', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const authorizeCaller = vi.fn(() => ({ ok: true as const }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage,
      authorizeCaller
    })

    try {
      const text = 'hello from cli'
      const { stdout } = await execFileAsync(
        process.execPath,
        [imcliPath, 'send', 'agent-b', '--text-b64', Buffer.from(text, 'utf8').toString('base64')],
        {
          env: {
            ...process.env,
            MULTI_AI_CODE_IMCLI_URL: bridge.url,
            MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
            MULTI_AI_CODE_PROJECT_ID: 'project-1',
            MULTI_AI_CODE_SESSION_ID: 'session-a'
          }
        }
      )

      expect(stdout).toContain('sent to agent-b')
      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', text, 'agent-b')
      expect(authorizeCaller).toHaveBeenCalledWith('project-1', 'session-a')
    } finally {
      await bridge.close()
    }
  })

  it('delivers newlines, CJK, quotes and backslash paths byte for byte', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage
    })

    // 这一条集齐了此前三条通道各自的杀手输入：
    //   真实换行      —— cmd.exe 会在这里截断参数
    //   中文          —— PowerShell 5.1 的 ASCII 管道会压成 '?'
    //   C:\new/\notes —— 旧的 \n 字面量展开会把它们拆成换行
    //   引号 / % / `  —— shell 元字符
    const text = [
      '报告在 C:\\new\\report.md',
      '第二行：引号 "double" 与 \'single\'',
      '',
      '百分号 100% 反引号 `tick` 路径 C:\\temp\\notes.txt'
    ].join('\n')

    try {
      await execFileAsync(
        process.execPath,
        [imcliPath, 'send', 'agent-b', '--text-b64', Buffer.from(text, 'utf8').toString('base64')],
        {
          env: {
            ...process.env,
            MULTI_AI_CODE_IMCLI_URL: bridge.url,
            MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
            MULTI_AI_CODE_PROJECT_ID: 'project-1'
          }
        }
      )

      expect(sendPeerMessage).toHaveBeenCalledWith('project-1', text, 'agent-b')
    } finally {
      await bridge.close()
    }
  })

  it('fails loudly instead of sending a damaged message when base64 is invalid', async () => {
    const rootDir = await createTempDir()
    const sendPeerMessage = vi.fn(async () => ({ ok: true as const, toUserId: 'agent-b' }))
    const bridge = await startRemoteImCliServer({
      rootDir,
      getConfig: async () => config,
      getStatus: async () => status,
      listMessages: () => [],
      sendPeerMessage
    })

    const env = {
      ...process.env,
      MULTI_AI_CODE_IMCLI_URL: bridge.url,
      MULTI_AI_CODE_IMCLI_TOKEN: bridge.token,
      MULTI_AI_CODE_PROJECT_ID: 'project-1'
    }

    try {
      // 一个被 shell 截断过的 payload 必须报错，而不是解出半截正文照样发出去——
      // 静默发出残缺内容正是过去几次事故的共同形态。
      await expect(
        execFileAsync(process.execPath, [imcliPath, 'send', 'agent-b', '--text-b64', '不是base64'], {
          env
        })
      ).rejects.toThrow(/standard Base64/)

      // 完全没给正文时也不能当成空消息发出去。
      await expect(
        execFileAsync(process.execPath, [imcliPath, 'send', 'agent-b'], { env })
      ).rejects.toThrow(/--text-b64/)

      expect(sendPeerMessage).not.toHaveBeenCalled()
    } finally {
      await bridge.close()
    }
  })

  it('documents base64 as the only message channel and why the old ones were removed', async () => {
    const { stdout } = await execFileAsync(process.execPath, [imcliPath, 'help'])
    // help 是折行排版的，断言前把连续空白压成单空格，免得调整折行就误伤测试。
    const flat = stdout.replace(/\s+/g, ' ')

    expect(stdout).toContain('imcli send <user> --text-b64 <base64>')
    expect(stdout).toContain('imcli broadcast <user1,user2> --text-b64 <base64>')
    // 三条被删掉的通道各自的失败原因都要留在文档里：不写清楚，下一个人会把它们加回来。
    expect(flat).toContain('cmd.exe truncates at the first newline')
    expect(flat).toContain("turns non-ASCII into '?'")
    expect(flat).toContain('expansion mangled Windows paths')
    // 旧通道不得再出现在 Usage 里。
    expect(stdout).not.toContain('imcli send <user> <text | ->')
    expect(stdout).not.toContain('--file <path>')
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
