import { describe, expect, it, vi } from 'vitest'
import { interceptTerminalRightMouseDown } from '../../../src/components/terminalClipboard.js'

describe('interceptTerminalRightMouseDown', () => {
  it('blocks right clicks before a TUI mouse tracker can consume them', () => {
    const preventDefault = vi.fn()
    const stopPropagation = vi.fn()

    expect(
      interceptTerminalRightMouseDown({
        button: 2,
        preventDefault,
        stopPropagation
      })
    ).toBe(true)
    expect(preventDefault).toHaveBeenCalledOnce()
    expect(stopPropagation).toHaveBeenCalledOnce()
  })

  it('leaves other mouse buttons available to the TUI', () => {
    const preventDefault = vi.fn()
    const stopPropagation = vi.fn()

    expect(
      interceptTerminalRightMouseDown({
        button: 0,
        preventDefault,
        stopPropagation
      })
    ).toBe(false)
    expect(preventDefault).not.toHaveBeenCalled()
    expect(stopPropagation).not.toHaveBeenCalled()
  })
})
