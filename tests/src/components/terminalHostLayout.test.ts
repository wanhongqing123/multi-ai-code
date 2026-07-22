import { describe, expect, it } from 'vitest'
import { stretchTerminalRootToHost } from '../../../src/components/terminalHostLayout.js'

describe('stretchTerminalRootToHost', () => {
  it('forces the xterm root node to fill the host container', () => {
    const root = { style: {} as Record<string, string> }
    const host = {
      querySelector: (selector: string) =>
        selector === '.xterm' ? (root as unknown as HTMLElement) : null
    } as unknown as ParentNode

    const applied = stretchTerminalRootToHost(host)

    expect(applied).toBe(true)
    expect(root.style.width).toBe('100%')
    expect(root.style.height).toBe('100%')
  })

  it('returns false when the host does not contain an xterm root', () => {
    const host = {
      querySelector: () => null
    } as unknown as ParentNode
    expect(stretchTerminalRootToHost(host)).toBe(false)
  })
})
