import { describe, expect, it } from 'vitest'
import { applyControlledStateUpdate } from './controlledState.js'

describe('applyControlledStateUpdate', () => {
  it('applies updater functions against the latest parent state', () => {
    let state = ['ann-1', 'ann-2', 'ann-3']

    const setState = (
      updater:
        | string[]
        | ((prev: string[]) => string[])
    ): void => {
      state = typeof updater === 'function' ? updater(state) : updater
    }

    applyControlledStateUpdate(setState, (prev) =>
      prev.filter((id) => id !== 'ann-2')
    )
    applyControlledStateUpdate(setState, (prev) => [...prev, 'ann-4'])

    expect(state).toEqual(['ann-1', 'ann-3', 'ann-4'])
  })

  it('also supports plain replacement values', () => {
    let state = ['old']

    const setState = (
      updater:
        | string[]
        | ((prev: string[]) => string[])
    ): void => {
      state = typeof updater === 'function' ? updater(state) : updater
    }

    applyControlledStateUpdate(setState, ['new'])

    expect(state).toEqual(['new'])
  })
})
