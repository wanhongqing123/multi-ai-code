import type { HabitFlowRow } from './habitTypes'

export interface GroupedHabitFlows {
  activeFlows: HabitFlowRow[]
  activeAdjustments: HabitFlowRow[]
  highRiskCandidates: HabitFlowRow[]
}

interface Props {
  flows: HabitFlowRow[]
  busy?: boolean
  onDisable: (id: number) => void | Promise<void>
}

function sortFlows(flows: HabitFlowRow[]): HabitFlowRow[] {
  return [...flows].sort((left, right) => {
    if (right.evidence_count !== left.evidence_count) {
      return right.evidence_count - left.evidence_count
    }
    return right.updated_at - left.updated_at
  })
}

export function groupHabitFlows(flows: HabitFlowRow[]): GroupedHabitFlows {
  return {
    activeFlows: sortFlows(
      flows.filter((flow) => flow.status === 'active' && flow.kind !== 'ui-adjustment')
    ),
    activeAdjustments: sortFlows(
      flows.filter((flow) => flow.status === 'active' && flow.kind === 'ui-adjustment')
    ),
    highRiskCandidates: sortFlows(
      flows.filter((flow) => flow.status === 'candidate' && flow.risk_level === 'high')
    )
  }
}

function FlowList(props: {
  title: string
  flows: HabitFlowRow[]
  busy: boolean
  actionLabel?: string
  onAction?: (id: number) => void | Promise<void>
  badgeText?: string
  badgeClassName?: string
}): JSX.Element | null {
  const { title, flows, busy, actionLabel, onAction, badgeText, badgeClassName } = props
  if (flows.length === 0) return null

  return (
    <section className="habit-settings-section">
      <header className="habit-settings-section-head">
        <strong>{title}</strong>
        <span className="habit-settings-hint">{flows.length} 项</span>
      </header>

      <ul className="habit-flow-list">
        {flows.map((flow) => (
          <li key={flow.id} className="habit-flow-item">
            <div className="habit-flow-main">
              <div className="habit-flow-title-row">
                <strong>{flow.title}</strong>
                <span className={`habit-flow-badge ${badgeClassName ?? ''}`}>
                  {badgeText ?? (flow.risk_level === 'high' ? '高风险' : '低风险')}
                </span>
              </div>
              <div className="habit-flow-summary">{flow.summary}</div>
              <div className="habit-flow-meta">证据 {flow.evidence_count}</div>
            </div>

            {actionLabel && onAction ? (
              <button
                type="button"
                className="drawer-btn"
                disabled={busy}
                onClick={() => void onAction(flow.id)}
              >
                {actionLabel}
              </button>
            ) : null}
          </li>
        ))}
      </ul>
    </section>
  )
}

export default function FlowsPanel(props: Props): JSX.Element {
  const { flows, busy = false, onDisable } = props
  const groups = groupHabitFlows(flows)
  const totalItems =
    groups.activeFlows.length + groups.activeAdjustments.length + groups.highRiskCandidates.length

  if (totalItems === 0) {
    return <div className="drawer-empty">还没有生成可用流程，先继续使用一段时间再回来查看。</div>
  }

  return (
    <div className="habit-flows-panel">
      <FlowList
        title="活跃流程"
        flows={groups.activeFlows}
        busy={busy}
        actionLabel="关闭"
        onAction={onDisable}
        badgeText="自动启用"
        badgeClassName="habit-flow-badge-active"
      />
      <FlowList
        title="界面调整"
        flows={groups.activeAdjustments}
        busy={busy}
        actionLabel="关闭"
        onAction={onDisable}
        badgeText="低风险"
        badgeClassName="habit-flow-badge-adjustment"
      />
      <FlowList
        title="高风险候选"
        flows={groups.highRiskCandidates}
        busy={busy}
        badgeText="需人工确认"
        badgeClassName="habit-flow-badge-warning"
      />
    </div>
  )
}
