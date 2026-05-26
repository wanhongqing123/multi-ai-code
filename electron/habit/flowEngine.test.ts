import { describe, expect, it } from 'vitest'
import type { AggregatedCluster } from './aggregator.js'
import { generateFlowsFromClusters } from './flowEngine.js'

function mkCluster(overrides: Partial<AggregatedCluster> = {}): AggregatedCluster {
  return {
    id: 'cluster-1',
    kind: 'panel_open',
    sourceEventIds: [1, 2, 3],
    size: 3,
    representativeSamples: ['Open Build Panel'],
    projectCount: 1,
    crossProject: false,
    firstTs: 1,
    lastTs: 2,
    score: 3,
    ...overrides
  }
}

describe('generateFlowsFromClusters', () => {
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

  it('turns repeated app actions into app flows', () => {
    const [flow] = generateFlowsFromClusters([
      mkCluster({
        kind: 'action_triggered',
        representativeSamples: ['Repeat build and collect logs']
      })
    ])

    expect(flow).toMatchObject({
      kind: 'app-flow',
      riskLevel: 'low',
      enabledByDefault: true
    })
    expect(flow.payload).toMatchObject({
      action: 'repeat-app-action'
    })
  })
})
