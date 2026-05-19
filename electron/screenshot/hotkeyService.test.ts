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

function createSaveFailure(message = 'save failed') {
  return vi.fn<(_: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>>().mockRejectedValue(
    new Error(message)
  )
}

function createDeferredSave() {
  let resolve: ((settings: ScreenshotHotkeySettings) => void) | null = null
  let reject: ((error: Error) => void) | null = null
  const promise = new Promise<ScreenshotHotkeySettings>((res, rej) => {
    resolve = res
    reject = rej
  })

  return {
    save: vi.fn<(_: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>>().mockImplementation(
      () => promise
    ),
    resolve(settings: ScreenshotHotkeySettings) {
      resolve?.(settings)
    },
    reject(message = 'save failed') {
      reject?.(new Error(message))
    }
  }
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
    expect(failure.settings).toEqual({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })
    expect(failure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(owner.isRegistered('Alt+Shift+S')).toBe(true)
    expect(other.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })
})

describe('applyScreenshotHotkeySettings', () => {
  it('normalizes the renderer shortcut before registration and persistence', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: false,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSave()

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: ' CommandOrControl+Shift+A ' },
      { registrar, save }
    )

    expect(result.ok).toBe(true)
    expect(result.activeAccelerator).toBe('CommandOrControl+Shift+A')
    expect(result.settings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(save).toHaveBeenCalledWith({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(true)
    expect(registrar.isRegistered(' CommandOrControl+Shift+A ')).toBe(false)
  })

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

  it('returns the previous effective settings when registration fails', async () => {
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
      { enabled: true, shortcut: ' CommandOrControl+Shift+A ' },
      { registrar, save }
    )

    const failure = expectFailure(result)
    expect(failure.error).toContain('CommandOrControl+Shift+A')
    expect(failure.activeAccelerator).toBe('Alt+Shift+S')
    expect(failure.settings).toEqual({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })
    expect(failure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(save).not.toHaveBeenCalled()
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(true)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('returns disabled effective settings if rollback cannot restore the previous shortcut', async () => {
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
    expect(failure.settings).toEqual({
      enabled: false,
      shortcut: 'Alt+Shift+S'
    })
    expect(failure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('returns the restored effective settings when save fails and rollback succeeds', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createSaveFailure()

    const result = await applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save }
    )

    const failure = expectFailure(result)
    expect(failure.error).toContain('save failed')
    expect(failure.activeAccelerator).toBe('Alt+Shift+S')
    expect(failure.settings).toEqual({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })
    expect(failure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(true)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })

  it('serializes overlapping applies so an earlier failure cannot roll back a later update', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const firstSave = createDeferredSave()
    const secondSave = createDeferredSave()

    const firstApplyPromise = applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save: firstSave.save }
    )
    const secondApplyPromise = applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'Alt+Shift+X' },
      { registrar, save: secondSave.save }
    )
    let secondSettled = false
    void secondApplyPromise.finally(() => {
      secondSettled = true
    })

    await Promise.resolve()
    expect(secondSettled).toBe(false)

    firstSave.reject()
    const firstResult = await firstApplyPromise
    expect(secondSettled).toBe(false)

    secondSave.resolve({
      enabled: true,
      shortcut: 'Alt+Shift+X'
    })
    const secondResult = await secondApplyPromise

    const firstFailure = expectFailure(firstResult)
    expect(firstFailure.settings).toEqual({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })
    expect(firstFailure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(secondResult.ok).toBe(true)
    expect(secondResult.activeAccelerator).toBe('Alt+Shift+X')
    expect(registrar.isRegistered('Alt+Shift+X')).toBe(true)
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
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
    expect(failure.settings).toEqual({
      enabled: true,
      shortcut: 'Alt+Shift+S'
    })
    expect(failure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(save).not.toHaveBeenCalled()
    expect(owner.isRegistered('Alt+Shift+S')).toBe(true)
    expect(other.isRegistered('CommandOrControl+Shift+A')).toBe(false)
  })
})

describe('disposeScreenshotHotkey', () => {
  it('is not undone by a later completion of an in-flight apply', async () => {
    const registrar = createRegistrar()
    await initializeScreenshotHotkey({
      registrar,
      load: async () => ({
        enabled: true,
        shortcut: 'Alt+Shift+S'
      })
    })
    const save = createDeferredSave()

    const applyPromise = applyScreenshotHotkeySettings(
      { enabled: true, shortcut: 'CommandOrControl+Shift+A' },
      { registrar, save: save.save }
    )

    await Promise.resolve()
    disposeScreenshotHotkey(registrar, 'CommandOrControl+Shift+A')
    save.resolve({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })

    const result = await applyPromise
    const failure = expectFailure(result)
    expect(failure.error).toContain('interrupted')
    expect(failure.activeAccelerator).toBeNull()
    expect(failure.settings).toEqual({
      enabled: false,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(failure.requestedSettings).toEqual({
      enabled: true,
      shortcut: 'CommandOrControl+Shift+A'
    })
    expect(registrar.isRegistered('CommandOrControl+Shift+A')).toBe(false)
    expect(registrar.isRegistered('Alt+Shift+S')).toBe(false)
  })

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
