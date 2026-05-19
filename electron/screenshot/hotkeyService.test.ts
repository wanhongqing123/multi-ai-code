import { afterEach, describe, expect, it, vi } from 'vitest'
import type { ScreenshotHotkeySettings } from './hotkeySettings.js'
import {
  applyScreenshotHotkeySettings,
  disposeScreenshotHotkey,
  initializeScreenshotHotkey
} from './hotkeyService.js'

class FakeRegistrar {
  readonly registered = new Map<string, () => void>()
  nextRegistrationResult = true

  register(accelerator: string, callback: () => void): boolean {
    if (!this.nextRegistrationResult) {
      this.nextRegistrationResult = true
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

afterEach(() => {
  disposeScreenshotHotkey(new FakeRegistrar())
})

describe('initializeScreenshotHotkey', () => {
  it('registers the saved shortcut when enabled', async () => {
    const registrar = new FakeRegistrar()
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
    expect(result).toEqual({
      ok: true,
      activeAccelerator: 'Alt+Shift+S',
      settings: {
        enabled: true,
        shortcut: 'Alt+Shift+S'
      }
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(true)

    registrar.registered.get('Alt+Shift+S')?.()
    expect(trigger).toHaveBeenCalledOnce()
  })
})

describe('applyScreenshotHotkeySettings', () => {
  it('unregisters the previous shortcut and keeps the new one on success', async () => {
    const registrar = new FakeRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = vi.fn<(_: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>>().mockImplementation(
      async (settings) => settings
    )

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    expect(result).toEqual({
      ok: true,
      activeAccelerator: 'CommandOrControl+Shift+A',
      settings: {
        enabled: true,
        shortcut: 'CommandOrControl+Shift+A'
      }
    })
    expect(save).toHaveBeenCalledWith({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(true)
  })

  it('rolls back to the previous shortcut when registration fails', async () => {
    const registrar = new FakeRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = vi.fn<(_: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>>().mockImplementation(
      async (settings) => settings
    )
    registrar.nextRegistrationResult = false

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    expect(result.ok).toBe(false)
    expect(result.error).toContain('CommandOrControl+Shift+A')
    expect(save).not.toHaveBeenCalled()
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(true)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('supports disabling without clearing the stored accelerator', async () => {
    const registrar = new FakeRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = vi.fn<(_: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>>().mockImplementation(
      async (settings) => settings
    )

    const result = await applyScreenshotHotkeySettings(
      { enabled: false, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    expect(result).toEqual({
      ok: true,
      activeAccelerator: null,
      settings: {
        enabled: false,
        shortcut: 'CommandOrControl+Shift+A'
      }
    })
    expect(save).toHaveBeenCalledWith({
      enabled: false,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })
})

describe('disposeScreenshotHotkey', () => {
  it('unregisters the active accelerator', async () => {
    const registrar = new FakeRegistrar()
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
})
