import { renderToStaticMarkup } from 'react-dom/server'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import ScheduledTaskEditorDialog, {
  appendMissingScheduledTaskImageMarkdown,
  referencedScheduledTaskImages
} from '../../../src/scheduled-tasks/ScheduledTaskEditorDialog'
import { createDefaultScheduledTaskDraft } from '../../../src/scheduled-tasks/scheduledTaskViewModel'

function normalizeNewlines(value: string): string {
  return value.replace(/\r\n/g, '\n')
}

describe('ScheduledTaskEditorDialog', () => {
  it('shows task intent and execution constraints without prompt preview', () => {
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

    expect(markup).toContain('新建任务')
    // 定时/手动统一成「任务」，编辑器不该再自称「定时任务」。
    expect(markup).not.toContain('新建定时任务')
    // 触发方式必须能选到手动——这是普通任务的落点。
    expect(markup).toContain('触发方式')
    expect(markup).toContain('<option value="manual">手动执行</option>')
    expect(markup).toContain('任务描述')
    expect(markup).toContain('aria-label="添加图片"')
    expect(markup).toContain('accept="image/png,image/jpeg,image/gif,image/webp"')
    expect(markup).toContain('怎么干与限制')
    expect(markup).toContain('默认不允许自动改代码')
    expect(markup).toContain('允许直接修改代码')
    expect(markup).toContain('允许提交 git')
    expect(markup).not.toContain('scheduled-task-preview')
    expect(markup).not.toContain('scheduled-task-preview-markdown')
    expect(markup).not.toContain('E:\\OpenSource\\multi-ai-code')
    expect(markup).not.toContain('Prompt ')
    expect(markup).toContain('运行测试前先说明')
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
      readFileSync(fileURLToPath(new URL('../../../src/styles.css', import.meta.url)), 'utf8')
    )

    expect(markup).toContain('class="scheduled-task-goal-input"')
    expect(styles).toContain('width: min(1380px, calc(100vw - 40px));')
    expect(styles).toContain('height: min(900px, calc(100vh - 32px));')
    expect(styles).toContain('.scheduled-task-goal-input {')
    expect(styles).toContain('min-height: 168px;')
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

  it('allows a task description that contains only an image', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')
    draft.name = '检查截图'
    draft.imageAttachments = [
      {
        id: 'image-1',
        localPath: '/tmp/image-1.png',
        fileName: 'image-1.png',
        mimeType: 'image/png',
        sizeBytes: 1024
      }
    ]

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

    expect(markup).toContain('image-1.png')
    expect(markup).toContain('![image-1.png](&lt;/tmp/image-1.png&gt;)')
    expect(markup).not.toContain('scheduled-task-image-chip')
    expect(markup).toContain('>保存任务</button>')
    expect(markup).not.toContain('disabled="" class="drawer-btn primary"')
  })

  it('keeps image attachments aligned with Markdown references in the description', () => {
    const attachment = {
      id: 'image-1',
      localPath: '/tmp/image-1.png',
      fileName: 'screen]shot.png',
      mimeType: 'image/png',
      sizeBytes: 1024
    }
    const goal = appendMissingScheduledTaskImageMarkdown('检查截图', [attachment])

    expect(goal).toBe('检查截图\n\n![screen\\]shot.png](</tmp/image-1.png>)')
    expect(referencedScheduledTaskImages(goal, [attachment])).toEqual([attachment])
    expect(referencedScheduledTaskImages('路径：/tmp/image-1.png', [attachment])).toEqual([])
    expect(referencedScheduledTaskImages('检查截图', [attachment])).toEqual([])
  })

  it('does not render the AICLI prompt preview in the editor', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')
    draft.name = 'Markdown preview task'
    draft.goal = '# Inspect changes\n\n- Review **risk**'

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
    const source = readFileSync(
      fileURLToPath(new URL('../../../src/scheduled-tasks/ScheduledTaskEditorDialog.tsx', import.meta.url)),
      'utf8'
    )

    expect(markup).not.toContain('scheduled-task-preview')
    expect(markup).not.toContain('scheduled-task-preview-markdown')
    expect(markup).not.toContain('<pre>')
    expect(source).not.toContain('<ReactMarkdown')
    expect(source).not.toContain('buildScheduledTaskPreviewPrompt')
  })
})
