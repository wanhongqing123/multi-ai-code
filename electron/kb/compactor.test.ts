import { describe, expect, it } from 'vitest'
import {
  KB_TIER_DEFAULTS,
  buildMergePrompt,
  importanceOf,
  localMerge,
  parseMergeResponse,
  planCompaction
} from './compactor.js'
import type { KbEntry } from './db.js'

const NOW = 1_700_000_000_000

function entry(over: Partial<KbEntry> = {}): KbEntry {
  return {
    id: 1,
    repoPath: '/r',
    createdAt: NOW - 24 * 60 * 60 * 1000,
    updatedAt: NOW - 24 * 60 * 60 * 1000,
    topic: 't',
    summary: 's',
    evidence: {},
    importance: 0.5,
    tier: 'hot',
    accessCount: 0,
    lastAccessedAt: null,
    ...over
  }
}

describe('importanceOf', () => {
  it('returns +Infinity for pinned entries', () => {
    expect(importanceOf(entry({ tier: 'pinned' }), NOW)).toBe(Number.POSITIVE_INFINITY)
  })

  it('decays with age', () => {
    const fresh = importanceOf(entry({ updatedAt: NOW }), NOW)
    const old = importanceOf(
      entry({ updatedAt: NOW - 60 * 24 * 60 * 60 * 1000 }),
      NOW
    )
    expect(fresh).toBeGreaterThan(old)
  })

  it('rewards access_count', () => {
    const cold = importanceOf(entry({ accessCount: 0 }), NOW)
    const hot = importanceOf(entry({ accessCount: 50 }), NOW)
    expect(hot).toBeGreaterThan(cold)
  })

  it('respects the importance field as a multiplier', () => {
    const low = importanceOf(entry({ importance: 0 }), NOW)
    const high = importanceOf(entry({ importance: 1 }), NOW)
    expect(high).toBeGreaterThan(low)
  })
})

describe('planCompaction', () => {
  function entries(count: number, tier: KbEntry['tier']): KbEntry[] {
    return Array.from({ length: count }, (_, i) =>
      entry({
        id: i,
        tier,
        updatedAt: NOW - i * 24 * 60 * 60 * 1000,
        topic: `t${i}`,
        summary: `s${i}`
      })
    )
  }

  it('returns empty demotion lists when tiers are under budget', () => {
    const p = planCompaction(entries(10, 'hot'), NOW, KB_TIER_DEFAULTS)
    expect(p.demoteFromHot).toHaveLength(0)
    expect(p.demoteFromWarm).toHaveLength(0)
  })

  it('demotes from hot when over budget, picking lowest-scored first', () => {
    const list = entries(35, 'hot') // 5 over budget=30
    const p = planCompaction(list, NOW, KB_TIER_DEFAULTS)
    expect(p.demoteFromHot).toHaveLength(5)
    // The demoted entries should be the oldest (highest i = oldest updatedAt).
    const demotedIds = p.demoteFromHot.map((e) => e.id).sort((a, b) => a - b)
    expect(demotedIds).toEqual([30, 31, 32, 33, 34])
  })

  it('never demotes pinned entries even when tier is over budget', () => {
    const list = [
      ...entries(35, 'hot'),
      entry({ id: 100, tier: 'pinned', updatedAt: NOW - 5 * 365 * 24 * 60 * 60 * 1000 })
    ]
    const p = planCompaction(list, NOW, KB_TIER_DEFAULTS)
    for (const e of p.demoteFromHot) {
      expect(e.tier).toBe('hot')
    }
  })

  it('flags totalBytesOver when summaries push past the budget', () => {
    const huge = entry({
      id: 1,
      tier: 'hot',
      summary: 'x'.repeat(600 * 1024),
      topic: 't'
    })
    const p = planCompaction([huge], NOW)
    expect(p.totalBytesOver).toBe(true)
  })

  it('does NOT flag totalBytesOver for normal-sized content', () => {
    const list = entries(20, 'hot')
    const p = planCompaction(list, NOW)
    expect(p.totalBytesOver).toBe(false)
  })
})

describe('buildMergePrompt', () => {
  it('lists every entry with its topic and summary', () => {
    const prompt = buildMergePrompt(
      [entry({ topic: 'auth', summary: 'OAuth flow' }), entry({ topic: 'cache', summary: 'LRU' })],
      'warm'
    )
    expect(prompt).toContain('auth')
    expect(prompt).toContain('OAuth flow')
    expect(prompt).toContain('cache')
    expect(prompt).toContain('LRU')
  })

  it('asks for strict JSON return', () => {
    expect(buildMergePrompt([entry()], 'warm')).toContain('严格 JSON')
  })

  it('mentions the target tier name (warm or cold)', () => {
    expect(buildMergePrompt([entry()], 'cold')).toContain('cold')
  })
})

describe('parseMergeResponse', () => {
  it('parses valid topic+summary JSON', () => {
    expect(parseMergeResponse('{"topic":"x","summary":"y"}')).toEqual({
      topic: 'x',
      summary: 'y'
    })
  })

  it('strips ```json``` fences', () => {
    expect(
      parseMergeResponse('```json\n{"topic":"x","summary":"y"}\n```')
    ).toEqual({ topic: 'x', summary: 'y' })
  })

  it('returns null on missing fields', () => {
    expect(parseMergeResponse('{"topic":"x"}')).toBeNull()
    expect(parseMergeResponse('{"summary":"y"}')).toBeNull()
  })

  it('returns null on malformed input', () => {
    expect(parseMergeResponse('garbage')).toBeNull()
    expect(parseMergeResponse('')).toBeNull()
  })
})

describe('localMerge fallback', () => {
  it('joins multiple entries into a bullet summary', () => {
    const m = localMerge(
      [entry({ topic: 'a', summary: 'A' }), entry({ topic: 'b', summary: 'B' })],
      'warm'
    )
    expect(m.summary).toContain('a')
    expect(m.summary).toContain('A')
    expect(m.summary).toContain('b')
    expect(m.summary).toContain('B')
  })

  it('truncates very long combined summaries', () => {
    const longEntries = Array.from({ length: 20 }, (_, i) =>
      entry({ topic: `t${i}`, summary: 'x'.repeat(500) })
    )
    const m = localMerge(longEntries, 'cold')
    expect(m.summary.length).toBeLessThanOrEqual(2001) // 2000 + '…'
  })

  it('uses the single entry topic when only one entry is being demoted', () => {
    const m = localMerge([entry({ topic: 'single', summary: 's' })], 'warm')
    expect(m.topic).toBe('single')
  })
})
