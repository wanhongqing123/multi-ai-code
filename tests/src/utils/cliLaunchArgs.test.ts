import { describe, expect, it } from 'vitest'
import { buildCliLaunchArgs } from '../../../src/utils/cliLaunchArgs.js'

describe('buildCliLaunchArgs', () => {
  it('adds Codex 1M context window config by default', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo')).toEqual([
      '--no-alt-screen',
      '--dangerously-bypass-approvals-and-sandbox',
      '--dangerously-bypass-hook-trust',
      '-c',
      'model_context_window=1000000'
    ])
  })

  it('does not add Codex context config to Claude', () => {
    expect(buildCliLaunchArgs('claude', '/repo/demo')).toEqual([
      '--dangerously-skip-permissions'
    ])
  })

  it('uses OpenCode permission bypass without Codex context config', () => {
    expect(buildCliLaunchArgs('opencode', '/repo/demo')).toEqual([
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
      '--no-alt-screen',
      '--dangerously-bypass-approvals-and-sandbox',
      '--dangerously-bypass-hook-trust',
      '-c',
      'model_context_window=272000'
    ])
  })

  it('does not duplicate Codex no-alt-screen when user supplies it', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', [
        '--no-alt-screen',
        '--verbose'
      ])
    ).toEqual([
      '--dangerously-bypass-approvals-and-sandbox',
      '--dangerously-bypass-hook-trust',
      '-c',
      'model_context_window=1000000',
      '--no-alt-screen',
      '--verbose'
    ])
  })

  it('does not duplicate Codex hook trust bypass when user supplies it', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', [
        '--dangerously-bypass-hook-trust',
        '--verbose'
      ])
    ).toEqual([
      '--no-alt-screen',
      '--dangerously-bypass-approvals-and-sandbox',
      '-c',
      'model_context_window=1000000',
      '--dangerously-bypass-hook-trust',
      '--verbose'
    ])
  })

  it('does not duplicate OpenCode permission bypass aliases', () => {
    expect(buildCliLaunchArgs('opencode', '/repo/demo', ['--auto'])).toEqual([
      '--auto'
    ])
  })
})
