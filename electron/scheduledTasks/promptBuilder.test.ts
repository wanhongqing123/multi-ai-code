import { describe, expect, it } from 'vitest'
import { buildScheduledTaskPrompt } from './promptBuilder.js'
import type { ScheduledTask } from './types.js'

function task(overrides: Partial<ScheduledTask> = {}): ScheduledTask {
  return {
    id: 1,
    projectId: 'project-1',
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

  it('adds completion marker instructions without echoing the full marker', () => {
    const prompt = buildScheduledTaskPrompt(task(), {
      targetRepo: 'E:\\OpenSource\\multi-ai-code',
      completionToken: 'abc123'
    })

    expect(prompt).toContain('Completion marker protocol')
    expect(prompt).toContain('Task token: abc123')
    expect(prompt).toContain('MULTI_AI_CODE_SCHEDULED_TASK_DONE:')
    expect(prompt).not.toContain('MULTI_AI_CODE_SCHEDULED_TASK_DONE:abc123:succeeded')
    expect(prompt).not.toContain('MULTI_AI_CODE_SCHEDULED_TASK_DONE:abc123:failed')
  })
})
