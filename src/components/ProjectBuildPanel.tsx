import type {
  BuildRuntimeState,
  BuildStepRuntime,
  ProjectBuildConfig,
  ProjectRuntimeConfig,
  RuntimeState
} from '../../electron/preload'

export interface ProjectBuildPanelProps {
  open: boolean
  currentProjectId: string | null
  currentProjectName: string | null
  buildConfig: ProjectBuildConfig
  buildConfigReady: boolean
  runtimeConfig?: ProjectRuntimeConfig
  runtimeConfigReady?: boolean
  runtimeState?: RuntimeState
  state: BuildRuntimeState
  sessionId: string | null
  sessionStatus: 'idle' | 'running' | 'exited'
  onClose: () => void
  onStartBuild: () => void
  onStartSingleBuild: (stepId: string) => void
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

export function getRuntimeStatusLabel(status: RuntimeState['status']): string {
  switch (status) {
    case 'running':
      return '运行中'
    case 'exited':
      return '已退出'
    case 'failed':
      return '运行失败'
    case 'stopped':
      return '已停止'
    case 'idle':
    default:
      return '空闲'
  }
}

export function getBuildStepStatusLabel(status: BuildStepRuntime['status']): string {
  switch (status) {
    case 'not-run':
      return '未执行'
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
  buildConfig: ProjectBuildConfig,
  scope: 'all' | 'single-step' = 'all',
  stepId: string | null = null
): string | null {
  if (!projectId) return '请先选择项目'
  if (!buildConfigReady) return '正在读取该项目的构建配置，请稍后再试'
  if (!buildConfig.enabled) return '当前项目未启用构建配置，请先到设置中开启'
  if (scope === 'all' && !buildConfig.steps.some((step) => step.enabled)) {
    return '当前项目没有启用的构建步骤，请先到设置中配置'
  }
  if (scope === 'single-step') {
    const step = buildConfig.steps.find((item) => item.id === stepId)
    if (!step) return `未找到构建步骤：${stepId ?? ''}`
    if (!step.enabled) return `该构建步骤尚未启用：${step.name}`
  }
  return null
}

export function canStartBuild(
  projectId: string | null,
  buildConfigReady: boolean,
  buildConfig: ProjectBuildConfig,
  scope: 'all' | 'single-step' = 'all',
  stepId: string | null = null
): boolean {
  return getBuildStartBlockedReason(projectId, buildConfigReady, buildConfig, scope, stepId) === null
}

export function canStopBuild(status: BuildRuntimeState['status']): boolean {
  return status === 'running'
}

export function canAnalyzeBuildFailure(state: BuildRuntimeState): boolean {
  return state.status === 'failed' && state.lastFailure !== null
}

export function getRuntimeStartBlockedReason(
  projectId: string | null,
  runtimeConfigReady: boolean,
  runtimeConfig: ProjectRuntimeConfig,
  runtimeState: RuntimeState
): string | null {
  if (!projectId) return '请先选择项目'
  if (!runtimeConfigReady) return '正在读取项目运行配置，请稍后再试'
  if (!runtimeConfig.enabled) return '当前项目未启用运行配置，请先到设置中开启'
  if (!runtimeConfig.command.trim()) return '运行命令不能为空'
  if (runtimeState.status === 'running') return '当前已有运行进程'
  return null
}

export function canSendRuntimeLog(
  projectId: string | null,
  runtimeState: RuntimeState,
  sessionId: string | null,
  sessionStatus: 'idle' | 'running' | 'exited'
): boolean {
  return (
    !!projectId &&
    runtimeState.projectId === projectId &&
    runtimeState.log.trim().length > 0 &&
    !!sessionId &&
    sessionStatus === 'running'
  )
}

export function getDisplayStepsForBuildPanel(
  buildConfig: ProjectBuildConfig,
  state: BuildRuntimeState
): BuildStepRuntime[] {
  if (state.status === 'running') {
    return state.steps
  }

  const runtimeStepById = new Map(state.steps.map((step) => [step.id, step]))
  return buildConfig.steps.map((step) => {
    const runtime = runtimeStepById.get(step.id)
    return {
      ...step,
      visualStudioDisplayName:
        step.envType === 'visual-studio' ? runtime?.visualStudioDisplayName ?? null : null,
      status: runtime?.status ?? ('not-run' as const),
      resolvedCwd: runtime?.resolvedCwd ?? null,
      startedAt: runtime?.startedAt ?? null,
      finishedAt: runtime?.finishedAt ?? null,
      exitCode: runtime?.exitCode ?? null,
      signal: runtime?.signal ?? null
    }
  })
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

function formatOutputEncodingLabel(
  encoding: BuildStepRuntime['outputEncoding']
): string {
  if (encoding === 'auto') return '自动'
  if (encoding === 'utf8') return 'UTF-8'
  return 'GBK'
}

export default function ProjectBuildPanel(props: ProjectBuildPanelProps): JSX.Element | null {
  if (!props.open) return null

  const displaySteps = getDisplayStepsForBuildPanel(props.buildConfig, props.state)
  const startBlockedReason = getBuildStartBlockedReason(
    props.currentProjectId,
    props.buildConfigReady,
    props.buildConfig,
    'all',
    null
  )
  const startEnabled = startBlockedReason === null && props.state.status !== 'running'
  const stopEnabled = canStopBuild(props.state.status)
  const analyzeVisible = canAnalyzeBuildFailure(props.state)
  const failure = props.state.lastFailure
  const buildHasLog = props.state.log.trim().length > 0
  const logText = buildHasLog ? props.state.log : '暂无日志输出'
  const logStatus = getBuildLogStatusLabel(props.state)

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
              title={startEnabled ? '顺序执行当前已启用步骤' : startBlockedReason ?? '构建正在运行中'}
            >
              顺序构建
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
            <span>{displaySteps.length} 个步骤</span>
          </div>
          {displaySteps.length === 0 ? (
            <div className="build-panel-empty">当前还没有运行过构建步骤。</div>
          ) : (
            <ol className="build-step-list">
              {displaySteps.map((step) => {
                const exitSummary = formatExitSummary(step)
                const singleBuildBlockedReason =
                  props.state.status === 'running'
                    ? '当前已有构建正在执行'
                    : getBuildStartBlockedReason(
                        props.currentProjectId,
                        props.buildConfigReady,
                        props.buildConfig,
                        'single-step',
                        step.id
                      )
                const singleBuildEnabled = singleBuildBlockedReason === null
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
                      {step.envType === 'visual-studio' && step.visualStudioDisplayName ? (
                        <span>VS: {step.visualStudioDisplayName}</span>
                      ) : null}
                      <span>编码: {formatOutputEncodingLabel(step.outputEncoding)}</span>
                      <span>cwd: {step.resolvedCwd ?? step.cwd}</span>
                    </div>
                    <code className="build-step-command">{step.command}</code>
                    <div className="build-step-actions">
                      <button
                        className="tile-btn"
                        onClick={() => props.onStartSingleBuild(step.id)}
                        disabled={!singleBuildEnabled}
                        title={singleBuildEnabled ? `单独执行 ${step.name}` : singleBuildBlockedReason ?? '单独构建不可用'}
                      >
                        单独构建
                      </button>
                    </div>
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
            <span>{logStatus}</span>
          </div>
          <pre className="build-panel-log">{logText}</pre>
        </section>
      </aside>
    </div>
  )
}
