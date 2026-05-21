import { readFileSync } from 'fs'
import { describe, expect, it } from 'vitest'

const css = readFileSync(new URL('../styles.css', import.meta.url), 'utf8')

function getBlock(selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const match = css.match(new RegExp(`${escaped}\\s*\\{([^}]*)\\}`, 's'))
  expect(match, `${selector} should define a CSS block`).not.toBeNull()
  return match?.[1] ?? ''
}

describe('terminal host layout styles', () => {
  it('keeps terminal padding inside the allocated panel height', () => {
    const block = getBlock('.term-host')
    expect(block).toContain('height: 100%')
    expect(block).toContain('padding: var(--mac-sp-1)')
    expect(block).toContain('box-sizing: border-box')
  })
})
