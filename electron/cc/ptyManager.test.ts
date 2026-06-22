import { promises as fs } from 'fs'
import { readFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { fileURLToPath } from 'url'
import { beforeEach, describe, expect, it, vi } from 'vitest'

const ipcHandlers = vi.hoisted(() => new Map<string, (...args: unknown[]) => unknown>())
const buildSystemPromptMock = vi.hoisted(() => vi.fn(async () => 'system prompt'))
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
    getAllWindows: () => [],
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

  async function spawnNoPlanSession(): Promise<{
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
      command: 'claude',
      args: [],
      mode: 'new',
    })

    expect(result).toEqual({ ok: true })
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
