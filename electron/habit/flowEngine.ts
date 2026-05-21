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

const MUTATING_ACTION_RE =
  /\b(submit|publish|delete|confirm|send|payment|checkout|login|logout)\b/i

function firstSample(cluster: AggregatedCluster): string {
  return cluster.representativeSamples.find((sample) => sample.trim().length > 0) ?? ''
}

function extractUrl(text: string): string | null {
  const match = text.match(/https?:\/\/[^\s)]+/i)
  if (!match) return null
  try {
    const parsed = new URL(match[0])
    parsed.search = ''
    parsed.hash = ''
    return parsed.toString()
  } catch {
    return match[0]
  }
}

function hostFromUrl(url: string): string {
  try {
    return new URL(url).host
  } catch {
    return url
  }
}

function pathFromUrl(url: string): string {
  try {
    return new URL(url).pathname || '/'
  } catch {
    return '/'
  }
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

  if (cluster.kind === 'site_visit') {
    const url = extractUrl(sample)
    if (!url) return null
    const host = hostFromUrl(url)
    const path = pathFromUrl(url)
    return {
      kind: 'site-flow',
      title: `Open ${host}`,
      summary: `Repeated visit to ${path}`,
      riskLevel: 'low',
      enabledByDefault: true,
      evidenceCount: cluster.size,
      payload: {
        action: 'open-managed-chrome-url',
        url
      }
    }
  }

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

  if (cluster.kind === 'site_click') {
    return {
      kind: 'site-flow',
      title: sample || 'Repeat site action',
      summary: 'Repeated site click pattern detected.',
      riskLevel: MUTATING_ACTION_RE.test(sample) ? 'high' : 'low',
      enabledByDefault: !MUTATING_ACTION_RE.test(sample),
      evidenceCount: cluster.size,
      payload: {
        action: 'site-click-hint',
        sample
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
