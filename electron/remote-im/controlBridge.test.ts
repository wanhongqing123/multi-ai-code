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
    expect(result.text).toContain('/model')
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

  it('requests model listing and switching through the source-level bridge', async () => {
    const executeCommand = vi.fn(async () => ({
      ok: true as const,
      text: '当前模型：gpt-5.6-sol\n可用模型：\n1. gpt-5.6-sol'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'model',
      args: '2',
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
    expect(result.text).toContain('当前模型')
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'model',
      model: '2'
    })
  })

  it('maps /models to the source-level model list command without a selection', async () => {
    const executeCommand = vi.fn(async () => ({
      ok: true as const,
      text: '当前模型：gpt-5.6-sol\n可用模型：\n1. GPT-5.6 Sol (gpt-5.6-sol)'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'models',
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
    expect(result.text).toContain('可用模型')
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'model'
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

  it('reports the AICLI rejection instead of a fake success', async () => {
    const switchMode = vi.fn(async () => ({
      ok: false as const,
      error: 'Collaboration modes are disabled.'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'plan',
      session: {
        sessionId: 'session-codex',
        targetRepo: '/repo',
        command: 'codex',
        startedAtMs: 0
      },
      sourceKind: 'codex',
      switchMode
    })

    expect(result.ok).toBe(false)
    expect(result.text).toContain('切换到计划模式失败')
    expect(result.text).toContain('Collaboration modes are disabled.')
  })

  it('distinguishes a timeout from an explicit AICLI rejection', async () => {
    const switchMode = vi.fn(async () => ({
      ok: false as const,
      error: 'AICLI control command timed out'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'build',
      session: {
        sessionId: 'session-codex',
        targetRepo: '/repo',
        command: 'codex',
        startedAtMs: 0
      },
      sourceKind: 'codex',
      switchMode
    })

    expect(result.ok).toBe(false)
    expect(result.text).toContain('未收到 AICLI 确认')
    expect(result.text).toContain('/status')
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
