import { describe, expect, it } from 'vitest'
import { buildScheduledTaskPrompt } from './promptBuilder.js'
import type { ScheduledTask } from './types.js'

function task(overrides: Partial<ScheduledTask> = {}): ScheduledTask {
  return {
    id: 1,
    projectId: 'project-1',
    name: '每日代码巡检',
    description: '每天检查项目风险',
    goal: '检查当前项目最近的代码变更，找出潜在风险。',
    instructions: ['分析代码风险', '给出修改建议', '不要直接修改代码'],
    enabled: true,
    scheduleType: 'daily',
    scheduleTime: '21:30',
    scheduleDays: [],
    nextRunAt: Date.UTC(2026, 5, 19, 13, 30),
    timeoutMinutes: 30,
    allowCodeChanges: false,
    allowGitCommit: false,
    requireTestConfirmation: false,
    createdAt: 1,
    updatedAt: 1,
    lastRun: null,
    ...overrides
  }
}

describe('buildScheduledTaskPrompt', () => {
  it('marks the prompt as a Multi-AI Code scheduled task and includes the workdir', () => {
    const prompt = buildScheduledTaskPrompt(task(), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code'
    })

    expect(prompt).toContain('你现在要执行一个由 Multi-AI Code 触发的定时任务。')
    expect(prompt).toContain('任务名称：每日代码巡检')
    expect(prompt).toContain('工作目录：E:\\OpenSource\\multi-ai-code')
    expect(prompt).toContain('任务目标：')
    expect(prompt).toContain('检查当前项目最近的代码变更，找出潜在风险。')
  })

  it('adds conservative safety rules by default', () => {
    const prompt = buildScheduledTaskPrompt(task(), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code'
    })

    expect(prompt).toContain('不要直接修改代码。')
    expect(prompt).toContain('不要提交 git。')
    expect(prompt).toContain('用中文总结。')
    expect(prompt).not.toContain('允许直接修改代码')
  })

  it('states explicit permissions when code changes and test confirmation are enabled', () => {
    const prompt = buildScheduledTaskPrompt(
      task({
        allowCodeChanges: true,
        requireTestConfirmation: true
      }),
      {
        targetRepo: 'E:\\OpenSource\\multi-ai-code'
      }
    )

    expect(prompt).toContain('允许直接修改代码，但必须保持改动聚焦在本任务。')
    expect(prompt).toContain('如果需要运行测试，先说明要运行什么命令。')
  })
})
