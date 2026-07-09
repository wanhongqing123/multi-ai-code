import { promises as fs } from 'fs'
import { readFileSync } from 'fs'
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

  async function spawnClaudeSession(): Promise<{
    proc: (typeof ptyInstances)[number]
    targetRepo: string
  }> {
    const targetRepo = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-target-'))
    const projectDir = await fs.mkdtemp(join(tmpdir(), 'multi-ai-code-project-'))
    await fs.writeFile(join(projectDir, 'project.json'), JSON.stringify({ name: 'demo' }), 'utf8')

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

  it('resolves Codex launch notice for the boot gate before spawning', async () => {
    const { registerPtyIpc } = await import('./ptyManager.js')
    registerPtyIpc()

    const handler = ipcHandlers.get('cc:resolve-launch')
    if (!handler) throw new Error('cc:resolve-launch handler was not registered')

    const result = await handler({}, { command: '/custom/bin/codex', env: {} })

    expect(result).toMatchObject({
      ok: true,
      notice: '当前启动 Codex：自定义路径 /custom/bin/codex'
    })
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

  it('skips the local display echo and double submit for opencode sessions', async () => {
    const { proc } = await spawnNoPlanSession('opencode')

    const { sendUserMessageToSession } = await import('./ptyManager.js')
    const result = await sendUserMessageToSession(
      'session-no-plan',
      'full AICLI protocol prompt',
      {
        displayText: '[来自远程 IM：mac-apollo-u3player]\n你好'
      }
    )

    expect(result).toEqual({ ok: true })
    const written = proc.writes.join('')
    expect(written).toContain('full AICLI protocol prompt')
    // opencode 的 TUI 自己展示已提交消息，合成回显会画花 TUI，必须跳过。
    expect(browserWindowSends.filter((send) => send.channel === 'cc:data')).toEqual([])
    // 首个回车即提交；第二个回车会追加一条空消息，必须只发一次。
    expect(written.match(/\r/g)).toHaveLength(1)
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
