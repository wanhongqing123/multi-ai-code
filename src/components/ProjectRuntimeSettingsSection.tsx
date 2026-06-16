import type {
  BuildConfigValidationIssue,
  ProjectRuntimeConfig,
  VisualStudioInstallation,
} from '../../electron/preload'

export interface ProjectRuntimeSettingsSectionProps {
  projectId: string | null
  loading: boolean
  value: ProjectRuntimeConfig
  disabled: boolean
  visualStudioInstallations?: VisualStudioInstallation[]
  visualStudioInstallationsLoading?: boolean
  onRefreshVisualStudioInstallations?: () => void
  onChange: (next: ProjectRuntimeConfig) => void
}

export function formatRuntimeConfigSaveError(
  error: string,
  details?: BuildConfigValidationIssue[]
): string {
  if (!details?.length) return `项目运行配置保存失败：${error}`
  return [
    `项目运行配置保存失败：${error}`,
    ...details.map((detail) => `- ${detail.path}：${detail.message}`),
  ].join('\n')
}

function isMissingVisualStudioInstance(
  config: ProjectRuntimeConfig,
  installations: VisualStudioInstallation[]
): boolean {
  if (config.envType !== 'visual-studio') return false
  if (!config.visualStudioInstanceId) return false
  return !installations.some((item) => item.instanceId === config.visualStudioInstanceId)
}

export default function ProjectRuntimeSettingsSection(
  props: ProjectRuntimeSettingsSectionProps
): JSX.Element {
  const visualStudioInstallations = props.visualStudioInstallations ?? []
  const visualStudioInstallationsLoading = props.visualStudioInstallationsLoading ?? false
  const update = (patch: Partial<ProjectRuntimeConfig>) =>
    props.onChange({ ...props.value, ...patch })

  return (
    <section className="ai-settings-card">
      <div className="ai-settings-title">项目运行</div>
      {!props.projectId ? (
        <div className="ai-settings-note">选择项目后可编辑项目运行配置</div>
      ) : props.loading ? (
        <div className="ai-settings-note">正在读取项目运行配置...</div>
      ) : (
        <>
          <label className="ai-settings-checkbox">
            <input
              type="checkbox"
              checked={props.value.enabled}
              onChange={(event) => update({ enabled: event.target.checked })}
              disabled={props.disabled}
            />
            <span>启用项目运行</span>
          </label>

          <div className="project-runtime-settings-grid">
            <label>
              环境
              <select
                value={props.value.envType}
                onChange={(event) =>
                  update({ envType: event.target.value as ProjectRuntimeConfig['envType'] })
                }
                disabled={props.disabled}
              >
                <option value="msys">MSYS2</option>
                <option value="visual-studio">Visual Studio Developer Command Prompt</option>
              </select>
            </label>

            <label>
              输出编码
              <select
                value={props.value.outputEncoding}
                onChange={(event) =>
                  update({
                    outputEncoding: event.target.value as ProjectRuntimeConfig['outputEncoding'],
                  })
                }
                disabled={props.disabled}
              >
                <option value="auto">自动</option>
                <option value="utf8">UTF-8</option>
                <option value="gbk">GBK</option>
              </select>
            </label>

            {props.value.envType === 'visual-studio' ? (
              <label>
                Visual Studio 实例
                <select
                  value={props.value.visualStudioInstanceId}
                  onChange={(event) => update({ visualStudioInstanceId: event.target.value })}
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
                value={props.value.cwd}
                onChange={(event) => update({ cwd: event.target.value })}
                placeholder="."
                disabled={props.disabled}
              />
              <div className="ai-settings-note">相对 target_repo 的目录，填 . 表示仓库根目录</div>
            </label>

            <label className="project-build-settings-grid-full">
              运行命令
              <textarea
                value={props.value.command}
                onChange={(event) => update({ command: event.target.value })}
                rows={3}
                placeholder="npm run dev"
                disabled={props.disabled}
              />
            </label>
          </div>

          {isMissingVisualStudioInstance(props.value, visualStudioInstallations) ? (
            <div className="ai-settings-note project-build-step-warning">
              所选 Visual Studio 实例当前不可用
            </div>
          ) : null}
        </>
      )}
    </section>
  )
}
