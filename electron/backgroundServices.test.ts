import { describe, expect, it, vi } from 'vitest'
import { startBackgroundServices } from './backgroundServices.js'

describe('startBackgroundServices', () => {
  it('does not auto-start non-main AI CLI services by default', async () => {
    const startScreenSamplerService = vi.fn().mockResolvedValue(undefined)
    const startHabitAiScheduler = vi.fn()
    const startKbAiScheduler = vi.fn()

    await startBackgroundServices({
      startScreenSamplerService,
      startHabitAiScheduler,
      startKbAiScheduler
    })

    expect(startScreenSamplerService).toHaveBeenCalledTimes(1)
    expect(startHabitAiScheduler).not.toHaveBeenCalled()
    expect(startKbAiScheduler).not.toHaveBeenCalled()
  })

  it('starts non-main AI CLI services only when explicitly allowed', async () => {
    const startScreenSamplerService = vi.fn().mockResolvedValue(undefined)
    const startHabitAiScheduler = vi.fn()
    const startKbAiScheduler = vi.fn()

    await startBackgroundServices({
      startScreenSamplerService,
      startHabitAiScheduler,
      startKbAiScheduler,
      allowNonMainAiCliAutostart: true
    })

    expect(startScreenSamplerService).toHaveBeenCalledTimes(1)
    expect(startHabitAiScheduler).toHaveBeenCalledTimes(1)
    expect(startKbAiScheduler).toHaveBeenCalledTimes(1)
  })
})
