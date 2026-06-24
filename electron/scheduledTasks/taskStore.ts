import { getDb } from '../store/db.js'
import type {
  CreateScheduledTaskInput,
  CreateScheduledTaskRunInput,
  ScheduledTask,
  ScheduledTaskRun,
  ScheduledTaskScheduleType,
  UpdateScheduledTaskInput,
  UpdateScheduledTaskRunInput
} from './types.js'

interface ScheduledTaskRow {
  id: number
  project_id: string
  target_repo: string | null
  name: string
  description: string
  goal: string
  instructions: string
  enabled: number
  schedule_type: ScheduledTaskScheduleType
  schedule_time: string
  schedule_days: string
  next_run_at: number | null
  timeout_minutes: number
  allow_code_changes: number
  allow_git_commit: number
  require_test_confirmation: number
  created_at: number
  updated_at: number
}

interface ScheduledTaskRunRow {
  id: number
  task_id: number
  status: ScheduledTaskRun['status']
  scheduled_at: number
  started_at: number | null
  finished_at: number | null
  prompt: string
  output_excerpt: string | null
  error: string | null
  timeout_minutes: number
}

export interface ScheduleInput {
  scheduleType: ScheduledTaskScheduleType
  scheduleTime: string
  scheduleDays: number[]
}

const SCHEDULED_TASK_SELECT = `scheduled_tasks.*, projects.target_repo AS target_repo`
const SCHEDULED_TASK_JOIN = `FROM scheduled_tasks
LEFT JOIN projects ON projects.id = scheduled_tasks.project_id`

function parseJsonArray(value: string): unknown[] {
  try {
    const parsed = JSON.parse(value)
    return Array.isArray(parsed) ? parsed : []
  } catch {
    return []
  }
}

function rowToRun(row: ScheduledTaskRunRow): ScheduledTaskRun {
  return {
    id: row.id,
    taskId: row.task_id,
    status: row.status,
    scheduledAt: row.scheduled_at,
    startedAt: row.started_at,
    finishedAt: row.finished_at,
    prompt: row.prompt,
    outputExcerpt: row.output_excerpt,
    error: row.error,
    timeoutMinutes: row.timeout_minutes
  }
}

function latestRunForTask(taskId: number): ScheduledTaskRun | null {
  const row = getDb()
    .prepare(
      `SELECT * FROM scheduled_task_runs
       WHERE task_id = ?
       ORDER BY scheduled_at DESC, id DESC
       LIMIT 1`
    )
    .get(taskId) as ScheduledTaskRunRow | undefined
  return row ? rowToRun(row) : null
}

function rowToTask(row: ScheduledTaskRow): ScheduledTask {
  return {
    id: row.id,
    projectId: row.project_id,
    targetRepo: row.target_repo,
    name: row.name,
    description: row.description,
    goal: row.goal,
    instructions: parseJsonArray(row.instructions).filter(
      (value): value is string => typeof value === 'string'
    ),
    enabled: row.enabled === 1,
    scheduleType: row.schedule_type,
    scheduleTime: row.schedule_time,
    scheduleDays: parseJsonArray(row.schedule_days).filter(
      (value): value is number => Number.isInteger(value)
    ),
    nextRunAt: row.next_run_at,
    timeoutMinutes: row.timeout_minutes,
    allowCodeChanges: row.allow_code_changes === 1,
    allowGitCommit: row.allow_git_commit === 1,
    requireTestConfirmation: row.require_test_confirmation === 1,
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    lastRun: latestRunForTask(row.id)
  }
}

function parseScheduleTime(scheduleTime: string): { hour: number; minute: number } {
  const match = /^(\d{1,2}):(\d{2})$/.exec(scheduleTime.trim())
  if (!match) return { hour: 9, minute: 0 }
  const hour = Math.min(Math.max(Number(match[1]), 0), 23)
  const minute = Math.min(Math.max(Number(match[2]), 0), 59)
  return { hour, minute }
}

function dateAtLocalTime(base: Date, scheduleTime: string): Date {
  const { hour, minute } = parseScheduleTime(scheduleTime)
  return new Date(
    base.getFullYear(),
    base.getMonth(),
    base.getDate(),
    hour,
    minute,
    0,
    0
  )
}

function parseIntervalMinutes(scheduleTime: string): number {
  const normalized = scheduleTime.trim()
  if (!/^\d+$/.test(normalized)) return 60
  const minutes = Number.parseInt(normalized, 10)
  if (!Number.isFinite(minutes) || minutes < 1) return 60
  return minutes
}

export function computeNextRunAt(input: ScheduleInput, now = Date.now()): number | null {
  const current = new Date(now)
  if (input.scheduleType === 'once') {
    const onceAt = dateAtLocalTime(current, input.scheduleTime)
    return onceAt.getTime()
  }

  if (input.scheduleType === 'daily') {
    const today = dateAtLocalTime(current, input.scheduleTime)
    if (today.getTime() > now) return today.getTime()
    const tomorrow = new Date(today)
    tomorrow.setDate(tomorrow.getDate() + 1)
    return tomorrow.getTime()
  }

  if (input.scheduleType === 'interval') {
    return now + parseIntervalMinutes(input.scheduleTime) * 60 * 1000
  }

  if (input.scheduleType === 'weekly') {
    const selectedDays = input.scheduleDays.filter((day) => day >= 0 && day <= 6)
    const days = selectedDays.length > 0 ? selectedDays : [current.getDay()]
    for (let offset = 0; offset <= 7; offset += 1) {
      const candidateBase = new Date(
        current.getFullYear(),
        current.getMonth(),
        current.getDate() + offset,
        0,
        0,
        0,
        0
      )
      if (!days.includes(candidateBase.getDay())) continue
      const candidate = dateAtLocalTime(candidateBase, input.scheduleTime)
      if (candidate.getTime() > now) return candidate.getTime()
    }
  }
  return null
}

export function listScheduledTasks(projectId: string): ScheduledTask[] {
  const rows = getDb()
    .prepare(
      `SELECT ${SCHEDULED_TASK_SELECT}
       ${SCHEDULED_TASK_JOIN}
       WHERE scheduled_tasks.project_id = ?
       ORDER BY scheduled_tasks.enabled DESC,
                scheduled_tasks.next_run_at IS NULL ASC,
                scheduled_tasks.next_run_at ASC,
                scheduled_tasks.updated_at DESC`
    )
    .all(projectId) as ScheduledTaskRow[]
  return rows.map(rowToTask)
}

export function getScheduledTask(id: number): ScheduledTask | null {
  const row = getDb()
    .prepare(
      `SELECT ${SCHEDULED_TASK_SELECT}
       ${SCHEDULED_TASK_JOIN}
       WHERE scheduled_tasks.id = ?`
    )
    .get(id) as ScheduledTaskRow | undefined
  return row ? rowToTask(row) : null
}

export function createScheduledTask(
  input: CreateScheduledTaskInput,
  now = Date.now()
): ScheduledTask {
  const nextRunAt = input.enabled
    ? computeNextRunAt(
        {
          scheduleType: input.scheduleType,
          scheduleTime: input.scheduleTime,
          scheduleDays: input.scheduleDays
        },
        now
      )
    : null
  const info = getDb()
    .prepare(
      `INSERT INTO scheduled_tasks (
        project_id, name, description, goal, instructions, enabled,
        schedule_type, schedule_time, schedule_days, next_run_at,
        timeout_minutes, allow_code_changes, allow_git_commit,
        require_test_confirmation, created_at, updated_at
      )
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      input.projectId,
      input.name,
      input.description,
      input.goal,
      JSON.stringify(input.instructions),
      input.enabled ? 1 : 0,
      input.scheduleType,
      input.scheduleTime,
      JSON.stringify(input.scheduleDays),
      nextRunAt,
      input.timeoutMinutes,
      input.allowCodeChanges ? 1 : 0,
      input.allowGitCommit ? 1 : 0,
      input.requireTestConfirmation ? 1 : 0,
      now,
      now
    )
  return getScheduledTask(Number(info.lastInsertRowid))!
}

export function updateScheduledTask(id: number, patch: UpdateScheduledTaskInput): void {
  const current = getScheduledTask(id)
  if (!current) return
  const next = {
    name: patch.name ?? current.name,
    description: patch.description ?? current.description,
    goal: patch.goal ?? current.goal,
    instructions: patch.instructions ?? current.instructions,
    enabled: patch.enabled ?? current.enabled,
    scheduleType: patch.scheduleType ?? current.scheduleType,
    scheduleTime: patch.scheduleTime ?? current.scheduleTime,
    scheduleDays: patch.scheduleDays ?? current.scheduleDays,
    timeoutMinutes: patch.timeoutMinutes ?? current.timeoutMinutes,
    allowCodeChanges: patch.allowCodeChanges ?? current.allowCodeChanges,
    allowGitCommit: patch.allowGitCommit ?? current.allowGitCommit,
    requireTestConfirmation:
      patch.requireTestConfirmation ?? current.requireTestConfirmation
  }
  const now = Date.now()
  const nextRunAt = next.enabled
    ? computeNextRunAt(
        {
          scheduleType: next.scheduleType,
          scheduleTime: next.scheduleTime,
          scheduleDays: next.scheduleDays
        },
        now
      )
    : null
  getDb()
    .prepare(
      `UPDATE scheduled_tasks SET
        name = ?, description = ?, goal = ?, instructions = ?, enabled = ?,
        schedule_type = ?, schedule_time = ?, schedule_days = ?, next_run_at = ?,
        timeout_minutes = ?, allow_code_changes = ?, allow_git_commit = ?,
        require_test_confirmation = ?, updated_at = ?
       WHERE id = ?`
    )
    .run(
      next.name,
      next.description,
      next.goal,
      JSON.stringify(next.instructions),
      next.enabled ? 1 : 0,
      next.scheduleType,
      next.scheduleTime,
      JSON.stringify(next.scheduleDays),
      nextRunAt,
      next.timeoutMinutes,
      next.allowCodeChanges ? 1 : 0,
      next.allowGitCommit ? 1 : 0,
      next.requireTestConfirmation ? 1 : 0,
      now,
      id
    )
}

export function setScheduledTaskEnabled(id: number, enabled: boolean): void {
  updateScheduledTask(id, { enabled })
}

export function deleteScheduledTask(id: number): void {
  getDb().prepare(`DELETE FROM scheduled_tasks WHERE id = ?`).run(id)
}

export function listDueScheduledTasks(projectId: string, now = Date.now()): ScheduledTask[] {
  const rows = getDb()
    .prepare(
      `SELECT ${SCHEDULED_TASK_SELECT}
       ${SCHEDULED_TASK_JOIN}
       WHERE scheduled_tasks.project_id = ?
         AND scheduled_tasks.enabled = 1
         AND scheduled_tasks.next_run_at IS NOT NULL
         AND scheduled_tasks.next_run_at <= ?
       ORDER BY scheduled_tasks.next_run_at ASC, scheduled_tasks.id ASC`
    )
    .all(projectId, now) as ScheduledTaskRow[]
  return rows.map(rowToTask)
}

export function listAllDueScheduledTasks(now = Date.now()): ScheduledTask[] {
  const rows = getDb()
    .prepare(
      `SELECT ${SCHEDULED_TASK_SELECT}
       ${SCHEDULED_TASK_JOIN}
       WHERE scheduled_tasks.enabled = 1
         AND scheduled_tasks.next_run_at IS NOT NULL
         AND scheduled_tasks.next_run_at <= ?
       ORDER BY scheduled_tasks.next_run_at ASC, scheduled_tasks.id ASC`
    )
    .all(now) as ScheduledTaskRow[]
  return rows.map(rowToTask)
}

function shouldRepairIntervalNextRun(task: ScheduledTask, now: number): boolean {
  if (!task.enabled || task.scheduleType !== 'interval') return false
  const intervalMs = parseIntervalMinutes(task.scheduleTime) * 60 * 1000
  if (task.nextRunAt === null) return true
  if (task.nextRunAt <= now) return false
  return task.nextRunAt - now > intervalMs + 60_000
}

export function repairScheduledTaskNextRunAt(now = Date.now()): number {
  const rows = getDb()
    .prepare(
      `SELECT ${SCHEDULED_TASK_SELECT}
       ${SCHEDULED_TASK_JOIN}
       WHERE scheduled_tasks.enabled = 1
         AND scheduled_tasks.schedule_type = 'interval'`
    )
    .all() as ScheduledTaskRow[]
  let repaired = 0
  const update = getDb().prepare(
    `UPDATE scheduled_tasks
     SET next_run_at = ?, updated_at = ?
     WHERE id = ?`
  )
  for (const row of rows) {
    const task = rowToTask(row)
    if (!shouldRepairIntervalNextRun(task, now)) continue
    const nextRunAt = computeNextRunAt(
      {
        scheduleType: task.scheduleType,
        scheduleTime: task.scheduleTime,
        scheduleDays: task.scheduleDays
      },
      now
    )
    update.run(nextRunAt, now, task.id)
    repaired += 1
  }
  return repaired
}

export function advanceScheduledTaskAfterQueue(task: ScheduledTask, now = Date.now()): void {
  if (task.scheduleType === 'once') {
    getDb()
      .prepare(
        `UPDATE scheduled_tasks
         SET enabled = 0, next_run_at = NULL, updated_at = ?
         WHERE id = ?`
      )
      .run(now, task.id)
    return
  }
  const nextRunAt = computeNextRunAt(
    {
      scheduleType: task.scheduleType,
      scheduleTime: task.scheduleTime,
      scheduleDays: task.scheduleDays
    },
    now
  )
  getDb()
    .prepare(
      `UPDATE scheduled_tasks
       SET next_run_at = ?, updated_at = ?
       WHERE id = ?`
    )
    .run(nextRunAt, now, task.id)
}

export function createScheduledTaskRun(input: CreateScheduledTaskRunInput): ScheduledTaskRun {
  const info = getDb()
    .prepare(
      `INSERT INTO scheduled_task_runs (
        task_id, status, scheduled_at, started_at, finished_at,
        prompt, output_excerpt, error, timeout_minutes
      )
      VALUES (?, ?, ?, NULL, NULL, ?, NULL, NULL, ?)`
    )
    .run(input.taskId, input.status, input.scheduledAt, input.prompt, input.timeoutMinutes)
  const row = getDb()
    .prepare(`SELECT * FROM scheduled_task_runs WHERE id = ?`)
    .get(info.lastInsertRowid) as ScheduledTaskRunRow
  return rowToRun(row)
}

export function updateScheduledTaskRun(id: number, patch: UpdateScheduledTaskRunInput): void {
  const current = getDb()
    .prepare(`SELECT * FROM scheduled_task_runs WHERE id = ?`)
    .get(id) as ScheduledTaskRunRow | undefined
  if (!current) return
  getDb()
    .prepare(
      `UPDATE scheduled_task_runs SET
        status = ?, started_at = ?, finished_at = ?, output_excerpt = ?, error = ?
       WHERE id = ?`
    )
    .run(
      patch.status ?? current.status,
      patch.startedAt === undefined ? current.started_at : patch.startedAt,
      patch.finishedAt === undefined ? current.finished_at : patch.finishedAt,
      patch.outputExcerpt === undefined ? current.output_excerpt : patch.outputExcerpt,
      patch.error === undefined ? current.error : patch.error,
      id
    )
}

export function cancelInterruptedScheduledTaskRuns(now = Date.now()): number {
  const result = getDb()
    .prepare(
      `UPDATE scheduled_task_runs
       SET status = 'cancelled',
           finished_at = ?,
           error = 'App restarted before scheduled task completed.'
       WHERE finished_at IS NULL
         AND status IN ('queued', 'running')`
    )
    .run(now)
  return result.changes
}
