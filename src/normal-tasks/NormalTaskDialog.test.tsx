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
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(markup).toContain('普通任务管理')
    expect(markup).toContain('当前 AICLI：已启动')
    expect(markup).toContain('+ 新建普通任务')
    expect(markup).toContain('Fix_login_flow')
    expect(markup).toContain('方案文档')
    expect(markup).toContain('查看方案文档')
    expect(markup).toContain('运行')
    expect(markup).not.toContain('使用')
    expect(markup).toContain('编辑')
    expect(markup).not.toContain('保存描述')
    expect(markup).not.toContain('normal-task-description-input')
    expect(markup).toContain('E:\\OpenSource\\app\\.multi-ai-code\\designs\\Fix_login_flow.md')
    expect(markup).not.toContain('导入外部方案')
  })

  it('does not block creating a normal task while AICLI is running', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[task()]}
        selectedName="Fix_login_flow"
        sessionRunning={true}
        onCreate={vi.fn()}
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )
    const createStart = markup.indexOf('+ 新建普通任务')
    const createMarkup = markup.slice(Math.max(0, createStart - 160), createStart + 80)

    expect(createMarkup).not.toContain('disabled')
    expect(markup).toContain('title="创建普通任务"')
    expect(markup).not.toContain('运行中无法新建普通任务')
  })

  it('does not require a running AICLI session to create a normal task', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[task()]}
        selectedName="Fix_login_flow"
        sessionRunning={false}
        onCreate={vi.fn()}
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )
    const createStart = markup.indexOf('drawer-btn primary')
    const createMarkup = markup.slice(Math.max(0, createStart - 160), createStart + 80)

    expect(createMarkup).not.toContain('disabled')
    expect(markup).toContain('AICLI')
    expect(markup).toContain('drawer-btn primary')
  })

  it('renders markdown task details and falls back to the task name for description', () => {
    const withDescription = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[
          task({
            description: '登录修复摘要',
            details: '## 修复登录流程\n\n- 确认重试逻辑'
          })
        ]}
        selectedName="Fix_login_flow"
        sessionRunning={false}
        onCreate={vi.fn()}
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )
    const withoutDescription = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[task({ name: 'Legacy_task' })]}
        selectedName="Legacy_task"
        sessionRunning={false}
        onCreate={vi.fn()}
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(withDescription).toContain('任务描述')
    expect(withDescription).toContain('登录修复摘要')
    expect(withDescription).toContain('任务详情')
    expect(withDescription).toContain('<h2>修复登录流程</h2>')
    expect(withDescription).toContain('<li>确认重试逻辑</li>')
    expect(withDescription).not.toContain('保存任务信息')
    expect(withoutDescription).toContain('Legacy_task')
  })

  it('allows selecting a different task while AICLI is running', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[task({ name: 'Other_running_task' })]}
        selectedName="Fix_login_flow"
        sessionRunning={true}
        onCreate={vi.fn()}
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )
    const cardMarkup = markup.slice(
      Math.max(0, markup.indexOf('Other_running_task') - 500),
      markup.indexOf('Other_running_task') + 500
    )

    expect(cardMarkup).not.toContain('disabled')
    expect(cardMarkup).toContain('选择普通任务')
  })

  it('renders an empty state without hiding the create action', () => {
    const markup = renderToStaticMarkup(
      <NormalTaskDialog
        tasks={[]}
        selectedName=""
        sessionRunning={false}
        onCreate={vi.fn()}
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
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
        onRun={vi.fn()}
        onPreview={vi.fn()}
        onSaveDescription={vi.fn()}
        onRefresh={vi.fn()}
        onClose={vi.fn()}
      />
    )

    expect(markup).toContain('外部方案')
    expect(markup).toContain('不再支持导入新的外部方案')
  })
})
