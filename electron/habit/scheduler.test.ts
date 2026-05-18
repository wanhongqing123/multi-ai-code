import { describe, expect, it } from 'vitest'
import type { AggregatedCluster } from './aggregator.js'
import type { SkillCandidateRow } from './db.js'
import {
  AGGREGATION_INTERVAL_MS,
  pickNewClusters,
  shouldRunAggregation
} from './scheduler.js'

function mkCluster(
  ids: number[],
  overrides: Partial<AggregatedCluster> = {}
): AggregatedCluster {
  return {
    id: `c:${ids.join('-')}`,
    kind: 'ai_prompt_main',
    sourceEventIds: ids,
    size: ids.length,
    representativeSamples: ['sample'],
    projectCount: 1,
    crossProject: false,
    firstTs: 0,
    lastTs: 0,
    score: ids.length,
    ...overrides
  }
}

function mkExisting(ids: number[]): SkillCandidateRow {
  return {
    id: 0,
    created_at: 0,
    cluster_kind: 'ai_prompt_main',
    cluster_size: ids.length,
    source_event_ids: JSON.stringify(ids),
    representative_samples: '[]',
    generated_title: null,
    generated_body: null,
    generated_meta: null,
    status: 'pending',
    reviewed_at: null,
    snoozed_until: null,
    error_message: null
  }
}

describe('shouldRunAggregation', () => {
  it('returns false when master switch is off', () => {
    expect(
      shouldRunAggregation({ enabled: false, lastAggregatedAt: 0 }, 1_000_000_000_000)
    ).toBe(false)
  })

  it('returns true on first ever run (lastAggregatedAt == 0)', () => {
    expect(
      shouldRunAggregation({ enabled: true, lastAggregatedAt: 0 }, 1_000_000_000_000)
    ).toBe(true)
  })

  it('returns false if last run was less than 24h ago', () => {
    const now = 1_000_000_000_000
    expect(
      shouldRunAggregation(
        { enabled: true, lastAggregatedAt: now - 12 * 60 * 60 * 1000 },
        now
      )
    ).toBe(false)
  })

  it('returns true once 24h has elapsed', () => {
    const now = 1_000_000_000_000
    expect(
      shouldRunAggregation(
        { enabled: true, lastAggregatedAt: now - AGGREGATION_INTERVAL_MS },
        now
      )
    ).toBe(true)
  })
})

describe('pickNewClusters', () => {
  it('keeps clusters that share no event ids with existing candidates', () => {
    const existing = [mkExisting([1, 2, 3])]
    const fresh = [mkCluster([4, 5, 6])]
    expect(pickNewClusters(fresh, existing)).toHaveLength(1)
  })

  it('drops a cluster whose event ids are fully covered by an existing candidate', () => {
    const existing = [mkExisting([1, 2, 3, 4, 5])]
    const fresh = [mkCluster([1, 2, 3])]
    expect(pickNewClusters(fresh, existing)).toHaveLength(0)
  })

  it('keeps a cluster that overlaps but extends beyond existing coverage', () => {
    const existing = [mkExisting([1, 2, 3])]
    const fresh = [mkCluster([1, 2, 3, 4, 5])]
    // Cluster ids 1,2,3 are in existing but 4,5 are new, so cluster is NOT
    // fully covered — should be kept.
    expect(pickNewClusters(fresh, existing)).toHaveLength(1)
  })

  it('handles malformed existing source_event_ids gracefully', () => {
    const bad: SkillCandidateRow = {
      ...mkExisting([1]),
      source_event_ids: 'not-json'
    }
    expect(pickNewClusters([mkCluster([42])], [bad])).toHaveLength(1)
  })

  it('returns empty when input cluster list is empty', () => {
    expect(pickNewClusters([], [mkExisting([1])])).toEqual([])
  })
})
