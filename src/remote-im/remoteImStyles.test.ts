import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'

const css = readFileSync(new URL('../styles.css', import.meta.url), 'utf8')

function getBlock(selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const match = css.match(new RegExp(`${escaped}\\s*\\{([^}]*)\\}`, 's'))
  expect(match, `${selector} should define a CSS block`).not.toBeNull()
  return match?.[1] ?? ''
}

describe('remote IM message styles', () => {
  it('uses a wider, denser bubble layout for long AICLI replies', () => {
    const bubble = getBlock('.remote-im-bubble')

    expect(bubble).toContain('width: min(920px, 92%)')
    expect(bubble).toContain('max-width: min(920px, 92%)')
    expect(bubble).toContain('box-sizing: border-box')
    expect(bubble).toContain('position: relative')
    expect(bubble).toContain('font-size: 14px')
    expect(bubble).toContain('line-height: 1.55')
    expect(bubble).not.toContain('width: fit-content')
    expect(bubble).not.toContain('max-width: 390px')
  })

  it('keeps markdown tables compact inside message bubbles', () => {
    const tableCells = getBlock('.remote-im-markdown th,\n.remote-im-markdown td')

    expect(tableCells).toContain('padding: 5px 7px')
    expect(tableCells).toContain('font-size: 13px')
  })

  it('positions compact status badges without affecting text wrapping', () => {
    const status = getBlock('.remote-im-message-status')

    expect(status).toContain('position: absolute')
    expect(status).toContain('right: 10px')
    expect(status).toContain('bottom: 8px')
    expect(status).not.toContain('margin-top')
  })
})
