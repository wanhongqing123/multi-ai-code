import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { BuildRuntimeState, ProjectBuildConfig } from '../../electron/preload'
import ProjectBuildPanel, {
  canAnalyzeBuildFailure,
  canStartBuild,
  canStopBuild,
  getBuildLogStatusLabel,
  getBuildStartBlockedReason,
  getBuildStatusLabel
} from './ProjectBuildPanel.js'

const disabledBuildConfig: ProjectBuildConfig = {
  enabled: false,
  steps: []
}

const enabledBuildConfig: ProjectBuildConfig = {
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
}

const baseState: BuildRuntimeState = {
  status: 'idle',
  projectId: null,
  projectName: null,
  targetRepo: null,
  startedAt: null,
  finishedAt: null,
  activeStepId: null,
  steps: [],
  log: '',
  lastFailure: null
}

describe('ProjectBuildPanel', () => {
  it('returns non-empty labels for each build status', () => {
    const labels = [
      getBuildStatusLabel('idle'),
      getBuildStatusLabel('running'),
      getBuildStatusLabel('succeeded'),
      getBuildStatusLabel('failed'),
      getBuildStatusLabel('stopped')
    ]

    expect(labels.every((label) => label.length > 0)).toBe(true)
    expect(new Set(labels).size).toBe(labels.length)
  })

  it('blocks start when the project or build config is unavailable', () => {
    expect(getBuildStartBlockedReason(null, false, enabledBuildConfig)).toBeTruthy()
    expect(getBuildStartBlockedReason('project-1', false, enabledBuildConfig)).toBeTruthy()
    expect(getBuildStartBlockedReason('project-1', true, disabledBuildConfig)).toBeTruthy()
    expect(canStartBuild('project-1', true, enabledBuildConfig)).toBe(true)
  })

  it('only allows stop while a build is running', () => {
    expect(canStopBuild('idle')).toBe(false)
    expect(canStopBuild('running')).toBe(true)
    expect(canStopBuild('failed')).toBe(false)
  })

  it('only allows failure analysis when the build failed with saved failure context', () => {
    const failedState: BuildRuntimeState = {
      ...baseState,
      status: 'failed',
      lastFailure: {
        projectId: 'project-1',
        projectName: 'Demo',
        targetRepo: 'E:/demo',
        stepId: 'build',
        stepName: 'Build',
        envType: 'msys',
        cwd: '.',
        command: 'cmake --build build',
        exitCode: 1,
        signal: null,
        reason: 'build failed',
        logTail: 'fatal error'
      }
    }

    expect(canAnalyzeBuildFailure(failedState)).toBe(true)
    expect(canAnalyzeBuildFailure(baseState)).toBe(false)
  })

  it('uses the overall build state for the log header after a failure', () => {
    const failedState: BuildRuntimeState = {
      ...baseState,
      status: 'failed',
      activeStepId: null,
      lastFailure: {
        projectId: 'project-1',
        projectName: 'Demo',
        targetRepo: 'E:/demo',
        stepId: 'configure',
        stepName: 'Configure',
        envType: 'msys',
        cwd: '.',
        command: 'cmake -S . -B build',
        exitCode: 127,
        signal: null,
        reason: 'process exited with code 127',
        logTail: 'not found'
      }
    }

    expect(getBuildLogStatusLabel(failedState)).toContain('Configure')
    expect(getBuildLogStatusLabel(failedState)).not.toContain('等待中')
  })

  it('renders the panel body only when open', () => {
    const closedMarkup = renderToStaticMarkup(
      <ProjectBuildPanel
        open={false}
        currentProjectId="project-1"
        currentProjectName="Demo"
        buildConfig={enabledBuildConfig}
        buildConfigReady={true}
        state={baseState}
        sessionId="session-1"
        sessionStatus="running"
        onClose={vi.fn()}
        onStartBuild={vi.fn()}
        onStopBuild={vi.fn()}
        onAnalyzeFailure={vi.fn()}
      />
    )
    const openMarkup = renderToStaticMarkup(
      <ProjectBuildPanel
        open={true}
        currentProjectId="project-1"
        currentProjectName="Demo"
        buildConfig={enabledBuildConfig}
        buildConfigReady={true}
        state={{
          ...baseState,
          status: 'failed',
          steps: [
            {
              ...enabledBuildConfig.steps[0],
              status: 'failed',
              resolvedCwd: 'E:/demo',
              startedAt: '2026-05-20T10:00:00.000Z',
              finishedAt: '2026-05-20T10:00:01.000Z',
              exitCode: 1,
              signal: null
            }
          ],
          lastFailure: {
            projectId: 'project-1',
            projectName: 'Demo',
            targetRepo: 'E:/demo',
            stepId: 'configure',
            stepName: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            exitCode: 1,
            signal: null,
            reason: 'build failed',
            logTail: 'fatal error'
          }
        }}
        sessionId="session-1"
        sessionStatus="running"
        onClose={vi.fn()}
        onStartBuild={vi.fn()}
        onStopBuild={vi.fn()}
        onAnalyzeFailure={vi.fn()}
      />
    )

    expect(closedMarkup).toBe('')
    expect(openMarkup).toContain('build-panel')
    expect(openMarkup).toContain('build-step-card')
    expect(openMarkup).toContain('Configure')
  })
})
