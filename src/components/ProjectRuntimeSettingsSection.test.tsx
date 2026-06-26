import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type {
  ProjectRuntimeConfig,
  VisualStudioInstallation,
} from '../../electron/preload'
import ProjectRuntimeSettingsSection, {
  formatRuntimeConfigSaveError,
  normalizeRuntimeConfigForHost,
} from './ProjectRuntimeSettingsSection.js'

const visualStudioInstallations: VisualStudioInstallation[] = [
  {
    instanceId: 'vs-2022-community',
    displayName: 'Visual Studio 2022 Community',
    installationPath: 'C:\\VS\\2022\\Community',
    productLineVersion: '2022',
    isPrerelease: false,
  },
]

const config: ProjectRuntimeConfig = {
  enabled: true,
  cwd: 'app',
  command: 'npm run dev',
  envType: 'visual-studio',
  visualStudioInstanceId: 'vs-2022-community',
  outputEncoding: 'gbk',
}

describe('ProjectRuntimeSettingsSection', () => {
  it('renders a no-project hint when no project is selected', () => {
    const markup = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId={null}
        loading={false}
        value={{ ...config, enabled: false }}
        disabled={false}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('ai-settings-note')
    expect(markup).not.toContain('project-runtime-settings-grid')
  })

  it('renders runtime environment and command fields', () => {
    const markup = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId="project-1"
        loading={false}
        value={config}
        disabled={false}
        visualStudioInstallations={visualStudioInstallations}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('project-runtime-settings-grid')
    expect(markup).toContain('Visual Studio Developer Command Prompt')
    expect(markup).toContain('Visual Studio 2022 Community')
    expect(markup).toContain('UTF-8')
    expect(markup).toContain('GBK')
    expect(markup).toContain('npm run dev')
  })

  it('uses the original environment on macOS without rendering Windows runtime choices', () => {
    const markup = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId="project-1"
        loading={false}
        value={config}
        disabled={false}
        hostPlatform="MacIntel"
        visualStudioInstallations={visualStudioInstallations}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('原始环境')
    expect(markup).not.toContain('MSYS2')
    expect(markup).not.toContain('Visual Studio Developer Command Prompt')
    expect(markup).not.toContain('Visual Studio 实例')
  })

  it('normalizes macOS runtime configs to the original environment', () => {
    expect(normalizeRuntimeConfigForHost(config, 'MacIntel')).toEqual({
      ...config,
      envType: 'system',
      visualStudioInstanceId: '',
    })
  })

  it('renders a loading hint while runtime config is still loading', () => {
    const markup = renderToStaticMarkup(
      <ProjectRuntimeSettingsSection
        projectId="project-1"
        loading={true}
        value={{ ...config, enabled: false }}
        disabled={true}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('ai-settings-note')
    expect(markup).not.toContain('project-runtime-settings-grid')
  })

  it('formats validation details into a readable save error', () => {
    const message = formatRuntimeConfigSaveError('invalid runtime config', [
      {
        path: 'runtime_config.command',
        message: 'command must be a non-empty string',
      },
      {
        path: 'runtime_config.cwd',
        message: 'cwd must be a relative path within target_repo',
      },
    ])

    expect(message).toContain('invalid runtime config')
    expect(message).toContain('runtime_config.command')
    expect(message).toContain('command must be a non-empty string')
    expect(message).toContain('runtime_config.cwd')
  })
})
