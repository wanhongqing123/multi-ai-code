import { beforeEach, describe, expect, it, vi } from 'vitest'
import type { ScheduledTask } from '../../../electron/scheduledTasks/types.js'

const mockStore = vi.hoisted(() => ({
  dueTasks: [] as ScheduledTask[],
  nextRunId: 1,
  runs: new Map<number, { id: number; taskId: number; status: string }>()
}))

vi.mock('../../../electron/scheduledTasks/promptBuilder.js', () => ({
  buildScheduledTaskPrompt: (task: ScheduledTask) => `prompt:${task.name}`
}))

vi.mock('../../../electron/scheduledTasks/taskStore.js', () => ({
  advanceScheduledTaskAfterQueue: vi.fn(),
  createScheduledTaskRun: vi.fn((input: { taskId: number; status: string }) => {
    const run = {
      id: mockStore.nextRunId,
      taskId: input.taskId,
      status: input.status
    }
    mockStore.nextRunId += 1
    mockStore.runs.set(run.id, run)
    return run
  }),
  listAllDueScheduledTasks: vi.fn(() => mockStore.dueTasks),
  listDueScheduledTasks: vi.fn((projectId: string) =>
    mockStore.dueTasks.filter((task) => task.projectId === projectId)
  ),
  updateScheduledTaskRun: vi.fn((id: number, patch: { status?: string }) => {
    const run = mockStore.runs.get(id)
    if (run && patch.status) run.status = patch.status
  })
}))

import { advanceScheduledTaskAfterQueue } from '../../../electron/scheduledTasks/taskStore.js'
import {
  cancelScheduledTaskQueueRun,
  enqueueScheduledTaskNow,
  getScheduledTaskQueueState,
  handleScheduledTaskSessionData,
  handleScheduledTaskSessionExit,
  resetScheduledTaskSchedulerForTests,
  runScheduledTaskScanOnce,
  setScheduledTaskSendHandler
} from '../../../electron/scheduledTasks/scheduler.js'

function task(id: number, projectId: string, name: string): ScheduledTask {
  return {
    id,
    projectId,
    targetRepo: null,
    name,
    description: '',
    goal: 'execute task',
    instructions: [],
    enabled: true,
    scheduleType: 'once',
    scheduleTime: '09:00',
    scheduleDays: [],
    nextRunAt: 1,
    timeoutMinutes: 30,
    allowCodeChanges: false,
    allowGitCommit: false,
    requireTestConfirmation: false,
    createdAt: 1,
    updatedAt: 1,
    lastRun: null
  }
}

describe('scheduled task queue draining', () => {
  beforeEach(() => {
    resetScheduledTaskSchedulerForTests()
    mockStore.dueTasks = []
    mockStore.nextRunId = 1
    mockStore.runs.clear()
    vi.mocked(advanceScheduledTaskAfterQueue).mockClear()
  })

  it('leaves automatic due tasks pending until a matching AICLI session is available', async () => {
    mockStore.dueTasks = [task(1, 'project-1', 'delayed task')]
    let sessionAvailable = false
    const sent: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: () =>
        sessionAvailable
          ? {
              sessionId: 'session-1',
              targetRepo: 'E:\\OpenSource\\project-1'
            }
          : null,
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    await runScheduledTaskScanOnce({ now: 2 })
    expect(getScheduledTaskQueueState().waiting).toEqual([])
    expect(mockStore.runs.size).toBe(0)
    expect(advanceScheduledTaskAfterQueue).not.toHaveBeenCalled()

    sessionAvailable = true
    await runScheduledTaskScanOnce({ now: 3 })

    expect(sent[0]).toContain('prompt:delayed task')
    expect(getScheduledTaskQueueState().waiting).toEqual([])
    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
  })

  it('does not let a task without a session block runnable tasks for another project', async () => {
    mockStore.dueTasks = [
      task(1, 'project-1', 'blocked project task'),
      task(2, 'project-2', 'runnable project task')
    ]
    const sent: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: (projectId) =>
        projectId === 'project-2'
          ? {
              sessionId: 'session-2',
              targetRepo: 'E:\\OpenSource\\project-2'
            }
          : null,
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    await runScheduledTaskScanOnce({ now: 2 })

    expect(sent[0]).toContain('prompt:runnable project task')
    const state = getScheduledTaskQueueState()
    expect(state.running).toBeNull()
    expect(state.waiting).toEqual([])
    expect(mockStore.runs.size).toBe(1)
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
  })

  it('does not resolve automatic tasks by target repo when the project id changed', async () => {
    mockStore.dueTasks = [
      {
        ...task(1, 'old-project-id', 'repo matched task'),
        targetRepo: 'E:\\OpenSource\\stable-repo'
      }
    ]
    const sent: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: (_projectId: string, targetRepo?: string | null) =>
        targetRepo === 'E:\\OpenSource\\stable-repo'
          ? {
              sessionId: 'session-1',
              targetRepo: 'E:\\OpenSource\\stable-repo'
            }
          : null,
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    await runScheduledTaskScanOnce({ now: 2 })

    expect(sent).toEqual([])
    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.size).toBe(0)
  })

  it('marks a delivered task succeeded as soon as the prompt is sent', async () => {
    mockStore.dueTasks = [task(1, 'project-1', 'long task')]
    const sent: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: () => ({
        sessionId: 'session-1',
        targetRepo: 'E:\\OpenSource\\project-1'
      }),
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    await runScheduledTaskScanOnce({ now: 2 })

    expect(sent[0]).toContain('prompt:long task')
    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
  })

  it('does not require a completion marker after delivery', async () => {
    mockStore.dueTasks = [task(1, 'project-1', 'marker task')]
    const sent: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: () => ({
        sessionId: 'session-1',
        targetRepo: 'E:\\OpenSource\\project-1'
      }),
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    await runScheduledTaskScanOnce({ now: 2 })

    handleScheduledTaskSessionData(
      'session-1',
      'done\nMULTI_AI_CODE_SCHEDULED_TASK_DONE:ignored:succeeded\n'
    )

    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
  })

  it('does not cancel an already delivered task when its AICLI session exits', async () => {
    mockStore.dueTasks = [task(1, 'project-1', 'exited task')]
    setScheduledTaskSendHandler({
      resolveSession: () => ({
        sessionId: 'session-1',
        targetRepo: 'E:\\OpenSource\\project-1'
      }),
      sendUser: async () => ({ ok: true })
    })

    await runScheduledTaskScanOnce({ now: 2 })
    expect(getScheduledTaskQueueState().running).toBeNull()

    handleScheduledTaskSessionExit('session-1')

    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
  })

  it('uses an explicit session for manual runs when project lookup is unavailable', async () => {
    const sentSessions: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: () => null,
      sendUser: async (sessionId) => {
        sentSessions.push(sessionId)
        return { ok: true }
      }
    })

    const result = await enqueueScheduledTaskNow(
      task(1, 'project-1', 'manual task'),
      'E:\\OpenSource\\project-1',
      2,
      {
        sessionId: 'manual-session',
        targetRepo: 'E:\\OpenSource\\project-1'
      }
    )

    expect(result.delivery).toBe('sent')
    expect(sentSessions).toEqual(['manual-session'])
    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
  })

  it('reports manual send failures instead of looking successful', async () => {
    setScheduledTaskSendHandler({
      resolveSession: () => null,
      sendUser: async () => ({ ok: false, error: 'no session' })
    })

    const result = await enqueueScheduledTaskNow(
      task(1, 'project-1', 'manual task'),
      'E:\\OpenSource\\project-1',
      2,
      {
        sessionId: 'stale-session',
        targetRepo: 'E:\\OpenSource\\project-1'
      }
    )

    expect(result.delivery).toBe('failed')
    expect(result.error).toBe('no session')
    expect(mockStore.runs.get(1)?.status).toBe('failed')
  })

  it('sends multiple runnable tasks without waiting for prior completion', async () => {
    const sent: string[] = []
    const session = {
      sessionId: 'manual-session',
      targetRepo: 'E:\\OpenSource\\project-1'
    }
    setScheduledTaskSendHandler({
      resolveSession: () => session,
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    mockStore.dueTasks = [
      task(1, 'project-1', 'first runnable task'),
      task(2, 'project-1', 'second runnable task')
    ]
    await runScheduledTaskScanOnce({ now: 2 })

    expect(sent).toHaveLength(2)
    expect(sent[0]).toContain('prompt:first runnable task')
    expect(sent[1]).toContain('prompt:second runnable task')
    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(mockStore.runs.get(1)?.status).toBe('succeeded')
    expect(mockStore.runs.get(2)?.status).toBe('succeeded')
  })
})
