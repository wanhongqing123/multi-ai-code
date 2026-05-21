import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { HabitFlowRow } from './habitTypes'
import FlowsPanel, { groupHabitFlows } from './FlowsPanel.js'

const flows: HabitFlowRow[] = [
  {
    id: 1,
    kind: 'site-flow',
    title: '打开 GitHub',
    summary: '经常访问 issues 页面',
    evidence_count: 6,
    risk_level: 'low',
    enabled_by_default: 1,
    status: 'active',
    payload: JSON.stringify({ action: 'open-managed-chrome-url', url: 'https://github.com/issues' }),
    created_at: 1,
    updated_at: 1
  },
  {
    id: 2,
    kind: 'ui-adjustment',
    title: '隐藏模板入口',
    summary: '很少使用模板入口',
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
    kind: 'site-flow',
    title: '提交周报',
    summary: '检测到可能提交远端状态的操作',
    evidence_count: 2,
    risk_level: 'high',
    enabled_by_default: 0,
    status: 'candidate',
    payload: JSON.stringify({ action: 'submit-form' }),
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

    expect(markup).toContain('活跃流程')
    expect(markup).toContain('界面调整')
    expect(markup).toContain('高风险候选')
    expect(markup).toContain('打开 GitHub')
    expect(markup).toContain('隐藏模板入口')
    expect(markup).toContain('提交周报')
    expect(markup).toContain('关闭')
    expect(markup).toContain('需人工确认')
  })
})
