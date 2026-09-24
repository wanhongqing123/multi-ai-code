import { describe, expect, it } from 'vitest'
import { beginMachineActivityPhase } from '../../../electron/remote-im/machineActivity.js'

describe('machine activity clock', () => {
  it('keeps the task clock while changing phase', () => {
    const next = beginMachineActivityPhase(
      {
        activityId: 'machine:old',
        sequence: 4,
        startedAtMs: 1_000,
        taskStartedAtMs: 500
      },
      false,
      2_000,
      () => 'machine:new'
    )

    expect(next).toEqual({
      activityId: 'machine:old',
      sequence: 4,
      startedAtMs: 2_000,
      taskStartedAtMs: 500
    })
  })

  it('starts a fresh clock for every real task_started event', () => {
    const next = beginMachineActivityPhase(
      {
        activityId: 'machine:old',
        sequence: 9,
        startedAtMs: 8_000,
        taskStartedAtMs: 1_000
      },
      true,
      10_000,
      () => 'machine:new'
    )

    expect(next).toEqual({
      activityId: 'machine:new',
      sequence: 0,
      startedAtMs: 10_000,
      taskStartedAtMs: 10_000
    })
  })
})
