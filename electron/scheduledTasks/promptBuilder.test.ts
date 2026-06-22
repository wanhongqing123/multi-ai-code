import { describe, expect, it } from 'vitest'
import { buildScheduledTaskPrompt } from './promptBuilder.js'
import type { ScheduledTask } from './types.js'

function task(overrides: Partial<ScheduledTask> = {}): ScheduledTask {
  return {
    id: 1,
    projectId: 'project-1',
    targetRepo: null,
    name: 'Daily code review',
    description: 'Check project risks',
    goal: 'Check recent code changes in the current project.',
    instructions: ['Analyze code risk', 'Give suggestions', 'Do not modify code'],
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
  it('marks the prompt as a Multi-AI Code scheduled task and includes task context', () => {
    const prompt = buildScheduledTaskPrompt(task(), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code'
    })

    expect(prompt).toContain('Multi-AI Code')
    expect(prompt).toContain('Daily code review')
    expect(prompt).toContain('E:\\OpenSource\\multi-ai-code')
    expect(prompt).toContain('Check recent code changes in the current project.')
  })

  it('includes the task timeout so AICLI can bound the work', () => {
    const prompt = buildScheduledTaskPrompt(task({ timeoutMinutes: 45 }), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code'
    })

    expect(prompt).toContain('任务超时时间：45 分钟')
    expect(prompt).toContain('如果无法在 45 分钟内完成')
  })

  it('asks AICLI to report execution time and duration in the execution requirements', () => {
    const prompt = buildScheduledTaskPrompt(
      task({
        scheduleType: 'daily',
        scheduleTime: '21:30',
        timeoutMinutes: 45
      }),
      {
        targetRepo: 'E:\\OpenSource\\multi-ai-code'
      }
    )

    expect(prompt).toContain('执行要求：\n1. Analyze code risk')
    expect(prompt).toContain(
      '4. 任务开始执行时先记录当前时间；任务完成时在总结中写明本次任务的执行时间范围和实际执行时长；任务时长上限：45 分钟。'
    )
  })

  it('adds conservative safety rules by default', () => {
    const prompt = buildScheduledTaskPrompt(task(), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code'
    })

    expect(prompt).toContain('Analyze code risk')
    expect(prompt).toContain('Give suggestions')
    expect(prompt).not.toContain('Completion marker protocol')
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

    expect(prompt).toContain('Daily code review')
    expect(prompt).toContain('E:\\OpenSource\\multi-ai-code')
  })

  it('asks AICLI to run the scheduled task after any previous task finishes', () => {
    const prompt = buildScheduledTaskPrompt(task(), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code'
    })

    expect(prompt).toContain('如果你正在处理上一项任务')
    expect(prompt).toContain('请在上一项任务完成后继续执行下面的定时任务')
    expect(prompt).not.toContain('Completion marker protocol')
    expect(prompt).not.toContain('MULTI_AI_CODE_SCHEDULED_TASK_DONE:')
  })
})
