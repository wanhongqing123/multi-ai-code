import Database from 'better-sqlite3'
import { promises as fs } from 'fs'
import { join, resolve } from 'path'

/**
 * Per-repo KB SQLite connection manager.
 *
 * Each project gets its own `<repo>/.multi-ai-code/kb/kb.db`. That file is
 * created on first use (schema applied via CREATE … IF NOT EXISTS) and
 * cached in-process so subsequent reads / writes share one Database
 * handle. The platform shared DB at `<rootDir>/multi-ai-code.db` is
 * untouched — it still hosts projects, habit_events, etc.
 *
 * The cache key is the resolved absolute path (normalized once), so
 * differing input casings / trailing slashes don't open the same repo
 * twice.
 */

const SCHEMA = `
CREATE TABLE IF NOT EXISTS kb_entries (
  id               INTEGER PRIMARY KEY AUTOINCREMENT,
  created_at       INTEGER NOT NULL,
  updated_at       INTEGER NOT NULL,
  topic            TEXT NOT NULL,
  summary          TEXT NOT NULL,
  evidence         TEXT,
  importance       REAL NOT NULL DEFAULT 0.5,
  tier             TEXT NOT NULL DEFAULT 'hot',
  access_count     INTEGER NOT NULL DEFAULT 0,
  last_accessed_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_kb_topic   ON kb_entries(topic);
CREATE INDEX IF NOT EXISTS idx_kb_tier    ON kb_entries(tier);
CREATE INDEX IF NOT EXISTS idx_kb_updated ON kb_entries(updated_at DESC);

CREATE VIRTUAL TABLE IF NOT EXISTS kb_fts USING fts5(
  topic, summary,
  content='kb_entries', content_rowid='id'
);

CREATE TRIGGER IF NOT EXISTS kb_fts_ai AFTER INSERT ON kb_entries BEGIN
  INSERT INTO kb_fts(rowid, topic, summary) VALUES (new.id, new.topic, new.summary);
END;
CREATE TRIGGER IF NOT EXISTS kb_fts_ad AFTER DELETE ON kb_entries BEGIN
  INSERT INTO kb_fts(kb_fts, rowid, topic, summary) VALUES('delete', old.id, old.topic, old.summary);
END;
CREATE TRIGGER IF NOT EXISTS kb_fts_au AFTER UPDATE ON kb_entries BEGIN
  INSERT INTO kb_fts(kb_fts, rowid, topic, summary) VALUES('delete', old.id, old.topic, old.summary);
  INSERT INTO kb_fts(rowid, topic, summary) VALUES (new.id, new.topic, new.summary);
END;

-- kb_meta is a single-row record (id = 1) holding per-repo run state +
-- the latest digest text. Constraint guards against accidental duplicates.
CREATE TABLE IF NOT EXISTS kb_meta (
  id                 INTEGER PRIMARY KEY CHECK (id = 1),
  last_summary_at    INTEGER NOT NULL DEFAULT 0,
  last_compaction_at INTEGER NOT NULL DEFAULT 0,
  digest             TEXT    NOT NULL DEFAULT ''
);
INSERT OR IGNORE INTO kb_meta (id) VALUES (1);
`

interface CachedConn {
  db: Database.Database
  filePath: string
}

const cache = new Map<string, CachedConn>()

/** Canonical cache key — resolves trailing slashes / casing on Windows. */
function normalizeRepoPath(repoPath: string): string {
  return resolve(repoPath)
}

export function kbDbFilePath(repoPath: string): string {
  return join(normalizeRepoPath(repoPath), '.multi-ai-code', 'kb', 'kb.db')
}

/**
 * Returns (and caches) the Database for `repoPath`. Creates the parent
 * `<repo>/.multi-ai-code/kb/` directory and applies the schema on first
 * open. Subsequent calls reuse the same handle.
 *
 * Sync (rather than async) so existing CRUD callers that don't await can
 * keep their shape. The directory creation uses Node's fs sync API; this
 * runs once per repo per process lifetime, so the cost is negligible.
 */
export function getKbDb(repoPath: string): Database.Database {
  const key = normalizeRepoPath(repoPath)
  const cached = cache.get(key)
  if (cached) return cached.db

  const file = kbDbFilePath(key)
  // Synchronous mkdir via fs.mkdirSync from the node `fs` module — async
  // version would force every caller to be async, which the existing
  // CRUD API doesn't want.
  const dir = join(key, '.multi-ai-code', 'kb')
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const fsSync = require('fs') as typeof import('fs')
  fsSync.mkdirSync(dir, { recursive: true })

  const db = new Database(file)
  db.pragma('journal_mode = WAL')
  db.pragma('foreign_keys = ON')
  db.exec(SCHEMA)
  cache.set(key, { db, filePath: file })
  return db
}

/** Closes and removes a single repo's connection from the cache. */
export function closeKbDb(repoPath: string): void {
  const key = normalizeRepoPath(repoPath)
  const cached = cache.get(key)
  if (!cached) return
  try {
    cached.db.close()
  } catch {
    /* tolerate close failure */
  }
  cache.delete(key)
}

/** Closes every cached connection. Called on app quit. */
export function closeAllKbDbs(): void {
  for (const [key, cached] of cache) {
    try {
      cached.db.close()
    } catch {
      /* ignore */
    }
    cache.delete(key)
  }
}

/**
 * Deletes the entire kb.db file for a repo (used by "clear" UI action).
 * Closes the cached connection first so the file isn't locked.
 */
export async function deleteKbDbFile(repoPath: string): Promise<void> {
  closeKbDb(repoPath)
  const file = kbDbFilePath(repoPath)
  try {
    await fs.unlink(file)
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code !== 'ENOENT') throw err
  }
  // Also drop the kb/ directory if it's now empty so we don't leave a
  // stray dir; readdir will tell us if it can be removed.
  const dir = join(normalizeRepoPath(repoPath), '.multi-ai-code', 'kb')
  try {
    const remaining = await fs.readdir(dir)
    if (remaining.length === 0) await fs.rmdir(dir)
  } catch {
    /* missing dir is fine */
  }
}

/** Test-only reset. */
export function _resetKbConnectionsForTests(): void {
  for (const cached of cache.values()) {
    try {
      cached.db.close()
    } catch {
      /* ignore */
    }
  }
  cache.clear()
}
