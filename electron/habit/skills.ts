import { getDb } from '../store/db.js'

export type SkillStepType = 'prompt' | 'wait-response'

export interface PromptStep {
  type: 'prompt'
  text: string
}

export interface WaitResponseStep {
  type: 'wait-response'
  /** Static idle window — bail out after the PTY has been silent this long. */
  timeoutMs?: number
}

export type SkillStep = PromptStep | WaitResponseStep

export type SkillSource = 'manual' | 'candidate' | 'imported'

export interface SkillRow {
  id: number
  created_at: number
  updated_at: number
  name: string
  description: string | null
  trigger: string | null
  /** Stored as JSON-encoded SkillStep[]. */
  steps: string
  source: SkillSource | null
  candidate_id: number | null
  enabled?: number | null
  last_used_at: number | null
}

export interface Skill {
  id: number
  name: string
  description: string | null
  trigger: string | null
  steps: SkillStep[]
  source: SkillSource | null
  candidateId: number | null
  enabled: boolean
  createdAt: number
  updatedAt: number
  lastUsedAt: number | null
}

export function rowToSkill(row: SkillRow): Skill {
  let steps: SkillStep[] = []
  try {
    const parsed = JSON.parse(row.steps) as unknown
    if (Array.isArray(parsed)) steps = parsed.filter(isValidStep)
  } catch {
    /* malformed steps — treat as empty so the runner reports cleanly */
  }
  return {
    id: row.id,
    name: row.name,
    description: row.description,
    trigger: row.trigger,
    steps,
    source: row.source,
    candidateId: row.candidate_id,
    enabled: row.enabled !== 0,
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    lastUsedAt: row.last_used_at
  }
}

/**
 * Defensive runtime validator. We never trust the steps column blindly: it
 * was JSON-serialized at insert and can be mutated externally (DB editor,
 * older clients), so we filter to known shapes here.
 */
export function isValidStep(value: unknown): value is SkillStep {
  if (!value || typeof value !== 'object') return false
  const v = value as { type?: unknown; text?: unknown; timeoutMs?: unknown }
  if (v.type === 'prompt') return typeof v.text === 'string' && v.text.length > 0
  if (v.type === 'wait-response')
    return v.timeoutMs === undefined || typeof v.timeoutMs === 'number'
  return false
}

export interface CreateSkillInput {
  name: string
  description?: string | null
  trigger?: string | null
  steps: SkillStep[]
  source?: SkillSource
  candidateId?: number | null
  enabled?: boolean
}

export function createSkill(input: CreateSkillInput): number {
  const now = Date.now()
  const info = getDb()
    .prepare(
      `INSERT INTO skills
         (created_at, updated_at, name, description, trigger, steps, source, candidate_id, enabled, last_used_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULL)`
    )
    .run(
      now,
      now,
      input.name,
      input.description ?? null,
      input.trigger ?? null,
      JSON.stringify(input.steps.filter(isValidStep)),
      input.source ?? 'manual',
      input.candidateId ?? null,
      input.enabled === false ? 0 : 1
    )
  return Number(info.lastInsertRowid)
}

export interface UpdateSkillInput {
  name?: string
  description?: string | null
  trigger?: string | null
  steps?: SkillStep[]
  enabled?: boolean
}

export function updateSkill(id: number, patch: UpdateSkillInput): void {
  const existing = getDb()
    .prepare(`SELECT * FROM skills WHERE id = ?`)
    .get(id) as SkillRow | undefined
  if (!existing) return
  const next: SkillRow = {
    ...existing,
    name: patch.name ?? existing.name,
    description:
      patch.description === undefined ? existing.description : patch.description,
    trigger: patch.trigger === undefined ? existing.trigger : patch.trigger,
    steps: patch.steps ? JSON.stringify(patch.steps.filter(isValidStep)) : existing.steps,
    enabled: patch.enabled === undefined ? existing.enabled : patch.enabled ? 1 : 0,
    updated_at: Date.now()
  }
  getDb()
    .prepare(
      `UPDATE skills
         SET name = ?, description = ?, trigger = ?, steps = ?, enabled = ?, updated_at = ?
       WHERE id = ?`
    )
    .run(next.name, next.description, next.trigger, next.steps, next.enabled, next.updated_at, id)
}

export function deleteSkill(id: number): void {
  getDb().prepare(`DELETE FROM skills WHERE id = ?`).run(id)
}

export interface ListSkillsOptions {
  includeDisabled?: boolean
}

export function listSkills(options: ListSkillsOptions = {}): Skill[] {
  const where = options.includeDisabled ? '' : 'WHERE enabled != 0'
  const rows = getDb()
    .prepare(
      `SELECT * FROM skills
       ${where}
       ORDER BY (last_used_at IS NULL), last_used_at DESC, updated_at DESC`
    )
    .all() as SkillRow[]
  return rows.map(rowToSkill)
}

export function getSkill(id: number): Skill | null {
  const row = getDb()
    .prepare(`SELECT * FROM skills WHERE id = ?`)
    .get(id) as SkillRow | undefined
  return row ? rowToSkill(row) : null
}

export function touchSkillLastUsed(id: number, at: number = Date.now()): void {
  getDb().prepare(`UPDATE skills SET last_used_at = ? WHERE id = ?`).run(at, id)
}

/**
 * Extracts all `{var}` placeholder names from a skill's prompt steps. Used by
 * the param-collection dialog before run, and exposed here so the renderer
 * doesn't need to duplicate the regex.
 */
export function collectSkillVariables(steps: SkillStep[]): string[] {
  const found = new Set<string>()
  for (const step of steps) {
    if (step.type !== 'prompt') continue
    const matches = step.text.matchAll(/\{([A-Za-z0-9_一-龥]+)\}/g)
    for (const m of matches) found.add(m[1])
  }
  return Array.from(found)
}

/** Substitutes `{var}` placeholders in a prompt template. Missing vars stay literal. */
export function substituteVariables(
  text: string,
  vars: Record<string, string>
): string {
  return text.replace(/\{([A-Za-z0-9_一-龥]+)\}/g, (_, key: string) =>
    Object.prototype.hasOwnProperty.call(vars, key) ? vars[key] : `{${key}}`
  )
}
