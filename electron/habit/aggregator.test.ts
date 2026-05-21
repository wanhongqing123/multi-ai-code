import { describe, expect, it } from 'vitest'
import type { HabitEventRow } from './db.js'
import {
  RECENCY_HALF_LIFE_DAYS,
  aggregateHabitEvents,
  buildShingles,
  eventText,
  jaccard,
  normalizeText,
  recencyDecay
} from './aggregator.js'

function evt(id: number, ts: number, kind: HabitEventRow['kind'], text: string, projectId: string | null = 'p1'): HabitEventRow {
  return {
    id,
    ts,
    kind,
    payload: JSON.stringify({ text }),
    source: null,
    project_id: projectId,
    repo_path: null,
    source_window: null
  }
}

describe('normalizeText', () => {
  it('lowercases and tokenizes ascii words', () => {
    expect(normalizeText('Hello World')).toEqual(['hello', 'world'])
  })

  it('strips path-like segments', () => {
    const tokens = normalizeText('Explain src/foo.ts and electron/main.ts please')
    expect(tokens).not.toContain('src/foo.ts')
    expect(tokens).not.toContain('electron/main.ts')
    expect(tokens).toContain('explain')
    expect(tokens).toContain('please')
  })

  it('strips line refs and ranges', () => {
    const tokens = normalizeText('around L42 and lines 12-30')
    expect(tokens).not.toContain('l42')
    expect(tokens).not.toContain('12-30')
    expect(tokens).toContain('<line>')
    expect(tokens).toContain('<range>')
  })

  it('preserves Chinese characters intact', () => {
    const tokens = normalizeText('帮我看看 src/foo.ts 的实现')
    expect(tokens.some((t) => t.includes('帮我看看'))).toBe(true)
    expect(tokens.some((t) => t.includes('的实现'))).toBe(true)
  })
})

describe('buildShingles + jaccard', () => {
  it('produces k-grams across tokens', () => {
    const shingles = buildShingles(['a', 'b', 'c', 'd'], 3)
    expect(shingles.size).toBe(2)
    expect(shingles.has(['a', 'b', 'c'].join('␟'))).toBe(true)
    expect(shingles.has(['b', 'c', 'd'].join('␟'))).toBe(true)
  })

  it('returns 1.0 for identical sets', () => {
    const s = buildShingles(['x', 'y', 'z'], 2)
    expect(jaccard(s, s)).toBe(1)
  })

  it('returns 0 for disjoint sets', () => {
    expect(jaccard(buildShingles(['a', 'b'], 2), buildShingles(['c', 'd'], 2))).toBe(0)
  })
})

describe('recencyDecay', () => {
  it('returns 1 when ts equals now', () => {
    expect(recencyDecay(1000, 1000)).toBe(1)
  })

  it('halves after one half-life', () => {
    const now = 1_000_000_000_000
    const oneHalfLifeAgo = now - RECENCY_HALF_LIFE_DAYS * 24 * 60 * 60 * 1000
    expect(recencyDecay(oneHalfLifeAgo, now)).toBeCloseTo(0.5, 5)
  })

  it('floors negative ages to zero', () => {
    expect(recencyDecay(2000, 1000)).toBe(1)
  })
})

describe('eventText', () => {
  it('returns the payload text field', () => {
    const row = evt(1, 1, 'pty_cmd', 'hello')
    expect(eventText(row)).toBe('hello')
  })

  it('tolerates malformed JSON payloads', () => {
    expect(
      eventText({
        id: 1,
        ts: 1,
        kind: 'pty_cmd',
        payload: '{not-json',
        source: null,
        project_id: null,
        repo_path: null,
        source_window: null
      })
    ).toBe('')
  })

  it('returns empty when text field is missing', () => {
    expect(
      eventText({
        id: 1,
        ts: 1,
        kind: 'pty_cmd',
        payload: '{}',
        source: null,
        project_id: null,
        repo_path: null,
        source_window: null
      })
    ).toBe('')
  })
})

describe('aggregateHabitEvents: clustering', () => {
  it('keeps site visits grouped by stable host/path instead of collapsing all URLs together', () => {
    const rows: HabitEventRow[] = [
      evt(1, 1, 'site_visit', 'Visit https://example.test/builds?from=nav'),
      evt(2, 2, 'site_visit', 'Visit https://example.test/builds?from=menu'),
      evt(3, 3, 'site_visit', 'Visit https://other.test/builds?from=nav')
    ]
    const clusters = aggregateHabitEvents(rows, { now: 4, minClusterSize: 2 })
    expect(clusters).toHaveLength(1)
    expect(clusters[0].kind).toBe('site_visit')
    expect(clusters[0].sourceEventIds).toEqual([1, 2])
  })

  it('merges near-identical prompts into one cluster', () => {
    const rows: HabitEventRow[] = [
      evt(1, 1, 'ai_prompt_main', 'please explain the implementation of foo.ts in detail'),
      evt(2, 2, 'ai_prompt_main', 'please explain the implementation of bar.ts in detail'),
      evt(3, 3, 'ai_prompt_main', 'please explain the implementation of baz.ts in detail')
    ]
    const clusters = aggregateHabitEvents(rows, { now: 4, minClusterSize: 3 })
    expect(clusters).toHaveLength(1)
    expect(clusters[0].size).toBe(3)
    expect(clusters[0].kind).toBe('ai_prompt_main')
  })

  it('keeps disjoint prompts in separate clusters and filters by minClusterSize', () => {
    const rows: HabitEventRow[] = [
      evt(1, 1, 'ai_prompt_main', 'please explain the implementation of foo'),
      evt(2, 2, 'ai_prompt_main', 'please explain the implementation of bar'),
      evt(3, 3, 'ai_prompt_main', 'please explain the implementation of baz'),
      evt(4, 4, 'ai_prompt_main', 'completely different question about deployment timeline')
    ]
    const clusters = aggregateHabitEvents(rows, { now: 5, minClusterSize: 3 })
    expect(clusters).toHaveLength(1)
    expect(clusters[0].sourceEventIds).toEqual([1, 2, 3])
  })

  it('never merges across different event kinds', () => {
    const rows: HabitEventRow[] = [
      evt(1, 1, 'ai_prompt_main', 'please explain the implementation of foo'),
      evt(2, 2, 'ai_prompt_main', 'please explain the implementation of bar'),
      evt(3, 3, 'ai_prompt_main', 'please explain the implementation of baz'),
      evt(4, 4, 'diff_annotation', 'please explain the implementation of foo'),
      evt(5, 5, 'diff_annotation', 'please explain the implementation of bar'),
      evt(6, 6, 'diff_annotation', 'please explain the implementation of baz')
    ]
    const clusters = aggregateHabitEvents(rows, { now: 7, minClusterSize: 3 })
    const kinds = new Set(clusters.map((c) => c.kind))
    expect(kinds.size).toBe(2)
    expect(clusters).toHaveLength(2)
  })

  it('orders top results by recency-weighted size (score)', () => {
    const dayMs = 24 * 60 * 60 * 1000
    const now = 1_000_000_000_000
    const rows: HabitEventRow[] = [
      // Older cluster of 5
      evt(1, now - 30 * dayMs, 'ai_prompt_main', 'please explain the implementation of foo'),
      evt(2, now - 29 * dayMs, 'ai_prompt_main', 'please explain the implementation of bar'),
      evt(3, now - 28 * dayMs, 'ai_prompt_main', 'please explain the implementation of baz'),
      evt(4, now - 27 * dayMs, 'ai_prompt_main', 'please explain the implementation of qux'),
      evt(5, now - 26 * dayMs, 'ai_prompt_main', 'please explain the implementation of quux'),
      // Newer cluster of 3
      evt(6, now - 1 * dayMs, 'diff_annotation', 'this needs better error handling and tests'),
      evt(7, now - 1 * dayMs, 'diff_annotation', 'this needs better error handling and tests'),
      evt(8, now - 1 * dayMs, 'diff_annotation', 'this needs better error handling and tests')
    ]
    const clusters = aggregateHabitEvents(rows, { now, minClusterSize: 3 })
    expect(clusters).toHaveLength(2)
    expect(clusters[0].kind).toBe('diff_annotation')
    expect(clusters[0].score).toBeGreaterThan(clusters[1].score)
  })

  it('flags crossProject correctly', () => {
    const rows: HabitEventRow[] = [
      evt(1, 1, 'ai_prompt_main', 'please explain the implementation of foo', 'p1'),
      evt(2, 2, 'ai_prompt_main', 'please explain the implementation of bar', 'p2'),
      evt(3, 3, 'ai_prompt_main', 'please explain the implementation of baz', 'p3')
    ]
    const clusters = aggregateHabitEvents(rows, { now: 4, minClusterSize: 3 })
    expect(clusters[0].crossProject).toBe(true)
    expect(clusters[0].projectCount).toBe(3)
  })

  it('treats malformed payloads as zero-token events and drops them', () => {
    const rows: HabitEventRow[] = [
      {
        id: 1,
        ts: 1,
        kind: 'ai_prompt_main',
        payload: 'not-json',
        source: null,
        project_id: 'p1',
        repo_path: null,
        source_window: null
      },
      evt(2, 2, 'ai_prompt_main', 'please explain the implementation of bar'),
      evt(3, 3, 'ai_prompt_main', 'please explain the implementation of baz')
    ]
    const clusters = aggregateHabitEvents(rows, { now: 4, minClusterSize: 2 })
    expect(clusters[0].size).toBe(2)
    expect(clusters[0].sourceEventIds).not.toContain(1)
  })

  it('returns empty when no events meet the threshold', () => {
    const rows: HabitEventRow[] = []
    expect(aggregateHabitEvents(rows)).toEqual([])
  })

  it('respects topK', () => {
    const rows: HabitEventRow[] = []
    let id = 1
    // Each cluster's text shares zero tokens with the others so they stay disjoint.
    const themes = [
      ['alpha', 'configures', 'deploy', 'pipelines'],
      ['beta', 'reviews', 'security', 'audits'],
      ['gamma', 'designs', 'database', 'schemas'],
      ['delta', 'tunes', 'performance', 'metrics'],
      ['epsilon', 'translates', 'documentation', 'paragraphs']
    ]
    for (const theme of themes) {
      for (let i = 0; i < 3; i++) {
        rows.push(evt(id++, id, 'ai_prompt_main', theme.join(' ')))
      }
    }
    const clusters = aggregateHabitEvents(rows, { now: 1000, topK: 2 })
    expect(clusters).toHaveLength(2)
  })

  it('caps representativeSamples to representativeCount', () => {
    const rows: HabitEventRow[] = []
    for (let i = 0; i < 10; i++) {
      rows.push(evt(i + 1, i + 1, 'ai_prompt_main', `please explain the implementation of file${i}`))
    }
    const clusters = aggregateHabitEvents(rows, { now: 100, representativeCount: 3 })
    expect(clusters[0].representativeSamples.length).toBeLessThanOrEqual(3)
  })
})
