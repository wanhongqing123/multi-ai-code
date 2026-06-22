import { buildScheduledTaskPrompt } from './promptBuilder.js'
import {
  advanceScheduledTaskAfterQueue,
  createScheduledTaskRun,
  listAllDueScheduledTasks,
  listDueScheduledTasks,
  updateScheduledTaskRun
} from './taskStore.js'
import type { ScheduledTask, ScheduledTaskRunStatus } from './types.js'

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
  targetRepo: string | null
  runId: number
  scheduledAt: number
  prompt: string
  timeoutMinutes: number
  preferredSession?: ScheduledTaskSessionInfo
}

export interface ScheduledTaskQueueState {
  running: ScheduledTaskQueueItem | null
  waiting: ScheduledTaskQueueItem[]
}

export type ScheduledTaskDelivery = 'sent' | 'queued' | 'failed'

export interface ScheduledTaskEnqueueResult {
  state: ScheduledTaskQueueState
  delivery: ScheduledTaskDelivery
  error?: string
}

export interface ScheduledTaskCancelResult {
  cancelled: boolean
  state: ScheduledTaskQueueState
}

let sendHandler: ScheduledTaskSendHandler | null = null
let timer: NodeJS.Timeout | null = null
let draining = false
let runningItem: ScheduledTaskQueueItem | null = null
let runningSessionId: string | null = null
const waitingItems: ScheduledTaskQueueItem[] = []
const deliveryResults = new Map<
  number,
  { delivery: 'sent' | 'failed'; error?: string }
>()

function hasQueuedTask(taskId: number): boolean {
  return (
    runningItem?.taskId === taskId || waitingItems.some((item) => item.taskId === taskId)
  )
}

function enqueueTask(
  task: ScheduledTask,
  scheduledAt: number,
  targetRepo: string,
  preferredSession?: ScheduledTaskSessionInfo
): ScheduledTaskQueueItem | null {
  if (hasQueuedTask(task.id)) return null
  const prompt = buildScheduledTaskPrompt(task, { targetRepo })
  const run = createScheduledTaskRun({
    taskId: task.id,
    status: 'queued',
    scheduledAt,
    prompt,
    timeoutMinutes: task.timeoutMinutes
  })
  const item: ScheduledTaskQueueItem = {
    taskId: task.id,
    taskName: task.name,
    projectId: task.projectId,
    targetRepo,
    runId: run.id,
    scheduledAt,
    prompt,
    timeoutMinutes: task.timeoutMinutes,
    preferredSession
  }
  waitingItems.push(item)
  advanceScheduledTaskAfterQueue(task, Date.now())
  return item
}

export async function enqueueScheduledTaskNow(
  task: ScheduledTask,
  targetRepo: string,
  scheduledAt = Date.now(),
  preferredSession?: ScheduledTaskSessionInfo
): Promise<ScheduledTaskEnqueueResult> {
  const item = enqueueTask(task, scheduledAt, targetRepo, preferredSession)
  await drainQueue()
  const state = getScheduledTaskQueueState()
  if (!item) {
    return { state, delivery: 'queued' }
  }
  const delivery = deliveryResults.get(item.runId)
  if (delivery) {
    deliveryResults.delete(item.runId)
    return { state, ...delivery }
  }
  if (state.running?.runId === item.runId) {
    return { state, delivery: 'sent' }
  }
  return { state, delivery: 'queued' }
}

async function drainQueue(): Promise<void> {
  if (draining || runningItem) return
  draining = true
  try {
    while (!runningItem && waitingItems.length > 0) {
      const next = takeNextRunnableItem()
      if (!next) return
      const { item, session } = next

      runningItem = item
      runningSessionId = session.sessionId
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

      if (runningItem?.runId !== item.runId) continue

      if (!result.ok) {
        const error = result.error ?? 'Failed to send scheduled task to AICLI.'
        deliveryResults.set(item.runId, { delivery: 'failed', error })
        completeRunningItem('failed', {
          error,
          drainNext: false
        })
        continue
      }

      deliveryResults.set(item.runId, { delivery: 'sent' })
      completeRunningItem('succeeded', {
        outputExcerpt: '已发送到当前 AICLI。',
        drainNext: false
      })
      continue
    }
  } finally {
    draining = false
  }
}

function completeRunningItem(
  status: Extract<ScheduledTaskRunStatus, 'succeeded' | 'failed' | 'timed_out' | 'cancelled'>,
  options: {
    error?: string | null
    outputExcerpt?: string | null
    drainNext?: boolean
  } = {}
): boolean {
  const item = runningItem
  if (!item) return false
  updateScheduledTaskRun(item.runId, {
    status,
    finishedAt: Date.now(),
    outputExcerpt:
      options.outputExcerpt ??
      (status === 'succeeded'
        ? 'AICLI reported scheduled task completion.'
        : null),
    error: status === 'succeeded' ? null : options.error ?? 'Scheduled task failed.'
  })
  runningItem = null
  runningSessionId = null
  if (options.drainNext !== false) {
    void drainQueue()
  }
  return true
}

export function handleScheduledTaskSessionExit(sessionId: string): void {
  let shouldDrain = false
  if (runningItem && runningSessionId === sessionId) {
    completeRunningItem('cancelled', {
      error: 'AICLI session exited before scheduled task completed.',
      drainNext: false
    })
    shouldDrain = true
  }
  for (let index = waitingItems.length - 1; index >= 0; index -= 1) {
    const item = waitingItems[index]
    if (item.preferredSession?.sessionId !== sessionId) continue
    waitingItems.splice(index, 1)
    updateScheduledTaskRun(item.runId, {
      status: 'cancelled',
      finishedAt: Date.now(),
      error: 'AICLI session exited before scheduled task was sent.'
    })
    shouldDrain = true
  }
  if (shouldDrain) {
    void drainQueue()
  }
}

export async function cancelScheduledTaskQueueRun(
  runId?: number
): Promise<ScheduledTaskCancelResult> {
  let cancelled = false
  if (runningItem && (runId === undefined || runningItem.runId === runId)) {
    completeRunningItem('cancelled', {
      error: 'Scheduled task run was cancelled by the user.',
      drainNext: false
    })
    cancelled = true
  }
  for (let index = waitingItems.length - 1; index >= 0; index -= 1) {
    const item = waitingItems[index]
    if (runId !== undefined && item.runId !== runId) continue
    waitingItems.splice(index, 1)
    updateScheduledTaskRun(item.runId, {
      status: 'cancelled',
      finishedAt: Date.now(),
      error: 'Scheduled task run was cancelled by the user.'
    })
    cancelled = true
  }
  await drainQueue()
  return { cancelled, state: getScheduledTaskQueueState() }
}

export function handleScheduledTaskSessionData(sessionId: string, chunk: string): void {
  void sessionId
  void chunk
}

function publicQueueItem(item: ScheduledTaskQueueItem): ScheduledTaskQueueItem {
  const { preferredSession: _preferredSession, ...publicItem } = item
  return { ...publicItem }
}

function takeNextRunnableItem(): {
  item: ScheduledTaskQueueItem
  session: ScheduledTaskSessionInfo
} | null {
  if (!sendHandler) return null
  for (let index = 0; index < waitingItems.length; index += 1) {
    const item = waitingItems[index]
    const session = item.preferredSession ?? sendHandler.resolveSession(item.projectId)
    if (!session) continue
    waitingItems.splice(index, 1)
    return { item, session }
  }
  return null
}

export async function drainScheduledTaskQueue(): Promise<ScheduledTaskQueueState> {
  await drainQueue()
  return getScheduledTaskQueueState()
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
    if (!session) continue
    enqueueTask(task, task.nextRunAt ?? now, session.targetRepo ?? task.targetRepo ?? 'current project')
  }
  await drainQueue()
  return getScheduledTaskQueueState()
}

export function getScheduledTaskQueueState(): ScheduledTaskQueueState {
  return {
    running: runningItem ? publicQueueItem(runningItem) : null,
    waiting: waitingItems.map(publicQueueItem)
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
  runningSessionId = null
  waitingItems.splice(0)
  deliveryResults.clear()
}
