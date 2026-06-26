import type {
  BuildConfigValidationIssue,
  BuildStepConfig,
  ProjectBuildConfig,
  VisualStudioInstallation
} from '../../electron/preload'

export interface ProjectBuildSettingsSectionProps {
  projectId: string | null
  loading: boolean
  value: ProjectBuildConfig
  disabled: boolean
  hostPlatform?: string
  visualStudioInstallations?: VisualStudioInstallation[]
  visualStudioInstallationsLoading?: boolean
  onRefreshVisualStudioInstallations?: () => void
  onChange: (next: ProjectBuildConfig) => void
}

function resolveHostPlatform(hostPlatform?: string): string {
  if (hostPlatform !== undefined) return hostPlatform
  if (typeof window === 'undefined') return ''
  return window.navigator?.platform ?? ''
}

export function isMacBuildSettingsPlatform(hostPlatform?: string): boolean {
  return resolveHostPlatform(hostPlatform).toLowerCase().includes('mac')
}

function getDefaultBuildStepEnvType(hostPlatform?: string): BuildStepConfig['envType'] {
  return isMacBuildSettingsPlatform(hostPlatform) ? 'system' : 'msys'
}

export function normalizeBuildConfigForHost(
  config: ProjectBuildConfig,
  hostPlatform?: string
): ProjectBuildConfig {
  if (!isMacBuildSettingsPlatform(hostPlatform)) return config
  return {
    ...config,
    steps: config.steps.map((step) => ({
      ...step,
      envType: 'system',
      visualStudioInstanceId: ''
    }))
  }
}

export function createBuildStep(id: string, hostPlatform?: string): BuildStepConfig {
  return {
    id,
    name: 'New Step',
    envType: getDefaultBuildStepEnvType(hostPlatform),
    cwd: '.',
    command: '',
    enabled: true,
    visualStudioInstanceId: '',
    outputEncoding: 'auto'
  }
}

export function appendBuildStep(
  config: ProjectBuildConfig,
  id: string,
  hostPlatform?: string
): ProjectBuildConfig {
  return {
    ...config,
    steps: [...config.steps, createBuildStep(id, hostPlatform)]
  }
}

export function removeBuildStep(config: ProjectBuildConfig, index: number): ProjectBuildConfig {
  return {
    ...config,
    steps: config.steps.filter((_, stepIndex) => stepIndex !== index)
  }
}

export function moveBuildStep(
  config: ProjectBuildConfig,
  index: number,
  direction: 'up' | 'down'
): ProjectBuildConfig {
  const targetIndex = direction === 'up' ? index - 1 : index + 1
  if (targetIndex < 0 || targetIndex >= config.steps.length) return config

  const steps = [...config.steps]
  ;[steps[index], steps[targetIndex]] = [steps[targetIndex], steps[index]]
  return { ...config, steps }
}

export function updateBuildStep(
  config: ProjectBuildConfig,
  index: number,
  patch: Partial<BuildStepConfig>
): ProjectBuildConfig {
  return {
    ...config,
    steps: config.steps.map((step, stepIndex) =>
      stepIndex === index ? { ...step, ...patch } : step
    )
  }
}

function formatBuildConfigDetailPath(path: string): string {
  const match = /^build_config\.steps\[(\d+)\]\.(.+)$/.exec(path)
  if (!match) return path
  const [, indexText, field] = match
  return `步骤 ${Number(indexText) + 1} / ${field}`
}

export function formatBuildConfigSaveError(
  error: string,
  details?: BuildConfigValidationIssue[]
): string {
  if (!details?.length) return `项目构建配置保存失败：${error}`
  return [
    `项目构建配置保存失败：${error}`,
    ...details.map((detail) => `- ${formatBuildConfigDetailPath(detail.path)}：${detail.message}`)
  ].join('\n')
}

function createBuildStepId(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID()
  }
  return `build-step-${Date.now()}-${Math.random().toString(16).slice(2, 8)}`
}

function isMissingVisualStudioInstance(
  step: BuildStepConfig,
  installations: VisualStudioInstallation[]
): boolean {
  if (step.envType !== 'visual-studio') return false
  if (!step.visualStudioInstanceId) return false
  return !installations.some((item) => item.instanceId === step.visualStudioInstanceId)
}

export default function ProjectBuildSettingsSection(
  props: ProjectBuildSettingsSectionProps
): JSX.Element {
  const hostPlatform = resolveHostPlatform(props.hostPlatform)
  const usesSystemEnvironmentOnly = isMacBuildSettingsPlatform(hostPlatform)
  const value = normalizeBuildConfigForHost(props.value, hostPlatform)
  const visualStudioInstallations = props.visualStudioInstallations ?? []
  const visualStudioInstallationsLoading = props.visualStudioInstallationsLoading ?? false

  const handleAddStep = (): void => {
    props.onChange(appendBuildStep(value, createBuildStepId(), hostPlatform))
  }

  return (
    <section className="ai-settings-card">
      <div className="ai-settings-title">项目构建</div>
      {!props.projectId ? (
        <div className="ai-settings-note">选择项目后可编辑项目构建配置</div>
      ) : props.loading ? (
        <div className="ai-settings-note">正在读取项目构建配置...</div>
      ) : (
        <>
          <label className="ai-settings-checkbox">
            <input
              type="checkbox"
              checked={value.enabled}
              onChange={(event) =>
                props.onChange({ ...value, enabled: event.target.checked })
              }
              disabled={props.disabled}
            />
            <span>启用项目构建</span>
          </label>

          <div className="project-build-settings-toolbar">
            <button
              type="button"
              className="drawer-btn"
              onClick={handleAddStep}
              disabled={props.disabled}
            >
              新增步骤
            </button>
          </div>

          {value.steps.length ? (
            <div className="project-build-settings-list">
              {value.steps.map((step, index) => (
                <article key={step.id} className="project-build-step-card">
                  <div className="project-build-step-head">
                    <div className="project-build-step-title">
                      <strong>{step.name || `步骤 ${index + 1}`}</strong>
                      <span>{step.id}</span>
                    </div>
                    <div className="project-build-step-actions">
                      <button
                        type="button"
                        className="drawer-btn"
                        onClick={() => props.onChange(moveBuildStep(value, index, 'up'))}
                        disabled={props.disabled || index === 0}
                      >
                        上移
                      </button>
                      <button
                        type="button"
                        className="drawer-btn"
                        onClick={() => props.onChange(moveBuildStep(value, index, 'down'))}
                        disabled={props.disabled || index === value.steps.length - 1}
                      >
                        下移
                      </button>
                      <button
                        type="button"
                        className="drawer-btn danger"
                        onClick={() => props.onChange(removeBuildStep(value, index))}
                        disabled={props.disabled}
                      >
                        删除
                      </button>
                    </div>
                  </div>

                  <label className="ai-settings-checkbox">
                    <input
                      type="checkbox"
                      checked={step.enabled}
                      onChange={(event) =>
                        props.onChange(
                          updateBuildStep(value, index, { enabled: event.target.checked })
                        )
                      }
                      disabled={props.disabled}
                    />
                    <span>启用此步骤</span>
                  </label>

                  <div className="project-build-settings-grid">
                    <label>
                      名称
                      <input
                        type="text"
                        value={step.name}
                        onChange={(event) =>
                          props.onChange(
                            updateBuildStep(value, index, { name: event.target.value })
                          )
                        }
                        disabled={props.disabled}
                      />
                    </label>

                    {usesSystemEnvironmentOnly ? (
                      <label>
                        环境
                        <input type="text" value="原始环境" disabled readOnly />
                      </label>
                    ) : (
                      <label>
                        环境
                        <select
                          value={step.envType}
                          onChange={(event) =>
                            props.onChange(
                              updateBuildStep(value, index, {
                                envType: event.target.value as BuildStepConfig['envType']
                              })
                            )
                          }
                          disabled={props.disabled}
                        >
                          <option value="msys">MSYS2</option>
                          <option value="visual-studio">
                            Visual Studio Developer Command Prompt
                          </option>
                        </select>
                      </label>
                    )}

                    <label>
                      输出编码
                      <select
                        value={step.outputEncoding}
                        onChange={(event) =>
                          props.onChange(
                            updateBuildStep(value, index, {
                              outputEncoding: event.target.value as BuildStepConfig['outputEncoding']
                            })
                          )
                        }
                        disabled={props.disabled}
                      >
                        <option value="auto">自动</option>
                        <option value="utf8">UTF-8</option>
                        <option value="gbk">GBK</option>
                      </select>
                    </label>

                    {step.envType === 'visual-studio' ? (
                      <label>
                        Visual Studio 实例
                        <select
                          value={step.visualStudioInstanceId}
                          onChange={(event) =>
                            props.onChange(
                              updateBuildStep(value, index, {
                                visualStudioInstanceId: event.target.value
                              })
                            )
                          }
                          disabled={props.disabled}
                        >
                          <option value="">
                            {visualStudioInstallationsLoading ? '正在读取实例...' : '请选择实例'}
                          </option>
                          {visualStudioInstallations.map((item) => (
                            <option key={item.instanceId} value={item.instanceId}>
                              {item.displayName}
                            </option>
                          ))}
                        </select>
                      </label>
                    ) : null}

                    <label className="project-build-settings-grid-full">
                      工作目录
                      <input
                        type="text"
                        value={step.cwd}
                        onChange={(event) =>
                          props.onChange(
                            updateBuildStep(value, index, { cwd: event.target.value })
                          )
                        }
                        placeholder="仓库根目录（.）"
                        disabled={props.disabled}
                      />
                      <div className="ai-settings-note">填“.”表示仓库根目录（target_repo）</div>
                    </label>

                    <label className="project-build-settings-grid-full">
                      命令
                      <textarea
                        value={step.command}
                        onChange={(event) =>
                          props.onChange(
                            updateBuildStep(value, index, { command: event.target.value })
                          )
                        }
                        rows={3}
                        placeholder="cmake --build build"
                        disabled={props.disabled}
                      />
                    </label>
                  </div>

                  {isMissingVisualStudioInstance(step, visualStudioInstallations) ? (
                    <div className="ai-settings-note project-build-step-warning">
                      所选 Visual Studio 实例当前不可用
                    </div>
                  ) : null}
                </article>
              ))}
            </div>
          ) : (
            <div className="ai-settings-note">还没有构建步骤，点击“新增步骤”开始配置。</div>
          )}
        </>
      )}
    </section>
  )
}
