import { readFileSync } from 'fs'
import { resolve } from 'path'
import { describe, expect, it } from 'vitest'

describe('index.html', () => {
  it('does not depend on remote font providers during startup', () => {
    const html = readFileSync(resolve(process.cwd(), 'src/index.html'), 'utf8')

    expect(html).not.toContain('fonts.googleapis.com')
    expect(html).not.toContain('fonts.gstatic.com')
    expect(html).not.toContain('rel="preconnect"')
  })
})
