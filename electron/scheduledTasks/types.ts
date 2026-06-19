export type ScheduledTaskScheduleType = 'once' | 'daily' | 'weekly'

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

export interface ScheduledTask {
  id: number
  projectId: string
  name: string
  description: string
  goal: string
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
