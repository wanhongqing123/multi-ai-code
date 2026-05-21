/**
 * One-shot migration from the old shared-DB KB layout to per-repo KB files.
 *
 * Before this change, all `kb_entries` / `kb_meta` rows for every project
 * lived in `<rootDir>/multi-ai-code.db` and were filtered by a `repo_path`
 * column. After this change each repo owns its own
 * `<repo>/.multi-ai-code/kb/kb.db`.
 *
 * Called once at app startup. Safe to call repeatedly: it detects whether
 * the legacy tables still exist and exits early when they don't.
 */

import { getDb } from '../store/db.js'
import { getKbDb } from './connection.js'

interface LegacyKbEntryRow {
  id: number
  repo_path: string
  created_at: number
  updated_at: number
  topic: string
  summary: string
  evidence: string | null
  importance: number
  tier: string
  access_count: number
  last_accessed_at: number | null
}

interface LegacyKbMetaRow {
  repo_path: string
  last_summary_at: number
  last_compaction_at: number
  digest: string
}

function tableExists(db: import('better-sqlite3').Database, name: string): boolean {
  const row = db
    .prepare(`SELECT name FROM sqlite_master WHERE type='table' AND name = ?`)
    .get(name) as { name?: string } | undefined
  return !!row?.name
}

/**
 * Inspects the shared DB; if it still holds legacy KB tables, copies every
 * row into the matching per-repo kb.db, then drops the legacy tables.
 * Idempotent — second run finds no tables and exits immediately.
 */
export function migrateSharedKbToPerRepo(): {
  migrated: boolean
  entriesMoved: number
  reposTouched: number
  error?: string
} {
  let shared: import('better-sqlite3').Database
  try {
    shared = getDb()
  } catch (err) {
    return {
      migrated: false,
      entriesMoved: 0,
      reposTouched: 0,
      error: (err as Error).message
    }
  }

  if (!tableExists(shared, 'kb_entries')) {
    return { migrated: false, entriesMoved: 0, reposTouched: 0 }
  }

  let entries: LegacyKbEntryRow[] = []
  try {
    entries = shared.prepare(`SELECT * FROM kb_entries`).all() as LegacyKbEntryRow[]
  } catch {
    /* table exists but unreadable — drop and bail */
    dropLegacyKbTables(shared)
    return { migrated: false, entriesMoved: 0, reposTouched: 0 }
  }

  const repos = new Set<string>()
  for (const row of entries) {
    if (!row.repo_path) continue
    repos.add(row.repo_path)
    try {
      const db = getKbDb(row.repo_path)
      db.prepare(
        `INSERT INTO kb_entries
           (created_at, updated_at, topic, summary, evidence, importance, tier, access_count, last_accessed_at)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
      ).run(
        row.created_at,
        row.updated_at,
        row.topic,
        row.summary,
        row.evidence,
        row.importance,
        row.tier,
        row.access_count,
        row.last_accessed_at
      )
    } catch {
      /* a single bad row shouldn't kill the migration */
    }
  }

  // Bring meta over too (digest is the most-valuable bit for the user).
  if (tableExists(shared, 'kb_meta')) {
    try {
      const metaRows = shared
        .prepare(`SELECT * FROM kb_meta`)
        .all() as LegacyKbMetaRow[]
      for (const m of metaRows) {
        if (!m.repo_path) continue
        repos.add(m.repo_path)
        try {
          const db = getKbDb(m.repo_path)
          db.prepare(
            `UPDATE kb_meta
               SET last_summary_at = ?, last_compaction_at = ?, digest = ?
             WHERE id = 1`
          ).run(m.last_summary_at, m.last_compaction_at, m.digest)
        } catch {
          /* swallow per-repo meta failures */
        }
      }
    } catch {
      /* tolerate */
    }
  }

  dropLegacyKbTables(shared)
  return {
    migrated: true,
    entriesMoved: entries.length,
    reposTouched: repos.size
  }
}

function dropLegacyKbTables(shared: import('better-sqlite3').Database): void {
  // Triggers reference kb_fts, drop them first to satisfy FK / ordering.
  for (const stmt of [
    'DROP TRIGGER IF EXISTS kb_fts_ai',
    'DROP TRIGGER IF EXISTS kb_fts_au',
    'DROP TRIGGER IF EXISTS kb_fts_ad',
    'DROP TABLE IF EXISTS kb_fts',
    'DROP TABLE IF EXISTS kb_entries',
    'DROP TABLE IF EXISTS kb_meta'
  ]) {
    try {
      shared.prepare(stmt).run()
    } catch {
      /* drop failures are non-fatal */
    }
  }
}
