import { promises as fs } from 'fs'
import { join } from 'path'
import { rootDir } from './paths.js'

// 通用界面偏好（与截图快捷键分开持久化）。
export interface UiPreferences {
  // 顶栏「构建 / 运行 / 日志」三个按钮默认隐藏；用户在设置里开启后才显示。
  showDevToolbarButtons: boolean
}

export const DEFAULT_UI_PREFERENCES: UiPreferences = {
  showDevToolbarButtons: false
}

export function uiPreferencesPath(): string {
  return join(rootDir(), 'ui-preferences.json')
}

let cache: UiPreferences | null = null

export function clearUiPreferencesCache(): void {
  cache = null
}

export function mergeUiPreferences(input: Partial<UiPreferences>): UiPreferences {
  return {
    showDevToolbarButtons:
      typeof input.showDevToolbarButtons === 'boolean'
        ? input.showDevToolbarButtons
        : DEFAULT_UI_PREFERENCES.showDevToolbarButtons
  }
}

export async function loadUiPreferences(): Promise<UiPreferences> {
  if (cache) return cache

  let raw: string
  try {
    raw = await fs.readFile(uiPreferencesPath(), 'utf8')
  } catch {
    cache = { ...DEFAULT_UI_PREFERENCES }
    return cache
  }

  cache = mergeUiPreferences(safeParse(raw))
  return cache
}

export async function saveUiPreferences(next: Partial<UiPreferences>): Promise<UiPreferences> {
  const merged = mergeUiPreferences({ ...(cache ?? DEFAULT_UI_PREFERENCES), ...next })
  await fs.mkdir(rootDir(), { recursive: true })
  await fs.writeFile(uiPreferencesPath(), JSON.stringify(merged, null, 2), 'utf8')
  cache = merged
  return merged
}

function safeParse(raw: string): Partial<UiPreferences> {
  try {
    const parsed = JSON.parse(raw)
    if (parsed && typeof parsed === 'object') return parsed as Partial<UiPreferences>
  } catch {
    /* corrupted file -> defaults */
  }
  return {}
}
