import { renderToStaticMarkup } from 'react-dom/server'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import ScheduledTaskEditorDialog from './ScheduledTaskEditorDialog'
import { createDefaultScheduledTaskDraft } from './scheduledTaskViewModel'

function normalizeNewlines(value: string): string {
  return value.replace(/\r\n/g, '\n')
}

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
    expect(markup).toContain('任务描述')
    expect(markup).toContain('怎么干与限制')
    expect(markup).toContain('默认不允许自动改代码')
    expect(markup).toContain('允许直接修改代码')
    expect(markup).toContain('允许提交 git')
    expect(markup).toContain('运行测试前先说明')
    expect(markup).toContain('最终发送给 AICLI')
    expect(markup).toContain('任务名称：每日代码巡检')
    expect(markup).toContain('任务超时时间：30 分钟')
    expect(markup).toContain(
      '4. 任务开始执行时先记录当前时间；任务完成时在总结中写明本次任务的执行时间范围和实际执行时长；任务时长上限：30 分钟。'
    )
    expect(markup).toContain('不要提交 git')
    expect(markup).not.toContain('执行方式')
    expect(markup).not.toContain('使用当前 AICLI')
    expect(markup).not.toContain('忙碌时排队等待')
    expect(markup).not.toContain('AICLI 忙时排队等待')
    expect(markup).not.toContain('任务说明')
    expect(markup).not.toContain('让 AICLI 做什么')
    expect(markup).not.toContain('AICLI 要做什么')
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
    const styles = normalizeNewlines(
      readFileSync(fileURLToPath(new URL('../styles.css', import.meta.url)), 'utf8')
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
    const styles = normalizeNewlines(
      readFileSync(fileURLToPath(new URL('../styles.css', import.meta.url)), 'utf8')
    )

    expect(source).toContain('goalTextareaRef')
    expect(source).toContain('adjustGoalTextareaHeight')
    expect(source).toContain('ref={goalTextareaRef}')
    expect(source).toContain('onInput={adjustGoalTextareaHeight}')
    expect(styles).toContain('.scheduled-task-goal-input {\n  min-height: 168px;\n  overflow: hidden;')
  })

  it('supports interval schedules with a minutes input', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')
    draft.scheduleType = 'interval'
    draft.scheduleTime = '15'

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

    expect(markup).toContain('value="interval" selected=""')
    expect(markup).toContain('每隔')
    expect(markup).toContain('间隔分钟')
    expect(markup).toContain('type="number"')
    expect(markup).toContain('value="15"')
    expect(markup).not.toContain('type="time"')
  })
})
