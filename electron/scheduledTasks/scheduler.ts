import { randomBytes } from 'crypto'
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
  completionToken: string
  timeoutMinutes: number
  preferredSession?: ScheduledTaskSessionInfo
}

export interface ScheduledTaskQueueState {
  running: ScheduledTaskQueueItem | null
  waiting: ScheduledTaskQueueItem[]
}

let sendHandler: ScheduledTaskSendHandler | null = null
let timer: NodeJS.Timeout | null = null
let draining = false
let runningItem: ScheduledTaskQueueItem | null = null
let runningSessionId: string | null = null
let runningTimeout: NodeJS.Timeout | null = null
let runningOutputBuffer = ''
const waitingItems: ScheduledTaskQueueItem[] = []
const SCHEDULED_TASK_OUTPUT_BUFFER_LIMIT = 65_536
const SCHEDULED_TASK_OUTPUT_EXCERPT_LIMIT = 16_384

function hasQueuedTask(taskId: number): boolean {
  return (
    runningItem?.taskId === taskId || waitingItems.some((item) => item.taskId === taskId)
  )
}

function createCompletionToken(): string {
  return randomBytes(12).toString('base64url')
}

function enqueueTask(
  task: ScheduledTask,
  scheduledAt: number,
  targetRepo: string,
  preferredSession?: ScheduledTaskSessionInfo
): void {
  if (hasQueuedTask(task.id)) return
  const completionToken = createCompletionToken()
  const prompt = buildScheduledTaskPrompt(task, { targetRepo, completionToken })
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
    prompt,
    completionToken,
    timeoutMinutes: task.timeoutMinutes,
    preferredSession
  })
  advanceScheduledTaskAfterQueue(task, Date.now())
}

export async function enqueueScheduledTaskNow(
  task: ScheduledTask,
  targetRepo: string,
  scheduledAt = Date.now(),
  preferredSession?: ScheduledTaskSessionInfo
): Promise<ScheduledTaskQueueState> {
  enqueueTask(task, scheduledAt, targetRepo, preferredSession)
  await drainQueue()
  return getScheduledTaskQueueState()
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
      runningOutputBuffer = ''
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
        completeRunningItem('failed', {
          error: result.error ?? 'Failed to send scheduled task to AICLI.',
          drainNext: false
        })
        continue
      }

      startRunningTimeout(item)
      return
    }
  } finally {
    draining = false
  }
}

function startRunningTimeout(item: ScheduledTaskQueueItem): void {
  clearRunningTimeout()
  const timeoutMs = Math.max(1, item.timeoutMinutes) * 60_000
  runningTimeout = setTimeout(() => {
    completeRunningItem('timed_out', {
      error: `Scheduled task timed out after ${item.timeoutMinutes} minutes.`
    })
  }, timeoutMs)
  runningTimeout.unref?.()
}

function clearRunningTimeout(): void {
  if (!runningTimeout) return
  clearTimeout(runningTimeout)
  runningTimeout = null
}

function completeRunningItem(
  status: 'succeeded' | 'failed' | 'timed_out',
  options: {
    error?: string | null
    outputExcerpt?: string | null
    drainNext?: boolean
  } = {}
): boolean {
  const item = runningItem
  if (!item) return false
  clearRunningTimeout()
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
  runningOutputBuffer = ''
  if (options.drainNext !== false) {
    void drainQueue()
  }
  return true
}

export function handleScheduledTaskSessionData(sessionId: string, chunk: string): void {
  const item = runningItem
  if (!item || sessionId !== runningSessionId) return
  runningOutputBuffer = (runningOutputBuffer + chunk).slice(
    -SCHEDULED_TASK_OUTPUT_BUFFER_LIMIT
  )
  const marker = `MULTI_AI_CODE_SCHEDULED_TASK_DONE:${item.completionToken}:`
  const markerIndex = runningOutputBuffer.indexOf(marker)
  if (markerIndex < 0) return
  const statusText = runningOutputBuffer.slice(markerIndex + marker.length)
  const statusMatch = /^(succeeded|failed)\b/.exec(statusText)
  if (!statusMatch) return
  const status = statusMatch[1] as 'succeeded' | 'failed'
  completeRunningItem(status, {
    error: status === 'failed' ? 'AICLI reported scheduled task failure.' : null,
    outputExcerpt: runningOutputBuffer.slice(-SCHEDULED_TASK_OUTPUT_EXCERPT_LIMIT)
  })
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
    enqueueTask(task, task.nextRunAt ?? now, session?.targetRepo ?? 'current project')
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
  runningOutputBuffer = ''
  clearRunningTimeout()
  waitingItems.splice(0)
}
