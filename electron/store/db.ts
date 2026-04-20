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
CREATE INDEX IF NOT EXISTS idx_events_project ON events(project_id, created_at DESC);

CREATE TABLE IF NOT EXISTS artifacts (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id   TEXT NOT NULL,
  stage_id     INTEGER NOT NULL,
  path         TEXT NOT NULL,
  kind         TEXT NOT NULL,
  created_at   TEXT NOT NULL,
  FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_artifacts_project ON artifacts(project_id, stage_id, created_at DESC);
`

export function initDb(): Database.Database {
  if (db) return db
  db = new Database(dbPath())
  db.pragma('journal_mode = WAL')
  db.pragma('foreign_keys = ON')
  db.exec(SCHEMA)
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

export interface EventRow {
  id: number
  project_id: string
  from_stage: number | null
  to_stage: number | null
  kind: string
  payload: string | null
  created_at: string
}

export function listEvents(projectId: string, limit = 500): EventRow[] {
  return getDb()
    .prepare(
      `SELECT * FROM events WHERE project_id = ? ORDER BY id DESC LIMIT ?`
    )
    .all(projectId, limit) as EventRow[]
}
