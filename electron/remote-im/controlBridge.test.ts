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
    expect(result.text).toContain('/goal')
    expect(result.text).toContain('/btw')
    expect(result.text).toContain('/diff')
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

  it('forwards Codex reasoning selection through the source-level model command', async () => {
    const executeCommand = vi.fn(async () => ({
      ok: true as const,
      text: '已切换推理档位：High'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'model',
      args: 'reasoning high',
      session: {
        sessionId: 'session-1',
        targetRepo: '/repo',
        command: '/bundled/codex',
        startedAtMs: 1000
      },
      sourceKind: 'codex',
      executeCommand,
      replyId: 'reply-btw-fixed'
    })

    expect(result.ok).toBe(true)
    expect(result.text).toContain('High')
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'model',
      reasoning: 'high'
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

  it('requests goal management through the source-level bridge', async () => {
    const executeCommand = vi.fn(async () => ({
      ok: true as const,
      text: 'Goal active\nObjective: 修复 IM 回传'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'goal',
      args: '修复 IM 回传',
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
    expect(result.text).toContain('Goal active')
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'goal',
      goal: '修复 IM 回传'
    })
  })

  it('submits /btw tasks through the source-level control bridge', async () => {
    const executeCommand = vi.fn(async () => ({
      ok: true as const,
      text: '已提交 /btw 子任务，完成后会通过 IM 回传。'
    }))
    const result = await executeRemoteImControlCommand({
      command: 'btw',
      args: '检查最近一次失败日志',
      session: {
        sessionId: 'session-1',
        targetRepo: '/repo',
        command: '/bundled/codex',
        startedAtMs: 1000
      },
      sourceKind: 'codex',
      executeCommand,
      replyId: 'reply-btw-fixed'
    })

    expect(result.ok).toBe(true)
    expect(result.text).toContain('/btw')
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'btw',
      task: '检查最近一次失败日志',
      replyId: 'reply-btw-fixed'
    })
  })

  it('collects /diff from the session repository without calling AICLI', async () => {
    const createDiffReport = vi.fn(async () => ({
      ok: true as const,
      text: '2 files, +3 / -1',
      attachmentPath: '/tmp/repo-diff.md'
    }))
    const executeCommand = vi.fn()
    const result = await executeRemoteImControlCommand({
      command: 'diff',
      args: '--stat src',
      session: {
        sessionId: 'session-1',
        targetRepo: '/repo',
        command: '/bundled/codex',
        startedAtMs: 1000
      },
      sourceKind: 'codex',
      createDiffReport,
      executeCommand
    })

    expect(result).toEqual({
      ok: true,
      text: '2 files, +3 / -1',
      attachmentPath: '/tmp/repo-diff.md'
    })
    expect(createDiffReport).toHaveBeenCalledWith({
      targetRepo: '/repo',
      args: '--stat src'
    })
    expect(executeCommand).not.toHaveBeenCalled()
  })

  it('forwards lifecycle commands through the source-level control bridge', async () => {
    const executeCommand = vi.fn(async ({ command }: { command: string }) => ({
      ok: true as const,
      text: `ok:${command}`
    }))
    for (const command of ['interrupt', 'compact', 'clear'] as const) {
      const result = await executeRemoteImControlCommand({
        command,
        session: {
          sessionId: 'session-1',
          targetRepo: '/repo',
          command: '/bundled/codex',
          startedAtMs: 1000
        },
        sourceKind: 'codex',
        executeCommand
      })

      expect(result).toEqual({ ok: true, text: `ok:${command}` })
    }

    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'interrupt'
    })
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'compact'
    })
    expect(executeCommand).toHaveBeenCalledWith({
      sessionId: 'session-1',
      sourceKind: 'codex',
      command: 'clear'
    })
  })

  it('requires a task body for /btw', async () => {
    const executeCommand = vi.fn()
    const result = await executeRemoteImControlCommand({
      command: 'btw',
      args: '   ',
      session: {
        sessionId: 'session-1',
        targetRepo: '/repo',
        command: '/bundled/codex',
        startedAtMs: 1000
      },
      sourceKind: 'codex',
      executeCommand
    })

    expect(result.ok).toBe(false)
    expect(result.text).toContain('/btw <任务>')
    expect(executeCommand).not.toHaveBeenCalled()
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
