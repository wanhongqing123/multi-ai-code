import { describe, expect, it } from 'vitest'
import { planSystemPromptInjection } from './systemPromptInjection.js'

describe('planSystemPromptInjection', () => {
  it('places claude injection under .injections instead of repo-root CLAUDE.md', () => {
    const plan = planSystemPromptInjection({
      command: 'claude',
      cwd: 'E:/repo',
      systemPrompt: 'system prompt',
      initialUserMessage: 'hello'
    })

    expect(plan.writePath.replace(/\\/g, '/')).toBe('E:/repo/.injections/claude-system.md')
    expect(plan.bootstrapMessage.replace(/\\/g, '/')).toContain('E:/repo/.injections/claude-system.md')
    expect(plan.writePath.endsWith('CLAUDE.md')).toBe(false)
  })

  it('never targets AGENT.md or AGENTS.md', () => {
    const plan = planSystemPromptInjection({
      command: 'claude',
      cwd: 'E:/repo',
      systemPrompt: 'system prompt',
      initialUserMessage: 'hello'
    })

    expect(plan.writePath.endsWith('AGENT.md')).toBe(false)
    expect(plan.writePath.endsWith('AGENTS.md')).toBe(false)
  })

  it('uses a dedicated codex injection file under .injections', () => {
    const plan = planSystemPromptInjection({
      command: 'codex',
      cwd: 'E:/repo',
      systemPrompt: 'system prompt',
      initialUserMessage: 'hello'
    })

    expect(plan.writePath.replace(/\\/g, '/')).toBe('E:/repo/.injections/codex-system.md')
    expect(plan.bootstrapMessage.replace(/\\/g, '/')).toContain('E:/repo/.injections/codex-system.md')
  })

  it('uses a dedicated opencode injection file under .injections', () => {
    const plan = planSystemPromptInjection({
      command: 'opencode',
      cwd: 'E:/repo',
      systemPrompt: 'system prompt',
      initialUserMessage: 'hello'
    })

    expect(plan.writePath.replace(/\\/g, '/')).toBe('E:/repo/.injections/opencode-system.md')
    expect(plan.bootstrapMessage.replace(/\\/g, '/')).toContain('E:/repo/.injections/opencode-system.md')
  })
})
