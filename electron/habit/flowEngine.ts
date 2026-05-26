import type { AggregatedCluster } from './aggregator.js'
import type { HabitFlowKind, HabitFlowRisk } from './db.js'

export interface GeneratedFlow {
  kind: HabitFlowKind
  title: string
  summary: string
  riskLevel: HabitFlowRisk
  enabledByDefault: boolean
  evidenceCount: number
  payload: Record<string, unknown>
}

function firstSample(cluster: AggregatedCluster): string {
  return cluster.representativeSamples.find((sample) => sample.trim().length > 0) ?? ''
}

function detectPanelKey(sample: string): string {
  const lowered = sample.toLowerCase()
  if (lowered.includes('build')) return 'build'
  if (lowered.includes('diff')) return 'diff'
  if (lowered.includes('repo')) return 'repo-view'
  if (lowered.includes('timeline')) return 'timeline'
  return 'panel'
}

function detectUiAdjustment(cluster: AggregatedCluster, sample: string): GeneratedFlow | null {
  const lowered = sample.toLowerCase()
  if (lowered.includes('template') && /hide|remove|dismiss|weak/i.test(lowered)) {
    return {
      kind: 'ui-adjustment',
      title: 'Hide Templates Entry',
      summary: 'Demote the templates topbar entry based on repeated low-frequency usage.',
      riskLevel: 'low',
      enabledByDefault: true,
      evidenceCount: cluster.size,
      payload: {
        action: 'hide-templates-entry'
      }
    }
  }
  if (lowered.includes('wizard') && /hide|remove|dismiss|weak/i.test(lowered)) {
    return {
      kind: 'ui-adjustment',
      title: 'Hide Wizard Entry',
      summary: 'Demote the wizard topbar entry based on repeated low-frequency usage.',
      riskLevel: 'low',
      enabledByDefault: true,
      evidenceCount: cluster.size,
      payload: {
        action: 'hide-wizard-entry'
      }
    }
  }
  return null
}

function flowFromCluster(cluster: AggregatedCluster): GeneratedFlow | null {
  const sample = firstSample(cluster)
  const uiAdjustment = detectUiAdjustment(cluster, sample)
  if (uiAdjustment) return uiAdjustment

  if (cluster.kind === 'panel_open') {
    const panelKey = detectPanelKey(sample)
    return {
      kind: 'app-flow',
      title: sample || 'Open panel',
      summary: 'Repeated app panel access detected.',
      riskLevel: 'low',
      enabledByDefault: true,
      evidenceCount: cluster.size,
      payload: {
        action: 'open-panel',
        panelKey
      }
    }
  }

  if (cluster.kind === 'action_triggered') {
    return {
      kind: 'app-flow',
      title: sample || 'Repeat app action',
      summary: 'Repeated app action detected.',
      riskLevel: 'low',
      enabledByDefault: true,
      evidenceCount: cluster.size,
      payload: {
        action: 'repeat-app-action',
        sample
      }
    }
  }

  return null
}

export function generateFlowsFromClusters(clusters: AggregatedCluster[]): GeneratedFlow[] {
  return clusters
    .map((cluster) => flowFromCluster(cluster))
    .filter((flow): flow is GeneratedFlow => flow !== null)
}
