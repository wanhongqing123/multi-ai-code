import { describe, expect, it, vi } from 'vitest'
import {
  decodeOsc52ClipboardText,
  interceptTerminalRightMouseEvent,
  installOsc52SelectionCapture,
  tuiOwnsRightClickCopy
} from '../../../src/components/terminalClipboard.js'

describe('interceptTerminalRightMouseEvent', () => {
  it('blocks right mouse events before a TUI mouse tracker can consume them', () => {
    const preventDefault = vi.fn()
    const stopPropagation = vi.fn()

    expect(
      interceptTerminalRightMouseEvent({
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
      interceptTerminalRightMouseEvent({
        button: 0,
        preventDefault,
        stopPropagation
      })
    ).toBe(false)
    expect(preventDefault).not.toHaveBeenCalled()
    expect(stopPropagation).not.toHaveBeenCalled()
  })

  it('forwards the right button to TUIs that copy on right click', () => {
    const preventDefault = vi.fn()
    const stopPropagation = vi.fn()

    // Swallowing it would strand the selection: xterm has none of its own
    // under mouse tracking, and OpenCode only emits the OSC 52 the menu reads
    // as part of the copy this click is supposed to trigger.
    expect(
      interceptTerminalRightMouseEvent(
        { button: 2, preventDefault, stopPropagation },
        true
      )
    ).toBe(false)
    expect(preventDefault).not.toHaveBeenCalled()
    expect(stopPropagation).not.toHaveBeenCalled()
  })
})

describe('tuiOwnsRightClickCopy', () => {
  it('claims OpenCode regardless of label casing or padding', () => {
    expect(tuiOwnsRightClickCopy('opencode')).toBe(true)
    expect(tuiOwnsRightClickCopy('  OpenCode ')).toBe(true)
  })

  it('leaves every other CLI on the Electron context menu', () => {
    for (const cli of ['claude', 'codex', 'unknown', '']) {
      expect(tuiOwnsRightClickCopy(cli)).toBe(false)
    }
  })
})

describe('OSC 52 selection capture', () => {
  it('decodes UTF-8 clipboard text', () => {
    const payload = globalThis.btoa(
      String.fromCharCode(...new TextEncoder().encode('OpenCode 选区'))
    )

    expect(decodeOsc52ClipboardText(`c;${payload}`)).toBe('OpenCode 选区')
  })

  it('ignores clipboard queries and malformed payloads', () => {
    expect(decodeOsc52ClipboardText('c;?')).toBeNull()
    expect(decodeOsc52ClipboardText('missing-separator')).toBeNull()
    expect(decodeOsc52ClipboardText('c;%%%')).toBeNull()
  })

  it('observes OSC 52 without consuming it and unregisters cleanly', () => {
    const dispose = vi.fn()
    let handler: ((data: string) => boolean) | undefined
    const term = {
      parser: {
        registerOscHandler: vi.fn(
          (_ident: number, callback: (data: string) => boolean) => {
            handler = callback
            return { dispose }
          }
        )
      }
    }
    const onSelection = vi.fn()
    const detach = installOsc52SelectionCapture(term as never, onSelection)

    expect(term.parser.registerOscHandler).toHaveBeenCalledWith(
      52,
      expect.any(Function)
    )
    expect(handler?.(`c;${globalThis.btoa('selected text')}`)).toBe(false)
    expect(onSelection).toHaveBeenCalledWith('selected text')

    detach()
    expect(dispose).toHaveBeenCalledOnce()
  })
})
