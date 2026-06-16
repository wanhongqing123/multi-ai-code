import { describe, expect, it } from 'vitest'
import type { RuntimeState } from '../../electron/preload'
import { selectVisibleRuntimeState } from './runtimeStateSelection.js'

const emptyRuntimeState: RuntimeState = {
  status: 'idle',
  projectId: null,
  projectName: null,
  targetRepo: null,
  cwd: null,
  command: null,
  envType: null,
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: null,
  startedAt: null,
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: ''
}

const runningOtherProject: RuntimeState = {
  status: 'running',
  projectId: 'project-2',
  projectName: 'Other',
  targetRepo: 'E:/other',
  cwd: 'E:/other',
  command: 'apollodemo.exe',
  envType: 'visual-studio',
  visualStudioInstanceId: 'vs-2022',
  visualStudioDisplayName: 'Visual Studio 2022',
  outputEncoding: 'gbk',
  startedAt: '2026-06-16T11:00:00.000Z',
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: '[apollo] live console output'
}

describe('selectVisibleRuntimeState', () => {
  it('shows the active runtime log even when a different project is selected', () => {
    expect(selectVisibleRuntimeState('project-1', runningOtherProject, emptyRuntimeState)).toBe(
      runningOtherProject
    )
  })

  it('returns an empty state for inactive runtime output from another project', () => {
    expect(
      selectVisibleRuntimeState(
        'project-1',
        { ...runningOtherProject, status: 'exited' },
        emptyRuntimeState
      )
    ).toBe(emptyRuntimeState)
  })
})
