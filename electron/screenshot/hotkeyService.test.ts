import { afterEach, describe, expect, it, vi } from 'vitest'
import type { ScreenshotHotkeySettings } from './hotkeySettings.js'
import type { ScreenshotHotkeyResult } from './hotkeyService.js'
import {
  applyScreenshotHotkeySettings,
  disposeScreenshotHotkey,
  initializeScreenshotHotkey
} from './hotkeyService.js'

class FakeRegistrar {
  readonly registered = new Map<string, () => void>()
  readonly failedAccelerators = new Set<string>()

  failNextRegistration(accelerator: string): void {
    this.failedAccelerators.add(accelerator)
  }

  register(accelerator: string, callback: () => void): boolean {
    if (this.failedAccelerators.delete(accelerator)) {
      return false
    }

    this.registered.set(accelerator, callback)
    return true
  }

  unregister(accelerator: string): void {
    this.registered.delete(accelerator)
  }

  isRegistered(accelerator: string): boolean {
    return this.registered.has(accelerator)
  }
}

const registrarsToDispose = new Set<FakeRegistrar>()

afterEach(() => {
  for (const registrar of registrarsToDispose) {
    disposeScreenshotHotkey(registrar)
  }
  registrarsToDispose.clear()
})

function createRegistrar(): FakeRegistrar {
  const registrar = new FakeRegistrar()
  registrarsToDispose.add(registrar)
  return registrar
}

function createSave() {
  return vi.fn<(_: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>>().mockImplementation(
    async (settings) => settings
  )
}

function expectFailure(result: ScreenshotHotkeyResult): Extract<ScreenshotHotkeyResult, { ok: false }> {
  if (result.ok) {
    throw new Error('Expected hotkey operation to fail')
  }
  return result
}

describe('initializeScreenshotHotkey', () => {
  it('registers the saved shortcut when enabled', async () => {
    const registrar = createRegistrar()
    const trigger = vi.fn()
    const load = vi.fn<() => Promise<ScreenshotHotkeySettings>>().mockResolvedValue({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })

    const result = await initializeScreenshotHotkey({
      registrar,
      trigger,
      load
    })

    expect(load).toHaveBeenCalledOnce()
    expect(result.ok).toBe(true)
    expect(result.activeAccelerator).toBe('Alt+Shift+S')
    expect(result.settings).toEqual({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(true)

    registrar.registered.get('Alt+Shift+S')?.()
    expect(trigger).toHaveBeenCalledOnce()
  })

  it('rejects initialization from a different registrar while one owns the active shortcut', async () => {
    const owner = createRegistrar()
    const other = createRegistrar()

    await initializeScreenshotHotkey({
      registrar: owner,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })

    const result = await initializeScreenshotHotkey({
      registrar: other,
      load: async () => ({
        enabled: true,
        shortcut: 'CommandOrControl+Shift+A'
      })
    })

    const failure = expectFailure(result)
    expect(failure.error).toContain('different registrar')
    expect(failure.activeAccelerator).toBe('Alt+Shift+S')
    expect(owner.isRegistered('Alt+Shift+S')).toBe(true)
    expect(other.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })
})

describe('applyScreenshotHotkeySettings', () => {
  it('unregisters the previous shortcut and keeps the new one on success', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSave()

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    expect(result.ok).toBe(true)
    expect(result.activeAccelerator).toBe('CommandOrControl+Shift+A')
    expect(save).toHaveBeenCalledWith({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(true)
  })

  it('rolls back to the previous shortcut when registration fails', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSave()
    registrar.failNextRegistration('CommandOrControl+Shift+A')

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    const failure = expectFailure(result)
    expect(failure.error).toContain('CommandOrControl+Shift+A')
    expect(failure.activeAccelerator).toBe('Alt+Shift+S')
    expect(save).not.toHaveBeenCalled()
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(true)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('reports no active shortcut if rollback cannot restore the previous shortcut', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSave()
    registrar.failNextRegistration('CommandOrControl+Shift+A')
    registrar.failNextRegistration('Alt+Shift+S')

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    const failure = expectFailure(result)
    expect(failure.error).toContain('rollback')
    expect(failure.activeAccelerator).toBeNull()
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('supports disabling without clearing the stored accelerator', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSave()

    const result = await applyScreenshotHotkeySettings(
      { enabled: false, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    expect(result.ok).toBe(true)
    expect(result.activeAccelerator).toBeNull()
    expect(result.settings).toEqual({
      enabled: false,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(save).toHaveBeenCalledWith({
      enabled: false,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('rejects apply from a different registrar without disturbing the active shortcut', async () => {
    const owner = createRegistrar()
    const other = createRegistrar()
    await initializeScreenshotHotkey({
      registrar: owner,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSave()

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar: other, save }
    )

    const failure = expectFailure(result)
    expect(failure.error).toContain('different registrar')
    expect(save).not.toHaveBeenCalled()
    expect(owner.isRegistered('Alt+Shift+S')).toBe(true)
    expect(other.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })
})

describe('disposeScreenshotHotkey', () => {
  it('unregisters the active accelerator', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })

    disposeScreenshotHotkey(registrar)

    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
  })

  it('ignores dispose from a different registrar', async () => {
    const owner = createRegistrar()
    const other = createRegistrar()
    await initializeScreenshotHotkey({
      registrar: owner,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })

    disposeScreenshotHotkey(other)

    expect(owner.isRegistered('Alt+Shift+S')).toBe(true)
    expect(other.isRegistered('Alt+Shift+S')).toBe(false)
  })
})
