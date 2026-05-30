import { describe, expect, it, vi } from 'vitest'
import { startBackgroundServices } from './backgroundServices.js'

describe('startBackgroundServices', () => {
  it('does not auto-start non-main AI CLI services by default', async () => {
    const startScreenSamplerService = vi.fn().mockResolvedValue(undefined)
    const startHabitAiScheduler = vi.fn()

    await startBackgroundServices({
      startScreenSamplerService,
      startHabitAiScheduler
    })

    expect(startScreenSamplerService).toHaveBeenCalledTimes(1)
    expect(startHabitAiScheduler).not.toHaveBeenCalled()
  })

  it('starts only the habit AI scheduler when non-main AI CLI autostart is explicitly allowed', async () => {
    const startScreenSamplerService = vi.fn().mockResolvedValue(undefined)
    const startHabitAiScheduler = vi.fn()

    await startBackgroundServices({
      startScreenSamplerService,
      startHabitAiScheduler,
      allowNonMainAiCliAutostart: true
    })

    expect(startScreenSamplerService).toHaveBeenCalledTimes(1)
    expect(startHabitAiScheduler).toHaveBeenCalledTimes(1)
  })
})
