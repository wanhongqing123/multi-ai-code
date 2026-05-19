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

interface ActiveRegistration {
  accelerator: string
  callback: () => void
}

interface HotkeyLoadResult {
  ok: true
  activeAccelerator: string | null
  settings: ScreenshotHotkeySettings
}

interface HotkeyErrorResult {
  ok: false
  activeAccelerator: string | null
  settings: ScreenshotHotkeySettings
  error: string
}

export type ScreenshotHotkeyResult = HotkeyLoadResult | HotkeyErrorResult

let activeRegistration: ActiveRegistration | null = null

export async function initializeScreenshotHotkey({
  registrar,
  trigger = defaultTrigger,
  load = loadScreenshotHotkeySettings
}: {
  registrar: ShortcutRegistrar
  trigger?: () => void | Promise<void>
  load?: () => Promise<ScreenshotHotkeySettings>
}): Promise<ScreenshotHotkeyResult> {
  const settings = await load()
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
  const previousRegistration = activeRegistration
  const registrationResult = applyRegistration(next, registrar, trigger)

  if (!registrationResult.ok) {
    return registrationResult
  }

  try {
    const settings = await save(next)
    return {
      ok: true,
      activeAccelerator: registrationResult.activeAccelerator,
      settings
    }
  } catch (error) {
    restorePreviousRegistration(registrar, previousRegistration)
    return {
      ok: false,
      activeAccelerator: previousRegistration?.accelerator ?? null,
      settings: next,
      error: (error as Error).message
    }
  }
}

export function disposeScreenshotHotkey(
  registrar: ShortcutRegistrar,
  accelerator = activeRegistration?.accelerator
): void {
  if (accelerator && registrar.isRegistered(accelerator)) {
    registrar.unregister(accelerator)
  }

  if (!activeRegistration || activeRegistration.accelerator === accelerator) {
    activeRegistration = null
  }
}

function applyRegistration(
  settings: ScreenshotHotkeySettings,
  registrar: ShortcutRegistrar,
  trigger: () => void | Promise<void>
): ScreenshotHotkeyResult {
  const previousRegistration = activeRegistration

  if (previousRegistration?.accelerator && registrar.isRegistered(previousRegistration.accelerator)) {
    registrar.unregister(previousRegistration.accelerator)
  }

  if (!settings.enabled) {
    activeRegistration = null
    return {
      ok: true,
      activeAccelerator: null,
      settings
    }
  }

  const callback = () => {
    void trigger()
  }

  if (!registrar.register(settings.shortcut, callback)) {
    restorePreviousRegistration(registrar, previousRegistration)
    return {
      ok: false,
      activeAccelerator: previousRegistration?.accelerator ?? null,
      settings,
      error: `Failed to register screenshot shortcut: ${settings.shortcut}`
    }
  }

  activeRegistration = {
    accelerator: settings.shortcut,
    callback
  }

  return {
    ok: true,
    activeAccelerator: settings.shortcut,
    settings
  }
}

function restorePreviousRegistration(
  registrar: ShortcutRegistrar,
  previousRegistration: ActiveRegistration | null
): void {
  disposeScreenshotHotkey(registrar)

  if (!previousRegistration) return

  registrar.register(previousRegistration.accelerator, previousRegistration.callback)
  activeRegistration = previousRegistration
}

async function defaultTrigger(): Promise<void> {
  const { beginScreenshotSession } = await import('./manager.js')
  await beginScreenshotSession()
}
