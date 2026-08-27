import { promises as fs } from 'fs'
import { readFileSync } from 'fs'
import net from 'net'
import { tmpdir } from 'os'
import { join } from 'path'
import { fileURLToPath } from 'url'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const ipcHandlers = vi.hoisted(() => new Map<string, (...args: unknown[]) => unknown>())
const browserWindowSends = vi.hoisted(() => [] as Array<{ channel: string; payload: unknown }>)
const interactionEvents = vi.hoisted(() => [] as string[])
const ptyInstances = vi.hoisted(() => [] as Array<{
  writes: string[]
  opts: Record<string, unknown>
  emitData: (chunk: string) => void
}>)

vi.mock('electron', () => ({
  ipcMain: {
    handle: vi.fn((channel: string, handler: (...args: unknown[]) => unknown) => {
      ipcHandlers.set(channel, handler)
    }),
    on: vi.fn((channel: string, handler: (...args: unknown[]) => unknown) => {
      ipcHandlers.set(channel, handler)
    }),
  },
  BrowserWindow: {
    getAllWindows: () => [
      {
        isDestroyed: () => false,
        webContents: {
          send: (channel: string, payload: unknown) => {
            const chunk =
              channel === 'cc:data' && payload && typeof payload === 'object'
                ? (payload as { chunk?: unknown }).chunk
                : null
            interactionEvents.push(
              typeof chunk === 'string' && chunk.includes('[来自远程 IM：')
                ? 'browser:remote-im-display'
                : `browser:${channel}`
            )
            browserWindowSends.push({ channel, payload })
          }
        }
      }
    ],
  },
}))

vi.mock('../../../electron/cc/PtyCCProcess.js', () => ({
  PtyCCProcess: class MockPtyCCProcess {
    writes: string[] = []
    opts: Record<string, unknown>
    private handlers = new Map<string, Array<(chunk: string) => void>>()

    constructor(opts: Record<string, unknown>) {
      this.opts = opts
      ptyInstances.push(this)
    }

    start(): void {
      /* no-op */
    }

    write(data: string): void {
      interactionEvents.push(`pty:${data}`)
      this.writes.push(data)
    }

    on(event: string, cb: (chunk: string) => void): void {
      const handlers = this.handlers.get(event) ?? []
      handlers.push(cb)
      this.handlers.set(event, handlers)
    }

    emitData(chunk: string): void {
      for (const cb of this.handlers.get('data') ?? []) cb(chunk)
    }
  },
}))

vi.mock('../../../electron/aicli/opencodeCredentials.js', () => ({
  readOpenCodeCredentialEnv: () => ({ ZHIPU_API_KEY: 'test-zhipu-key' })
}))

describe('registerPtyIpc prompt injection timing', () => {
  beforeEach(() => {
    vi.resetModules()
    ipcHandlers.clear()
    browserWindowSends.length = 0
    interactionEvents.length = 0
    ptyInstances.length = 0
  })

  const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms))

  async function connectAicliControlBridge(
    proc: (typeof ptyInstances)[number],
    receivedLines: string[] = [],
    controlResultExtra: Record<string, unknown> = {}
  ): Promise<net.Socket> {
    const args = proc.opts.args as string[]
    const endpoint = args[args.indexOf('--multi-ai-code-im-ipc') + 1]
    if (!endpoint) throw new Error('AICLI bridge endpoint was not passed to the process')
    const url = new URL(endpoint)
    const token = url.searchParams.get('token')
    if (!token) throw new Error('AICLI bridge endpoint did not include a token')

    const socket = await new Promise<net.Socket>((resolve, reject) => {
      let buffer = ''
      const client = net.createConnection(
        { host: url.hostname, port: Number(url.port) },
        () => {
          client.write(`${JSON.stringify({ token, kind: 'control_ready' })}\n`)
          resolve(client)
        }
      )
      client.setEncoding('utf8')
      client.on('data', (chunk) => {
        buffer += String(chunk)
        for (;;) {
          const lineEnd = buffer.indexOf('\n')
          if (lineEnd < 0) break
          const line = buffer.slice(0, lineEnd).trim()
          buffer = buffer.slice(lineEnd + 1)
          if (!line) continue
          receivedLines.push(line)
          const payload = JSON.parse(line) as {
            command?: string
            requestId?: string
          }
          if (payload.command !== 'submit_user_message' || !payload.requestId) continue
          client.write(
            `${JSON.stringify({
              token,
              kind: 'control_result',
              requestId: payload.requestId,
              ok: true,
              text: 'queued',
              ...controlResultExtra
            })}\n`
          )
        }
      })
      client.once('error', reject)
    })
    await sleep(20)
    return socket
  }

  async function spawnClaudeSession(): Promise<{
    proc: (typeof ptyInstances)[number]
    targetRepo: string
  }> {
    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-target-'))
    const projectDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-project-'))
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify({ name: 'demo' }), 'utf8')

    const { setActiveAccount } = await import('../../../electron/store/paths.js')
    setActiveAccount('test-account')
    const { registerPtyIpc } = await import('../../../electron/cc/ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-1',
      projectId: 'project-1',
      projectDir,
      targetRepo,
      command: 'claude',
      args: [],
      mode: 'new',
    })

    expect(result).toEqual({ ok: true })
    expect(ptyInstances).toHaveLength(1)
    return { proc: ptyInstances[0], targetRepo }
  }

  async function spawnNoPlanSession(command = 'claude'): Promise<{
    proc: (typeof ptyInstances)[number]
  }> {
    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-target-'))
    const projectDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-project-'))
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify({ name: 'demo' }), 'utf8')

    const { setActiveAccount } = await import('../../../electron/store/paths.js')
    setActiveAccount('test-account')
    const { registerPtyIpc } = await import('../../../electron/cc/ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-no-plan',
      projectId: 'project-1',
      projectDir,
      targetRepo,
      command,
      args: [],
      mode: 'new',
    })

    // opencode 会附带 launchNotice 字段，claude 不带；两者 ok 均为 true。
    expect(result).toMatchObject({ ok: true })
    expect(ptyInstances).toHaveLength(1)
    return { proc: ptyInstances[0] }
  }

  // 系统提示词注入链路已整体下线（planMode / .injections / buildSystemPrompt）。
  // 起一个新会话不该再往仓库里写 .injections，也不该往 PTY 里灌 bootstrap 文案——
  // 那两条以前分别由 spawnClaudeSession / spawnNoPlanSession 覆盖，现在合成一条。
  it('never injects a system prompt or writes .injections on spawn', async () => {
    const { proc, targetRepo } = await spawnClaudeSession()

    proc.emitData('Claude Code is starting...')
    await sleep(1_000)
    proc.emitData('ready\n? for shortcuts')
    await sleep(2_000)

    expect(proc.writes.join('')).toBe('')
    await expect(fs.access(join(targetRepo, '.injections'))).rejects.toThrow()
  }, 10_000)

  it('starts embedded Claude sessions with the default TUI renderer', async () => {
    const { proc } = await spawnNoPlanSession()

    expect(proc.opts.args).toEqual(['--settings', JSON.stringify({ tui: 'default' })])
  })

  it('exposes imcli environment to spawned AICLI sessions', async () => {
    const { proc } = await spawnNoPlanSession()
    const env = proc.opts.env as Record<string, string>
    const pathValue = env.PATH ?? env.Path ?? ''

    expect(env.MULTI_AI_CODE_PROJECT_ID).toBe('project-1')
    expect(env.MULTI_AI_CODE_SESSION_ID).toBe('session-no-plan')
    expect(env.MULTI_AI_CODE_ROOT_DIR).toBeTruthy()
    expect(pathValue.split(':').some((item) => item.endsWith('/bin'))).toBe(true)
  })

  it('starts OpenCode with the managed account runtime and curated providers', async () => {
    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-target-opencode-'))
    const projectDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-project-opencode-'))
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify({ name: 'demo' }), 'utf8')

    const { setActiveAccount } = await import('../../../electron/store/paths.js')
    setActiveAccount('test-account')
    const { registerPtyIpc } = await import('../../../electron/cc/ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-opencode',
      projectId: 'project-1',
      projectDir,
      targetRepo,
      command: 'opencode',
      args: [],
      opencode: {
        providerId: 'legacy-custom-provider',
        name: '旧的项目级模型服务',
        baseURL: 'https://llm.example.test/v1',
        apiKey: 'test-api-key',
        mainModel: 'legacy-model'
      },
      mode: 'new',
    })

    expect(result).toMatchObject({ ok: true })
    const env = ptyInstances[0].opts.env as Record<string, string>
    const config = JSON.parse(env.OPENCODE_CONFIG_CONTENT) as Record<string, unknown>
    expect(config).toMatchObject({
      lsp: true,
      model: 'zhipu/glm-5.3',
      small_model: 'zhipu/glm-5.3',
      enabled_providers: ['zhipu']
    })
    expect(config).not.toHaveProperty('provider')
    expect(env.OPENCODE_RUNTIME_ROOT).toContain('/accounts/test-account/aicli/opencode')
    expect(env.OPENCODE_MODELS_PATH).toMatch(/managed-models\.json$/)
    expect(env.OPENCODE_MANAGED_ROUTING_PATH).toMatch(/managed-routing\.json$/)
    expect(env.ZHIPU_API_KEY).toBe('test-zhipu-key')
    expect(env.LEGACY_CUSTOM_PROVIDER_API_KEY).toBeUndefined()
  })

  it('resolves Codex launch through the bundled policy, never a host/custom path', async () => {
    const { registerPtyIpc } = await import('../../../electron/cc/ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:resolve-launch')
    if (!handler) throw new Error('cc:resolve-launch handler was not registered')

    // codex 已深度定制，只走内置版本：即便配置成宿主机上的自定义路径，也强制解析到内置，
    // 找不到内置就直接报错——绝不再以「自定义路径 / 系统 PATH」启动宿主机上的 codex。
    const result = (await handler({}, { command: '/custom/bin/codex', env: {} })) as {
      ok: boolean
      notice?: string
      error?: string
    }

    if (result.ok) {
      expect(result.notice).toContain('内置版本')
      expect(result.notice).not.toContain('自定义路径')
      expect(result.notice).not.toContain('系统 PATH')
    } else {
      expect(result.error).toContain('内置')
    }
  })

  it('exposes every session to the scheduled task scheduler', async () => {
    // 定时任务与普通任务并存：会话怎么起的都一样能接定时任务。
    // 此前会话上带一个 allowScheduledTasks 标记，只有"定时任务模式"下启动的
    // 会话才会被调度器看见——普通模式下起的会话，定时任务到点也不跑。
    await spawnClaudeSession()
    const ptyManager = await import('../../../electron/cc/ptyManager.js')

    // spawnClaudeSession 起的是一个带方案的普通会话，没有任何"允许定时任务"
    // 的标记，调度器照样能看到它。
    expect(ptyManager.getActiveSessionForProject('project-1')).toMatchObject({
      sessionId: 'session-1'
    })

    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-target-watch-'))
    const projectDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-project-watch-'))
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify({ name: 'watch' }), 'utf8')
    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-watch',
      projectId: 'project-watch',
      projectDir,
      targetRepo,
      command: 'claude',
      args: [],
      mode: 'new',
    })

    expect(result).toEqual({ ok: true })
    expect(ptyManager.getActiveSessionForProject('project-watch')).toEqual({
      sessionId: 'session-watch',
      targetRepo
    })
  })

  it('waits for no-plan Claude sessions to become interactive before sending user messages', async () => {
    const { proc } = await spawnNoPlanSession()
    const sendHandler = ipcHandlers.get('cc:send-user')
    if (!sendHandler) throw new Error('cc:send-user handler was not registered')

    const sendPromise = sendHandler(
      {},
      { sessionId: 'session-no-plan', text: 'scheduled task prompt' }
    ) as Promise<{ ok: boolean; error?: string }>

    await sleep(300)
    expect(proc.writes.join('')).not.toContain('scheduled task prompt')

    proc.emitData(
      'ready\nAdministrator@WIN  C:\\repo  Opus 4.8\n▸ bypass permissions on (shift+tab to cycle) · ← for agents'
    )
    const result = await sendPromise

    expect(result).toEqual({ ok: true })
    expect(proc.writes.join('')).toContain('scheduled task prompt')
  }, 10_000)

  it('does not fall back to PTY typing when the Codex source bridge is disconnected', async () => {
    const { proc } = await spawnNoPlanSession('codex')

    proc.emitData(
      [
        '› Find and fix a bug in @filename',
        '',
        'gpt-5.6-sol high · ~/Apollo/u3player · gpt-5.6-sol · u3player · Context 6% used · weekly 46% left'
      ].join('\n')
    )

    const { sendUserMessageToSession } = await import('../../../electron/cc/ptyManager.js')
    const result = await sendUserMessageToSession('session-no-plan', 'remote im text')

    expect(result).toEqual({ ok: false, error: 'AICLI control bridge is not connected' })
    expect(proc.writes.join('')).not.toContain('remote im text')
  })

  it('accepts Codex messages after the source-level control bridge is ready', async () => {
    const { proc } = await spawnNoPlanSession('codex')
    const receivedLines: string[] = []
    const socket = await connectAicliControlBridge(proc, receivedLines)

    const { sendUserMessageToSession } = await import('../../../electron/cc/ptyManager.js')
    const result = await sendUserMessageToSession('session-no-plan', 'remote im text', {
      attachments: [
        {
          type: 'image',
          localPath: '/tmp/scheduled-task-image.png',
          mimeType: 'image/png',
          fileName: 'screen.png'
        }
      ]
    })

    socket.destroy()

    expect(result).toEqual({ ok: true })
    expect(proc.writes.join('')).not.toContain('remote im text')
    expect(JSON.parse(receivedLines[0] ?? '{}').attachments).toEqual([
      {
        type: 'image',
        localPath: '/tmp/scheduled-task-image.png',
        mimeType: 'image/png',
        fileName: 'screen.png'
      }
    ])
  })

  it('returns structured approval auto-decline metadata from Codex', async () => {
    const { proc } = await spawnNoPlanSession('codex')
    const socket = await connectAicliControlBridge(proc, [], {
      autoDeclinedApprovals: [
        { approvalId: 'approval-remove-1', commandSummary: 'Remove-Item …' }
      ]
    })

    const { sendUserMessageToSession } = await import('../../../electron/cc/ptyManager.js')
    const result = await sendUserMessageToSession('session-no-plan', 'new remote IM', {
      inputOrigin: 'remote-im'
    })

    socket.destroy()
    expect(result).toEqual({
      ok: true,
      detail: 'queued',
      autoDeclinedApprovals: [
        { approvalId: 'approval-remove-1', commandSummary: 'Remove-Item …' }
      ]
    })
  })

  it('submits machine IM input without reporting a local takeover', async () => {
    const { proc } = await spawnNoPlanSession('codex')
    const receivedLines: string[] = []
    const socket = await connectAicliControlBridge(proc, receivedLines)
    const localKinds: string[] = []
    const { addSessionLocalInputListener, sendUserMessageToSession } = await import(
      '../../../electron/cc/ptyManager.js'
    )
    const unsubscribe = addSessionLocalInputListener(({ kind }) => localKinds.push(kind))

    const result = await sendUserMessageToSession('session-no-plan', 'machine steer', {
      inputOrigin: 'remote-im-machine'
    })

    unsubscribe()
    socket.destroy()
    expect(result).toEqual({ ok: true })
    expect(localKinds).toEqual([])
    expect(JSON.parse(receivedLines[0] ?? '{}')).toMatchObject({
      command: 'submit_user_message',
      inputOrigin: 'remote-im-machine'
    })
  })

  it('broadcasts remote IM display text to the local terminal without changing PTY input', async () => {
    const { proc } = await spawnNoPlanSession()
    proc.emitData(
      'ready\nAdministrator@WIN  C:\\repo  Opus 4.8\n▸ bypass permissions on (shift+tab to cycle) · ← for agents'
    )

    const { sendUserMessageToSession } = await import('../../../electron/cc/ptyManager.js')
    const result = await sendUserMessageToSession(
      'session-no-plan',
      'full AICLI protocol prompt',
      {
        displayText: '[来自远程 IM：mac-apollo-u3player]\n你好'
      }
    )

    expect(result).toEqual({ ok: true })
    expect(proc.writes.join('')).toContain('full AICLI protocol prompt')
    expect(proc.writes.join('')).not.toContain('[来自远程 IM：mac-apollo-u3player]\n你好')
    const firstDisplayIndex = interactionEvents.findIndex(
      (event) => event === 'browser:remote-im-display'
    )
    const firstPtyInputIndex = interactionEvents.findIndex((event) =>
      event.startsWith('pty:full AICLI protocol prompt')
    )
    expect(firstPtyInputIndex).toBeGreaterThan(-1)
    expect(firstDisplayIndex).toBeGreaterThan(firstPtyInputIndex)
    expect(browserWindowSends).toContainEqual({
      channel: 'cc:data',
      payload: {
        sessionId: 'session-no-plan',
        chunk: '\r\n[来自远程 IM：mac-apollo-u3player]\r\n你好\r\n'
      }
    })
  })

  it('submits OpenCode messages through the source bridge without PTY input or local echo', async () => {
    const { proc } = await spawnNoPlanSession('opencode')
    const receivedLines: string[] = []
    const socket = await connectAicliControlBridge(proc, receivedLines)

    const { sendUserMessageToSession } = await import('../../../electron/cc/ptyManager.js')
    const result = await sendUserMessageToSession(
      'session-no-plan',
      'full AICLI protocol prompt',
      {
        displayText: '[来自远程 IM：mac-apollo-u3player]\n你好',
        attachments: [
          {
            type: 'image',
            localPath: '/tmp/remote-im/photo.png',
            mimeType: 'image/png',
            fileName: 'photo.png'
          }
        ]
      }
    )
    socket.destroy()

    expect(result).toEqual({ ok: true })
    const written = proc.writes.join('')
    expect(written).not.toContain('full AICLI protocol prompt')
    expect(browserWindowSends.filter((send) => send.channel === 'cc:data')).toEqual([])
    expect(written).not.toContain('\r')
    expect(JSON.parse(receivedLines[0] ?? '{}').attachments).toEqual([
      {
        type: 'image',
        localPath: '/tmp/remote-im/photo.png',
        mimeType: 'image/png',
        fileName: 'photo.png'
      }
    ])
  })

  it('serializes concurrent programmatic messages to the same AICLI session', async () => {
    const { proc } = await spawnNoPlanSession('opencode')
    const receivedLines: string[] = []
    const socket = await connectAicliControlBridge(proc, receivedLines)

    const { sendUserMessageToSession } = await import('../../../electron/cc/ptyManager.js')
    const firstMessage = `first:${'A'.repeat(160)}`
    const secondMessage = `second:${'B'.repeat(160)}`

    const [firstResult, secondResult] = await Promise.all([
      sendUserMessageToSession('session-no-plan', firstMessage),
      sendUserMessageToSession('session-no-plan', secondMessage)
    ])
    socket.destroy()

    expect(firstResult).toEqual({ ok: true })
    expect(secondResult).toEqual({ ok: true })
    expect(proc.writes.join('')).toBe('')
    expect(receivedLines.map((line) => JSON.parse(line).text)).toEqual([
      firstMessage,
      secondMessage
    ])
  })

  it('does not scan local skills when sending user messages', () => {
    const source = readFileSync(
      fileURLToPath(new URL('../../../electron/cc/ptyManager.ts', import.meta.url)),
      'utf8'
    )

    expect(source).not.toContain("import { scanLocalSkills }")
    expect(source).not.toContain('scanLocalSkills()')
    expect(source).not.toContain('decorateUserMessageWithSkillContext')
  })

  it('classifies terminal editing, navigation, submission and cancellation distinctly', async () => {
    const { proc } = await spawnNoPlanSession()
    const kinds: string[] = []
    const { addSessionLocalInputListener } = await import(
      '../../../electron/cc/ptyManager.js'
    )
    const unsubscribe = addSessionLocalInputListener(({ kind }) => kinds.push(kind))
    const inputHandler = ipcHandlers.get('cc:input')
    if (!inputHandler) throw new Error('cc:input handler was not registered')

    inputHandler({}, { sessionId: 'session-no-plan', data: '中' })
    inputHandler({}, { sessionId: 'session-no-plan', data: '\x1B[A' })
    inputHandler({}, { sessionId: 'session-no-plan', data: '\r' })
    inputHandler({}, { sessionId: 'session-no-plan', data: '\x03' })
    inputHandler({}, { sessionId: 'session-no-plan', data: '\x15' })
    await sleep(20)
    unsubscribe()

    const suppressesMainTuiCtrlC =
      process.platform === 'win32' || process.platform === 'darwin'
    expect(kinds).toEqual(
      suppressesMainTuiCtrlC
        ? ['editing', 'navigation', 'submit-key', 'cancel-editing']
        : ['editing', 'navigation', 'submit-key', 'interrupt', 'cancel-editing']
    )
    expect(proc.writes).toEqual(
      suppressesMainTuiCtrlC
        ? ['中', '\x1B[A', '\r', '\x15']
        : ['中', '\x1B[A', '\r', '\x03', '\x15']
    )
  })

  it('suppresses only a bare main-TUI Ctrl+C signal on Windows and macOS', async () => {
    const { shouldSuppressMainTuiCtrlC } = await import(
      '../../../electron/cc/ptyManager.js'
    )

    expect(shouldSuppressMainTuiCtrlC('\x03', 'win32')).toBe(true)
    expect(shouldSuppressMainTuiCtrlC('\x03', 'darwin')).toBe(true)
    expect(shouldSuppressMainTuiCtrlC('\x03', 'linux')).toBe(false)
    expect(shouldSuppressMainTuiCtrlC('text\x03', 'win32')).toBe(false)
  })
})
