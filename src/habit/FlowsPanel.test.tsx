import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { HabitFlowRow } from './habitTypes'
import FlowsPanel, { groupHabitFlows } from './FlowsPanel.js'

const flows: HabitFlowRow[] = [
  {
    id: 1,
    kind: 'app-flow',
    title: 'Open GitHub',
    summary: 'Frequently opens the project issue tracker',
    evidence_count: 6,
    risk_level: 'low',
    enabled_by_default: 1,
    status: 'active',
    payload: JSON.stringify({ action: 'open-panel', panelKey: 'repo-view' }),
    created_at: 1,
    updated_at: 1
  },
  {
    id: 2,
    kind: 'ui-adjustment',
    title: 'Hide Templates Entry',
    summary: 'Templates is low-frequency',
    evidence_count: 3,
    risk_level: 'low',
    enabled_by_default: 1,
    status: 'active',
    payload: JSON.stringify({ action: 'hide-templates-entry' }),
    created_at: 2,
    updated_at: 2
  },
  {
    id: 3,
    kind: 'app-flow',
    title: 'Delete Workspace',
    summary: 'Potentially mutating action',
    evidence_count: 2,
    risk_level: 'high',
    enabled_by_default: 0,
    status: 'candidate',
    payload: JSON.stringify({ action: 'repeat-app-action', sample: 'delete workspace' }),
    created_at: 3,
    updated_at: 3
  }
]

describe('FlowsPanel', () => {
  it('groups active flows, ui adjustments, and high-risk candidates separately', () => {
    const groups = groupHabitFlows(flows)

    expect(groups.activeFlows.map((flow) => flow.id)).toEqual([1])
    expect(groups.activeAdjustments.map((flow) => flow.id)).toEqual([2])
    expect(groups.highRiskCandidates.map((flow) => flow.id)).toEqual([3])
  })

  it('renders grouped sections with disable actions and manual confirmation badges', () => {
    const markup = renderToStaticMarkup(
      <FlowsPanel flows={flows} busy={false} onDisable={vi.fn()} />
    )

    expect(markup).toContain('Open GitHub')
    expect(markup).toContain('Hide Templates Entry')
    expect(markup).toContain('Delete Workspace')
    expect(markup).toContain('自动启用')
    expect(markup).toContain('需人工确认')
  })
})
