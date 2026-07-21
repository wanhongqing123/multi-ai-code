import { promises as fs } from 'fs'
import { readFileSync } from 'fs'
import net from 'net'
import { tmpdir } from 'os'
import { join } from 'path'
import { fileURLToPath } from 'url'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const ipcHandlers = vi.hoisted(() => new Map<string, (...args: unknown[]) => unknown>())
const buildSystemPromptMock = vi.hoisted(() => vi.fn(async () => 'system prompt'))
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
    on: vi.fn(),
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

vi.mock('../orchestrator/prompts.js', () => ({
  buildSystemPrompt: buildSystemPromptMock,
}))

vi.mock('./PtyCCProcess.js', () => ({
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
    receivedLines: string[] = []
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
              text: 'queued'
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

    const { setActiveAccount } = await import('../store/paths.js')
    setActiveAccount('test-account')
    const { registerPtyIpc } = await import('./ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-1',
      projectId: 'project-1',
      projectDir,
      targetRepo,
      planName: 'demo',
      planAbsPath: join(targetRepo, '.multi-ai-code', 'designs', 'demo.md'),
      planPending: true,
      initialUserMessage: 'start task',
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

    const { setActiveAccount } = await import('../store/paths.js')
    setActiveAccount('test-account')
    const { registerPtyIpc } = await import('./ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-no-plan',
      projectId: 'project-1',
      projectDir,
      targetRepo,
      planName: '',
      planMode: 'none',
      allowScheduledTasks: true,
      command,
      args: [],
      mode: 'new',
    })

    // opencode 会附带 launchNotice 字段，claude 不带；两者 ok 均为 true。
    expect(result).toMatchObject({ ok: true })
    expect(ptyInstances).toHaveLength(1)
    return { proc: ptyInstances[0] }
  }

  it('waits for Claude to become interactive before injecting the bootstrap prompt', async () => {
    const { proc, targetRepo } = await spawnClaudeSession()

    proc.emitData('Claude Code is starting...')
    await sleep(3_000)

    expect(proc.writes.join('')).not.toContain('.injections')

    proc.emitData('ready\n? for shortcuts')
    await sleep(2_000)

    const written = proc.writes.join('')
    expect(written).toContain('Please fully read')
    expect(written.replace(/\\/g, '/')).toContain(`${targetRepo.replace(/\\/g, '/')}/.injections/claude-system.md`)
  }, 10_000)

  it('allows no-plan sessions without injecting the plan bootstrap prompt', async () => {
    buildSystemPromptMock.mockClear()
    const { proc } = await spawnNoPlanSession()

    proc.emitData(
      'ready\nAdministrator@WIN  C:\\repo  Opus 4.8\n▸ bypass permissions on (shift+tab to cycle) · ← for agents'
    )
    await sleep(2_000)

    expect(proc.writes.join('')).toBe('')
    expect(buildSystemPromptMock).not.toHaveBeenCalled()
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
    expect(env.MULTI_AI_CODE_ROOT_DIR).toBeTruthy()
    expect(pathValue.split(':').some((item) => item.endsWith('/bin'))).toBe(true)
  })

  it('enables OpenCode LSP for spawned AICLI sessions', async () => {
    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-target-opencode-'))
    const projectDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-project-opencode-'))
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify({ name: 'demo' }), 'utf8')

    const { setActiveAccount } = await import('../store/paths.js')
    setActiveAccount('test-account')
    const { registerPtyIpc } = await import('./ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:spawn')
    if (!handler) throw new Error('cc:spawn handler was not registered')

    const result = await handler({}, {
      sessionId: 'session-opencode',
      projectId: 'project-1',
      projectDir,
      targetRepo,
      planName: '',
      planMode: 'none',
      command: 'opencode',
      args: [],
      opencode: {
        providerId: 'multi-ai-deepseek-internal',
        name: '公司内网 DeepSeek',
        baseURL: 'https://llm.example.test/v1',
        apiKey: 'test-api-key',
        mainModel: 'deepseek-v4-pro'
      },
      mode: 'new',
    })

    expect(result).toMatchObject({ ok: true })
    const env = ptyInstances[0].opts.env as Record<string, string>
    expect(JSON.parse(env.OPENCODE_CONFIG_CONTENT)).toMatchObject({
      lsp: true,
      model: 'multi-ai-deepseek-internal/deepseek-v4-pro',
      provider: {
        'multi-ai-deepseek-internal': {
          options: {
            baseURL: 'https://llm.example.test/v1',
            apiKey: 'test-api-key'
          }
        }
      }
    })
  })

  it('resolves Codex launch through the bundled policy, never a host/custom path', async () => {
    const { registerPtyIpc } = await import('./ptyManager.js')
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

  it('only exposes task-watch sessions to the scheduled task scheduler', async () => {
    await spawnClaudeSession()
    const ptyManager = (await import('./ptyManager.js')) as typeof import('./ptyManager.js') & {
      getScheduledTaskSessionForProject?: (projectId: string) => {
        sessionId: string
        targetRepo: string
      } | null
    }

    expect(typeof ptyManager.getScheduledTaskSessionForProject).toBe('function')
    expect(ptyManager.getScheduledTaskSessionForProject?.('project-1') ?? null).toBeNull()

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
      planName: '',
      planMode: 'none',
      allowScheduledTasks: true,
      command: 'claude',
      args: [],
      mode: 'new',
    })

    expect(result).toEqual({ ok: true })
    expect(ptyManager.getScheduledTaskSessionForProject?.('project-watch')).toEqual({
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

    const { sendUserMessageToSession } = await import('./ptyManager.js')
    const result = await sendUserMessageToSession('session-no-plan', 'remote im text')

    expect(result).toEqual({ ok: false, error: 'AICLI control bridge is not connected' })
    expect(proc.writes.join('')).not.toContain('remote im text')
  })

  it('accepts Codex messages after the source-level control bridge is ready', async () => {
    const { proc } = await spawnNoPlanSession('codex')
    const socket = await connectAicliControlBridge(proc)

    const { sendUserMessageToSession } = await import('./ptyManager.js')
    const result = await sendUserMessageToSession('session-no-plan', 'remote im text')

    socket.destroy()

    expect(result).toEqual({ ok: true })
    expect(proc.writes.join('')).not.toContain('remote im text')
  })

  it('broadcasts remote IM display text to the local terminal without changing PTY input', async () => {
    const { proc } = await spawnNoPlanSession()
    proc.emitData(
      'ready\nAdministrator@WIN  C:\\repo  Opus 4.8\n▸ bypass permissions on (shift+tab to cycle) · ← for agents'
    )

    const { sendUserMessageToSession } = await import('./ptyManager.js')
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
    const socket = await connectAicliControlBridge(proc)

    const { sendUserMessageToSession } = await import('./ptyManager.js')
    const result = await sendUserMessageToSession(
      'session-no-plan',
      'full AICLI protocol prompt',
      {
        displayText: '[来自远程 IM：mac-apollo-u3player]\n你好'
      }
    )
    socket.destroy()

    expect(result).toEqual({ ok: true })
    const written = proc.writes.join('')
    expect(written).not.toContain('full AICLI protocol prompt')
    expect(browserWindowSends.filter((send) => send.channel === 'cc:data')).toEqual([])
    expect(written).not.toContain('\r')
  })

  it('serializes concurrent programmatic messages to the same AICLI session', async () => {
    const { proc } = await spawnNoPlanSession('opencode')
    const receivedLines: string[] = []
    const socket = await connectAicliControlBridge(proc, receivedLines)

    const { sendUserMessageToSession } = await import('./ptyManager.js')
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
      fileURLToPath(new URL('./ptyManager.ts', import.meta.url)),
      'utf8'
    )

    expect(source).not.toContain("import { scanLocalSkills }")
    expect(source).not.toContain('scanLocalSkills()')
    expect(source).not.toContain('decorateUserMessageWithSkillContext')
  })
})
