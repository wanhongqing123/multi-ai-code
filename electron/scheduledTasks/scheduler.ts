import { buildScheduledTaskPrompt } from './promptBuilder.js'
import {
  advanceScheduledTaskAfterQueue,
  createScheduledTaskRun,
  listAllDueScheduledTasks,
  listDueScheduledTasks,
  updateScheduledTaskRun
} from './taskStore.js'
import type { ScheduledTask } from './types.js'

export interface ScheduledTaskSessionInfo {
  sessionId: string
  targetRepo: string
}

export interface ScheduledTaskSendHandler {
  resolveSession(projectId: string): ScheduledTaskSessionInfo | null
  sendUser(sessionId: string, prompt: string): Promise<{ ok: boolean; error?: string }>
}

export interface ScheduledTaskQueueItem {
  taskId: number
  taskName: string
  projectId: string
  runId: number
  scheduledAt: number
  prompt: string
}

export interface ScheduledTaskQueueState {
  running: ScheduledTaskQueueItem | null
  waiting: ScheduledTaskQueueItem[]
}

let sendHandler: ScheduledTaskSendHandler | null = null
let timer: NodeJS.Timeout | null = null
let draining = false
let runningItem: ScheduledTaskQueueItem | null = null
const waitingItems: ScheduledTaskQueueItem[] = []

function hasQueuedTask(taskId: number): boolean {
  return (
    runningItem?.taskId === taskId || waitingItems.some((item) => item.taskId === taskId)
  )
}

function enqueueTask(task: ScheduledTask, scheduledAt: number, targetRepo: string): void {
  if (hasQueuedTask(task.id)) return
  const prompt = buildScheduledTaskPrompt(task, { targetRepo })
  const run = createScheduledTaskRun({
    taskId: task.id,
    status: 'queued',
    scheduledAt,
    prompt,
    timeoutMinutes: task.timeoutMinutes
  })
  waitingItems.push({
    taskId: task.id,
    taskName: task.name,
    projectId: task.projectId,
    runId: run.id,
    scheduledAt,
    prompt
  })
  advanceScheduledTaskAfterQueue(task, Date.now())
}

export async function enqueueScheduledTaskNow(
  task: ScheduledTask,
  targetRepo: string,
  scheduledAt = Date.now()
): Promise<ScheduledTaskQueueState> {
  enqueueTask(task, scheduledAt, targetRepo)
  await drainQueue()
  return getScheduledTaskQueueState()
}

async function drainQueue(): Promise<void> {
  if (draining) return
  draining = true
  try {
    while (waitingItems.length > 0) {
      const item = waitingItems[0]
      const session = sendHandler?.resolveSession(item.projectId) ?? null
      if (!session) return

      runningItem = item
      waitingItems.shift()
      updateScheduledTaskRun(item.runId, {
        status: 'running',
        startedAt: Date.now(),
        error: null
      })

      let result: { ok: boolean; error?: string }
      try {
        result = await sendHandler!.sendUser(session.sessionId, item.prompt)
      } catch (err) {
        result = {
          ok: false,
          error: err instanceof Error ? err.message : String(err)
        }
      }
      updateScheduledTaskRun(item.runId, {
        status: result.ok ? 'succeeded' : 'failed',
        finishedAt: Date.now(),
        outputExcerpt: result.ok ? '已发送到当前 AICLI。' : null,
        error: result.ok ? null : result.error ?? '发送到当前 AICLI 失败'
      })
      runningItem = null
    }
  } finally {
    runningItem = null
    draining = false
  }
}

export function setScheduledTaskSendHandler(handler: ScheduledTaskSendHandler | null): void {
  sendHandler = handler
  if (handler) {
    void drainQueue()
  }
}

export async function runScheduledTaskScanOnce(options: {
  projectId?: string
  now?: number
} = {}): Promise<ScheduledTaskQueueState> {
  const now = options.now ?? Date.now()
  const tasks = options.projectId
    ? listDueScheduledTasks(options.projectId, now)
    : listAllDueScheduledTasks(now)
  for (const task of tasks) {
    const session = sendHandler?.resolveSession(task.projectId) ?? null
    enqueueTask(task, task.nextRunAt ?? now, session?.targetRepo ?? '当前项目')
  }
  await drainQueue()
  return getScheduledTaskQueueState()
}

export function getScheduledTaskQueueState(): ScheduledTaskQueueState {
  return {
    running: runningItem ? { ...runningItem } : null,
    waiting: waitingItems.map((item) => ({ ...item }))
  }
}

export function startScheduledTaskScheduler(intervalMs = 60_000): void {
  if (timer) return
  timer = setInterval(() => {
    void runScheduledTaskScanOnce()
  }, intervalMs)
}

export function stopScheduledTaskScheduler(): void {
  if (timer) {
    clearInterval(timer)
    timer = null
  }
}

export function resetScheduledTaskSchedulerForTests(): void {
  stopScheduledTaskScheduler()
  sendHandler = null
  draining = false
  runningItem = null
  waitingItems.splice(0)
}
