import { describe, expect, it } from 'vitest'
import {
  buildScheduledTaskPreviewPrompt,
  createDefaultScheduledTaskDraft,
  formatScheduledTaskStatus,
  formatScheduleLabel
} from './scheduledTaskViewModel'

describe('scheduled task view model', () => {
  it('creates a conservative default draft', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')

    expect(draft.projectId).toBe('project-1')
    expect(draft.enabled).toBe(true)
    expect(draft.allowCodeChanges).toBe(false)
    expect(draft.allowGitCommit).toBe(false)
    expect(draft.requireTestConfirmation).toBe(false)
    expect(draft.instructions).toContain('不要直接修改代码')
  })

  it('formats schedule labels', () => {
    expect(formatScheduleLabel('once', '21:30', [])).toBe('一次性 21:30')
    expect(formatScheduleLabel('daily', '21:30', [])).toBe('每天 21:30')
    expect(formatScheduleLabel('weekly', '09:00', [1, 5])).toBe('每周一、周五 09:00')
    expect(formatScheduleLabel('interval', '15', [])).toBe('每隔 15 分钟')
  })

  it('formats status labels', () => {
    expect(formatScheduledTaskStatus(false, null).label).toBe('禁用')
    expect(formatScheduledTaskStatus(true, null).label).toBe('等待中')
    expect(formatScheduledTaskStatus(true, 'queued').label).toBe('排队')
    expect(formatScheduledTaskStatus(true, 'succeeded').label).toBe('成功')
  })

  it('builds the same style of prompt preview the backend sends', () => {
    const draft = createDefaultScheduledTaskDraft('project-1')
    draft.name = '每日代码巡检'
    draft.goal = '检查当前项目最近的代码变更。'

    const preview = buildScheduledTaskPreviewPrompt(draft, 'E:\\OpenSource\\multi-ai-code')

    expect(preview).toContain('你现在要执行一个由 Multi-AI Code 触发的定时任务。')
    expect(preview).toContain('任务名称：每日代码巡检')
    expect(preview).toContain('工作目录：E:\\OpenSource\\multi-ai-code')
    expect(preview).toContain('不要直接修改代码。')
    expect(preview).toContain('不要提交 git。')
  })
})
