import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { closeDb, createProject, initDb } from '../store/db.js'
import { createScheduledTask, listScheduledTasks } from './taskStore.js'
import {
  getScheduledTaskQueueState,
  resetScheduledTaskSchedulerForTests,
  runScheduledTaskScanOnce,
  setScheduledTaskSendHandler
} from './scheduler.js'

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

function createDueTask(name: string): number {
  return createScheduledTask(
    {
      projectId: 'project-1',
      name,
      description: '',
      goal: '执行任务',
      instructions: ['给出中文总结'],
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

describe('scheduled task scheduler', () => {
  it('keeps due tasks queued when no AICLI session is available', async () => {
    createDueTask('无会话任务')
    setScheduledTaskSendHandler({
      resolveSession: () => null,
      sendUser: async () => ({ ok: true })
    })

    await runScheduledTaskScanOnce({
      now: new Date(2026, 5, 19, 10, 0).getTime()
    })

    const state = getScheduledTaskQueueState()
    expect(state.waiting).toHaveLength(1)
    expect(state.waiting[0].taskName).toBe('无会话任务')
    expect(listScheduledTasks('project-1')[0].lastRun?.status).toBe('queued')
  })

  it('sends queued tasks through the current AICLI session in FIFO order', async () => {
    createDueTask('第一个任务')
    createDueTask('第二个任务')
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
    expect(sent[0]).toContain('任务名称：第一个任务')
    expect(sent[1]).toContain('任务名称：第二个任务')
    const tasks = listScheduledTasks('project-1')
    expect(tasks.map((task) => task.lastRun?.status)).toEqual(['succeeded', 'succeeded'])
    expect(getScheduledTaskQueueState().waiting).toEqual([])
  })
})
