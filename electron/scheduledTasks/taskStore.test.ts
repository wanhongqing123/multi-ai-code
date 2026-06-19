import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { closeDb, createProject, initDb } from '../store/db.js'
import {
  computeNextRunAt,
  createScheduledTask,
  createScheduledTaskRun,
  deleteScheduledTask,
  listDueScheduledTasks,
  listScheduledTasks,
  setScheduledTaskEnabled,
  updateScheduledTask,
  updateScheduledTaskRun
} from './taskStore.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'scheduled-task-store-'))
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
})

afterEach(async () => {
  closeDb()
  delete process.env.MULTI_AI_ROOT
  if (tempRoot) {
    await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  }
})

describe('computeNextRunAt', () => {
  it('returns today when a daily task time is still ahead', () => {
    const now = new Date(2026, 5, 19, 10, 0).getTime()
    const next = computeNextRunAt({ scheduleType: 'daily', scheduleTime: '21:30', scheduleDays: [] }, now)

    const nextDate = new Date(next!)
    expect(nextDate.getFullYear()).toBe(2026)
    expect(nextDate.getMonth()).toBe(5)
    expect(nextDate.getDate()).toBe(19)
    expect(nextDate.getHours()).toBe(21)
    expect(nextDate.getMinutes()).toBe(30)
  })

  it('returns tomorrow when a daily task time has passed', () => {
    const now = new Date(2026, 5, 19, 22, 0).getTime()
    const next = computeNextRunAt({ scheduleType: 'daily', scheduleTime: '21:30', scheduleDays: [] }, now)

    const nextDate = new Date(next!)
    expect(nextDate.getFullYear()).toBe(2026)
    expect(nextDate.getMonth()).toBe(5)
    expect(nextDate.getDate()).toBe(20)
    expect(nextDate.getHours()).toBe(21)
    expect(nextDate.getMinutes()).toBe(30)
  })

  it('returns the next selected weekday for weekly tasks', () => {
    const friday = new Date(2026, 5, 19, 10, 0).getTime()
    const next = computeNextRunAt({
      scheduleType: 'weekly',
      scheduleTime: '09:00',
      scheduleDays: [1]
    }, friday)

    const nextDate = new Date(next!)
    expect(nextDate.getFullYear()).toBe(2026)
    expect(nextDate.getMonth()).toBe(5)
    expect(nextDate.getDate()).toBe(22)
    expect(nextDate.getHours()).toBe(9)
    expect(nextDate.getMinutes()).toBe(0)
  })
})

describe('scheduled task persistence', () => {
  it('creates, lists, updates, disables, and deletes a task', () => {
    const created = createScheduledTask({
      projectId: 'project-1',
      name: '每日代码巡检',
      description: '检查项目风险',
      goal: '找出风险',
      instructions: ['分析代码风险'],
      enabled: true,
      scheduleType: 'daily',
      scheduleTime: '21:30',
      scheduleDays: [],
      timeoutMinutes: 30,
      allowCodeChanges: false,
      allowGitCommit: false,
      requireTestConfirmation: false
    }, new Date(2026, 5, 19, 10, 0).getTime())

    expect(created.id).toBeGreaterThan(0)
    expect(new Date(created.nextRunAt!).getHours()).toBe(21)
    expect(new Date(created.nextRunAt!).getMinutes()).toBe(30)

    updateScheduledTask(created.id, {
      name: '每日项目巡检',
      instructions: ['分析代码风险', '给出修改建议']
    })
    setScheduledTaskEnabled(created.id, false)

    const [updated] = listScheduledTasks('project-1')
    expect(updated.name).toBe('每日项目巡检')
    expect(updated.enabled).toBe(false)
    expect(updated.instructions).toEqual(['分析代码风险', '给出修改建议'])

    deleteScheduledTask(created.id)
    expect(listScheduledTasks('project-1')).toEqual([])
  })

  it('lists due enabled tasks for one project only', () => {
    const now = new Date(2026, 5, 19, 22, 0).getTime()
    const due = createScheduledTask({
      projectId: 'project-1',
      name: '到期任务',
      description: '',
      goal: '执行',
      instructions: [],
      enabled: true,
      scheduleType: 'once',
      scheduleTime: '21:30',
      scheduleDays: [],
      timeoutMinutes: 30,
      allowCodeChanges: false,
      allowGitCommit: false,
      requireTestConfirmation: false
    }, new Date(2026, 5, 19, 20, 0).getTime())

    createScheduledTask({
      projectId: 'project-2',
      name: '其他项目任务',
      description: '',
      goal: '执行',
      instructions: [],
      enabled: true,
      scheduleType: 'once',
      scheduleTime: '21:30',
      scheduleDays: [],
      timeoutMinutes: 30,
      allowCodeChanges: false,
      allowGitCommit: false,
      requireTestConfirmation: false
    }, new Date(2026, 5, 19, 20, 0).getTime())

    const dueTasks = listDueScheduledTasks('project-1', now)
    expect(dueTasks.map((task) => task.id)).toEqual([due.id])
  })

  it('creates and updates run records', () => {
    const task = createScheduledTask({
      projectId: 'project-1',
      name: '运行任务',
      description: '',
      goal: '执行',
      instructions: [],
      enabled: true,
      scheduleType: 'daily',
      scheduleTime: '21:30',
      scheduleDays: [],
      timeoutMinutes: 30,
      allowCodeChanges: false,
      allowGitCommit: false,
      requireTestConfirmation: false
    }, new Date(2026, 5, 19, 10, 0).getTime())

    const run = createScheduledTaskRun({
      taskId: task.id,
      status: 'queued',
      scheduledAt: new Date(2026, 5, 19, 21, 30).getTime(),
      prompt: 'hello',
      timeoutMinutes: 30
    })

    updateScheduledTaskRun(run.id, {
      status: 'failed',
      error: 'AICLI 未启动',
      finishedAt: new Date(2026, 5, 19, 21, 31).getTime()
    })

    const [withLastRun] = listScheduledTasks('project-1')
    expect(withLastRun.lastRun?.status).toBe('failed')
    expect(withLastRun.lastRun?.error).toBe('AICLI 未启动')
  })
})
