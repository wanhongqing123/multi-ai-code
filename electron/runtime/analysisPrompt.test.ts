import { describe, expect, it } from 'vitest'
import { buildRuntimeAnalysisPrompt, getRuntimeAnalysisPrompt } from './analysisPrompt.js'
import type { RuntimeState } from './types.js'

const baseState: RuntimeState = {
  status: 'running',
  projectId: 'p1',
  projectName: 'Demo',
  targetRepo: 'E:\\repo',
  cwd: 'E:\\repo\\app',
  command: 'npm run dev',
  envType: 'msys',
  visualStudioInstanceId: null,
  visualStudioDisplayName: null,
  outputEncoding: 'auto',
  startedAt: '2026-06-12T10:00:00.000Z',
  finishedAt: null,
  exitCode: null,
  signal: null,
  log: 'server started\nGET /health 200\n',
}

describe('buildRuntimeAnalysisPrompt', () => {
  it('builds a diagnostic-only prompt with runtime context', () => {
    const prompt = buildRuntimeAnalysisPrompt(baseState, { logTailLimit: 200 })

    expect(prompt).toContain('Analyze this runtime log for likely problems.')
    expect(prompt).toContain('Do not modify code.')
    expect(prompt).toContain('Project: Demo (p1)')
    expect(prompt).toContain('Working directory: E:\\repo\\app')
    expect(prompt).toContain('Command: npm run dev')
    expect(prompt).toContain('server started')
    expect(prompt).toContain('What to check first')
  })

  it('caps the runtime log tail', () => {
    const prompt = buildRuntimeAnalysisPrompt(
      { ...baseState, log: `${'x'.repeat(300)}fatal tail\n` },
      { logTailLimit: 20 }
    )

    expect(prompt).not.toContain('x'.repeat(100))
    expect(prompt).toContain('fatal tail')
  })
})

describe('getRuntimeAnalysisPrompt', () => {
  it('rejects empty runtime logs', () => {
    expect(getRuntimeAnalysisPrompt({ ...baseState, log: '   ' })).toEqual({
      ok: false,
      error: 'no runtime log available',
    })
  })

  it('returns a prompt for running, failed, stopped, or exited runtime states', () => {
    for (const status of ['running', 'failed', 'stopped', 'exited'] as const) {
      const result = getRuntimeAnalysisPrompt({ ...baseState, status })
      expect(result.ok).toBe(true)
    }
  })
})
