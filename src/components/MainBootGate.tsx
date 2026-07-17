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
  /** Resolved Codex/OpenCode launch path shown before the user starts a session. */
  launchNotice?: string | null
  /** User-facing work mode selected for this session start. */
  workMode: BootGateWorkMode
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
  if (command === 'opencode') return 'OpenCode'
  return command || '(未配置)'
}

function isClaudeCliCommand(command: string): boolean {
  const cleaned = command.trim().replace(/^['"]|['"]$/g, '')
  const base = cleaned.split(/[\\/]/).pop()?.toLowerCase() ?? cleaned.toLowerCase()
  return /^claude(\.(exe|cmd|bat|ps1))?$/.test(base)
}

export default function MainBootGate(props: MainBootGateProps): JSX.Element {
  const {
    phase,
    command,
    launchNotice,
    workMode,
    disabled = false,
    onChoose,
    onDismissFailure
  } = props
  const [showTail, setShowTail] = useState(false)

  const spawning = phase.kind === 'spawning'
  const spawningMode = phase.kind === 'spawning' ? phase.mode : null
  const workModeLabel = workMode === 'scheduled-task' ? '定时任务' : '普通任务'
  const requiresClaudeRiskConfirmation = isClaudeCliCommand(command)

  return (
    <div className="main-panel">
      <div className="main-panel-body boot-gate-body">
        <div className="boot-gate-card">
          <div className="boot-gate-title">选择本次主会话启动方式</div>
          <div className="boot-gate-subtitle">
            当前模式：<b>{workModeLabel}</b> · 当前 CLI：<b>{describeCli(command)}</b>
          </div>
          {launchNotice && (
            <div className="boot-gate-launch-path">
              <span>AICLI 启动路径</span>
              <code>{launchNotice}</code>
            </div>
          )}

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

          {requiresClaudeRiskConfirmation ? (
            <div className="boot-gate-claude-risk">
              <div className="boot-gate-claude-risk-title">
                Claude 目前风险很高，请谨慎使用
              </div>
              <div className="boot-gate-claude-risk-copy">
                建议更换 Codex。只有在你明确接受风险时，才继续启动 Claude Code。
              </div>
              <div className="boot-gate-buttons boot-gate-claude-risk-actions">
                <button
                  className="tile-btn boot-gate-btn danger"
                  onClick={() => onChoose('new')}
                  disabled={disabled || spawning}
                  autoFocus
                >
                  {spawningMode === 'new'
                    ? '正在启动…'
                    : `即使有风险也要继续使用 Claude · 新${workModeLabel}会话`}
                </button>
                <button
                  className="tile-btn boot-gate-btn danger"
                  onClick={() => onChoose('resume')}
                  disabled={disabled || spawning}
                  title={`即使有风险也要继续使用 Claude，并续聊上次${workModeLabel}会话`}
                >
                  {spawningMode === 'resume'
                    ? '正在续聊…'
                    : `即使有风险也要继续使用 Claude · 继续${workModeLabel}`}
                </button>
              </div>
            </div>
          ) : (
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
                disabled={
                  disabled ||
                  spawning ||
                  (command !== 'codex' && command !== 'opencode')
                }
                title={
                  command === 'codex' || command === 'opencode'
                    ? `继续上次${workModeLabel}会话（由 CLI 自身回放历史）`
                    : '当前 CLI 不支持续聊'
                }
              >
                {spawningMode === 'resume' ? '正在续聊…' : `继续${workModeLabel}`}
              </button>
            </div>
          )}

          <div className="boot-gate-hint">
            续聊将由 CLI 自身加载历史，显示可能与上次不完全一致；
            若该项目从未保存过对话，续聊会失败并提示重新选择。
          </div>
        </div>
      </div>
    </div>
  )
}
