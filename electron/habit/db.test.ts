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
  insertHabitEvent,
  insertHabitFlow,
  listHabitFlows,
  listRecentHabitEvents,
  type HabitEventKind,
  type SkillCandidateStatus,
  updateHabitFlowStatus
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
  it('declares exactly 11 distinct event kinds', () => {
    expect(ALL_HABIT_EVENT_KINDS).toHaveLength(11)
    expect(new Set(ALL_HABIT_EVENT_KINDS).size).toBe(11)
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
      kind: 'app-flow',
      title: 'Open Build Panel',
      summary: 'Frequently opens the build panel',
      evidenceCount: 5,
      riskLevel: 'low',
      enabledByDefault: true,
      payload: { action: 'open-panel', panelKey: 'build' }
    })

    const flows = listHabitFlows()
    expect(flows).toHaveLength(1)
    expect(flows[0].kind).toBe('app-flow')
    expect(flows[0].risk_level).toBe('low')
    expect(flows[0].status).toBe('active')
    expect(JSON.parse(flows[0].payload)).toEqual({
      action: 'open-panel',
      panelKey: 'build'
    })
  })

  it('updates a flow status from active to disabled', () => {
    const id = insertHabitFlow({
      kind: 'ui-adjustment',
      title: 'Hide Templates Entry',
      summary: 'Demote low-frequency templates entry',
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

describe('habit event source filtering', () => {
  afterEach(() => {
    clearAllHabitEvents()
  })

  it('filters recent events by source', () => {
    insertHabitEvent({
      ts: 1,
      kind: 'panel_open',
      source: 'app_ui',
      payload: { text: 'Open build panel' }
    })
    insertHabitEvent({
      ts: 2,
      kind: 'screen_window',
      payload: { text: 'QQ - team chat' }
    })

    const appOnly = listRecentHabitEvents(20, 'app_ui')
    expect(appOnly).toHaveLength(1)
    expect(appOnly[0].source).toBe('app_ui')
    expect(appOnly[0].kind).toBe('panel_open')
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
      kind: 'screen_window',
      payload: { text: 'post-migration row' }
    })

    const recent = listRecentHabitEvents(10)
    expect(recent).toHaveLength(2)
    expect(recent[0].source).toBeNull()
    expect(recent[1].source).toBeNull()
  })
})
