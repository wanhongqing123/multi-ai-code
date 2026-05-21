import { describe, expect, it } from 'vitest'
import type { AggregatedCluster } from './aggregator.js'
import { generateFlowsFromClusters } from './flowEngine.js'

function mkCluster(overrides: Partial<AggregatedCluster> = {}): AggregatedCluster {
  return {
    id: 'cluster-1',
    kind: 'site_visit',
    sourceEventIds: [1, 2, 3],
    size: 3,
    representativeSamples: ['Visit https://example.test/builds'],
    projectCount: 1,
    crossProject: false,
    firstTs: 1,
    lastTs: 2,
    score: 3,
    ...overrides
  }
}

describe('generateFlowsFromClusters', () => {
  it('turns repeated site visits into active low-risk site flows', () => {
    const [flow] = generateFlowsFromClusters([
      mkCluster({
        kind: 'site_visit',
        representativeSamples: ['Visit https://example.test/builds']
      })
    ])

    expect(flow).toMatchObject({
      kind: 'site-flow',
      riskLevel: 'low',
      enabledByDefault: true
    })
    expect(flow.title).toContain('example.test')
    expect(flow.payload).toMatchObject({
      action: 'open-managed-chrome-url',
      url: 'https://example.test/builds'
    })
  })

  it('turns repeated panel opens into active low-risk app flows', () => {
    const [flow] = generateFlowsFromClusters([
      mkCluster({
        kind: 'panel_open',
        representativeSamples: ['Open Build Panel']
      })
    ])

    expect(flow).toMatchObject({
      kind: 'app-flow',
      riskLevel: 'low',
      enabledByDefault: true
    })
    expect(flow.payload).toMatchObject({
      action: 'open-panel',
      panelKey: 'build'
    })
  })

  it('turns hiding the template entry into a low-risk ui adjustment', () => {
    const [flow] = generateFlowsFromClusters([
      mkCluster({
        kind: 'action_triggered',
        representativeSamples: ['Hide template entry from topbar']
      })
    ])

    expect(flow).toMatchObject({
      kind: 'ui-adjustment',
      riskLevel: 'low',
      enabledByDefault: true
    })
    expect(flow.payload).toMatchObject({
      action: 'hide-templates-entry'
    })
  })

  it('marks mutating submit-like site actions as high risk and disabled by default', () => {
    const [flow] = generateFlowsFromClusters([
      mkCluster({
        kind: 'site_click',
        representativeSamples: ['Click Submit deployment button']
      })
    ])

    expect(flow).toMatchObject({
      kind: 'site-flow',
      riskLevel: 'high',
      enabledByDefault: false
    })
    expect(flow.payload).toMatchObject({
      action: 'site-click-hint'
    })
  })
})
