import { deleteKbDbFile, getKbDb } from './connection.js'

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

/**
 * Per-repo row shape. Note: there is no `repo_path` column — the repo is
 * implicit in which kb.db file the connection points at.
 */
export interface KbEntryRow {
  id: number
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

export function rowToEntry(row: KbEntryRow, repoPath: string): KbEntry {
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
    repoPath,
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
  const info = getKbDb(input.repoPath)
    .prepare(
      `INSERT INTO kb_entries
         (created_at, updated_at, topic, summary, evidence, importance, tier)
       VALUES (?, ?, ?, ?, ?, ?, ?)`
    )
    .run(
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

export function updateKbEntry(repoPath: string, id: number, patch: UpdateKbEntry): void {
  const db = getKbDb(repoPath)
  const existing = db
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
  db.prepare(
    `UPDATE kb_entries
       SET topic = ?, summary = ?, evidence = ?, importance = ?, tier = ?, updated_at = ?
     WHERE id = ?`
  ).run(
    next.topic,
    next.summary,
    next.evidence,
    next.importance,
    next.tier,
    next.updated_at,
    id
  )
}

export function deleteKbEntry(repoPath: string, id: number): void {
  getKbDb(repoPath).prepare(`DELETE FROM kb_entries WHERE id = ?`).run(id)
}

export function listKbEntries(
  repoPath: string,
  opts: { tier?: KbTier; limit?: number; orderBy?: 'updated' | 'importance' } = {}
): KbEntry[] {
  const orderClause =
    opts.orderBy === 'importance' ? 'importance DESC, updated_at DESC' : 'updated_at DESC'
  const limitClause = opts.limit ? `LIMIT ${Math.max(1, Math.floor(opts.limit))}` : ''
  const db = getKbDb(repoPath)
  let rows: KbEntryRow[]
  if (opts.tier) {
    rows = db
      .prepare(
        `SELECT * FROM kb_entries WHERE tier = ?
         ORDER BY ${orderClause} ${limitClause}`
      )
      .all(opts.tier) as KbEntryRow[]
  } else {
    rows = db
      .prepare(`SELECT * FROM kb_entries ORDER BY ${orderClause} ${limitClause}`)
      .all() as KbEntryRow[]
  }
  return rows.map((r) => rowToEntry(r, repoPath))
}

export function getKbEntry(repoPath: string, id: number): KbEntry | null {
  const row = getKbDb(repoPath)
    .prepare(`SELECT * FROM kb_entries WHERE id = ?`)
    .get(id) as KbEntryRow | undefined
  return row ? rowToEntry(row, repoPath) : null
}

export function findKbEntryByTopic(repoPath: string, topic: string): KbEntry | null {
  const row = getKbDb(repoPath)
    .prepare(`SELECT * FROM kb_entries WHERE topic = ? LIMIT 1`)
    .get(topic) as KbEntryRow | undefined
  return row ? rowToEntry(row, repoPath) : null
}

export function listKbTopics(repoPath: string): string[] {
  const rows = getKbDb(repoPath)
    .prepare(`SELECT DISTINCT topic FROM kb_entries ORDER BY topic ASC`)
    .all() as { topic: string }[]
  return rows.map((r) => r.topic)
}

export function countKbEntries(repoPath: string, tier?: KbTier): number {
  const db = getKbDb(repoPath)
  if (tier) {
    const row = db
      .prepare(`SELECT COUNT(*) as n FROM kb_entries WHERE tier = ?`)
      .get(tier) as { n: number }
    return row.n
  }
  const row = db.prepare(`SELECT COUNT(*) as n FROM kb_entries`).get() as { n: number }
  return row.n
}

export function touchKbAccess(repoPath: string, id: number): void {
  const now = Date.now()
  getKbDb(repoPath)
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

export function searchKb(
  repoPath: string,
  query: string,
  limit = 20
): KbSearchResult[] {
  const trimmed = query.trim()
  if (!trimmed) return []
  const db = getKbDb(repoPath)
  try {
    const rows = db
      .prepare(
        `SELECT e.*, bm25(kb_fts) AS bm
           FROM kb_fts
           JOIN kb_entries e ON e.id = kb_fts.rowid
          WHERE kb_fts MATCH ?
          ORDER BY bm ASC
          LIMIT ?`
      )
      .all(escapeFtsQuery(trimmed), limit) as (KbEntryRow & { bm: number })[]
    return rows.map((r) => ({
      entry: rowToEntry(r, repoPath),
      score: r.bm
    }))
  } catch {
    /* fall back to LIKE — happens when query has FTS syntax errors */
  }
  const like = `%${trimmed.replace(/[%_]/g, (m) => '\\' + m)}%`
  const rows = db
    .prepare(
      `SELECT * FROM kb_entries
         WHERE topic LIKE ? ESCAPE '\\' OR summary LIKE ? ESCAPE '\\'
         ORDER BY updated_at DESC
         LIMIT ?`
    )
    .all(like, like, limit) as KbEntryRow[]
  return rows.map((r) => ({ entry: rowToEntry(r, repoPath), score: 1 }))
}

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

interface InternalKbMetaRow {
  id: number
  last_summary_at: number
  last_compaction_at: number
  digest: string
}

export function getKbMeta(repoPath: string): KbMetaRow {
  const row = getKbDb(repoPath)
    .prepare(`SELECT * FROM kb_meta WHERE id = 1`)
    .get() as InternalKbMetaRow | undefined
  return {
    repo_path: repoPath,
    last_summary_at: row?.last_summary_at ?? 0,
    last_compaction_at: row?.last_compaction_at ?? 0,
    digest: row?.digest ?? ''
  }
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
  getKbDb(repoPath)
    .prepare(
      `UPDATE kb_meta SET last_summary_at = ?, last_compaction_at = ?, digest = ? WHERE id = 1`
    )
    .run(next.last_summary_at, next.last_compaction_at, next.digest)
  return next
}

/**
 * Wipes everything for a repo: deletes the per-repo kb.db file. Caller-facing
 * UI "clear" button uses this.
 */
export async function clearKbForRepo(repoPath: string): Promise<number> {
  let prior = 0
  try {
    prior = countKbEntries(repoPath)
  } catch {
    /* ignore — db may already be gone */
  }
  await deleteKbDbFile(repoPath)
  return prior
}
