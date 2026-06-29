import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import NormalTaskDialog, { type NormalTaskEntry } from './NormalTaskDialog'

function task(overrides: Partial<NormalTaskEntry> = {}): NormalTaskEntry {
  return {
    name: 'Fix_login_flow',
    abs: 'E:\\OpenSource\\app\\.multi-ai-code\\designs\\Fix_login_flow.md',
    source: 'internal',
    ...overrides
  }
}

describe('NormalTaskDialog', () => {
  it('renders the manager shell and selected normal task details', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[task()]}
        selectedName="Fix_login_flow"
        sessionRunning={true}
        onCreate={vi.fn()}
        onSelect={vi.fn()}
        onPreview={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(markup).toContain('普通任务管理')
    expect(markup).toContain('当前 AICLI：已启动')
    expect(markup).toContain('+ 新建普通任务')
    expect(markup).toContain('Fix_login_flow')
    expect(markup).toContain('任务文档')
    expect(markup).toContain('查看任务文档')
    expect(markup).toContain('E:\\OpenSource\\app\\.multi-ai-code\\designs\\Fix_login_flow.md')
    expect(markup).not.toContain('导入外部方案')
  })

  it('renders an empty state without hiding the create action', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[]}
        selectedName=""
        sessionRunning={false}
        onCreate={vi.fn()}
        onSelect={vi.fn()}
        onPreview={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(markup).toContain('还没有普通任务')
    expect(markup).toContain('+ 新建普通任务')
    expect(markup).toContain('当前 AICLI：未启动')
  })

  it('marks external mappings as unavailable in this manager', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[task({ name: 'legacy_external', source: 'external' })]}
        selectedName="legacy_external"
        sessionRunning={false}
        onCreate={vi.fn()}
        onSelect={vi.fn()}
        onPreview={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(markup).toContain('外部方案')
    expect(markup).toContain('不再支持导入新的外部方案')
  })
})
