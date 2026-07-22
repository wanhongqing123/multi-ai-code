import { describe, expect, it } from 'vitest'
import { buildFailureAnalysisPrompt, getFailureAnalysisPrompt } from '../../../electron/build/analysisPrompt.js'
import type { BuildFailureContext, BuildRuntimeState } from '../../../electron/build/types.js'

const failure: BuildFailureContext = {
  projectId: 'p1',
  projectName: 'Demo',
  stepId: 'compile',
  stepName: 'Compile',
  envType: 'visual-studio',
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: 'auto',
  targetRepo: 'E:\\repo',
  cwd: 'E:\\repo\\build',
  command: 'cmake --build .',
  exitCode: 2,
  signal: null,
  reason: 'process exited with code 2',
  logTail: 'fatal error C1000: Internal compiler error\n',
}

describe('buildFailureAnalysisPrompt', () => {
  it('builds a constrained prompt from the failed run context', () => {
    const prompt = buildFailureAnalysisPrompt({
      status: 'failed',
      scope: 'all',
      requestedStepId: null,
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      startedAt: '2026-05-20T10:00:00.000Z',
      finishedAt: '2026-05-20T10:10:00.000Z',
      activeStepId: null,
      log: 'configure ok\nfatal error C1000',
      steps: [
        {
          id: 'configure',
          name: 'Configure',
          envType: 'msys',
          cwd: '.',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
          visualStudioDisplayName: null,
          status: 'succeeded',
          resolvedCwd: 'E:\\repo',
          startedAt: '2026-05-20T10:00:00.000Z',
          finishedAt: '2026-05-20T10:01:00.000Z',
          exitCode: 0,
          signal: null,
        },
        {
          id: 'compile',
          name: 'Compile',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cmake --build .',
          enabled: true,
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'auto',
          visualStudioDisplayName: null,
          status: 'failed',
          resolvedCwd: 'E:\\repo\\build',
          startedAt: '2026-05-20T10:01:00.000Z',
          finishedAt: '2026-05-20T10:02:00.000Z',
          exitCode: 2,
          signal: null,
        },
        {
          id: 'package',
          name: 'Package',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cpack',
          enabled: true,
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'auto',
          visualStudioDisplayName: null,
          status: 'skipped',
          resolvedCwd: null,
          startedAt: null,
          finishedAt: null,
          exitCode: null,
          signal: null,
        },
      ],
      lastFailure: failure,
    })

    expect(prompt).toContain('Analyze only the failure cause for this failed build run.')
    expect(prompt).toContain('Do not modify code.')
    expect(prompt).toContain('Do not provide patches.')
    expect(prompt).toContain('Do not execute commands.')
    expect(prompt).toContain('Do not suggest command execution.')
    expect(prompt).toContain('Project: Demo (p1)')
    expect(prompt).toContain('Target repo: E:\\repo')
    expect(prompt).toContain('Step: Compile (compile)')
    expect(prompt).toContain('Step summary:')
    expect(prompt).toContain('- Configure [configure]: succeeded')
    expect(prompt).toContain('- Compile [compile]: failed')
    expect(prompt).toContain('- Package [package]: skipped')
    expect(prompt).toContain('Reply using exactly these sections:')
    expect(prompt).toContain('Failure category')
    expect(prompt).toContain('Most likely cause')
    expect(prompt).toContain('Evidence')
    expect(prompt).toContain('What to check first')
    expect(prompt).toContain('fatal error C1000')
  })
})

describe('getFailureAnalysisPrompt', () => {
  it('returns a structured error when there is no failed build context', () => {
    const state: BuildRuntimeState = {
      status: 'succeeded',
      scope: 'all',
      requestedStepId: null,
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      startedAt: '2026-05-20T10:00:00.000Z',
      finishedAt: '2026-05-20T10:10:00.000Z',
      activeStepId: null,
      steps: [],
      log: '',
      lastFailure: null,
    }

    expect(getFailureAnalysisPrompt(state)).toEqual({
      ok: false,
      error: 'no failed build context available',
    })
  })

  it('returns a prompt when failure context exists', () => {
    const state: BuildRuntimeState = {
      status: 'failed',
      scope: 'all',
      requestedStepId: null,
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      startedAt: '2026-05-20T10:00:00.000Z',
      finishedAt: '2026-05-20T10:10:00.000Z',
      activeStepId: null,
      steps: [
        {
          id: 'configure',
          name: 'Configure',
          envType: 'msys',
          cwd: '.',
          command: 'cmake -S . -B build',
          enabled: true,
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
          visualStudioDisplayName: null,
          status: 'succeeded',
          resolvedCwd: 'E:\\repo',
          startedAt: '2026-05-20T10:00:00.000Z',
          finishedAt: '2026-05-20T10:01:00.000Z',
          exitCode: 0,
          signal: null,
        },
        {
          id: 'compile',
          name: 'Compile',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cmake --build .',
          enabled: true,
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'auto',
          visualStudioDisplayName: null,
          status: 'failed',
          resolvedCwd: 'E:\\repo\\build',
          startedAt: '2026-05-20T10:01:00.000Z',
          finishedAt: '2026-05-20T10:02:00.000Z',
          exitCode: 2,
          signal: null,
        },
      ],
      log: 'fatal error C1000',
      lastFailure: failure,
    }

    const result = getFailureAnalysisPrompt(state)

    expect(result.ok).toBe(true)
    if (!result.ok) {
      throw new Error('expected prompt')
    }
    expect(result.prompt).toContain('Compile')
    expect(result.prompt).toContain('Do not modify code.')
    expect(result.prompt).toContain('Target repo: E:\\repo')
  })
})
