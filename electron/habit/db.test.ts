import Database from 'better-sqlite3'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { closeDb, getDb, initDb } from '../store/db.js'
import { dbPath } from '../store/paths.js'
import {
  ALL_HABIT_EVENT_KINDS,
  clearAllHabitEvents,
  clearAllHabitFlows,
  clearAllManagedChromeSessions,
  insertHabitEvent,
  insertHabitFlow,
  listHabitFlows,
  listManagedChromeSessions,
  listRecentHabitEvents,
  type HabitEventKind,
  type SkillCandidateStatus,
  updateHabitFlowStatus,
  upsertManagedChromeSession
} from './db.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'habit-db-'))
  process.env.MULTI_AI_ROOT = tempRoot
  await fs.mkdir(tempRoot, { recursive: true })
  closeDb()
  initDb()
})

afterEach(async () => {
  closeDb()
  delete process.env.MULTI_AI_ROOT
  if (tempRoot) {
    await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  }
})

describe('habit event kinds', () => {
  it('declares exactly 15 distinct event kinds', () => {
    expect(ALL_HABIT_EVENT_KINDS).toHaveLength(15)
    expect(new Set(ALL_HABIT_EVENT_KINDS).size).toBe(15)
  })

  it('keeps the agreed set of kinds in sync with the design doc', () => {
    expect(ALL_HABIT_EVENT_KINDS).toEqual<HabitEventKind[]>([
      'pty_cmd',
      'ai_prompt_main',
      'ai_prompt_repo',
      'diff_annotation',
      'repo_view_annotation',
      'template_used',
      'plan_imported',
      'panel_open',
      'action_triggered',
      'site_visit',
      'site_click',
      'site_input_hint',
      'tab_switch',
      'screen_window',
      'screen_frame'
    ])
  })
})

describe('skill candidate status values', () => {
  it('uses only the allowed status literals', () => {
    const allowed: SkillCandidateStatus[] = [
      'pending',
      'accepted',
      'edited',
      'discarded',
      'snoozed',
      'error'
    ]
    expect(new Set(allowed).size).toBe(6)
  })
})

describe('habit flow persistence', () => {
  afterEach(() => {
    clearAllHabitFlows()
  })

  it('inserts and lists habit flows', () => {
    insertHabitFlow({
      kind: 'site-flow',
      title: '打开构建站点',
      summary: '经常访问构建仪表盘',
      evidenceCount: 5,
      riskLevel: 'low',
      enabledByDefault: true,
      payload: { action: 'open-managed-chrome-url', url: 'https://example.test/build' }
    })

    const flows = listHabitFlows()
    expect(flows).toHaveLength(1)
    expect(flows[0].kind).toBe('site-flow')
    expect(flows[0].risk_level).toBe('low')
    expect(flows[0].status).toBe('active')
    expect(JSON.parse(flows[0].payload)).toEqual({
      action: 'open-managed-chrome-url',
      url: 'https://example.test/build'
    })
  })

  it('updates a flow status from active to disabled', () => {
    const id = insertHabitFlow({
      kind: 'ui-adjustment',
      title: '隐藏模板入口',
      summary: '低频入口自动隐藏',
      evidenceCount: 4,
      riskLevel: 'low',
      enabledByDefault: true,
      payload: { action: 'hide-templates-entry' }
    })

    updateHabitFlowStatus(id, 'disabled')

    const [flow] = listHabitFlows()
    expect(flow.status).toBe('disabled')
  })
})

describe('managed Chrome session persistence', () => {
  afterEach(() => {
    clearAllManagedChromeSessions()
  })

  it('inserts and lists managed Chrome sessions', () => {
    const row = upsertManagedChromeSession({
      port: 9222,
      profileDir: 'C:/tmp/managed-profile',
      startedAt: 1_700_000_000_000,
      lastActiveAt: 1_700_000_000_500,
      running: true,
      lastActiveUrl: 'https://example.test/dashboard'
    })

    expect(row.running).toBe(1)

    const sessions = listManagedChromeSessions()
    expect(sessions).toHaveLength(1)
    expect(sessions[0].port).toBe(9222)
    expect(sessions[0].last_active_url).toBe('https://example.test/dashboard')
  })

  it('updates an existing managed Chrome session with the same port/profile', () => {
    upsertManagedChromeSession({
      port: 9333,
      profileDir: 'C:/tmp/managed-profile-2',
      startedAt: 10,
      running: true
    })

    const updated = upsertManagedChromeSession({
      port: 9333,
      profileDir: 'C:/tmp/managed-profile-2',
      startedAt: 20,
      lastActiveAt: 30,
      running: false,
      lastActiveUrl: 'https://example.test/last'
    })

    expect(updated.running).toBe(0)
    expect(updated.started_at).toBe(20)

    const sessions = listManagedChromeSessions()
    expect(sessions).toHaveLength(1)
    expect(sessions[0].running).toBe(0)
  })
})

describe('habit event source filtering', () => {
  afterEach(() => {
    clearAllHabitEvents()
  })

  it('filters recent events by source', () => {
    insertHabitEvent({
      ts: 1,
      kind: 'panel_open',
      source: 'app_ui',
      payload: { text: '打开构建面板' }
    })
    insertHabitEvent({
      ts: 2,
      kind: 'site_visit',
      source: 'managed_chrome',
      payload: { text: '访问 https://example.test/dashboard' }
    })

    const chromeOnly = listRecentHabitEvents(20, 'managed_chrome')
    expect(chromeOnly).toHaveLength(1)
    expect(chromeOnly[0].source).toBe('managed_chrome')
    expect(chromeOnly[0].kind).toBe('site_visit')
  })

  it('migrates legacy habit_events tables before creating source indexes', () => {
    closeDb()
    const legacyDb = new Database(dbPath())
    legacyDb.exec(`
      DROP TABLE IF EXISTS habit_events;
      CREATE TABLE habit_events (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        ts         INTEGER NOT NULL,
        kind       TEXT NOT NULL,
        payload    TEXT NOT NULL
      );
      INSERT INTO habit_events (ts, kind, payload)
      VALUES (1, 'panel_open', '{"text":"legacy row"}');
    `)
    legacyDb.close()

    expect(() => initDb()).not.toThrow()

    const columns = (
      getDb().prepare(`PRAGMA table_info(habit_events)`).all() as Array<{ name: string }>
    ).map((column) => column.name)
    expect(columns).toContain('source')
    expect(columns).toContain('project_id')
    expect(columns).toContain('repo_path')
    expect(columns).toContain('source_window')

    insertHabitEvent({
      ts: 2,
      kind: 'site_visit',
      source: 'managed_chrome',
      sourceWindow: 'managed-chrome',
      payload: { text: 'post-migration row' }
    })

    const recent = listRecentHabitEvents(10)
    expect(recent).toHaveLength(2)
    expect(recent[0].source).toBe('managed_chrome')
    expect(recent[1].source).toBeNull()
  })
})
