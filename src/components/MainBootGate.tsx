import { useState } from 'react'

/**
 * Phase the gate UI renders. When MainPanel is mounted instead, the gate is
 * not rendered at all — see BootState in App.tsx for the parent-level state.
 */
export type BootGatePhase =
  | { kind: 'idle' }
  | { kind: 'spawning'; mode: 'new' | 'resume' }
  | { kind: 'failed'; reason: string; tail?: string }

export type BootGateWorkMode = 'normal-task' | 'scheduled-task'

export interface MainBootGateProps {
  phase: BootGatePhase
  /** Current CLI binary name (claude / codex / other custom). */
  command: string
  /** User-facing work mode selected for this session start. */
  workMode: BootGateWorkMode
  /** Plan name shown alongside the gate to mirror MainPanel's chrome. */
  planName: string
  /** Whether user can interact (false when no project / no plan). */
  disabled?: boolean
  /** Called when user picks how to start the session. */
  onChoose: (mode: 'new' | 'resume') => void
  /** Called when user dismisses a failed-resume notice to retry. */
  onDismissFailure: () => void
}

function describeCli(command: string): string {
  if (command === 'claude') return 'Claude Code'
  if (command === 'codex') return 'Codex'
  return command || '(未配置)'
}

export default function MainBootGate(props: MainBootGateProps): JSX.Element {
  const {
    phase,
    command,
    workMode,
    planName,
    disabled = false,
    onChoose,
    onDismissFailure
  } = props
  const [showTail, setShowTail] = useState(false)

  const spawning = phase.kind === 'spawning'
  const spawningMode = phase.kind === 'spawning' ? phase.mode : null
  const workModeLabel = workMode === 'scheduled-task' ? '定时任务' : '普通任务'

  return (
    <div className="main-panel">
      <div className="main-panel-head">
        <div className="main-panel-title">
          <span className="main-panel-plan">{planName || '(未选择方案)'}</span>
          <span className="tile-badge idle">待启动</span>
        </div>
      </div>

      <div className="main-panel-body boot-gate-body">
        <div className="boot-gate-card">
          <div className="boot-gate-title">选择本次主会话启动方式</div>
          <div className="boot-gate-subtitle">
            当前模式：<b>{workModeLabel}</b> · 当前 CLI：<b>{describeCli(command)}</b>
          </div>

          {phase.kind === 'failed' && (
            <div className="boot-gate-failure">
              <div className="boot-gate-failure-head">
                <span>续聊失败：{phase.reason}</span>
                <button
                  className="tile-btn boot-gate-failure-dismiss"
                  onClick={onDismissFailure}
                  title="回到选择界面"
                >
                  重新选择
                </button>
              </div>
              {phase.tail && phase.tail.trim() && (
                <details
                  className="boot-gate-failure-tail"
                  open={showTail}
                  onToggle={(e) => setShowTail((e.target as HTMLDetailsElement).open)}
                >
                  <summary>查看 CLI 输出</summary>
                  <pre>{phase.tail}</pre>
                </details>
              )}
            </div>
          )}

          <div className="boot-gate-buttons">
            <button
              className="tile-btn boot-gate-btn"
              onClick={() => onChoose('new')}
              disabled={disabled || spawning}
              autoFocus
            >
              {spawningMode === 'new' ? '正在启动…' : `新${workModeLabel}会话`}
            </button>
            <button
              className="tile-btn boot-gate-btn"
              onClick={() => onChoose('resume')}
              disabled={disabled || spawning || command !== 'claude' && command !== 'codex'}
              title={
                command === 'claude' || command === 'codex'
                  ? `继续上次${workModeLabel}会话（由 CLI 自身回放历史）`
                  : '当前 CLI 不支持续聊'
              }
            >
              {spawningMode === 'resume' ? '正在续聊…' : `继续${workModeLabel}`}
            </button>
          </div>

          <div className="boot-gate-hint">
            续聊将由 CLI 自身加载历史，显示可能与上次不完全一致；
            若该项目从未保存过对话，续聊会失败并提示重新选择。
          </div>
        </div>
      </div>
    </div>
  )
}
