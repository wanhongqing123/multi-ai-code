import { describe, expect, it } from 'vitest'
import { getCliTargetLabel } from './cliTarget'

describe('getCliTargetLabel', () => {
  it('returns claude cli for claude provider', () => {
    expect(getCliTargetLabel('claude')).toBe('claude cli')
  })

  it('returns codex cli for codex provider', () => {
    expect(getCliTargetLabel('codex')).toBe('codex cli')
  })
})
