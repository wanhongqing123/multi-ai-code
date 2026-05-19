import { promises as fs } from 'fs'
import { join } from 'path'
import { rootDir } from '../store/paths.js'

export interface ScreenshotHotkeySettings {
  enabled: boolean
  shortcut: string
}

export const DEFAULT_SCREENSHOT_HOTKEY_SETTINGS: ScreenshotHotkeySettings = {
  enabled: true,
  shortcut: 'CommandOrControl+Shift+A'
}

export function screenshotHotkeySettingsPath(): string {
  return join(rootDir(), 'screenshot-hotkey-settings.json')
}

let cache: ScreenshotHotkeySettings | null = null

export function clearScreenshotHotkeySettingsCache(): void {
  cache = null
}

export function mergeScreenshotHotkeySettings(
  input: Partial<ScreenshotHotkeySettings>
): ScreenshotHotkeySettings {
  const shortcut = typeof input.shortcut === 'string' ? input.shortcut.trim() : ''
  return {
    enabled: typeof input.enabled === 'boolean' ? input.enabled : DEFAULT_SCREENSHOT_HOTKEY_SETTINGS.enabled,
    shortcut: shortcut || DEFAULT_SCREENSHOT_HOTKEY_SETTINGS.shortcut
  }
}

export async function loadScreenshotHotkeySettings(): Promise<ScreenshotHotkeySettings> {
  if (cache) return cache

  let raw: string
  try {
    raw = await fs.readFile(screenshotHotkeySettingsPath(), 'utf8')
  } catch {
    cache = { ...DEFAULT_SCREENSHOT_HOTKEY_SETTINGS }
    return cache
  }

  cache = mergeScreenshotHotkeySettings(safeParse(raw))
  return cache
}

export async function saveScreenshotHotkeySettings(
  next: ScreenshotHotkeySettings
): Promise<ScreenshotHotkeySettings> {
  const merged = mergeScreenshotHotkeySettings(next)
  await fs.mkdir(rootDir(), { recursive: true })
  await fs.writeFile(screenshotHotkeySettingsPath(), JSON.stringify(merged, null, 2), 'utf8')
  cache = merged
  return merged
}

function safeParse(raw: string): Partial<ScreenshotHotkeySettings> {
  try {
    const parsed = JSON.parse(raw)
    if (parsed && typeof parsed === 'object') return parsed as Partial<ScreenshotHotkeySettings>
  } catch {
    /* corrupted file -> defaults */
  }
  return {}
}
