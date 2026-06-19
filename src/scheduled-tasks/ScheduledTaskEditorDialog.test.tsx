import { renderToStaticMarkup } from 'react-dom/server'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
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

  it('gives the create editor and AICLI goal input more working room', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')
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
    const styles = readFileSync(
      fileURLToPath(new URL('../styles.css', import.meta.url)),
      'utf8'
    )

    expect(markup).toContain('class="scheduled-task-goal-input"')
    expect(styles).toContain('width: min(1380px, calc(100vw - 40px));')
    expect(styles).toContain('height: min(900px, calc(100vh - 32px));')
    expect(styles).toContain('.scheduled-task-goal-input {\n  min-height: 168px;')
  })

  it('auto-resizes the AICLI goal textarea as content changes', () => {
    const source = readFileSync(
      fileURLToPath(new URL('./ScheduledTaskEditorDialog.tsx', import.meta.url)),
      'utf8'
    )
    const styles = readFileSync(
      fileURLToPath(new URL('../styles.css', import.meta.url)),
      'utf8'
    )

    expect(source).toContain('goalTextareaRef')
    expect(source).toContain('adjustGoalTextareaHeight')
    expect(source).toContain('ref={goalTextareaRef}')
    expect(source).toContain('onInput={adjustGoalTextareaHeight}')
    expect(styles).toContain('.scheduled-task-goal-input {\n  min-height: 168px;\n  overflow: hidden;')
  })
})
