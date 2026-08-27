import { describe, expect, it, vi } from 'vitest'
import {
  decodeOsc52ClipboardText,
  interceptTerminalRightMouseEvent,
  installCopyBinding,
  installOsc52SelectionCapture,
  tuiOwnsRightClickCopy
} from '../../../src/components/terminalClipboard.js'

describe('installCopyBinding', () => {
  function install(
    selection: string,
    ctrlCAsCopyInMainTui: boolean,
    platform = 'Win32'
  ) {
    let handler: ((event: KeyboardEvent) => boolean) | undefined
    const term = {
      getSelection: () => selection,
      attachCustomKeyEventHandler: vi.fn((callback: (event: KeyboardEvent) => boolean) => {
        handler = callback
      })
    }
    installCopyBinding(term as never, {
      platform,
      ctrlCAsCopyInMainTui
    })
    if (!handler) throw new Error('copy key handler was not installed')
    return handler
  }

  function keyEvent(input: Partial<KeyboardEvent> = {}): KeyboardEvent {
    return {
      type: 'keydown',
      code: 'KeyC',
      ctrlKey: true,
      shiftKey: false,
      altKey: false,
      metaKey: false,
      preventDefault: vi.fn(),
      ...input
    } as unknown as KeyboardEvent
  }

  it('consumes Windows main-TUI Ctrl+C even when there is no selection', () => {
    const handler = install('', true)
    const event = keyEvent()

    expect(handler(event)).toBe(false)
    expect(event.preventDefault).toHaveBeenCalledOnce()
  })

  it('leaves Windows Ctrl+C available to repository shell terminals', () => {
    const handler = install('', false)
    const event = keyEvent()

    expect(handler(event)).toBe(true)
    expect(event.preventDefault).not.toHaveBeenCalled()
  })

  it('consumes macOS main-TUI Ctrl+C even when there is no selection', () => {
    const handler = install('', true, 'MacIntel')
    const event = keyEvent()

    expect(handler(event)).toBe(false)
    expect(event.preventDefault).toHaveBeenCalledOnce()
  })

  it('keeps macOS Cmd+C copy available alongside protected Ctrl+C', () => {
    const handler = install('selected text', true, 'MacIntel')
    const event = keyEvent({ ctrlKey: false, metaKey: true })

    expect(handler(event)).toBe(false)
    expect(event.preventDefault).toHaveBeenCalledOnce()
  })

  it('keeps Ctrl+Shift+C copy for Windows terminals', () => {
    const handler = install('selected text', false)
    const event = keyEvent({ shiftKey: true })

    expect(handler(event)).toBe(false)
    expect(event.preventDefault).toHaveBeenCalledOnce()
  })
})

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
