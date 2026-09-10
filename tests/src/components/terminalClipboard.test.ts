import { describe, expect, it, vi } from 'vitest'
import {
  interceptTerminalRightMouseEvent,
  installCopyBinding
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

    // 传 forwardToTui 时右键必须原样交给 TUI：吞掉会让自带右键复制的 TUI
    // 拿不到事件，而 xterm 在鼠标跟踪下没有自己的选区可复制。
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
