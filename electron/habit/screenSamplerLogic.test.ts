import { describe, expect, it } from 'vitest'
import {
  DEFAULT_APP_BLOCKLIST,
  SampleDedupe,
  applyBlocklist,
  dateFolderFor,
  redactSecrets,
  safeIsoStamp
} from './screenSamplerLogic.js'

describe('redactSecrets', () => {
  it('masks sk-* api keys', () => {
    expect(redactSecrets('use sk-abc1234567890DEFXYZ in prompt')).toContain(
      '<redacted>'
    )
    expect(redactSecrets('use sk-abc1234567890DEFXYZ in prompt')).not.toContain(
      'sk-abc1234567890DEFXYZ'
    )
  })

  it('masks Bearer tokens', () => {
    expect(
      redactSecrets('Authorization: Bearer abcdef0123456789zzzz')
    ).toContain('<redacted>')
  })

  it('masks long high-entropy strings (likely api keys)', () => {
    const long = 'A'.repeat(50)
    expect(redactSecrets(`token=${long}`)).toContain('<redacted>')
  })

  it('masks GitHub personal access tokens (ghp_/gho_/github_pat_)', () => {
    // Realistic-length tokens — real GH PATs are ~82 chars; ghp_ tokens 40+.
    const ghp = 'ghp_' + 'A'.repeat(36)
    const pat = 'github_pat_' + 'B'.repeat(22) + '_' + 'C'.repeat(59)
    const text = `use ${ghp} or ${pat}`
    const out = redactSecrets(text)
    expect(out).not.toContain(ghp)
    expect(out).not.toContain(pat)
  })

  it('leaves normal short text alone', () => {
    expect(redactSecrets('Hello world')).toBe('Hello world')
    expect(redactSecrets('VSCode — App.tsx')).toBe('VSCode — App.tsx')
  })

  it('handles empty / undefined-ish input', () => {
    expect(redactSecrets('')).toBe('')
  })
})

describe('applyBlocklist', () => {
  it('drops samples whose app name is on the default blocklist', () => {
    const dropped = applyBlocklist(
      { title: 'Vault', appName: '1Password 7' },
      DEFAULT_APP_BLOCKLIST
    )
    expect(dropped).toBeNull()
  })

  it('drops samples whose title contains an incognito / private marker', () => {
    expect(
      applyBlocklist(
        { title: 'Google — Incognito', appName: 'Chrome' },
        DEFAULT_APP_BLOCKLIST
      )
    ).toBeNull()
    expect(
      applyBlocklist(
        { title: '微信 - 隐私浏览', appName: 'Chrome' },
        DEFAULT_APP_BLOCKLIST
      )
    ).toBeNull()
  })

  it('keeps non-matching samples but redacts secrets inside the title', () => {
    const kept = applyBlocklist(
      { title: 'Postman — Bearer abcdef0123456789zzzz', appName: 'Postman' },
      DEFAULT_APP_BLOCKLIST
    )
    expect(kept).not.toBeNull()
    expect(kept!.title).toContain('<redacted>')
    expect(kept!.title).not.toContain('abcdef0123456789zzzz')
  })

  it('matches case-insensitively', () => {
    expect(
      applyBlocklist({ title: 'X', appName: 'BITWARDEN' }, DEFAULT_APP_BLOCKLIST)
    ).toBeNull()
  })

  it('respects a custom blocklist passed in', () => {
    const dropped = applyBlocklist(
      { title: 'banking page', appName: 'Edge' },
      ['banking']
    )
    expect(dropped).toBeNull()
  })

  it('ignores empty rules in the blocklist', () => {
    expect(
      applyBlocklist({ title: 'Editor', appName: 'VSCode' }, ['', '   '])
    ).not.toBeNull()
  })
})

describe('SampleDedupe', () => {
  it('keeps the first occurrence of a key', () => {
    const d = new SampleDedupe({ windowMs: 1000 })
    expect(d.shouldKeep('foo', 1000)).toBe(true)
  })

  it('drops the same key inside the window', () => {
    const d = new SampleDedupe({ windowMs: 60_000 })
    d.shouldKeep('foo', 1000)
    expect(d.shouldKeep('foo', 1500)).toBe(false)
    expect(d.shouldKeep('foo', 59_999 + 1000)).toBe(false)
  })

  it('keeps the key again after the window expires', () => {
    const d = new SampleDedupe({ windowMs: 60_000 })
    d.shouldKeep('foo', 1000)
    expect(d.shouldKeep('foo', 1000 + 60_001)).toBe(true)
  })

  it('treats different keys independently', () => {
    const d = new SampleDedupe({ windowMs: 60_000 })
    expect(d.shouldKeep('a', 1000)).toBe(true)
    expect(d.shouldKeep('b', 1000)).toBe(true)
  })

  it('drops empty keys', () => {
    const d = new SampleDedupe()
    expect(d.shouldKeep('', 1)).toBe(false)
  })

  it('garbage-collects oldest entries when over maxKeys', () => {
    const d = new SampleDedupe({ windowMs: 1000, maxKeys: 3 })
    d.shouldKeep('a', 1)
    d.shouldKeep('b', 2)
    d.shouldKeep('c', 3)
    d.shouldKeep('d', 4)
    // After exceeding maxKeys + gc, "a"/"b"/"c"/"d" should still be tracked
    // (gc runs but window hasn't expired at t=4, so they remain).
    // After advancing time past windowMs, gc should evict expired.
    d.shouldKeep('e', 10_000)
    expect(d._size()).toBeLessThan(5)
  })
})

describe('dateFolderFor + safeIsoStamp', () => {
  it('formats date folder as YYYY-MM-DD', () => {
    const ts = Date.UTC(2026, 0, 5, 12, 30) // 2026-01-05
    const folder = dateFolderFor(ts)
    expect(folder).toMatch(/^\d{4}-\d{2}-\d{2}$/)
  })

  it('safeIsoStamp replaces `:` and `.` with `-` (filename-safe)', () => {
    const ts = Date.UTC(2026, 0, 5, 12, 30, 45, 123)
    expect(safeIsoStamp(ts)).not.toContain(':')
    expect(safeIsoStamp(ts)).not.toContain('.')
    expect(safeIsoStamp(ts)).toMatch(/^2026-01-05T12-30-45-123Z$/)
  })
})
