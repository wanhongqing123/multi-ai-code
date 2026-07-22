import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { closeDb, createProject, initDb } from '../../../electron/store/db.js'
import { createScheduledTask, listScheduledTasks } from '../../../electron/scheduledTasks/taskStore.js'
import {
  getScheduledTaskQueueState,
  resetScheduledTaskSchedulerForTests,
  runScheduledTaskScanOnce,
  setScheduledTaskSendHandler
} from '../../../electron/scheduledTasks/scheduler.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'scheduled-task-scheduler-'))
  process.env.MULTI_AI_ROOT = tempRoot
  await fs.mkdir(tempRoot, { recursive: true })
  closeDb()
  initDb()
  createProject({
    id: 'project-1',
    name: 'Project 1',
    target_repo: 'E:\\OpenSource\\project-1'
  })
  createProject({
    id: 'project-2',
    name: 'Project 2',
    target_repo: 'E:\\OpenSource\\project-2'
  })
  resetScheduledTaskSchedulerForTests()
})

afterEach(async () => {
  resetScheduledTaskSchedulerForTests()
  closeDb()
  delete process.env.MULTI_AI_ROOT
  if (tempRoot) {
    await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  }
})

function createDueTaskForProject(projectId: string, name: string): number {
  return createScheduledTask(
    {
      projectId,
      name,
      description: '',
      goal: 'execute task',
      instructions: ['summarize in Chinese'],
      enabled: true,
      scheduleType: 'once',
      scheduleTime: '09:00',
      scheduleDays: [],
      timeoutMinutes: 30,
      allowCodeChanges: false,
      allowGitCommit: false,
      requireTestConfirmation: false
    },
    new Date(2026, 5, 19, 8, 0).getTime()
  ).id
}

function createDueTask(name: string): number {
  return createDueTaskForProject('project-1', name)
}

describe('scheduled task scheduler', () => {
  it('leaves due tasks pending when no AICLI session is available', async () => {
    createDueTask('no session task')
    setScheduledTaskSendHandler({
      resolveSession: () => null,
      sendUser: async () => ({ ok: true })
    })

    await runScheduledTaskScanOnce({
      now: new Date(2026, 5, 19, 10, 0).getTime()
    })

    const state = getScheduledTaskQueueState()
    expect(state.waiting).toEqual([])
    const [task] = listScheduledTasks('project-1')
    expect(task.lastRun).toBeNull()
    expect(task.nextRunAt).toBe(new Date(2026, 5, 19, 9, 0).getTime())
  })

  it('sends queued tasks through the current AICLI session in FIFO order', async () => {
    createDueTask('first task')
    createDueTask('second task')
    const sent: string[] = []
    setScheduledTaskSendHandler({
      resolveSession: () => ({
        sessionId: 'session-1',
        targetRepo: 'E:\\OpenSource\\multi-ai-code'
      }),
      sendUser: async (_sessionId, prompt) => {
        sent.push(prompt)
        return { ok: true }
      }
    })

    await runScheduledTaskScanOnce({
      now: new Date(2026, 5, 19, 10, 0).getTime()
    })

    expect(sent).toHaveLength(2)
    expect(sent[0]).toContain('first task')
    expect(sent[1]).toContain('second task')

    const tasks = listScheduledTasks('project-1')
    expect(tasks.map((task) => task.lastRun?.status)).toEqual(['succeeded', 'succeeded'])
    expect(getScheduledTaskQueueState().waiting).toEqual([])
  })

  it('runs overdue pending tasks when a matching AICLI session becomes available', async () => {
    createDueTaskForProject('project-1', 'delayed task')
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

    await runScheduledTaskScanOnce({
      now: new Date(2026, 5, 19, 10, 0).getTime()
    })
    expect(getScheduledTaskQueueState().waiting).toEqual([])
    expect(listScheduledTasks('project-1')[0].lastRun).toBeNull()

    sessionAvailable = true
    await runScheduledTaskScanOnce({
      now: new Date(2026, 5, 19, 10, 1).getTime()
    })

    expect(sent).toHaveLength(1)
    expect(getScheduledTaskQueueState().waiting).toEqual([])
    expect(getScheduledTaskQueueState().running).toBeNull()
    expect(listScheduledTasks('project-1')[0].lastRun?.status).toBe('succeeded')
  })

  it('does not let a task without a session block runnable tasks for another project', async () => {
    createDueTaskForProject('project-1', 'blocked project task')
    createDueTaskForProject('project-2', 'runnable project task')
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

    await runScheduledTaskScanOnce({
      now: new Date(2026, 5, 19, 10, 0).getTime()
    })

    expect(sent).toHaveLength(1)
    expect(sent[0]).toContain('runnable project task')
    const state = getScheduledTaskQueueState()
    expect(state.running).toBeNull()
    expect(state.waiting).toEqual([])
    expect(listScheduledTasks('project-1')[0].lastRun).toBeNull()
    expect(listScheduledTasks('project-2')[0].lastRun?.status).toBe('succeeded')
  })
})
