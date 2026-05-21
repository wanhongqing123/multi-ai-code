import { readFileSync } from 'fs'
import { describe, expect, it } from 'vitest'

const css = readFileSync(new URL('../styles.css', import.meta.url), 'utf8')

function getBlock(selector: string): string {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const match = css.match(new RegExp(`${escaped}\\s*\\{([^}]*)\\}`, 's'))
  expect(match, `${selector} should define a CSS block`).not.toBeNull()
  return match?.[1] ?? ''
}

describe('habit monitor layout styles', () => {
  it('defines stacked toggle row layout for the overview strategy list', () => {
    const row = getBlock('.habit-overview-toggle')
    expect(row).toContain('display: grid')
    expect(row).toContain('grid-template-columns')

    const copy = getBlock('.habit-overview-toggle-copy')
    expect(copy).toContain('display: flex')
    expect(copy).toContain('flex-direction: column')
  })

  it('defines managed Chrome card layout for metadata and actions', () => {
    const card = getBlock('.habit-managed-chrome-card')
    expect(card).toContain('display: flex')
    expect(card).toContain('flex-direction: column')

    const meta = getBlock('.habit-managed-chrome-meta')
    expect(meta).toContain('display: grid')

    const label = getBlock('.habit-managed-chrome-label')
    expect(label).toContain('display: block')
  })
})
