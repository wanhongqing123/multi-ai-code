import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { promises as fs } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import {
  DEFAULT_SCREENSHOT_HOTKEY_SETTINGS,
  clearScreenshotHotkeySettingsCache,
  loadScreenshotHotkeySettings,
  mergeScreenshotHotkeySettings,
  saveScreenshotHotkeySettings,
  screenshotHotkeySettingsPath
} from '../../../electron/screenshot/hotkeySettings.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'screenshot-hotkey-settings-'))
  process.env.MULTI_AI_ROOT = tempRoot
  clearScreenshotHotkeySettingsCache()
})

afterEach(async () => {
  if (tempRoot) {
    await fs.rm(tempRoot, { recursive: true, force: true })
    tempRoot = null
  }
  delete process.env.MULTI_AI_ROOT
  clearScreenshotHotkeySettingsCache()
})

describe('loadScreenshotHotkeySettings', () => {
  it('returns defaults when no settings file exists', async () => {
    const settings = await loadScreenshotHotkeySettings()
    expect(settings).toEqual(DEFAULT_SCREENSHOT_HOTKEY_SETTINGS)
  })

  it('falls back to defaults when the settings file is corrupted', async () => {
    await fs.mkdir(tempRoot!, { recursive: true })
    await fs.writeFile(screenshotHotkeySettingsPath(), 'not json', 'utf8')

    const settings = await loadScreenshotHotkeySettings()

    expect(settings).toEqual(DEFAULT_SCREENSHOT_HOTKEY_SETTINGS)
  })

  it('round-trips through save and reload', async () => {
    await saveScreenshotHotkeySettings({ enabled: false, shortcut: 'Alt+Shift+S' })
    clearScreenshotHotkeySettingsCache()

    const settings = await loadScreenshotHotkeySettings()

    expect(settings).toEqual({
      enabled: false,
      shortcut: 'Alt+Shift+S'
    })
  })
})

describe('mergeScreenshotHotkeySettings', () => {
  it('trims whitespace from a padded shortcut', () => {
    const settings = mergeScreenshotHotkeySettings({
      enabled: false,
      shortcut: '  Alt+Shift+S  '
    })

    expect(settings).toEqual({
      enabled: false,
      shortcut: 'Alt+Shift+S'
    })
  })

  it('restores the default shortcut when input is empty after trimming', () => {
    const settings = mergeScreenshotHotkeySettings({
      enabled: false,
      shortcut: '   '
    })

    expect(settings).toEqual({
      enabled: false,
      shortcut: DEFAULT_SCREENSHOT_HOTKEY_SETTINGS.shortcut
    })
  })
})
