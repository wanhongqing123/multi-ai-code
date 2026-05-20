import type { ComponentProps } from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import MainBootGate from './MainBootGate.js'

function render(
  overrides: Partial<ComponentProps<typeof MainBootGate>> = {}
): string {
  return renderToStaticMarkup(
    <MainBootGate
      phase={{ kind: 'idle' }}
      command="claude"
      planName="项目后续规划"
      onChoose={vi.fn()}
      onDismissFailure={vi.fn()}
      {...overrides}
    />
  )
}

describe('MainBootGate', () => {
  it('idle: renders two enabled choice buttons', () => {
    const html = render()
    expect(html).toContain('新会话')
    expect(html).toContain('继续上次')
    // No failure block when idle.
    expect(html).not.toContain('boot-gate-failure')
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

  it('unknown CLI: disables the resume button (only claude/codex supported)', () => {
    const html = render({ command: 'gemini' })
    // The "继续上次" button should be present but disabled.
    expect(html).toContain('继续上次')
    expect(html).toContain('当前 CLI 不支持续聊')
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
})
