import { describe, expect, it, vi } from 'vitest'
import { executeRemoteImControlCommand } from './controlBridge.js'

describe('remote IM control bridge', () => {
  it('returns help without touching an AICLI session', async () => {
    const result = await executeRemoteImControlCommand({
      command: 'help',
      session: null,
      sourceKind: 'unknown'
    })

    expect(result.ok).toBe(true)
    expect(result.text).toContain('/status')
    expect(result.text).toContain('/plan')
    expect(result.text).toContain('/build')
    expect(result.text).not.toContain('/stop')
  })

  it('requests the active Codex status through the source-level bridge', async () => {
    const executeCommand = vi.fn(async () => ({
      ok: true as const,
      text: 'OpenAI Codex\nModel gpt-5.6-sol\nContext window 100% left'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'status',
      session: {
        sessionId: 'session-1',
        targetRepo: '/repo',
        command: '/bundled/codex',
        startedAtMs: 1000
      },
      sourceKind: 'codex',
      executeCommand
    })

    expect(result.ok).toBe(true)
    expect(result.text).toContain('OpenAI Codex')
    expect(result.text).toContain('Context window')
    expect(result.text).not.toContain('运行时长')
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'status'
    })
  })

  it('switches OpenCode mode through an injected source-level bridge', async () => {
    const switchMode = vi.fn(async () => ({ ok: true as const }))
    const result = await executeRemoteImControlCommand({
      command: 'plan',
      session: {
        sessionId: 'session-opencode',
        targetRepo: '/repo',
        command: 'opencode',
        startedAtMs: 0
      },
      sourceKind: 'opencode',
      switchMode
    })

    expect(result.ok).toBe(true)
    expect(result.text).toContain('已切换到计划模式')
    expect(switchMode).toHaveBeenCalledWith({
      sessionId: 'session-opencode',
      sourceKind: 'opencode',
      mode: 'plan'
    })
  })

  it('does not simulate slash commands when source-level bridge is missing', async () => {
    const result = await executeRemoteImControlCommand({
      command: 'build',
      session: {
        sessionId: 'session-codex',
        targetRepo: '/repo',
        command: 'codex',
        startedAtMs: 0
      },
      sourceKind: 'codex'
    })

    expect(result.ok).toBe(false)
    expect(result.text).toContain('源码级控制通道尚未接入')
  })

  it('rejects mode switching for Claude', async () => {
    const result = await executeRemoteImControlCommand({
      command: 'plan',
      session: {
        sessionId: 'session-claude',
        targetRepo: '/repo',
        command: 'claude',
        startedAtMs: 0
      },
      sourceKind: 'claude'
    })

    expect(result.ok).toBe(false)
    expect(result.text).toContain('Claude')
  })
})
