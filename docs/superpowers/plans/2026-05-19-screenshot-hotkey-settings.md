# Screenshot Hotkey Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users configure, disable, and restore the app-wide screenshot global shortcut from the existing settings dialog, while keeping project-level AI CLI settings intact.

**Architecture:** Split the change into four seams: a screenshot-specific app-settings persistence module, a rollback-safe hotkey application service in the main process, a preload/main IPC bridge for app-wide settings, and a renderer-side settings dialog section that now owns both project-scoped AI settings and app-scoped screenshot hotkey settings. Keep screenshot capture flow in `electron/screenshot/manager.ts`; only move hotkey registration policy out of it.

**Tech Stack:** Electron 33, React 18, TypeScript, Vitest, `electron.globalShortcut`, existing `window.api` preload bridge, existing modal/styles system.

---

## File Structure

- Create: `electron/screenshot/hotkeySettings.ts`
  - App-wide screenshot shortcut persistence, defaults, cache, and merge logic.
- Create: `electron/screenshot/hotkeySettings.test.ts`
  - Unit tests for defaults, file round-trip, corrupted file fallback, and cache reset.
- Create: `electron/screenshot/hotkeyService.ts`
  - Rollback-safe “apply + optionally save” service around `globalShortcut`.
- Create: `electron/screenshot/hotkeyService.test.ts`
  - Unit tests for enable, disable, invalid/failed registration, and rollback behavior.
- Modify: `electron/screenshot/manager.ts`
  - Remove hard-coded global shortcut registration exports; keep manual screenshot trigger + `beginScreenshotSession`.
- Modify: `electron/main.ts`
  - Initialize screenshot shortcut from saved settings on boot, dispose on quit, and expose new app-settings IPC handlers.
- Modify: `electron/preload.ts`
  - Export app settings types and preload bridge methods for get/set.
- Modify: `src/components/AiSettingsDialog.tsx`
  - Rename visible title to “设置”, add app-wide screenshot shortcut section, and save both project settings + app settings.
- Create: `src/components/AiSettingsDialog.test.tsx`
  - Static render coverage for the renamed dialog and new screenshot shortcut section.
- Modify: `src/App.tsx`
  - Load app settings once, open settings even without a selected project, rename visible entry labels, and pass app settings into the dialog.
- Modify: `src/styles.css`
  - Add small layout helpers for the screenshot shortcut setting row, help text, and disabled state polish.

## Task 1: Add Screenshot Hotkey App Settings Storage

**Files:**
- Create: `electron/screenshot/hotkeySettings.ts`
- Create: `electron/screenshot/hotkeySettings.test.ts`

- [ ] **Step 1: Write the failing storage tests**

```ts
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
} from './hotkeySettings.js'

let tempRoot: string | null = null

beforeEach(async () => {
  tempRoot = await fs.mkdtemp(join(tmpdir(), 'mac-screenshot-hotkey-'))
  process.env.MULTI_AI_ROOT = tempRoot
  clearScreenshotHotkeySettingsCache()
})

afterEach(async () => {
  if (tempRoot) await fs.rm(tempRoot, { recursive: true, force: true })
  tempRoot = null
  delete process.env.MULTI_AI_ROOT
  clearScreenshotHotkeySettingsCache()
})

describe('loadScreenshotHotkeySettings', () => {
  it('returns defaults when no settings file exists', async () => {
    await expect(loadScreenshotHotkeySettings()).resolves.toEqual(
      DEFAULT_SCREENSHOT_HOTKEY_SETTINGS
    )
  })

  it('falls back to defaults when the settings file is corrupted', async () => {
    await fs.mkdir(tempRoot!, { recursive: true })
    await fs.writeFile(screenshotHotkeySettingsPath(), 'not-json', 'utf8')

    await expect(loadScreenshotHotkeySettings()).resolves.toEqual(
      DEFAULT_SCREENSHOT_HOTKEY_SETTINGS
    )
  })

  it('round-trips save + reload', async () => {
    await saveScreenshotHotkeySettings({
      enabled: false,
      shortcut: 'Alt+Shift+S'
    })
    clearScreenshotHotkeySettingsCache()

    await expect(loadScreenshotHotkeySettings()).resolves.toEqual({
      enabled: false,
      shortcut: 'Alt+Shift+S'
    })
  })
})

describe('mergeScreenshotHotkeySettings', () => {
  it('trims whitespace and restores default shortcut when input is empty', () => {
    expect(
      mergeScreenshotHotkeySettings({
        enabled: true,
        shortcut: '   '
      })
    ).toEqual(DEFAULT_SCREENSHOT_HOTKEY_SETTINGS)
  })
})
```

- [ ] **Step 2: Run the storage tests to verify they fail**

Run: `npm.cmd test -- electron/screenshot/hotkeySettings.test.ts`

Expected: FAIL with module-not-found errors for `hotkeySettings.ts`.

- [ ] **Step 3: Implement the app-wide screenshot settings module**

```ts
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

let cache: ScreenshotHotkeySettings | null = null

export function screenshotHotkeySettingsPath(): string {
  return join(rootDir(), 'screenshot-hotkey-settings.json')
}

export function clearScreenshotHotkeySettingsCache(): void {
  cache = null
}

export function mergeScreenshotHotkeySettings(
  input: Partial<ScreenshotHotkeySettings>
): ScreenshotHotkeySettings {
  const shortcut = typeof input.shortcut === 'string' ? input.shortcut.trim() : ''
  return {
    enabled:
      typeof input.enabled === 'boolean'
        ? input.enabled
        : DEFAULT_SCREENSHOT_HOTKEY_SETTINGS.enabled,
    shortcut: shortcut || DEFAULT_SCREENSHOT_HOTKEY_SETTINGS.shortcut
  }
}

export async function loadScreenshotHotkeySettings(): Promise<ScreenshotHotkeySettings> {
  if (cache) return cache
  try {
    const raw = await fs.readFile(screenshotHotkeySettingsPath(), 'utf8')
    cache = mergeScreenshotHotkeySettings(JSON.parse(raw) as Partial<ScreenshotHotkeySettings>)
  } catch {
    cache = { ...DEFAULT_SCREENSHOT_HOTKEY_SETTINGS }
  }
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
```

- [ ] **Step 4: Re-run the storage tests**

Run: `npm.cmd test -- electron/screenshot/hotkeySettings.test.ts`

Expected: PASS

- [ ] **Step 5: Commit the storage layer**

```bash
git add electron/screenshot/hotkeySettings.ts electron/screenshot/hotkeySettings.test.ts
git commit -m "feat: persist screenshot hotkey settings"
```

## Task 2: Add a Rollback-Safe Screenshot Hotkey Service

**Files:**
- Create: `electron/screenshot/hotkeyService.ts`
- Create: `electron/screenshot/hotkeyService.test.ts`
- Modify: `electron/screenshot/manager.ts`

- [ ] **Step 1: Write the failing service tests**

```ts
import { describe, expect, it, vi } from 'vitest'
import {
  applyScreenshotHotkeySettings,
  disposeScreenshotHotkey,
  initializeScreenshotHotkey,
  type ShortcutRegistrar
} from './hotkeyService.js'

function makeRegistrar(registerOk = true): ShortcutRegistrar {
  return {
    register: vi.fn(() => registerOk),
    unregister: vi.fn(),
    isRegistered: vi.fn(() => false)
  }
}

describe('initializeScreenshotHotkey', () => {
  it('registers the saved shortcut when enabled', async () => {
    const registrar = makeRegistrar(true)
    const trigger = vi.fn()

    const result = await initializeScreenshotHotkey({
      registrar,
      trigger,
      load: async () => ({ enabled: true, shortcut: 'Alt+Shift+S' })
    })

    expect(result.ok).toBe(true)
    expect(registrar.register).toHaveBeenCalledWith('Alt+Shift+S', expect.any(Function))
  })
})

describe('applyScreenshotHotkeySettings', () => {
  it('unregisters the previous shortcut and keeps the new one when save succeeds', async () => {
    const registrar = makeRegistrar(true)
    const trigger = vi.fn()

    await initializeScreenshotHotkey({
      registrar,
      trigger,
      load: async () => ({ enabled: true, shortcut: 'Alt+Shift+S' })
    })

    const save = vi.fn(async (next) => next)
    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+J' },
      { registrar, trigger, save }
    )

    expect(result.ok).toBe(true)
    expect(save).toHaveBeenCalledWith({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+J'
    })
    expect(registrar.unregister).toHaveBeenCalledWith('Alt+Shift+S')
  })

  it('rolls back to the previous shortcut when registration fails', async () => {
    const registrar = makeRegistrar(true)
    const trigger = vi.fn()

    await initializeScreenshotHotkey({
      registrar,
      trigger,
      load: async () => ({ enabled: true, shortcut: 'Alt+Shift+S' })
    })

    const failingRegistrar: ShortcutRegistrar = {
      ...registrar,
      register: vi
        .fn()
        .mockReturnValueOnce(false)
        .mockReturnValueOnce(true)
    }

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'Bad+Shortcut' },
      { registrar: failingRegistrar, trigger, save: async (next) => next }
    )

    expect(result.ok).toBe(false)
    expect(failingRegistrar.register).toHaveBeenNthCalledWith(
      2,
      'Alt+Shift+S',
      expect.any(Function)
    )
  })

  it('supports disabling the global shortcut without clearing the stored accelerator', async () => {
    const registrar = makeRegistrar(true)
    const trigger = vi.fn()
    const save = vi.fn(async (next) => next)

    const result = await applyScreenshotHotkeySettings(
      { enabled: false, shortcut: 'Alt+Shift+S' },
      { registrar, trigger, save }
    )

    expect(result.ok).toBe(true)
    expect(registrar.register).not.toHaveBeenCalled()
    expect(save).toHaveBeenCalledWith({ enabled: false, shortcut: 'Alt+Shift+S' })
  })
})

describe('disposeScreenshotHotkey', () => {
  it('unregisters the active accelerator once', () => {
    const registrar = makeRegistrar(true)
    disposeScreenshotHotkey(registrar, 'Alt+Shift+S')
    expect(registrar.unregister).toHaveBeenCalledWith('Alt+Shift+S')
  })
})
```

- [ ] **Step 2: Run the service tests to verify they fail**

Run: `npm.cmd test -- electron/screenshot/hotkeyService.test.ts`

Expected: FAIL with module-not-found errors for `hotkeyService.ts`.

- [ ] **Step 3: Implement the hotkey application service**

```ts
import { beginScreenshotSession } from './manager.js'
import {
  loadScreenshotHotkeySettings,
  saveScreenshotHotkeySettings,
  type ScreenshotHotkeySettings
} from './hotkeySettings.js'

export interface ShortcutRegistrar {
  register(accelerator: string, callback: () => void): boolean
  unregister(accelerator: string): void
  isRegistered(accelerator: string): boolean
}

let activeSettings: ScreenshotHotkeySettings | null = null

function normalize(next: ScreenshotHotkeySettings): ScreenshotHotkeySettings {
  return {
    enabled: next.enabled,
    shortcut: next.shortcut.trim()
  }
}

function registerShortcut(
  registrar: ShortcutRegistrar,
  settings: ScreenshotHotkeySettings,
  trigger: () => void
): { ok: true } | { ok: false; error: string } {
  if (!settings.enabled) return { ok: true }
  if (!settings.shortcut) return { ok: false, error: '快捷键不能为空' }
  const registered = registrar.register(settings.shortcut, trigger)
  return registered
    ? { ok: true }
    : { ok: false, error: '快捷键注册失败，请更换未被占用的组合键后重试' }
}

export async function initializeScreenshotHotkey({
  registrar,
  trigger = () => void beginScreenshotSession(),
  load = loadScreenshotHotkeySettings
}: {
  registrar: ShortcutRegistrar
  trigger?: () => void
  load?: () => Promise<ScreenshotHotkeySettings>
}): Promise<{ ok: boolean; value: ScreenshotHotkeySettings; error?: string }> {
  const settings = normalize(await load())
  const applied = registerShortcut(registrar, settings, trigger)
  if (!applied.ok) return { ok: false, value: settings, error: applied.error }
  activeSettings = settings
  return { ok: true, value: settings }
}

export async function applyScreenshotHotkeySettings(
  next: ScreenshotHotkeySettings,
  {
    registrar,
    trigger = () => void beginScreenshotSession(),
    save = saveScreenshotHotkeySettings
  }: {
    registrar: ShortcutRegistrar
    trigger?: () => void
    save?: (next: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>
  }
): Promise<{ ok: boolean; value: ScreenshotHotkeySettings; error?: string }> {
  const previous = activeSettings
  const candidate = normalize(next)

  if (previous?.enabled && previous.shortcut) registrar.unregister(previous.shortcut)
  const applied = registerShortcut(registrar, candidate, trigger)
  if (!applied.ok) {
    if (previous) registerShortcut(registrar, previous, trigger)
    return {
      ok: false,
      value: previous ?? candidate,
      error: `${applied.error}；已恢复为之前的可用快捷键`
    }
  }

  const saved = await save(candidate)
  activeSettings = saved
  return { ok: true, value: saved }
}

export function disposeScreenshotHotkey(
  registrar: ShortcutRegistrar,
  accelerator = activeSettings?.shortcut ?? null
): void {
  if (accelerator) registrar.unregister(accelerator)
  activeSettings = null
}
```

- [ ] **Step 4: Remove the hard-coded hotkey registration from `manager.ts`**

```ts
// delete from manager.ts
const HOTKEY = 'CommandOrControl+Shift+A'

export function registerScreenshotHotkey(): void { /* ... */ }

export function unregisterScreenshotHotkey(): void { /* ... */ }
```

Keep this export intact because the service will still trigger the same capture pipeline:

```ts
export async function beginScreenshotSession(): Promise<void> {
  // existing overlay/editor pipeline remains unchanged
}
```

- [ ] **Step 5: Re-run the service tests**

Run: `npm.cmd test -- electron/screenshot/hotkeyService.test.ts`

Expected: PASS

- [ ] **Step 6: Commit the hotkey service**

```bash
git add electron/screenshot/hotkeyService.ts electron/screenshot/hotkeyService.test.ts electron/screenshot/manager.ts
git commit -m "feat: add rollback-safe screenshot hotkey service"
```

## Task 3: Wire App Settings IPC, Startup Registration, and Preload Types

**Files:**
- Modify: `electron/main.ts`
- Modify: `electron/preload.ts`

- [ ] **Step 1: Write the failing preload/main-facing type expectations**

Create a renderer-facing static render test first so the new preload type is exercised from UI code:

```ts
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import AiSettingsDialog from './AiSettingsDialog.js'

describe('AiSettingsDialog app settings wiring', () => {
  it('renders with screenshot app settings passed in', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId="proj-1"
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'claude' }}
        initialAppSettings={{
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'Alt+Shift+S'
        }}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
      />
    )

    expect(markup).toContain('Alt+Shift+S')
  })
})
```

Run: `npm.cmd test -- src/components/AiSettingsDialog.test.tsx`

Expected: FAIL because the component props and preload-exported types do not include app settings yet.

- [ ] **Step 2: Add app settings types and preload bridge methods**

```ts
export interface AppSettings {
  screenshotShortcutEnabled: boolean
  screenshotShortcut: string
}

export interface Api {
  // ...
  settings: {
    getAppSettings: () => Promise<AppSettings>
    setAppSettings: (
      settings: AppSettings
    ) => Promise<{ ok: boolean; value?: AppSettings; error?: string }>
  }
}

settings: {
  getAppSettings: () =>
    ipcRenderer.invoke('settings:get-app-settings') as Promise<AppSettings>,
  setAppSettings: (settings: AppSettings) =>
    ipcRenderer.invoke('settings:set-app-settings', { settings }) as Promise<{
      ok: boolean
      value?: AppSettings
      error?: string
    }>
},
```

- [ ] **Step 3: Add main-process handlers and startup lifecycle wiring**

```ts
import { globalShortcut } from 'electron'
import {
  applyScreenshotHotkeySettings,
  disposeScreenshotHotkey,
  initializeScreenshotHotkey
} from './screenshot/hotkeyService.js'
import {
  loadScreenshotHotkeySettings
} from './screenshot/hotkeySettings.js'

ipcMain.handle('settings:get-app-settings', async () => {
  return {
    screenshotShortcutEnabled: (await loadScreenshotHotkeySettings()).enabled,
    screenshotShortcut: (await loadScreenshotHotkeySettings()).shortcut
  }
})

ipcMain.handle(
  'settings:set-app-settings',
  async (_e, { settings }: { settings: AppSettings }) => {
    const result = await applyScreenshotHotkeySettings(
      {
        enabled: settings.screenshotShortcutEnabled,
        shortcut: settings.screenshotShortcut
      },
      { registrar: globalShortcut }
    )
    return result.ok
      ? {
          ok: true as const,
          value: {
            screenshotShortcutEnabled: result.value.enabled,
            screenshotShortcut: result.value.shortcut
          }
        }
      : { ok: false as const, error: result.error }
  }
)

app.whenReady().then(async () => {
  registerScreenshotIpc()
  await initializeScreenshotHotkey({ registrar: globalShortcut })
  createWindow()
})

app.on('before-quit', () => {
  disposeScreenshotHotkey(globalShortcut)
})
```

- [ ] **Step 4: Re-run typecheck and the service/storage tests**

Run: `npm.cmd test -- electron/screenshot/hotkeySettings.test.ts electron/screenshot/hotkeyService.test.ts`

Expected: PASS

Run: `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 5: Commit the IPC + startup wiring**

```bash
git add electron/main.ts electron/preload.ts
git commit -m "feat: wire screenshot hotkey app settings"
```

## Task 4: Expand the Settings Dialog and Rename Visible “AI 设置” Entry Points

**Files:**
- Modify: `src/components/AiSettingsDialog.tsx`
- Create: `src/components/AiSettingsDialog.test.tsx`
- Modify: `src/App.tsx`
- Modify: `src/styles.css`

- [ ] **Step 1: Write the failing dialog render tests**

```ts
import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import AiSettingsDialog from './AiSettingsDialog.js'

describe('AiSettingsDialog', () => {
  it('shows the renamed dialog title and screenshot shortcut section', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId="proj-1"
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'claude' }}
        initialAppSettings={{
          screenshotShortcutEnabled: true,
          screenshotShortcut: 'CommandOrControl+Shift+A'
        }}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
      />
    )

    expect(markup).toContain('设置')
    expect(markup).toContain('截图快捷键（全局）')
    expect(markup).toContain('恢复默认')
    expect(markup).toContain('CommandOrControl+Shift+A')
  })

  it('still renders without a selected project so app-level settings remain reachable', () => {
    const markup = renderToStaticMarkup(
      <AiSettingsDialog
        projectId={null}
        initial={{ ai_cli: 'claude' }}
        initialRepoView={{ ai_cli: 'claude' }}
        initialAppSettings={{
          screenshotShortcutEnabled: false,
          screenshotShortcut: 'Alt+Shift+S'
        }}
        onClose={vi.fn()}
        onSaved={vi.fn()}
        onSavedRepoView={vi.fn()}
        onSavedAppSettings={vi.fn()}
      />
    )

    expect(markup).toContain('截图快捷键（全局）')
    expect(markup).toContain('选择项目后可编辑 AI CLI 配置')
  })
})
```

- [ ] **Step 2: Run the dialog tests to verify they fail**

Run: `npm.cmd test -- src/components/AiSettingsDialog.test.tsx`

Expected: FAIL because the dialog title, props, and screenshot section do not exist yet.

- [ ] **Step 3: Extend `AiSettingsDialog.tsx` to save app settings and support no-project mode**

```tsx
import { useState } from 'react'
import type { AppSettings } from '../../electron/preload.js'

export interface AiSettingsDialogProps {
  projectId: string | null
  initial: AiSettings
  initialRepoView: AiSettings
  initialAppSettings: AppSettings
  onClose: () => void
  onSaved: (next: AiSettings) => void
  onSavedRepoView: (next: AiSettings) => void
  onSavedAppSettings: (next: AppSettings) => void
}

const [screenshotShortcutEnabled, setScreenshotShortcutEnabled] = useState(
  props.initialAppSettings.screenshotShortcutEnabled
)
const [screenshotShortcut, setScreenshotShortcut] = useState(
  props.initialAppSettings.screenshotShortcut
)

const handleRestoreDefault = (): void => {
  setScreenshotShortcutEnabled(true)
  setScreenshotShortcut('CommandOrControl+Shift+A')
}

const handleSave = async (): Promise<void> => {
  setSaving(true)
  setError(null)

  const nextApp = {
    screenshotShortcutEnabled,
    screenshotShortcut: screenshotShortcut.trim()
  }

  try {
    if (nextApp.screenshotShortcutEnabled && !nextApp.screenshotShortcut) {
      throw new Error('快捷键不能为空')
    }

    const appRes = await window.api.settings.setAppSettings(nextApp)
    if (!appRes.ok || !appRes.value) {
      throw new Error(appRes.error ?? 'save app settings failed')
    }

    if (props.projectId) {
      const nextMain = fromForm(aiCli, command, argsText, envText)
      const nextRepoView = fromForm(repoAiCli, repoCommand, repoArgsText, repoEnvText)
      const [mainRes, repoRes] = await Promise.all([
        window.api.project.setAiSettings(props.projectId, nextMain),
        window.api.project.setRepoViewAiSettings(props.projectId, nextRepoView)
      ])
      if (!mainRes.ok) throw new Error(mainRes.error ?? 'save main settings failed')
      if (!repoRes.ok) throw new Error(repoRes.error ?? 'save repo-view settings failed')
      props.onSaved(nextMain)
      props.onSavedRepoView(nextRepoView)
    }

    props.onSavedAppSettings(appRes.value)
    props.onClose()
  } catch (e) {
    setError(e instanceof Error ? e.message : String(e))
  } finally {
    setSaving(false)
  }
}
```

Add the new section markup:

```tsx
<section className="ai-settings-card">
  <div className="ai-settings-title">截图快捷键（全局）</div>
  <label className="ai-settings-inline-toggle">
    <input
      type="checkbox"
      checked={screenshotShortcutEnabled}
      onChange={(e) => setScreenshotShortcutEnabled(e.target.checked)}
    />
    <span>启用全局截图快捷键</span>
  </label>
  <label>
    快捷键
    <input
      type="text"
      value={screenshotShortcut}
      onChange={(e) => setScreenshotShortcut(e.target.value)}
      disabled={!screenshotShortcutEnabled}
      placeholder="CommandOrControl+Shift+A"
    />
  </label>
  <div className="ai-settings-inline-row">
    <button className="drawer-btn secondary" type="button" onClick={handleRestoreDefault}>
      恢复默认
    </button>
    <span className="ai-settings-help">
      默认：Ctrl/Cmd + Shift + A · 示例：CommandOrControl+Shift+A
    </span>
  </div>
</section>
```

Rename the dialog title:

```tsx
<h3>设置</h3>
```

When `projectId` is null, render a small note instead of the AI CLI section:

```tsx
{props.projectId ? (
  <SettingsSection /* existing AI CLI block */ />
) : (
  <section className="ai-settings-card">
    <div className="ai-settings-title">AI CLI</div>
    <div className="ai-settings-help">选择项目后可编辑 AI CLI 配置</div>
  </section>
)}
```

- [ ] **Step 4: Load app settings in `App.tsx`, rename visible labels, and keep settings accessible without a project**

```tsx
import type { AppSettings } from '../electron/preload.js'

const [appSettings, setAppSettings] = useState<AppSettings>({
  screenshotShortcutEnabled: true,
  screenshotShortcut: 'CommandOrControl+Shift+A'
})

useEffect(() => {
  void window.api.settings.getAppSettings().then((settings) => {
    setAppSettings(settings)
  })
}, [])
```

Rename the visible entries and remove the `hasProject` gate:

```tsx
<button
  className="topbar-btn"
  onClick={() => setShowAiSettings(true)}
  title="配置 AI CLI 与全局截图快捷键"
>
  ⚙️ 设置
</button>
```

```tsx
{
  id: 'settings',
  label: '⚙️ 设置',
  keywords: 'settings ai cli screenshot shortcut',
  action: () => setShowAiSettings(true)
}
```

Pass the new props into the dialog:

```tsx
{showAiSettings && (
  <AiSettingsDialog
    projectId={currentProjectId}
    initial={aiSettings}
    initialRepoView={repoViewAiSettings}
    initialAppSettings={appSettings}
    onClose={() => setShowAiSettings(false)}
    onSaved={(next) => setAiSettings(next)}
    onSavedRepoView={(next) => setRepoViewAiSettings(next)}
    onSavedAppSettings={(next) => setAppSettings(next)}
  />
)}
```

- [ ] **Step 5: Add the minimal styles for the new setting row**

```css
.ai-settings-inline-toggle {
  display: flex;
  flex-direction: row;
  align-items: center;
  gap: var(--mac-sp-2);
}

.ai-settings-inline-row {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-3);
  flex-wrap: wrap;
}

.ai-settings-help {
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-muted);
  line-height: 1.5;
}
```

- [ ] **Step 6: Re-run the dialog tests and typecheck**

Run: `npm.cmd test -- src/components/AiSettingsDialog.test.tsx`

Expected: PASS

Run: `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 7: Commit the renderer/UI changes**

```bash
git add src/components/AiSettingsDialog.tsx src/components/AiSettingsDialog.test.tsx src/App.tsx src/styles.css
git commit -m "feat: add screenshot shortcut controls to settings"
```

## Task 5: Final Regression Verification

**Files:**
- Modify: none
- Test: `electron/screenshot/hotkeySettings.test.ts`
- Test: `electron/screenshot/hotkeyService.test.ts`
- Test: `src/components/AiSettingsDialog.test.tsx`

- [ ] **Step 1: Run the focused regression suite**

Run: `npm.cmd test -- electron/screenshot/hotkeySettings.test.ts electron/screenshot/hotkeyService.test.ts src/components/AiSettingsDialog.test.tsx`

Expected: PASS

- [ ] **Step 2: Run full typecheck**

Run: `npm.cmd run typecheck`

Expected: PASS

- [ ] **Step 3: Manually verify the settings flow**

1. Start the app with no project selected and confirm “设置” is clickable.
2. Open “设置”, confirm the screenshot shortcut section renders and AI CLI area shows the no-project hint.
3. Enable the shortcut and save `Alt+Shift+S`; press the new shortcut and confirm screenshot capture starts.
4. Re-open “设置”, disable the shortcut, save, and confirm `Alt+Shift+S` no longer triggers screenshot.
5. Re-open “设置”, click “恢复默认”, save, and confirm `Ctrl/Cmd+Shift+A` triggers screenshot again.
6. Try an invalid or occupied shortcut string, save, and confirm the dialog stays open with an error while the previous shortcut remains active.

- [ ] **Step 4: Create the final feature commit**

```bash
git add electron/screenshot/hotkeySettings.ts electron/screenshot/hotkeySettings.test.ts electron/screenshot/hotkeyService.ts electron/screenshot/hotkeyService.test.ts electron/screenshot/manager.ts electron/main.ts electron/preload.ts src/components/AiSettingsDialog.tsx src/components/AiSettingsDialog.test.tsx src/App.tsx src/styles.css
git commit -m "feat: make screenshot hotkey configurable"
```
