import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import ScheduledTaskEditorDialog from './ScheduledTaskEditorDialog'
import { createDefaultScheduledTaskDraft } from './scheduledTaskViewModel'

describe('ScheduledTaskEditorDialog', () => {
  it('shows task intent, execution constraints, and prompt preview', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')
    draft.name = '每日代码巡检'
    draft.goal = '检查当前项目最近的代码变更。'

    const markup = renderToStaticMarkup(
      <ScheduledTaskEditorDialog
        mode="create"
        draft={draft}
        targetRepo="E:\\OpenSource\\multi-ai-code"
        onChange={() => {}}
        onCancel={() => {}}
        onSave={() => {}}
      />
    )

    expect(markup).toContain('新建定时任务')
    expect(markup).toContain('AICLI 要做什么')
    expect(markup).toContain('怎么干与限制')
    expect(markup).toContain('默认不允许自动改代码')
    expect(markup).toContain('允许直接修改代码')
    expect(markup).toContain('允许提交 git')
    expect(markup).toContain('运行测试前先说明')
    expect(markup).toContain('最终发送给 AICLI')
    expect(markup).toContain('任务名称：每日代码巡检')
    expect(markup).toContain('不要提交 git')
  })
})
