import { describe, expect, it } from 'vitest'
import { buildNormalTaskRunPrompt } from '../../../src/normal-tasks/normalTaskPrompt'
import type { NormalTaskEntry } from '../../../src/normal-tasks/NormalTaskDialog'

function task(overrides: Partial<NormalTaskEntry> = {}): NormalTaskEntry {
  return {
    name: 'Fix_login_flow',
    abs: 'E:\\OpenSource\\app\\.multi-ai-code\\designs\\Fix_login_flow.md',
    source: 'internal',
    description: '修复登录流程',
    details: '## 背景\n\n- 登录重试失败',
    ...overrides
  }
}

describe('buildNormalTaskRunPrompt', () => {
  it('includes the normal task identity, repository, document, description, and details', () => {
    const prompt = buildNormalTaskRunPrompt(task(), 'E:\\OpenSource\\app')

    expect(prompt).toContain('普通任务')
    expect(prompt).toContain('Fix_login_flow')
    expect(prompt).toContain('E:\\OpenSource\\app')
    expect(prompt).toContain('E:\\OpenSource\\app\\.multi-ai-code\\designs\\Fix_login_flow.md')
    expect(prompt).toContain('修复登录流程')
    expect(prompt).toContain('## 背景')
    expect(prompt).toContain('登录重试失败')
  })

  it('falls back to the task name and an empty-details placeholder for legacy tasks', () => {
    const prompt = buildNormalTaskRunPrompt(
      task({ description: '', details: '' }),
      'E:\\OpenSource\\app'
    )

    expect(prompt).toContain('任务描述：')
    expect(prompt).toContain('Fix_login_flow')
    expect(prompt).toContain('暂无任务详情')
  })
})
