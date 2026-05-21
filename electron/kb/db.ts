import { getDb } from '../store/db.js'

export type KbTier = 'hot' | 'warm' | 'cold' | 'pinned'

export const ALL_KB_TIERS: KbTier[] = ['hot', 'warm', 'cold', 'pinned']

export interface KbEvidence {
  /** Short commit hashes that contributed to this entry. */
  commits?: string[]
  /** Repo-relative file paths that contributed. */
  files?: string[]
  /** habit_events.id rows that contributed (user prompt provenance). */
  prompt_ids?: number[]
}

export interface KbEntryRow {
  id: number
  repo_path: string
  created_at: number
  updated_at: number
  topic: string
  summary: string
  evidence: string | null
  importance: number
  tier: KbTier
  access_count: number
  last_accessed_at: number | null
}

export interface KbEntry {
  id: number
  repoPath: string
  createdAt: number
  updatedAt: number
  topic: string
  summary: string
  evidence: KbEvidence
  importance: number
  tier: KbTier
  accessCount: number
  lastAccessedAt: number | null
}

export function rowToEntry(row: KbEntryRow): KbEntry {
  let evidence: KbEvidence = {}
  if (row.evidence) {
    try {
      const parsed = JSON.parse(row.evidence) as unknown
      if (parsed && typeof parsed === 'object') evidence = parsed as KbEvidence
    } catch {
      /* tolerate corrupt JSON, return empty evidence */
    }
  }
  return {
    id: row.id,
    repoPath: row.repo_path,
    createdAt: row.created_at,
    updatedAt: row.updated_at,
    topic: row.topic,
    summary: row.summary,
    evidence,
    importance: row.importance,
    tier: row.tier,
    accessCount: row.access_count,
    lastAccessedAt: row.last_accessed_at
  }
}

export interface InsertKbEntry {
  repoPath: string
  topic: string
  summary: string
  evidence?: KbEvidence
  importance?: number
  tier?: KbTier
}

export function insertKbEntry(input: InsertKbEntry): number {
  const now = Date.now()
  const info = getDb()
    .prepare(
      `INSERT INTO kb_entries
         (repo_path, created_at, updated_at, topic, summary, evidence, importance, tier)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
      input.repoPath,
      now,
      now,
      input.topic,
      input.summary,
      input.evidence ? JSON.stringify(input.evidence) : null,
      input.importance ?? 0.5,
      input.tier ?? 'hot'
    )
  return Number(info.lastInsertRowid)
}

export interface UpdateKbEntry {
  topic?: string
  summary?: string
  evidence?: KbEvidence
  importance?: number
  tier?: KbTier
}

export function updateKbEntry(id: number, patch: UpdateKbEntry): void {
  const existing = getDb()
    .prepare(`SELECT * FROM kb_entries WHERE id = ?`)
    .get(id) as KbEntryRow | undefined
  if (!existing) return
  const next = {
    topic: patch.topic ?? existing.topic,
    summary: patch.summary ?? existing.summary,
    evidence:
      patch.evidence !== undefined ? JSON.stringify(patch.evidence) : existing.evidence,
    importance: patch.importance ?? existing.importance,
    tier: patch.tier ?? existing.tier,
    updated_at: Date.now()
  }
  getDb()
    .prepare(
      `UPDATE kb_entries
         SET topic = ?, summary = ?, evidence = ?, importance = ?, tier = ?, updated_at = ?
       WHERE id = ?`
    )
    .run(
      next.topic,
      next.summary,
      next.evidence,
      next.importance,
      next.tier,
      next.updated_at,
      id
    )
}

export function deleteKbEntry(id: number): void {
  getDb().prepare(`DELETE FROM kb_entries WHERE id = ?`).run(id)
}

export function listKbEntries(
  repoPath: string,
  opts: { tier?: KbTier; limit?: number; orderBy?: 'updated' | 'importance' } = {}
): KbEntry[] {
  const orderClause =
    opts.orderBy === 'importance' ? 'importance DESC, updated_at DESC' : 'updated_at DESC'
  const limitClause = opts.limit ? `LIMIT ${Math.max(1, Math.floor(opts.limit))}` : ''
  let rows: KbEntryRow[]
  if (opts.tier) {
    rows = getDb()
      .prepare(
        `SELECT * FROM kb_entries WHERE repo_path = ? AND tier = ?
         ORDER BY ${orderClause} ${limitClause}`
      )
      .all(repoPath, opts.tier) as KbEntryRow[]
  } else {
    rows = getDb()
      .prepare(
        `SELECT * FROM kb_entries WHERE repo_path = ?
         ORDER BY ${orderClause} ${limitClause}`
      )
      .all(repoPath) as KbEntryRow[]
  }
  return rows.map(rowToEntry)
}

export function getKbEntry(id: number): KbEntry | null {
  const row = getDb()
    .prepare(`SELECT * FROM kb_entries WHERE id = ?`)
    .get(id) as KbEntryRow | undefined
  return row ? rowToEntry(row) : null
}

export function findKbEntryByTopic(repoPath: string, topic: string): KbEntry | null {
  const row = getDb()
    .prepare(`SELECT * FROM kb_entries WHERE repo_path = ? AND topic = ? LIMIT 1`)
    .get(repoPath, topic) as KbEntryRow | undefined
  return row ? rowToEntry(row) : null
}

export function listKbTopics(repoPath: string): string[] {
  const rows = getDb()
    .prepare(
      `SELECT DISTINCT topic FROM kb_entries WHERE repo_path = ? ORDER BY topic ASC`
    )
    .all(repoPath) as { topic: string }[]
  return rows.map((r) => r.topic)
}

export function countKbEntries(repoPath: string, tier?: KbTier): number {
  if (tier) {
    const row = getDb()
      .prepare(
        `SELECT COUNT(*) as n FROM kb_entries WHERE repo_path = ? AND tier = ?`
      )
      .get(repoPath, tier) as { n: number }
    return row.n
  }
  const row = getDb()
    .prepare(`SELECT COUNT(*) as n FROM kb_entries WHERE repo_path = ?`)
    .get(repoPath) as { n: number }
  return row.n
}

export function clearKbForRepo(repoPath: string): number {
  const info = getDb()
    .prepare(`DELETE FROM kb_entries WHERE repo_path = ?`)
    .run(repoPath)
  const metaInfo = getDb()
    .prepare(`DELETE FROM kb_meta WHERE repo_path = ?`)
    .run(repoPath)
  return Number(info.changes) + Number(metaInfo.changes)
}

export function touchKbAccess(id: number): void {
  const now = Date.now()
  getDb()
    .prepare(
      `UPDATE kb_entries SET access_count = access_count + 1, last_accessed_at = ? WHERE id = ?`
    )
    .run(now, id)
}

// ----- FTS5 search -----

export interface KbSearchResult {
  entry: KbEntry
  /** Lower = better (FTS5 bm25 score). */
  score: number
}

/**
 * Full-text search over a single repo's KB. Tries FTS5 first; falls back to a
 * LIKE query when the query string would trip FTS tokenizer rules (e.g.,
 * pure punctuation, very short, or contains an unmatched quote).
 */
export function searchKb(
  repoPath: string,
  query: string,
  limit = 20
): KbSearchResult[] {
  const trimmed = query.trim()
  if (!trimmed) return []
  try {
    const rows = getDb()
      .prepare(
        `SELECT e.*, bm25(kb_fts) AS bm
           FROM kb_fts
           JOIN kb_entries e ON e.id = kb_fts.rowid
          WHERE kb_fts MATCH ? AND e.repo_path = ?
          ORDER BY bm ASC
          LIMIT ?`
      )
      .all(escapeFtsQuery(trimmed), repoPath, limit) as (KbEntryRow & { bm: number })[]
    return rows.map((r) => ({
      entry: rowToEntry(r),
      score: r.bm
    }))
  } catch {
    /* fall back to LIKE — happens when query has FTS syntax errors */
  }
  const like = `%${trimmed.replace(/[%_]/g, (m) => '\\' + m)}%`
  const rows = getDb()
    .prepare(
      `SELECT * FROM kb_entries
         WHERE repo_path = ? AND (topic LIKE ? ESCAPE '\\' OR summary LIKE ? ESCAPE '\\')
         ORDER BY updated_at DESC
         LIMIT ?`
    )
    .all(repoPath, like, like, limit) as KbEntryRow[]
  return rows.map((r) => ({ entry: rowToEntry(r), score: 1 }))
}

/**
 * Pre-process a user-typed query for FTS5: quote each whitespace-separated
 * token so FTS treats it as a phrase (avoids "no such column"-style errors
 * when the user types `OR` or `:` or `*` etc.). Tokens whose only chars
 * are punctuation get dropped entirely.
 */
export function escapeFtsQuery(query: string): string {
  return query
    .split(/\s+/)
    .map((tok) => tok.replace(/"/g, ''))
    .filter((tok) => /\w|[\p{L}\p{N}]/u.test(tok))
    .map((tok) => `"${tok}"`)
    .join(' ')
}

// ----- kb_meta (per-repo run state + digest) -----

export interface KbMetaRow {
  repo_path: string
  last_summary_at: number
  last_compaction_at: number
  digest: string
}

export function getKbMeta(repoPath: string): KbMetaRow {
  const row = getDb()
    .prepare(`SELECT * FROM kb_meta WHERE repo_path = ?`)
    .get(repoPath) as KbMetaRow | undefined
  return (
    row ?? {
      repo_path: repoPath,
      last_summary_at: 0,
      last_compaction_at: 0,
      digest: ''
    }
  )
}

export function upsertKbMeta(
  repoPath: string,
  patch: Partial<Omit<KbMetaRow, 'repo_path'>>
): KbMetaRow {
  const current = getKbMeta(repoPath)
  const next: KbMetaRow = {
    repo_path: repoPath,
    last_summary_at: patch.last_summary_at ?? current.last_summary_at,
    last_compaction_at: patch.last_compaction_at ?? current.last_compaction_at,
    digest: patch.digest ?? current.digest
  }
  getDb()
    .prepare(
      `INSERT INTO kb_meta (repo_path, last_summary_at, last_compaction_at, digest)
       VALUES (?, ?, ?, ?)
       ON CONFLICT(repo_path) DO UPDATE SET
         last_summary_at = excluded.last_summary_at,
         last_compaction_at = excluded.last_compaction_at,
         digest = excluded.digest`
    )
    .run(next.repo_path, next.last_summary_at, next.last_compaction_at, next.digest)
  return next
}
