export interface StartBackgroundServicesOptions {
  startHabitAiScheduler: () => void
  startScreenSamplerService: () => Promise<void>
  startKbAiScheduler: () => void
  allowNonMainAiCliAutostart?: boolean
  onScreenSamplerError?: (err: unknown) => void
}

export async function startBackgroundServices(
  options: StartBackgroundServicesOptions
): Promise<void> {
  const {
    startHabitAiScheduler,
    startScreenSamplerService,
    startKbAiScheduler,
    allowNonMainAiCliAutostart = false,
    onScreenSamplerError
  } = options

  if (allowNonMainAiCliAutostart) {
    startHabitAiScheduler()
  }

  try {
    await startScreenSamplerService()
  } catch (err) {
    onScreenSamplerError?.(err)
  }

  if (allowNonMainAiCliAutostart) {
    startKbAiScheduler()
  }
}
