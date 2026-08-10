/**
 * 任务的触发方式。'manual' 是「手动任务」：不参与调度，只在用户点「立即执行」
 * 时跑一次——普通任务和定时任务的区别就只在这里，其余字段（目标、指令、超时、
 * 权限）完全共用。
 */
export type ScheduledTaskScheduleType =
  | 'manual'
  | 'once'
  | 'daily'
  | 'weekly'
  | 'interval'

export type ScheduledTaskRunStatus =
  | 'pending'
  | 'queued'
  | 'running'
  | 'succeeded'
  | 'failed'
  | 'timed_out'
  | 'cancelled'
  | 'skipped'

export interface ScheduledTaskRun {
  id: number
  taskId: number
  status: ScheduledTaskRunStatus
  scheduledAt: number
  startedAt: number | null
  finishedAt: number | null
  prompt: string
  outputExcerpt: string | null
  error: string | null
  timeoutMinutes: number
}

export interface ScheduledTaskImageAttachment {
  id: string
  localPath: string
  fileName: string
  mimeType: string
  sizeBytes: number
}

export interface SaveScheduledTaskImageInput {
  projectId: string
  fileName: string
  mimeType: string
  data: ArrayBuffer | Uint8Array
}

export type SaveScheduledTaskImageResult =
  | { ok: true; attachment: ScheduledTaskImageAttachment }
  | { ok: false; error: string }

export interface ReadScheduledTaskImageInput {
  projectId: string
  localPath: string
}

export type ReadScheduledTaskImageResult =
  | { ok: true; dataUrl: string }
  | { ok: false; error: string }

export interface ScheduledTask {
  id: number
  projectId: string
  targetRepo: string | null
  name: string
  description: string
  goal: string
  imageAttachments: ScheduledTaskImageAttachment[]
  instructions: string[]
  enabled: boolean
  scheduleType: ScheduledTaskScheduleType
  scheduleTime: string
  scheduleDays: number[]
  nextRunAt: number | null
  timeoutMinutes: number
  allowCodeChanges: boolean
  allowGitCommit: boolean
  requireTestConfirmation: boolean
  createdAt: number
  updatedAt: number
  lastRun: ScheduledTaskRun | null
}

export interface CreateScheduledTaskInput {
  projectId: string
  name: string
  description: string
  goal: string
  imageAttachments: ScheduledTaskImageAttachment[]
  instructions: string[]
  enabled: boolean
  scheduleType: ScheduledTaskScheduleType
  scheduleTime: string
  scheduleDays: number[]
  timeoutMinutes: number
  allowCodeChanges: boolean
  allowGitCommit: boolean
  requireTestConfirmation: boolean
}

export type UpdateScheduledTaskInput = Partial<
  Omit<CreateScheduledTaskInput, 'projectId'>
>

export interface CreateScheduledTaskRunInput {
  taskId: number
  status: ScheduledTaskRunStatus
  scheduledAt: number
  prompt: string
  timeoutMinutes: number
}

export interface UpdateScheduledTaskRunInput {
  status?: ScheduledTaskRunStatus
  startedAt?: number | null
  finishedAt?: number | null
  outputExcerpt?: string | null
  error?: string | null
}
