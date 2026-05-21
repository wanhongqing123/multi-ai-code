import { describe, expect, it } from 'vitest'
import { ALL_KB_TIERS, escapeFtsQuery, rowToEntry } from './db.js'

describe('KB tier vocabulary', () => {
  it('declares exactly four distinct tiers', () => {
    expect(ALL_KB_TIERS).toHaveLength(4)
    expect(new Set(ALL_KB_TIERS).size).toBe(4)
    expect(ALL_KB_TIERS).toEqual(['hot', 'warm', 'cold', 'pinned'])
  })
})

describe('escapeFtsQuery', () => {
  it('quotes each whitespace-separated token', () => {
    expect(escapeFtsQuery('auth flow')).toBe('"auth" "flow"')
  })

  it('strips embedded double quotes so they cannot break parsing', () => {
    expect(escapeFtsQuery('hello "world"')).toBe('"hello" "world"')
  })

  it('drops tokens that are pure punctuation', () => {
    expect(escapeFtsQuery('foo --- bar')).toBe('"foo" "bar"')
  })

  it('keeps Chinese tokens intact', () => {
    expect(escapeFtsQuery('认证 流程')).toBe('"认证" "流程"')
  })

  it('preserves FTS-reserved characters as plain content via quoting', () => {
    // Without quoting, "AND" / "OR" / "*" are FTS operators. Quoting forces
    // them to be matched literally.
    expect(escapeFtsQuery('cache OR purge')).toBe('"cache" "OR" "purge"')
    expect(escapeFtsQuery('x*y')).toBe('"x*y"')
  })

  it('returns an empty string for empty / whitespace-only input', () => {
    expect(escapeFtsQuery('')).toBe('')
    expect(escapeFtsQuery('   ')).toBe('')
  })
})

describe('rowToEntry: evidence JSON', () => {
  function row(over: Record<string, unknown> = {}) {
    return {
      id: 1,
      repo_path: '/r',
      created_at: 1,
      updated_at: 1,
      topic: 't',
      summary: 's',
      evidence: null,
      importance: 0.5,
      tier: 'hot',
      access_count: 0,
      last_accessed_at: null,
      ...over
    } as Parameters<typeof rowToEntry>[0]
  }

  it('parses valid evidence JSON into the camelCase shape', () => {
    const r = row({ evidence: '{"commits":["a1b2"],"files":["src/app.ts"]}' })
    const e = rowToEntry(r)
    expect(e.evidence.commits).toEqual(['a1b2'])
    expect(e.evidence.files).toEqual(['src/app.ts'])
  })

  it('returns empty evidence for null evidence', () => {
    expect(rowToEntry(row({ evidence: null })).evidence).toEqual({})
  })

  it('tolerates malformed JSON without throwing', () => {
    expect(rowToEntry(row({ evidence: '{not json' })).evidence).toEqual({})
  })

  it('maps snake_case row fields to camelCase entry fields', () => {
    const e = rowToEntry(
      row({
        id: 42,
        repo_path: '/foo/bar',
        created_at: 1000,
        updated_at: 2000,
        access_count: 5,
        last_accessed_at: 3000
      })
    )
    expect(e.id).toBe(42)
    expect(e.repoPath).toBe('/foo/bar')
    expect(e.createdAt).toBe(1000)
    expect(e.updatedAt).toBe(2000)
    expect(e.accessCount).toBe(5)
    expect(e.lastAccessedAt).toBe(3000)
  })
})
