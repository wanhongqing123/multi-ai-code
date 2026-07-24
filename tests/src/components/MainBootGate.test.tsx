import type { ComponentProps } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import MainBootGate from '../../../src/components/MainBootGate.js'

function render(
  overrides: Partial<ComponentProps<typeof MainBootGate>> = {}
): string {
  return renderToStaticMarkup(
    <MainBootGate
      phase={{ kind: 'idle' }}
      command="codex"
      workMode="normal-task"
      onChoose={vi.fn()}
      onDismissFailure={vi.fn()}
      {...overrides}
    />
  )
}

describe('MainBootGate', () => {
  it('idle: renders two enabled choice buttons', () => {
    const html = render()
    expect(html).toContain('另起炉灶')
    expect(html).toContain('接着唠')
    // No failure block when idle.
    expect(html).not.toContain('boot-gate-failure')
    // 内置 CLI 时代不再展示的文案：标题、启动路径、续聊说明。
    expect(html).not.toContain('选择本次主会话启动方式')
    expect(html).not.toContain('AICLI 启动路径')
    expect(html).not.toContain('续聊将由')
  })

  it('idle in scheduled-task mode: shows the mode in the subtitle', () => {
    const html = render({ workMode: 'scheduled-task' })
    // 模式只在副标题行体现（无「当前模式/当前 CLI」标签前缀），按钮文案两种模式统一。
    expect(html).not.toContain('当前模式')
    expect(html).not.toContain('当前 CLI')
    expect(html).toContain('定时任务')
    expect(html).toContain('另起炉灶')
    expect(html).toContain('接着唠')
  })

  it('spawning-new: marks the new button as in-progress and disables both', () => {
    const html = render({ phase: { kind: 'spawning', mode: 'new' } })
    expect(html).toContain('正在启动')
    // Two disabled attributes (both buttons disabled while spawning).
    expect((html.match(/disabled=""/g) ?? []).length).toBeGreaterThanOrEqual(2)
  })

  it('spawning-resume: marks the resume button as in-progress', () => {
    const html = render({ phase: { kind: 'spawning', mode: 'resume' } })
    expect(html).toContain('正在续聊')
  })

  it('failed: shows the reason and a tail-collapse section', () => {
    const html = render({
      phase: {
        kind: 'failed',
        reason: '未找到可恢复会话',
        tail: 'error: no conversation found'
      }
    })
    expect(html).toContain('boot-gate-failure')
    expect(html).toContain('未找到可恢复会话')
    expect(html).toContain('查看 CLI 输出')
    expect(html).toContain('error: no conversation found')
  })

  it('failed without tail: omits the collapse section', () => {
    const html = render({
      phase: { kind: 'failed', reason: '未找到可恢复会话' }
    })
    expect(html).toContain('未找到可恢复会话')
    expect(html).not.toContain('查看 CLI 输出')
  })

  it('unknown CLI: disables the resume button (only claude/codex/opencode supported)', () => {
    const html = render({ command: 'gemini' })
    // The resume button should be present but disabled.
    expect(html).toContain('接着唠')
    expect(html).toContain('当前 CLI 不支持续聊')
  })

  it('opencode: keeps the resume button enabled (CLI supports --continue)', () => {
    const html = render({ command: 'opencode' })
    expect(html).toContain('接着唠')
    expect(html).not.toContain('当前 CLI 不支持续聊')
  })

  it('disabled prop: disables both buttons even at idle', () => {
    const html = render({ disabled: true })
    expect((html.match(/disabled=""/g) ?? []).length).toBeGreaterThanOrEqual(2)
  })

  it('displays the current CLI label', () => {
    expect(render({ command: 'claude' })).toContain('Claude Code')
    expect(render({ command: 'codex' })).toContain('Codex')
    expect(render({ command: '' })).toContain('(未配置)')
  })

  it('shows an explicit risk confirmation when Claude is selected', () => {
    const html = render({ command: 'claude' })

    expect(html).toContain('Claude 目前风险很高，请谨慎使用')
    expect(html).toContain('建议更换 Codex')
    expect(html).toContain('即使有风险也要继续使用 Claude')
    expect(html).toContain('boot-gate-claude-risk')
  })

  it('does not show the Claude risk confirmation for Codex', () => {
    const html = render({ command: 'codex' })

    expect(html).not.toContain('Claude 目前风险很高')
    expect(html).not.toContain('boot-gate-claude-risk')
  })
})
