import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { ProjectBuildConfig } from '../../electron/preload'
import ProjectBuildSettingsSection, {
  appendBuildStep,
  formatBuildConfigSaveError,
  moveBuildStep,
  removeBuildStep
} from './ProjectBuildSettingsSection.js'

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

  it('renders build steps and editing controls for the selected project', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildSettingsSection
        projectId="project-1"
        loading={false}
        value={{
          enabled: true,
          steps: [
            {
              id: 'configure',
              name: 'Configure',
              envType: 'msys',
              cwd: '.',
              command: 'cmake -S . -B build',
              enabled: true
            }
          ]
        }}
        disabled={false}
        onChange={vi.fn()}
      />
    )

    expect(markup).toContain('project-build-settings-toolbar')
    expect(markup).toContain('Configure')
    expect(markup).toContain('MSYS2')
    expect(markup).toContain('Visual Studio Developer Command Prompt')
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
          enabled: true
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

  it('moves steps up and down without mutating the rest of the config', () => {
    const initial: ProjectBuildConfig = {
      enabled: true,
      steps: [
        { id: 'one', name: 'One', envType: 'msys', cwd: '.', command: 'echo 1', enabled: true },
        {
          id: 'two',
          name: 'Two',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'echo 2',
          enabled: false
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
          { id: 'one', name: 'One', envType: 'msys', cwd: '.', command: 'echo 1', enabled: true },
          { id: 'two', name: 'Two', envType: 'msys', cwd: '.', command: 'echo 2', enabled: true }
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
