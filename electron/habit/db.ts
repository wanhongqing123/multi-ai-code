import { getDb } from '../store/db.js'

export type HabitEventKind =
  | 'pty_cmd'
  | 'ai_prompt_main'
  | 'ai_prompt_repo'
  | 'diff_annotation'
  | 'repo_view_annotation'
  | 'template_used'
  | 'plan_imported'

export const ALL_HABIT_EVENT_KINDS: HabitEventKind[] = [
  'pty_cmd',
  'ai_prompt_main',
  'ai_prompt_repo',
  'diff_annotation',
  'repo_view_annotation',
  'template_used',
  'plan_imported'
]

export interface HabitEventRow {
  id: number
  ts: number
  kind: HabitEventKind
  payload: string
  project_id: string | null
  repo_path: string | null
  source_window: string | null
}

export interface InsertHabitEvent {
  ts: number
  kind: HabitEventKind
  payload: unknown
  projectId?: string
  repoPath?: string
  sourceWindow?: string
}

export function insertHabitEvent(e: InsertHabitEvent): number {
  const info = getDb()
    .prepare(
      `INSERT INTO habit_events (ts, kind, payload, project_id, repo_path, source_window)
       VALUES (?, ?, ?, ?, ?, ?)`
    )
    .run(
      e.ts,
      e.kind,
      JSON.stringify(e.payload ?? {}),
      e.projectId ?? null,
      e.repoPath ?? null,
      e.sourceWindow ?? null
    )
  return Number(info.lastInsertRowid)
}

export function listRecentHabitEvents(limit = 100): HabitEventRow[] {
  return getDb()
    .prepare(`SELECT * FROM habit_events ORDER BY ts DESC, id DESC LIMIT ?`)
    .all(limit) as HabitEventRow[]
}

export function listHabitEventsSince(sinceTs: number): HabitEventRow[] {
  return getDb()
    .prepare(`SELECT * FROM habit_events WHERE ts >= ? ORDER BY ts ASC`)
    .all(sinceTs) as HabitEventRow[]
}

export function deleteHabitEventsBefore(beforeTs: number): number {
  const info = getDb()
    .prepare(`DELETE FROM habit_events WHERE ts < ?`)
    .run(beforeTs)
  return Number(info.changes)
}

export function clearAllHabitEvents(): number {
  const info = getDb().prepare(`DELETE FROM habit_events`).run()
  return Number(info.changes)
}

export function countHabitEvents(): number {
  const row = getDb()
    .prepare(`SELECT COUNT(*) as n FROM habit_events`)
    .get() as { n: number }
  return row.n
}

// ----- Skill candidates -----

export type SkillCandidateStatus =
  | 'pending'
  | 'accepted'
  | 'edited'
  | 'discarded'
  | 'snoozed'
  | 'error'

export interface SkillCandidateRow {
  id: number
  created_at: number
  cluster_kind: string
  cluster_size: number
  source_event_ids: string
  representative_samples: string
  generated_title: string | null
  generated_body: string | null
  generated_meta: string | null
  status: SkillCandidateStatus
  reviewed_at: number | null
  snoozed_until: number | null
  error_message: string | null
}

export interface InsertSkillCandidate {
  createdAt: number
  clusterKind: string
  clusterSize: number
  sourceEventIds: number[]
  representativeSamples: string[]
  generatedTitle?: string | null
  generatedBody?: string | null
  generatedMeta?: unknown
  status?: SkillCandidateStatus
  errorMessage?: string | null
}

export function insertSkillCandidate(c: InsertSkillCandidate): number {
  const info = getDb()
    .prepare(
      `INSERT INTO skill_candidates
         (created_at, cluster_kind, cluster_size, source_event_ids,
          representative_samples, generated_title, generated_body,
          generated_meta, status, error_message)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      c.createdAt,
      c.clusterKind,
      c.clusterSize,
      JSON.stringify(c.sourceEventIds),
      JSON.stringify(c.representativeSamples),
      c.generatedTitle ?? null,
      c.generatedBody ?? null,
      c.generatedMeta != null ? JSON.stringify(c.generatedMeta) : null,
      c.status ?? 'pending',
      c.errorMessage ?? null
    )
  return Number(info.lastInsertRowid)
}

export function listSkillCandidates(opts?: {
  statuses?: SkillCandidateStatus[]
  limit?: number
}): SkillCandidateRow[] {
  const statuses = opts?.statuses
  const limit = opts?.limit ?? 200
  if (statuses && statuses.length > 0) {
    const placeholders = statuses.map(() => '?').join(',')
    return getDb()
      .prepare(
        `SELECT * FROM skill_candidates
         WHERE status IN (${placeholders})
         ORDER BY created_at DESC LIMIT ?`
      )
      .all(...statuses, limit) as SkillCandidateRow[]
  }
  return getDb()
    .prepare(
      `SELECT * FROM skill_candidates ORDER BY created_at DESC LIMIT ?`
    )
    .all(limit) as SkillCandidateRow[]
}

export function updateSkillCandidateStatus(
  id: number,
  status: SkillCandidateStatus,
  extras: { snoozedUntil?: number | null; errorMessage?: string | null } = {}
): void {
  const now = Date.now()
  getDb()
    .prepare(
      `UPDATE skill_candidates
         SET status = ?,
             reviewed_at = ?,
             snoozed_until = COALESCE(?, snoozed_until),
             error_message = COALESCE(?, error_message)
       WHERE id = ?`
    )
    .run(status, now, extras.snoozedUntil ?? null, extras.errorMessage ?? null, id)
}

export function clearAllSkillCandidates(): number {
  const info = getDb().prepare(`DELETE FROM skill_candidates`).run()
  return Number(info.changes)
}
