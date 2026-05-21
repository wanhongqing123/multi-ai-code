import type { BuildRuntimeState, BuildStepRuntime, ProjectBuildConfig } from '../../electron/preload'

export interface ProjectBuildPanelProps {
  open: boolean
  currentProjectId: string | null
  currentProjectName: string | null
  buildConfig: ProjectBuildConfig
  buildConfigReady: boolean
  state: BuildRuntimeState
  sessionId: string | null
  sessionStatus: 'idle' | 'running' | 'exited'
  onClose: () => void
  onStartBuild: () => void
  onStopBuild: () => void
  onAnalyzeFailure: () => void
}

export function getBuildStatusLabel(status: BuildRuntimeState['status']): string {
  switch (status) {
    case 'running':
      return '构建中'
    case 'succeeded':
      return '已成功'
    case 'failed':
      return '已失败'
    case 'stopped':
      return '已停止'
    case 'idle':
    default:
      return '空闲'
  }
}

export function getBuildStepStatusLabel(status: BuildStepRuntime['status']): string {
  switch (status) {
    case 'running':
      return '进行中'
    case 'succeeded':
      return '成功'
    case 'failed':
      return '失败'
    case 'skipped':
      return '跳过'
    case 'pending':
    default:
      return '等待中'
  }
}

export function getBuildStartBlockedReason(
  projectId: string | null,
  buildConfigReady: boolean,
  buildConfig: ProjectBuildConfig
): string | null {
  if (!projectId) return '请先选择项目'
  if (!buildConfigReady) return '正在读取该项目的构建配置，请稍后再试'
  if (!buildConfig.enabled) return '当前项目未启用构建配置，请先到设置中开启'
  if (!buildConfig.steps.some((step) => step.enabled)) {
    return '当前项目没有启用的构建步骤，请先到设置中配置'
  }
  return null
}

export function canStartBuild(
  projectId: string | null,
  buildConfigReady: boolean,
  buildConfig: ProjectBuildConfig
): boolean {
  return getBuildStartBlockedReason(projectId, buildConfigReady, buildConfig) === null
}

export function canStopBuild(status: BuildRuntimeState['status']): boolean {
  return status === 'running'
}

export function canAnalyzeBuildFailure(state: BuildRuntimeState): boolean {
  return state.status === 'failed' && state.lastFailure !== null
}

export function getBuildLogStatusLabel(state: BuildRuntimeState): string {
  if (state.activeStepId) return `当前步骤：${state.activeStepId}`

  switch (state.status) {
    case 'failed':
      return state.lastFailure?.stepName
        ? `失败步骤：${state.lastFailure.stepName}`
        : '构建失败'
    case 'succeeded':
      return '构建完成'
    case 'stopped':
      return '构建已停止'
    case 'running':
      return '准备执行'
    case 'idle':
    default:
      return '等待中'
  }
}

function formatDateTime(value: string | null): string {
  if (!value) return '—'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return date.toLocaleString()
}

function formatExitSummary(step: BuildStepRuntime): string | null {
  if (step.status !== 'failed' && step.status !== 'succeeded') return null
  if (step.exitCode === null && !step.signal) return null
  return `exit=${step.exitCode ?? 'null'}${step.signal ? ` signal=${step.signal}` : ''}`
}

export default function ProjectBuildPanel(props: ProjectBuildPanelProps): JSX.Element | null {
  if (!props.open) return null

  const startBlockedReason = getBuildStartBlockedReason(
    props.currentProjectId,
    props.buildConfigReady,
    props.buildConfig
  )
  const startEnabled = startBlockedReason === null && props.state.status !== 'running'
  const stopEnabled = canStopBuild(props.state.status)
  const analyzeVisible = canAnalyzeBuildFailure(props.state)
  const failure = props.state.lastFailure
  const logText = props.state.log.trim().length > 0 ? props.state.log : '暂无日志输出'

  return (
    <div className="build-panel-overlay" role="presentation" onClick={props.onClose}>
      <aside
        className="build-panel"
        role="dialog"
        aria-modal="true"
        aria-label="构建面板"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="build-panel-header">
          <div>
            <div className="build-panel-eyebrow">独立构建面板</div>
            <h2 className="build-panel-title">
              构建
              {props.currentProjectName ? (
                <span className="build-panel-project"> · {props.currentProjectName}</span>
              ) : null}
            </h2>
          </div>
          <button className="build-panel-close" onClick={props.onClose} aria-label="关闭构建面板">
            ×
          </button>
        </div>

        <div className="build-panel-status-card">
          <div className="build-panel-status-row">
            <span className="build-panel-label">当前状态</span>
            <span className={`build-panel-status build-panel-status-${props.state.status}`}>
              {getBuildStatusLabel(props.state.status)}
            </span>
          </div>
          <div className="build-panel-meta-grid">
            <span>开始时间：{formatDateTime(props.state.startedAt)}</span>
            <span>结束时间：{formatDateTime(props.state.finishedAt)}</span>
          </div>
          <div className="build-panel-actions">
            <button
              className="tile-btn"
              onClick={props.onStartBuild}
              disabled={!startEnabled}
              title={startEnabled ? '开始构建' : startBlockedReason ?? '构建正在运行中'}
            >
              开始构建
            </button>
            <button
              className="tile-btn"
              onClick={props.onStopBuild}
              disabled={!stopEnabled}
              title={stopEnabled ? '停止当前构建' : '当前没有正在运行的构建'}
            >
              停止构建
            </button>
            {analyzeVisible ? (
              <button className="tile-btn" onClick={props.onAnalyzeFailure}>
                分析失败原因
              </button>
            ) : null}
          </div>
          {!startEnabled && startBlockedReason ? (
            <p className="build-panel-note">{startBlockedReason}</p>
          ) : null}
          {analyzeVisible && failure ? (
            <p className="build-panel-note">
              最近失败：{failure.stepName} · {failure.reason}
            </p>
          ) : null}
          {analyzeVisible && (!props.sessionId || props.sessionStatus !== 'running') ? (
            <p className="build-panel-note">
              主会话未运行时，点击“分析失败原因”会提示先启动主会话。
            </p>
          ) : null}
        </div>

        <section className="build-panel-section">
          <div className="build-panel-section-head">
            <h3>步骤</h3>
            <span>{props.state.steps.length} 个步骤</span>
          </div>
          {props.state.steps.length === 0 ? (
            <div className="build-panel-empty">当前还没有运行过构建步骤。</div>
          ) : (
            <ol className="build-step-list">
              {props.state.steps.map((step) => {
                const exitSummary = formatExitSummary(step)
                return (
                  <li key={step.id} className="build-step-card">
                    <div className="build-step-head">
                      <strong>{step.name}</strong>
                      <span className={`build-step-status build-step-status-${step.status}`}>
                        {getBuildStepStatusLabel(step.status)}
                      </span>
                    </div>
                    <div className="build-step-meta">
                      <span>{step.envType === 'visual-studio' ? 'Visual Studio' : 'MSYS2'}</span>
                      <span>cwd: {step.resolvedCwd ?? step.cwd}</span>
                    </div>
                    <code className="build-step-command">{step.command}</code>
                    {exitSummary ? <div className="build-step-exit">{exitSummary}</div> : null}
                  </li>
                )
              })}
            </ol>
          )}
        </section>

        <section className="build-panel-section">
          <div className="build-panel-section-head">
            <h3>实时日志</h3>
            <span>{getBuildLogStatusLabel(props.state)}</span>
          </div>
          <pre className="build-panel-log">{logText}</pre>
        </section>
      </aside>
    </div>
  )
}
