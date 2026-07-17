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

CREATE TABLE IF NOT EXISTS scheduled_tasks (
  id                        INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id                TEXT NOT NULL,
  name                      TEXT NOT NULL,
  description               TEXT NOT NULL DEFAULT '',
  goal                      TEXT NOT NULL,
  instructions              TEXT NOT NULL,
  enabled                   INTEGER NOT NULL DEFAULT 1,
  schedule_type             TEXT NOT NULL,
  schedule_time             TEXT NOT NULL,
  schedule_days             TEXT NOT NULL DEFAULT '[]',
  next_run_at               INTEGER,
  timeout_minutes           INTEGER NOT NULL DEFAULT 30,
  allow_code_changes        INTEGER NOT NULL DEFAULT 0,
  allow_git_commit          INTEGER NOT NULL DEFAULT 0,
  require_test_confirmation INTEGER NOT NULL DEFAULT 0,
  created_at                INTEGER NOT NULL,
  updated_at                INTEGER NOT NULL,
  FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS scheduled_task_runs (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id         INTEGER NOT NULL,
  status          TEXT NOT NULL,
  scheduled_at    INTEGER NOT NULL,
  started_at      INTEGER,
  finished_at     INTEGER,
  prompt          TEXT NOT NULL,
  output_excerpt  TEXT,
  error           TEXT,
  timeout_minutes INTEGER NOT NULL,
  FOREIGN KEY (task_id) REFERENCES scheduled_tasks(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS remote_im_messages (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id        TEXT,
  session_id        TEXT,
  provider          TEXT NOT NULL,
  remote_message_id TEXT,
  from_user_id      TEXT,
  to_user_id        TEXT,
  role              TEXT NOT NULL,
  direction         TEXT NOT NULL,
  content           TEXT NOT NULL,
  kind              TEXT NOT NULL DEFAULT 'text',
  attachment_json   TEXT,
  status            TEXT NOT NULL,
  error             TEXT,
  created_at        INTEGER NOT NULL,
  sent_to_aicli_at  INTEGER,
  sent_to_im_at     INTEGER,
  FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);
`

const INDEXES = `
CREATE INDEX IF NOT EXISTS idx_events_project ON events(project_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_artifacts_project ON artifacts(project_id, stage_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_project ON scheduled_tasks(project_id, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_due ON scheduled_tasks(project_id, enabled, next_run_at);
CREATE INDEX IF NOT EXISTS idx_scheduled_task_runs_task ON scheduled_task_runs(task_id, scheduled_at DESC);
CREATE INDEX IF NOT EXISTS idx_scheduled_task_runs_status ON scheduled_task_runs(status);
CREATE INDEX IF NOT EXISTS idx_remote_im_messages_project ON remote_im_messages(project_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_remote_im_messages_remote_id ON remote_im_messages(provider, remote_message_id);
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
  try {
    db.prepare(`ALTER TABLE ${tableName} ADD COLUMN ${columnName} ${columnDefinition}`).run()
  } catch (err) {
    // PRAGMA 预检与 ALTER 之间若发生并发/重入，列可能已被另一路径加上；只吞
    // "duplicate column name"，其它错误（表不存在/语法）仍抛出。
    const message = err instanceof Error ? err.message : String(err)
    if (!/duplicate column name/i.test(message)) throw err
  }
}

export function initDb(): Database.Database {
  if (db) return db
  db = new Database(dbPath())
  db.pragma('journal_mode = WAL')
  db.pragma('foreign_keys = ON')
  db.exec(SCHEMA)
  ensureColumn(db, 'remote_im_messages', 'kind', "TEXT NOT NULL DEFAULT 'text'")
  ensureColumn(db, 'remote_im_messages', 'attachment_json', 'TEXT')
  try {
    db.prepare('DROP INDEX IF EXISTS idx_managed_chrome_sessions_running').run()
    db.prepare('DROP TABLE IF EXISTS managed_chrome_sessions').run()
    // 已下线的「习惯监控 / Skill 管理」存储：老库里可能残留大量屏幕采样事件。
    db.prepare('DROP TABLE IF EXISTS habit_events').run()
    db.prepare('DROP TABLE IF EXISTS habit_flows').run()
    db.prepare('DROP TABLE IF EXISTS skill_candidates').run()
    db.prepare('DROP TABLE IF EXISTS skills').run()
  } catch {
    /* best-effort cleanup for retired feature storage */
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
