import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type {
  ProjectBuildConfig,
  VisualStudioInstallation
} from '../../electron/preload'
import ProjectBuildSettingsSection, {
  appendBuildStep,
  formatBuildConfigSaveError,
  moveBuildStep,
  removeBuildStep
} from './ProjectBuildSettingsSection.js'

const visualStudioInstallations: VisualStudioInstallation[] = [
  {
    instanceId: 'vs-2022-community',
    displayName: 'Visual Studio 2022 Community',
    installationPath: 'C:\\VS\\2022\\Community',
    productLineVersion: '2022',
    isPrerelease: false
  }
]

describe('ProjectBuildSettingsSection', () => {
  it('renders a no-project hint when no project is selected', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildSettingsSection
        projectId={null}
        loading={false}
        value={{ enabled: false, steps: [] }}
        disabled={false}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('ai-settings-note')
    expect(markup).not.toContain('project-build-settings-toolbar')
  })

  it('renders visual studio instance and output encoding controls for visual-studio steps', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildSettingsSection
        projectId="project-1"
        loading={false}
        value={{
          enabled: true,
          steps: [
            {
              id: 'compile',
              name: 'Compile',
              envType: 'visual-studio',
              cwd: 'build',
              command: 'cmake --build .',
              enabled: true,
              visualStudioInstanceId: 'vs-2022-community',
              outputEncoding: 'gbk'
            }
          ]
        }}
        disabled={false}
        visualStudioInstallations={visualStudioInstallations}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('project-build-settings-toolbar')
    expect(markup).toContain('Compile')
    expect(markup).toContain('Visual Studio Developer Command Prompt')
    expect(markup).toContain('Visual Studio 实例')
    expect(markup).toContain('Visual Studio 2022 Community')
    expect(markup).toContain('输出编码')
    expect(markup).toContain('自动')
    expect(markup).toContain('UTF-8')
    expect(markup).toContain('GBK')
    expect(markup).toContain('project-build-step-card')
  })

  it('appends a default enabled step', () => {
    const next = appendBuildStep({ enabled: true, steps: [] }, 'step-1')

    expect(next).toEqual({
      enabled: true,
      steps: [
        {
          id: 'step-1',
          name: 'New Step',
          envType: 'msys',
          cwd: '.',
          command: '',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto'
        }
      ]
    })
  })

  it('renders a loading hint while the current project build config is still loading', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildSettingsSection
        projectId="project-1"
        loading={true}
        value={{ enabled: false, steps: [] }}
        disabled={true}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('ai-settings-note')
    expect(markup).not.toContain('project-build-settings-toolbar')
  })

  it('shows a warning when the selected visual studio instance is no longer available', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildSettingsSection
        projectId="project-1"
        loading={false}
        value={{
          enabled: true,
          steps: [
            {
              id: 'compile',
              name: 'Compile',
              envType: 'visual-studio',
              cwd: 'build',
              command: 'cmake --build .',
              enabled: true,
              visualStudioInstanceId: 'vs-missing',
              outputEncoding: 'auto'
            }
          ]
        }}
        disabled={false}
        visualStudioInstallations={visualStudioInstallations}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('project-build-step-warning')
    expect(markup).toContain('所选 Visual Studio 实例当前不可用')
  })

  it('moves steps up and down without mutating the rest of the config', () => {
    const initial: ProjectBuildConfig = {
      enabled: true,
      steps: [
        {
          id: 'one',
          name: 'One',
          envType: 'msys',
          cwd: '.',
          command: 'echo 1',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto'
        },
        {
          id: 'two',
          name: 'Two',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'echo 2',
          enabled: false,
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'auto'
        }
      ]
    }

    expect(moveBuildStep(initial, 1, 'up').steps.map((step) => step.id)).toEqual(['two', 'one'])
    expect(moveBuildStep(initial, 0, 'down').steps.map((step) => step.id)).toEqual(['two', 'one'])
  })

  it('removes a step by index', () => {
    const next = removeBuildStep(
      {
        enabled: false,
        steps: [
          {
            id: 'one',
            name: 'One',
            envType: 'msys',
            cwd: '.',
            command: 'echo 1',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'auto'
          },
          {
            id: 'two',
            name: 'Two',
            envType: 'msys',
            cwd: '.',
            command: 'echo 2',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'auto'
          }
        ]
      },
      0
    )

    expect(next.steps).toHaveLength(1)
    expect(next.steps[0].id).toBe('two')
  })

  it('formats validation details into a readable save error', () => {
    const message = formatBuildConfigSaveError('invalid build config', [
      {
        path: 'build_config.steps[0].command',
        message: 'command must be a non-empty string'
      },
      {
        path: 'build_config.steps[1].cwd',
        message: 'cwd must be a relative path within target_repo'
      }
    ])

    expect(message).toContain('invalid build config')
    expect(message).toContain('command must be a non-empty string')
    expect(message).toContain('cwd must be a relative path within target_repo')
    expect(message).toContain('1')
    expect(message).toContain('2')
  })
})
