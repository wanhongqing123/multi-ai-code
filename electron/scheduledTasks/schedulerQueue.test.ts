import { beforeEach, describe, expect, it, vi } from 'vitest'
import type { ScheduledTask } from './types.js'

const mockStore = vi.hoisted(() => ({
  dueTasks: [] as ScheduledTask[],
  nextRunId: 1,
  runs: new Map<number, { id: number; taskId: number; status: string }>()
}))

vi.mock('./promptBuilder.js', () => ({
  buildScheduledTaskPrompt: (
    task: ScheduledTask,
    context: { completionToken?: string }
  ) => `prompt:${task.name}:token:${context.completionToken ?? ''}`
}))

vi.mock('./taskStore.js', () => ({
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

import {
  drainScheduledTaskQueue,
  enqueueScheduledTaskNow,
  getScheduledTaskQueueState,
  handleScheduledTaskSessionData,
  resetScheduledTaskSchedulerForTests,
  runScheduledTaskScanOnce,
  setScheduledTaskSendHandler
} from './scheduler.js'

function task(id: number, projectId: string, name: string): ScheduledTask {
  return {
    id,
    projectId,
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
  })

  it('drains already queued tasks when a matching AICLI session becomes available', async () => {
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
    expect(getScheduledTaskQueueState().waiting).toHaveLength(1)

    sessionAvailable = true
    await drainScheduledTaskQueue()

    expect(sent[0]).toContain('prompt:delayed task:token:')
    expect(getScheduledTaskQueueState().waiting).toEqual([])
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

    expect(sent[0]).toContain('prompt:runnable project task:token:')
    const state = getScheduledTaskQueueState()
    expect(state.waiting).toHaveLength(1)
    expect(state.waiting[0].projectId).toBe('project-1')
  })

  it('keeps a delivered task running until AICLI reports completion', async () => {
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

    expect(sent[0]).toContain('prompt:long task:token:')
    expect(getScheduledTaskQueueState().running?.taskId).toBe(1)
    expect(mockStore.runs.get(1)?.status).toBe('running')
  })

  it('marks a running task succeeded when its completion marker appears', async () => {
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

    const token = /token:([A-Za-z0-9_-]+)/.exec(sent[0])?.[1]
    expect(token).toBeTruthy()
    handleScheduledTaskSessionData(
      'session-1',
      `done\nMULTI_AI_CODE_SCHEDULED_TASK_DONE:${token}:succeeded\n`
    )

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

    await enqueueScheduledTaskNow(
      task(1, 'project-1', 'manual task'),
      'E:\\OpenSource\\project-1',
      2,
      {
        sessionId: 'manual-session',
        targetRepo: 'E:\\OpenSource\\project-1'
      }
    )

    expect(sentSessions).toEqual(['manual-session'])
    expect(getScheduledTaskQueueState().running?.taskId).toBe(1)
  })
})
