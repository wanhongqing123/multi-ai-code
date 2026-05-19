import {
  DEFAULT_SCREENSHOT_HOTKEY_SETTINGS,
  loadScreenshotHotkeySettings,
  mergeScreenshotHotkeySettings,
  saveScreenshotHotkeySettings,
  type ScreenshotHotkeySettings
} from './hotkeySettings.js'

export interface ShortcutRegistrar {
  register(accelerator: string, callback: () => void): boolean
  unregister(accelerator: string): void
  isRegistered(accelerator: string): boolean
}

interface ActiveRegistration {
  registrar: ShortcutRegistrar
  accelerator: string
  callback: () => void
}

interface HotkeySuccessResult {
  ok: true
  activeAccelerator: string | null
  settings: ScreenshotHotkeySettings
}

interface HotkeyErrorResult {
  ok: false
  activeAccelerator: string | null
  settings: ScreenshotHotkeySettings
  requestedSettings: ScreenshotHotkeySettings
  error: string
}

export type ScreenshotHotkeyResult = HotkeySuccessResult | HotkeyErrorResult

let activeRegistration: ActiveRegistration | null = null
let effectiveSettings: ScreenshotHotkeySettings | null = null
let applyQueue: Promise<void> = Promise.resolve()

export async function initializeScreenshotHotkey({
  registrar,
  trigger = defaultTrigger,
  load = loadScreenshotHotkeySettings
}: {
  registrar: ShortcutRegistrar
  trigger?: () => void | Promise<void>
  load?: () => Promise<ScreenshotHotkeySettings>
}): Promise<ScreenshotHotkeyResult> {
  const settings = mergeScreenshotHotkeySettings(await load())
  const conflict = ensureRegistrarOwnership(registrar, settings)
  if (conflict) return conflict
  return applyRegistration(settings, registrar, trigger)
}

export async function applyScreenshotHotkeySettings(
  next: ScreenshotHotkeySettings,
  {
    registrar,
    trigger = defaultTrigger,
    save = saveScreenshotHotkeySettings
  }: {
    registrar: ShortcutRegistrar
    trigger?: () => void | Promise<void>
    save?: (settings: ScreenshotHotkeySettings) => Promise<ScreenshotHotkeySettings>
  }
): Promise<ScreenshotHotkeyResult> {
  return runSerializedApply(async () => {
    const requestedSettings = mergeScreenshotHotkeySettings(next)
    const conflict = ensureRegistrarOwnership(registrar, requestedSettings)
    if (conflict) return conflict

    const previousRegistration = activeRegistration
    const previousSettings = effectiveSettings
    const registrationResult = applyRegistration(requestedSettings, registrar, trigger)
    if (!registrationResult.ok) {
      return registrationResult
    }

    try {
      const settings = await save(requestedSettings)
      effectiveSettings = settings
      return {
        ok: true,
        activeAccelerator: registrationResult.activeAccelerator,
        settings
      }
    } catch (error) {
      const rollback = rollbackRegistration(
        activeRegistration,
        previousRegistration,
        previousSettings,
        requestedSettings.shortcut
      )
      return {
        ok: false,
        activeAccelerator: rollback.activeAccelerator,
        settings: rollback.settings,
        requestedSettings,
        error: formatSaveFailure(error, rollback.error)
      }
    }
  })
}

export function disposeScreenshotHotkey(
  registrar: ShortcutRegistrar,
  accelerator = activeRegistration?.accelerator
): void {
  const current = activeRegistration
  if (!current || current.registrar !== registrar) return
  if (!accelerator || current.accelerator !== accelerator) return

  if (registrar.isRegistered(accelerator)) {
    registrar.unregister(accelerator)
  }
  effectiveSettings = {
    enabled: false,
    shortcut: accelerator
  }
  activeRegistration = null
}

function applyRegistration(
  settings: ScreenshotHotkeySettings,
  registrar: ShortcutRegistrar,
  trigger: () => void | Promise<void>
): ScreenshotHotkeyResult {
  const previousRegistration = activeRegistration
  const previousSettings = effectiveSettings

  if (previousRegistration && previousRegistration.registrar === registrar) {
    unregisterIfPresent(previousRegistration)
    activeRegistration = null
  }

  if (!settings.enabled) {
    effectiveSettings = settings
    return {
      ok: true,
      activeAccelerator: null,
      settings
    }
  }

  const nextRegistration: ActiveRegistration = {
    registrar,
    accelerator: settings.shortcut,
    callback: () => {
      void trigger()
    }
  }

  if (!registrar.register(nextRegistration.accelerator, nextRegistration.callback)) {
    const rollback = restoreRegistration(previousRegistration, previousSettings, settings.shortcut)
    return {
      ok: false,
      activeAccelerator: rollback.activeAccelerator,
      settings: rollback.settings,
      requestedSettings: settings,
      error: formatRegistrationFailure(settings.shortcut, rollback.error)
    }
  }

  activeRegistration = nextRegistration
  effectiveSettings = settings
  return {
    ok: true,
    activeAccelerator: nextRegistration.accelerator,
    settings
  }
}

function ensureRegistrarOwnership(
  registrar: ShortcutRegistrar,
  settings: ScreenshotHotkeySettings
): HotkeyErrorResult | null {
  if (!activeRegistration || activeRegistration.registrar === registrar) {
    return null
  }

  return {
    ok: false,
    activeAccelerator: activeRegistration.accelerator,
    settings: effectiveSettings ?? {
      enabled: true,
      shortcut: activeRegistration.accelerator
    },
    requestedSettings: settings,
    error: 'Screenshot hotkey is already managed by a different registrar'
  }
}

function rollbackRegistration(
  currentRegistration: ActiveRegistration | null,
  previousRegistration: ActiveRegistration | null,
  previousSettings: ScreenshotHotkeySettings | null,
  fallbackShortcut: string
): { activeAccelerator: string | null; settings: ScreenshotHotkeySettings; error?: string } {
  if (
    currentRegistration &&
    activeRegistration &&
    activeRegistration.registrar === currentRegistration.registrar &&
    activeRegistration.accelerator === currentRegistration.accelerator
  ) {
    unregisterIfPresent(currentRegistration)
    activeRegistration = null
  }

  return restoreRegistration(previousRegistration, previousSettings, fallbackShortcut)
}

function restoreRegistration(
  previousRegistration: ActiveRegistration | null,
  previousSettings: ScreenshotHotkeySettings | null,
  fallbackShortcut: string | undefined
): { activeAccelerator: string | null; settings: ScreenshotHotkeySettings; error?: string } {
  if (!previousRegistration) {
    activeRegistration = null
    effectiveSettings = toInactiveSettings(previousSettings, fallbackShortcut)
    return {
      activeAccelerator: null,
      settings: effectiveSettings
    }
  }

  if (
    previousRegistration.registrar.register(
      previousRegistration.accelerator,
      previousRegistration.callback
    )
  ) {
    activeRegistration = previousRegistration
    effectiveSettings = previousSettings ?? {
      enabled: true,
      shortcut: previousRegistration.accelerator
    }
    return {
      activeAccelerator: previousRegistration.accelerator,
      settings: effectiveSettings
    }
  }

  activeRegistration = null
  effectiveSettings = toInactiveSettings(previousSettings, previousRegistration.accelerator)
  return {
    activeAccelerator: null,
    settings: effectiveSettings,
    error: `Failed to restore screenshot shortcut: ${previousRegistration.accelerator}`
  }
}

function unregisterIfPresent(registration: ActiveRegistration): void {
  if (registration.registrar.isRegistered(registration.accelerator)) {
    registration.registrar.unregister(registration.accelerator)
  }
}

function formatRegistrationFailure(shortcut: string, rollbackError?: string): string {
  const base = `Failed to register screenshot shortcut: ${shortcut}`
  return rollbackError ? `${base}; rollback failed: ${rollbackError}` : base
}

function formatSaveFailure(error: unknown, rollbackError?: string): string {
  const message = error instanceof Error ? error.message : String(error)
  return rollbackError ? `${message}; rollback failed: ${rollbackError}` : message
}

async function runSerializedApply<T>(operation: () => Promise<T>): Promise<T> {
  const previous = applyQueue
  let release!: () => void
  applyQueue = new Promise<void>((resolve) => {
    release = resolve
  })

  await previous
  try {
    return await operation()
  } finally {
    release()
  }
}

function toInactiveSettings(
  previousSettings: ScreenshotHotkeySettings | null,
  fallbackShortcut = DEFAULT_SCREENSHOT_HOTKEY_SETTINGS.shortcut
): ScreenshotHotkeySettings {
  return {
    enabled: false,
    shortcut: previousSettings?.shortcut ?? fallbackShortcut
  }
}

async function defaultTrigger(): Promise<void> {
  const { beginScreenshotSession } = await import('./manager.js')
  await beginScreenshotSession()
}
