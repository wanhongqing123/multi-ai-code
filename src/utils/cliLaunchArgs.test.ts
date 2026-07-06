import { describe, expect, it } from 'vitest'
import { buildCliLaunchArgs } from './cliLaunchArgs.js'

describe('buildCliLaunchArgs', () => {
  it('adds Codex 1M context window config by default', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo')).toEqual([
      '--dangerously-bypass-approvals-and-sandbox',
      '-c',
      'model_context_window=1000000'
    ])
  })

  it('does not add Codex context config to Claude', () => {
    expect(buildCliLaunchArgs('claude', '/repo/demo')).toEqual([
      '--dangerously-skip-permissions'
    ])
  })

  it('keeps user-supplied Codex context window override', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', [
        '-c',
        'model_context_window=272000'
      ])
    ).toEqual([
      '--dangerously-bypass-approvals-and-sandbox',
      '-c',
      'model_context_window=272000'
    ])
  })
})
