import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  ALLOWED_RETENTION_DAYS,
  DEFAULT_HABIT_SETTINGS,
  clearHabitSettingsCache,
  habitSettingsPath,
  isKindEnabled,
  loadHabitSettings,
  mergeWithDefaults,
  saveHabitSettings,
  updateHabitSettings
} from './settings.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'habit-settings-'))
  process.env.MULTI_AI_ROOT = tempRoot
  clearHabitSettingsCache()
})

afterEach(async () => {
  if (tempRoot) {
    await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  }
  delete process.env.MULTI_AI_ROOT
  clearHabitSettingsCache()
})

describe('habit settings defaults', () => {
  it('master switch defaults to true (collection opt-out, not opt-in)', () => {
    expect(DEFAULT_HABIT_SETTINGS.enabled).toBe(true)
  })

  it('all 15 kinds default to enabled', () => {
    const flags = Object.values(DEFAULT_HABIT_SETTINGS.kinds)
    expect(flags).toHaveLength(15)
    expect(flags.every((v) => v === true)).toBe(true)
  })

  it('retention defaults to 90 days', () => {
    expect(DEFAULT_HABIT_SETTINGS.retentionDays).toBe(90)
  })

  it('managed Chrome collection and low-risk automation default to enabled', () => {
    expect(DEFAULT_HABIT_SETTINGS.collectManagedChrome).toBe(true)
    expect(DEFAULT_HABIT_SETTINGS.autoEnableLowRiskFlows).toBe(true)
    expect(DEFAULT_HABIT_SETTINGS.autoPersonalizeUi).toBe(true)
  })
})

describe('loadHabitSettings', () => {
  it('returns defaults when no settings file exists', async () => {
    const s = await loadHabitSettings()
    expect(s.enabled).toBe(true)
    expect(s.retentionDays).toBe(90)
    expect(s.firstRunNoticeShownAt).toBe(0)
    expect(s.collectManagedChrome).toBe(true)
    expect(s.autoEnableLowRiskFlows).toBe(true)
    expect(s.autoPersonalizeUi).toBe(true)
  })

  it('falls back to defaults when settings file is corrupted', async () => {
    await fs.mkdir(tempRoot!, { recursive: true })
    await fs.writeFile(habitSettingsPath(), 'not json', 'utf8')
    const s = await loadHabitSettings()
    expect(s.enabled).toBe(true)
  })

  it('round-trips through saveHabitSettings + cache invalidation', async () => {
    await saveHabitSettings({
      ...DEFAULT_HABIT_SETTINGS,
      enabled: false,
      retentionDays: 30,
      firstRunNoticeShownAt: 12345,
      collectManagedChrome: false,
      autoEnableLowRiskFlows: false,
      autoPersonalizeUi: false
    })
    clearHabitSettingsCache()
    const s = await loadHabitSettings()
    expect(s.enabled).toBe(false)
    expect(s.retentionDays).toBe(30)
    expect(s.firstRunNoticeShownAt).toBe(12345)
    expect(s.collectManagedChrome).toBe(false)
    expect(s.autoEnableLowRiskFlows).toBe(false)
    expect(s.autoPersonalizeUi).toBe(false)
  })
})

describe('updateHabitSettings', () => {
  it('merges per-kind toggles without losing defaults for other kinds', async () => {
    const updated = await updateHabitSettings({
      kinds: { pty_cmd: false }
    })
    expect(updated.kinds.pty_cmd).toBe(false)
    expect(updated.kinds.ai_prompt_main).toBe(true)
    expect(updated.kinds.diff_annotation).toBe(true)
  })

  it('rejects out-of-band retention days, keeps default instead', async () => {
    const updated = await updateHabitSettings({ retentionDays: 7 as number })
    expect(updated.retentionDays).toBe(90)
  })

  it('accepts known retention day values', async () => {
    for (const days of ALLOWED_RETENTION_DAYS) {
      const updated = await updateHabitSettings({ retentionDays: days })
      expect(updated.retentionDays).toBe(days)
    }
  })

  it('updates the new managed Chrome and automation switches independently', async () => {
    const updated = await updateHabitSettings({
      collectManagedChrome: false,
      autoEnableLowRiskFlows: false,
      autoPersonalizeUi: false
    })
    expect(updated.collectManagedChrome).toBe(false)
    expect(updated.autoEnableLowRiskFlows).toBe(false)
    expect(updated.autoPersonalizeUi).toBe(false)
    expect(updated.enabled).toBe(true)
  })
})

describe('isKindEnabled', () => {
  it('returns false for every kind when master is off', () => {
    const s = mergeWithDefaults({ enabled: false })
    expect(isKindEnabled(s, 'pty_cmd')).toBe(false)
    expect(isKindEnabled(s, 'ai_prompt_main')).toBe(false)
  })

  it('returns false when the specific kind is disabled even if master is on', () => {
    const s = mergeWithDefaults({ enabled: true, kinds: { pty_cmd: false } })
    expect(isKindEnabled(s, 'pty_cmd')).toBe(false)
    expect(isKindEnabled(s, 'ai_prompt_main')).toBe(true)
  })

  it('treats missing per-kind entries as enabled when master is on', () => {
    const s = mergeWithDefaults({ enabled: true, kinds: {} })
    expect(isKindEnabled(s, 'plan_imported')).toBe(true)
  })
})

describe('mergeWithDefaults', () => {
  it('ignores non-boolean toggle values, falling back to defaults', () => {
    const raw = JSON.parse(
      '{ "kinds": { "pty_cmd": "yes", "bogus_kind": true }, "bogusField": "oops" }'
    )
    const s = mergeWithDefaults(raw)
    expect(s.kinds.pty_cmd).toBe(true) // non-boolean -> default true
    expect((s.kinds as Record<string, unknown>).bogus_kind).toBeUndefined()
  })
})
