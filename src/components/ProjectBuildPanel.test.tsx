import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'
import type { BuildRuntimeState, ProjectBuildConfig } from '../../electron/preload'
import ProjectBuildPanel, {
  canAnalyzeBuildFailure,
  canStartBuild,
  canStopBuild,
  getDisplayStepsForBuildPanel,
  getBuildLogStatusLabel,
  getBuildStartBlockedReason,
  getBuildStepStatusLabel,
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
      enabled: true,
      visualStudioInstanceId: '',
      outputEncoding: 'auto'
    }
  ]
}

const baseState: BuildRuntimeState = {
  status: 'idle',
  scope: null,
  requestedStepId: null,
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
    expect(
      getBuildStartBlockedReason('project-1', true, enabledBuildConfig, 'single-step', 'missing-step')
    ).toContain('missing-step')
    expect(canStartBuild('project-1', true, enabledBuildConfig)).toBe(true)
  })

  it('returns a dedicated label for not-run steps', () => {
    expect(getBuildStepStatusLabel('not-run')).toBe('未执行')
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
        visualStudioInstanceId: null,
        visualStudioDisplayName: null,
        outputEncoding: 'auto',
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
        visualStudioInstanceId: null,
        visualStudioDisplayName: null,
        outputEncoding: 'auto',
        cwd: '.',
        command: 'cmake -S . -B build',
        exitCode: 127,
        signal: null,
        reason: 'process exited with code 127',
        logTail: 'not found'
      }
    }

    expect(getBuildLogStatusLabel(failedState)).toContain('Configure')
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
        onStartSingleBuild={vi.fn()}
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
              visualStudioDisplayName: null,
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
            visualStudioInstanceId: null,
            visualStudioDisplayName: null,
            outputEncoding: 'auto',
            cwd: '.',
            command: 'cmake -S . -B build',
            exitCode: 1,
            signal: null,
            reason: 'build failed',
            logTail: 'fatal error'
          }
        } as BuildRuntimeState}
        sessionId="session-1"
        sessionStatus="running"
        onClose={vi.fn()}
        onStartBuild={vi.fn()}
        onStartSingleBuild={vi.fn()}
        onStopBuild={vi.fn()}
        onAnalyzeFailure={vi.fn()}
      />
    )

    expect(closedMarkup).toBe('')
    expect(openMarkup).toContain('build-panel')
    expect(openMarkup).toContain('build-step-card')
    expect(openMarkup).toContain('Configure')
  })

  it('renders configured steps even before any build has run', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildPanel
        open={true}
        currentProjectId="project-1"
        currentProjectName="Demo"
        buildConfig={enabledBuildConfig}
        buildConfigReady={true}
        state={baseState}
        sessionId="session-1"
        sessionStatus="running"
        onClose={vi.fn()}
        onStartBuild={vi.fn()}
        onStartSingleBuild={vi.fn()}
        onStopBuild={vi.fn()}
        onAnalyzeFailure={vi.fn()}
      />
    )

    expect(markup).toContain('Configure')
    expect(markup).toContain('单独构建')
    expect(markup).not.toContain('当前还没有运行过构建步骤')
  })

  it('renders visual studio display name and output encoding metadata', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildPanel
        open={true}
        currentProjectId="project-1"
        currentProjectName="Demo"
        buildConfig={{
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
        buildConfigReady={true}
        state={{
          ...baseState,
          status: 'failed',
          steps: [
            {
              id: 'compile',
              name: 'Compile',
              envType: 'visual-studio',
              cwd: 'build',
              command: 'cmake --build .',
              enabled: true,
              visualStudioInstanceId: 'vs-2022-community',
              visualStudioDisplayName: 'Visual Studio 2022 Community',
              outputEncoding: 'gbk',
              status: 'failed',
              resolvedCwd: 'E:/demo/build',
              startedAt: '2026-05-20T10:00:00.000Z',
              finishedAt: '2026-05-20T10:00:01.000Z',
              exitCode: 2,
              signal: null
            }
          ],
          lastFailure: {
            projectId: 'project-1',
            projectName: 'Demo',
            targetRepo: 'E:/demo',
            stepId: 'compile',
            stepName: 'Compile',
            envType: 'visual-studio',
            visualStudioInstanceId: 'vs-2022-community',
            visualStudioDisplayName: 'Visual Studio 2022 Community',
            outputEncoding: 'gbk',
            cwd: 'E:/demo/build',
            command: 'cmake --build .',
            exitCode: 2,
            signal: null,
            reason: 'build failed',
            logTail: 'fatal error'
          }
        } as BuildRuntimeState}
        sessionId="session-1"
        sessionStatus="running"
        onClose={vi.fn()}
        onStartBuild={vi.fn()}
        onStartSingleBuild={vi.fn()}
        onStopBuild={vi.fn()}
        onAnalyzeFailure={vi.fn()}
      />
    )

    expect(markup).toContain('Visual Studio 2022 Community')
    expect(markup).toContain('GBK')
  })

  it('renders the renamed sequential build button and per-step single-build buttons', () => {
    const markup = renderToStaticMarkup(
      <ProjectBuildPanel
        open={true}
        currentProjectId="project-1"
        currentProjectName="Demo"
        buildConfig={enabledBuildConfig}
        buildConfigReady={true}
        state={{
          ...baseState,
          steps: [
            {
              ...enabledBuildConfig.steps[0],
              visualStudioDisplayName: null,
              status: 'not-run',
              resolvedCwd: null,
              startedAt: null,
              finishedAt: null,
              exitCode: null,
              signal: null
            }
          ]
        }}
        sessionId="session-1"
        sessionStatus="running"
        onClose={vi.fn()}
        onStartBuild={vi.fn()}
        onStartSingleBuild={vi.fn()}
        onStopBuild={vi.fn()}
        onAnalyzeFailure={vi.fn()}
      />
    )

    expect(markup).toContain('顺序构建')
    expect(markup).toContain('单独构建')
    expect(markup).toContain('未执行')
  })
  it('prefers current build config step identities when the last run is not active', () => {
    const buildConfig: ProjectBuildConfig = {
      enabled: true,
      steps: [
        {
          id: 'demo',
          name: 'DEMO',
          envType: 'msys',
          cwd: '.',
          command: 'build-demo.cmd',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto'
        },
        {
          id: 'sdk',
          name: 'SDK',
          envType: 'msys',
          cwd: '.',
          command: 'build-sdk.cmd',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto'
        }
      ]
    }

    const staleState: BuildRuntimeState = {
      ...baseState,
      status: 'failed',
      steps: [
        {
          ...buildConfig.steps[1],
          name: 'Old SDK Label',
          visualStudioDisplayName: null,
          status: 'succeeded',
          resolvedCwd: 'E:/demo',
          startedAt: '2026-05-20T10:00:00.000Z',
          finishedAt: '2026-05-20T10:00:01.000Z',
          exitCode: 0,
          signal: null
        },
        {
          ...buildConfig.steps[0],
          name: 'Old Demo Label',
          visualStudioDisplayName: null,
          status: 'failed',
          resolvedCwd: 'E:/demo',
          startedAt: '2026-05-20T10:00:02.000Z',
          finishedAt: '2026-05-20T10:00:03.000Z',
          exitCode: 1,
          signal: null
        }
      ],
      lastFailure: {
        projectId: 'project-1',
        projectName: 'Demo',
        targetRepo: 'E:/demo',
        stepId: 'demo',
        stepName: 'Old Demo Label',
        envType: 'msys',
        visualStudioInstanceId: null,
        visualStudioDisplayName: null,
        outputEncoding: 'auto',
        cwd: '.',
        command: 'build-demo.cmd',
        exitCode: 1,
        signal: null,
        reason: 'build failed',
        logTail: 'fatal error'
      }
    }

    expect(
      getDisplayStepsForBuildPanel(buildConfig, staleState).map((step) => ({
        id: step.id,
        name: step.name,
        status: step.status
      }))
    ).toEqual([
      { id: 'demo', name: 'DEMO', status: 'failed' },
      { id: 'sdk', name: 'SDK', status: 'succeeded' }
    ])
  })
})
