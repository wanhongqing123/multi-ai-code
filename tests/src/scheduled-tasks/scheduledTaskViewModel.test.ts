import { describe, expect, it } from 'vitest'
import {
  buildScheduledTaskPreviewPrompt,
  createDefaultScheduledTaskDraft,
  formatScheduledTaskStatus,
  formatScheduleLabel
} from '../../../src/scheduled-tasks/scheduledTaskViewModel'

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
    // 手动任务按「怎么触发」描述，不能显示成某个时间点——它压根没有下次运行时间。
    expect(formatScheduleLabel('manual', '21:30', [])).toBe('手动执行')
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

    expect(preview).toContain('\u4efb\u52a1\u63cf\u8ff0\uff1a\n')
    expect(preview).not.toContain('\u4efb\u52a1\u76ee\u6807\uff1a')
    // 这句必须和 electron/scheduledTasks/promptBuilder.ts 里发出去的完全一致，
    // 否则预览显示的和实际发送的对不上。手动/定时统一成「任务」后两侧同步修改。
    expect(preview).toContain('你现在要执行一个由 Multi-AI Code 触发的任务。')
    expect(preview).toContain('任务名称：每日代码巡检')
    expect(preview).toContain('工作目录：E:\\OpenSource\\multi-ai-code')
    expect(preview).toContain('不要直接修改代码。')
    expect(preview).toContain('不要提交 git。')
  })
})
