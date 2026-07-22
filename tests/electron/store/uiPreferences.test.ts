import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { dirname, join } from 'path'
import { tmpdir } from 'os'
import { setActiveAccount } from '../../../electron/store/paths.js'
import {
  DEFAULT_UI_PREFERENCES,
  clearUiPreferencesCache,
  loadUiPreferences,
  mergeUiPreferences,
  saveUiPreferences,
  uiPreferencesPath
} from '../../../electron/store/uiPreferences.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'ui-preferences-'))
  process.env.MULTI_AI_ROOT = tempRoot
  setActiveAccount('test-account') // rootDir() 需要绑定账号
  clearUiPreferencesCache()
})

afterEach(async () => {
  if (tempRoot) {
    await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  }
  delete process.env.MULTI_AI_ROOT
  setActiveAccount(null)
  clearUiPreferencesCache()
})

describe('loadUiPreferences', () => {
  it('defaults to hiding the dev toolbar buttons when no file exists', async () => {
    const prefs = await loadUiPreferences()
    expect(prefs).toEqual(DEFAULT_UI_PREFERENCES)
    expect(prefs.showDevToolbarButtons).toBe(false)
  })

  it('falls back to defaults when the settings file is corrupted', async () => {
    await fs.mkdir(dirname(uiPreferencesPath()), { recursive: true })
    await fs.writeFile(uiPreferencesPath(), 'not json', 'utf8')

    const prefs = await loadUiPreferences()

    expect(prefs).toEqual(DEFAULT_UI_PREFERENCES)
  })

  it('round-trips through save and reload', async () => {
    await saveUiPreferences({ showDevToolbarButtons: true })
    clearUiPreferencesCache()

    const prefs = await loadUiPreferences()

    expect(prefs).toEqual({ showDevToolbarButtons: true })
  })
})

describe('mergeUiPreferences', () => {
  it('keeps a provided boolean value', () => {
    expect(mergeUiPreferences({ showDevToolbarButtons: true })).toEqual({ showDevToolbarButtons: true })
  })

  it('falls back to the default when the value is missing or the wrong type', () => {
    expect(mergeUiPreferences({})).toEqual(DEFAULT_UI_PREFERENCES)
    expect(mergeUiPreferences({ showDevToolbarButtons: 'yes' as unknown as boolean })).toEqual(
      DEFAULT_UI_PREFERENCES
    )
  })
})
