import { describe, expect, it } from 'vitest'
import { resolveBuildExecutionScope } from '../../../electron/build/executionScope.js'

const config = {
  enabled: true,
  steps: [
    {
      id: 'configure',
      name: 'Configure',
      envType: 'msys' as const,
      cwd: '.',
      command: 'cmake -S . -B build',
      enabled: true,
      visualStudioInstanceId: '',
      outputEncoding: 'auto' as const,
    },
    {
      id: 'package',
      name: 'Package',
      envType: 'msys' as const,
      cwd: 'build',
      command: 'cpack',
      enabled: false,
      visualStudioInstanceId: '',
      outputEncoding: 'auto' as const,
    },
  ],
}

describe('resolveBuildExecutionScope', () => {
  it('accepts full-pipeline builds without a step id', () => {
    expect(resolveBuildExecutionScope(config, { scope: 'all' })).toEqual({
      ok: true,
      scope: 'all',
      requestedStepId: null,
      runnableStepIds: ['configure'],
    })
  })

  it('accepts a selected enabled step for single-step builds', () => {
    expect(
      resolveBuildExecutionScope(config, { scope: 'single-step', stepId: 'configure' })
    ).toEqual({
      ok: true,
      scope: 'single-step',
      requestedStepId: 'configure',
      runnableStepIds: ['configure'],
    })
  })

  it('rejects single-step builds without a step id', () => {
    expect(resolveBuildExecutionScope(config, { scope: 'single-step' })).toEqual({
      ok: false,
      error: 'build step is required for single-step scope',
    })
  })

  it('rejects disabled selected steps', () => {
    expect(
      resolveBuildExecutionScope(config, { scope: 'single-step', stepId: 'package' })
    ).toEqual({
      ok: false,
      error: 'build step is disabled: package',
    })
  })
})
