import type {
  BuildConfigValidationIssue,
  BuildStepConfig,
  ProjectBuildConfig
} from '../../electron/preload'

export interface ProjectBuildSettingsSectionProps {
  projectId: string | null
  loading: boolean
  value: ProjectBuildConfig
  disabled: boolean
  onChange: (next: ProjectBuildConfig) => void
}

export function createBuildStep(id: string): BuildStepConfig {
  return {
    id,
    name: 'New Step',
    envType: 'msys',
    cwd: '.',
    command: '',
    enabled: true
  }
}

export function appendBuildStep(config: ProjectBuildConfig, id: string): ProjectBuildConfig {
  return {
    ...config,
    steps: [...config.steps, createBuildStep(id)]
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

export default function ProjectBuildSettingsSection(
  props: ProjectBuildSettingsSectionProps
): JSX.Element {
  const handleAddStep = (): void => {
    props.onChange(appendBuildStep(props.value, createBuildStepId()))
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
              checked={props.value.enabled}
              onChange={(event) =>
                props.onChange({ ...props.value, enabled: event.target.checked })
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
          {props.value.steps.length ? (
            <div className="project-build-settings-list">
              {props.value.steps.map((step, index) => (
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
                        onClick={() => props.onChange(moveBuildStep(props.value, index, 'up'))}
                        disabled={props.disabled || index === 0}
                      >
                        上移
                      </button>
                      <button
                        type="button"
                        className="drawer-btn"
                        onClick={() => props.onChange(moveBuildStep(props.value, index, 'down'))}
                        disabled={props.disabled || index === props.value.steps.length - 1}
                      >
                        下移
                      </button>
                      <button
                        type="button"
                        className="drawer-btn danger"
                        onClick={() => props.onChange(removeBuildStep(props.value, index))}
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
                        props.onChange(updateBuildStep(props.value, index, { enabled: event.target.checked }))
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
                          props.onChange(updateBuildStep(props.value, index, { name: event.target.value }))
                        }
                        disabled={props.disabled}
                      />
                    </label>
                    <label>
                      环境
                      <select
                        value={step.envType}
                        onChange={(event) =>
                          props.onChange(
                            updateBuildStep(props.value, index, {
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
                    <label className="project-build-settings-grid-full">
                      工作目录
                      <input
                        type="text"
                        value={step.cwd}
                        onChange={(event) =>
                          props.onChange(updateBuildStep(props.value, index, { cwd: event.target.value }))
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
                            updateBuildStep(props.value, index, { command: event.target.value })
                          )
                        }
                        rows={3}
                        placeholder="cmake --build build"
                        disabled={props.disabled}
                      />
                    </label>
                  </div>
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
