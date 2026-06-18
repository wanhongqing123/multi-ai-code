import Database from 'better-sqlite3'
import { dbPath } from './paths.js'

let db: Database.Database | null = null

const SCHEMA = `
CREATE TABLE IF NOT EXISTS projects (
  id           TEXT PRIMARY KEY,
  name         TEXT NOT NULL,
  target_repo  TEXT NOT NULL,
  current_stage INTEGER NOT NULL DEFAULT 2,
  created_at   TEXT NOT NULL,
  updated_at   TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS stages (
  project_id   TEXT NOT NULL,
  stage_id     INTEGER NOT NULL,
  status       TEXT NOT NULL DEFAULT 'idle',
  artifact     TEXT,
  verdict      TEXT,
  updated_at   TEXT NOT NULL,
  PRIMARY KEY (project_id, stage_id),
  FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS events (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id   TEXT NOT NULL,
  from_stage   INTEGER,
  to_stage     INTEGER,
  kind         TEXT NOT NULL,
  payload      TEXT,
  created_at   TEXT NOT NULL,
  FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS artifacts (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id   TEXT NOT NULL,
  stage_id     INTEGER NOT NULL,
  path         TEXT NOT NULL,
  kind         TEXT NOT NULL,
  created_at   TEXT NOT NULL,
  FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS habit_events (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  ts            INTEGER NOT NULL,
  kind          TEXT NOT NULL,
  payload       TEXT NOT NULL,
  source        TEXT,
  project_id    TEXT,
  repo_path     TEXT,
  source_window TEXT
);

CREATE TABLE IF NOT EXISTS skill_candidates (
  id                     INTEGER PRIMARY KEY AUTOINCREMENT,
  created_at             INTEGER NOT NULL,
  cluster_kind           TEXT NOT NULL,
  cluster_size           INTEGER NOT NULL,
  source_event_ids       TEXT NOT NULL,
  representative_samples TEXT NOT NULL,
  generated_title        TEXT,
  generated_body         TEXT,
  generated_meta         TEXT,
  status                 TEXT NOT NULL DEFAULT 'pending',
  reviewed_at            INTEGER,
  snoozed_until          INTEGER,
  error_message          TEXT
);

CREATE TABLE IF NOT EXISTS habit_flows (
  id                 INTEGER PRIMARY KEY AUTOINCREMENT,
  kind               TEXT NOT NULL,
  title              TEXT NOT NULL,
  summary            TEXT NOT NULL,
  evidence_count     INTEGER NOT NULL,
  risk_level         TEXT NOT NULL,
  enabled_by_default INTEGER NOT NULL DEFAULT 0,
  status             TEXT NOT NULL DEFAULT 'candidate',
  payload            TEXT NOT NULL,
  created_at         INTEGER NOT NULL,
  updated_at         INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS skills (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL,
  name          TEXT NOT NULL,
  description   TEXT,
  trigger       TEXT,
  steps         TEXT NOT NULL,
  source        TEXT,
  candidate_id  INTEGER,
  enabled       INTEGER NOT NULL DEFAULT 1,
  last_used_at  INTEGER
);
`

const INDEXES = `
CREATE INDEX IF NOT EXISTS idx_events_project ON events(project_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_artifacts_project ON artifacts(project_id, stage_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_habit_events_ts ON habit_events(ts);
CREATE INDEX IF NOT EXISTS idx_habit_events_kind ON habit_events(kind);
CREATE INDEX IF NOT EXISTS idx_habit_events_source ON habit_events(source);
CREATE INDEX IF NOT EXISTS idx_skill_candidates_status ON skill_candidates(status);
CREATE INDEX IF NOT EXISTS idx_habit_flows_status ON habit_flows(status);
CREATE INDEX IF NOT EXISTS idx_habit_flows_kind ON habit_flows(kind);
CREATE INDEX IF NOT EXISTS idx_skills_trigger ON skills(trigger);
CREATE INDEX IF NOT EXISTS idx_skills_enabled ON skills(enabled);
CREATE INDEX IF NOT EXISTS idx_skills_last_used_at ON skills(last_used_at DESC);
`

function ensureColumn(
  db: Database.Database,
  tableName: string,
  columnName: string,
  columnDefinition: string
): void {
  const columns = db
    .prepare(`PRAGMA table_info(${tableName})`)
    .all() as Array<{ name: string }>
  if (columns.some((column) => column.name === columnName)) {
    return
  }
  db.prepare(`ALTER TABLE ${tableName} ADD COLUMN ${columnName} ${columnDefinition}`).run()
}

export function initDb(): Database.Database {
  if (db) return db
  db = new Database(dbPath())
  db.pragma('journal_mode = WAL')
  db.pragma('foreign_keys = ON')
  db.exec(SCHEMA)
  ensureColumn(db, 'habit_events', 'source', 'TEXT')
  ensureColumn(db, 'habit_events', 'project_id', 'TEXT')
  ensureColumn(db, 'habit_events', 'repo_path', 'TEXT')
  ensureColumn(db, 'habit_events', 'source_window', 'TEXT')
  ensureColumn(db, 'skills', 'enabled', 'INTEGER NOT NULL DEFAULT 1')
  try {
    db.prepare('DROP INDEX IF EXISTS idx_managed_chrome_sessions_running').run()
    db.prepare('DROP TABLE IF EXISTS managed_chrome_sessions').run()
  } catch {
    /* best-effort cleanup for retired managed Chrome storage */
  }
  db.exec(INDEXES)

  // One-shot migration: single-stage architecture retires stages 2/3/4.
  // Drop any orphaned rows so UI filters and aggregates stay clean.
  try {
    db.prepare('DELETE FROM stages WHERE stage_id > 1').run()
  } catch (err) {
    // Table may not exist on fresh installs or under a different name.
  }
  try {
    db.prepare('DELETE FROM events WHERE from_stage > 1 OR to_stage > 1').run()
  } catch (err) {
    // Table may not exist on fresh installs or under a different name.
  }

  return db
}

export function getDb(): Database.Database {
  // Lazily initialize so that dev-time main-process module re-evaluations
  // (which reset the module-level `db` singleton without re-running
  // app.whenReady) don't leave the IPC handlers throwing "DB not
  // initialized". `initDb` is idempotent.
  if (!db) initDb()
  return db!
}

export function closeDb(): void {
  if (db) {
    db.close()
    db = null
  }
}

// ----- Project CRUD -----

export interface ProjectRow {
  id: string
  name: string
  target_repo: string
  current_stage: number
  created_at: string
  updated_at: string
}

export function createProject(p: {
  id: string
  name: string
  target_repo: string
}): ProjectRow {
  const now = new Date().toISOString()
  getDb()
    .prepare(
      `INSERT INTO projects (id, name, target_repo, current_stage, created_at, updated_at)
       VALUES (?, ?, ?, 2, ?, ?)`
    )
    .run(p.id, p.name, p.target_repo, now, now)

  const insertStage = getDb().prepare(
    `INSERT INTO stages (project_id, stage_id, status, updated_at) VALUES (?, ?, 'idle', ?)`
  )
  for (const stageId of [1, 2, 3, 4, 5, 6]) {
    insertStage.run(p.id, stageId, now)
  }
  return getProject(p.id)!
}

export function getProject(id: string): ProjectRow | undefined {
  return getDb().prepare(`SELECT * FROM projects WHERE id = ?`).get(id) as
    | ProjectRow
    | undefined
}

export function deleteProject(id: string): void {
  getDb().prepare(`DELETE FROM projects WHERE id = ?`).run(id)
}

export function updateProjectName(id: string, name: string): void {
  const now = new Date().toISOString()
  getDb()
    .prepare(`UPDATE projects SET name = ?, updated_at = ? WHERE id = ?`)
    .run(name, now, id)
}

export function touchProject(id: string): void {
  const now = new Date().toISOString()
  getDb().prepare(`UPDATE projects SET updated_at = ? WHERE id = ?`).run(now, id)
}

export function listProjects(): ProjectRow[] {
  return getDb()
    .prepare(`SELECT * FROM projects ORDER BY updated_at DESC`)
    .all() as ProjectRow[]
}

export function updateStageStatus(
  projectId: string,
  stageId: number,
  status: string,
  extras: { artifact?: string; verdict?: string } = {}
): void {
  const now = new Date().toISOString()
  getDb()
    .prepare(
      `UPDATE stages SET status = ?, artifact = COALESCE(?, artifact),
         verdict = COALESCE(?, verdict), updated_at = ?
       WHERE project_id = ? AND stage_id = ?`
    )
    .run(status, extras.artifact ?? null, extras.verdict ?? null, now, projectId, stageId)
}

export interface ArtifactRow {
  id: number
  project_id: string
  stage_id: number
  path: string
  kind: string
  created_at: string
}

export function recordArtifact(a: {
  project_id: string
  stage_id: number
  path: string
  kind: string
}): ArtifactRow {
  const now = new Date().toISOString()
  const info = getDb()
    .prepare(
      `INSERT INTO artifacts (project_id, stage_id, path, kind, created_at)
       VALUES (?, ?, ?, ?, ?)`
    )
    .run(a.project_id, a.stage_id, a.path, a.kind, now)
  return getDb()
    .prepare(`SELECT * FROM artifacts WHERE id = ?`)
    .get(info.lastInsertRowid) as ArtifactRow
}

/**
 * Upsert-style record: if a row with the same (project_id, stage_id, path)
 * already exists, bump its created_at + kind instead of inserting a new row.
 * Used by Stage 1 where the plan file is appended across iterations.
 */
export function recordOrTouchArtifact(a: {
  project_id: string
  stage_id: number
  path: string
  kind: string
}): ArtifactRow {
  const existing = getDb()
    .prepare(
      `SELECT * FROM artifacts WHERE project_id = ? AND stage_id = ? AND path = ?`
    )
    .get(a.project_id, a.stage_id, a.path) as ArtifactRow | undefined
  const now = new Date().toISOString()
  if (existing) {
    getDb()
      .prepare(`UPDATE artifacts SET created_at = ?, kind = ? WHERE id = ?`)
      .run(now, a.kind, existing.id)
    return { ...existing, created_at: now, kind: a.kind }
  }
  return recordArtifact(a)
}

export function listArtifacts(projectId: string, stageId?: number): ArtifactRow[] {
  const sql = stageId
    ? `SELECT * FROM artifacts WHERE project_id = ? AND stage_id = ? ORDER BY created_at DESC`
    : `SELECT * FROM artifacts WHERE project_id = ? ORDER BY stage_id ASC, created_at DESC`
  const args = stageId ? [projectId, stageId] : [projectId]
  return getDb().prepare(sql).all(...args) as ArtifactRow[]
}

export function recordEvent(e: {
  project_id: string
  from_stage?: number
  to_stage?: number
  kind: string
  payload?: unknown
}): void {
  getDb()
    .prepare(
      `INSERT INTO events (project_id, from_stage, to_stage, kind, payload, created_at)
       VALUES (?, ?, ?, ?, ?, ?)`
    )
    .run(
      e.project_id,
      e.from_stage ?? null,
      e.to_stage ?? null,
      e.kind,
      e.payload ? JSON.stringify(e.payload) : null,
      new Date().toISOString()
    )
}
