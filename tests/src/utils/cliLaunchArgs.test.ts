import { describe, expect, it } from 'vitest'
import {
  DEFAULT_AI_PERMISSION_MODE,
  buildCliLaunchArgs
} from '../../../src/utils/cliLaunchArgs.js'

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

describe('权限档位', () => {
  // 老项目的 project.json 里没有 permission_mode，读出来是 undefined。默认值必须
  // 落回「完全访问权限」，否则升级一次应用，所有无人值守任务都会卡在审批提示上。
  it('defaults to full access so existing configs keep working', () => {
    expect(DEFAULT_AI_PERMISSION_MODE).toBe('full-access')
    expect(buildCliLaunchArgs('codex', '/repo/demo', [], undefined)).toEqual(
      buildCliLaunchArgs('codex', '/repo/demo', [], 'full-access')
    )
  })

  // 「默认权限」= 一个 --dangerously-* 都不注入；非权限类的默认参数还得留着，
  // 不然 codex 会掉回 alt-screen、上下文窗口也缩回去。
  it('injects no dangerous bypass flag in default permission mode', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo', [], 'default')).toEqual([
      '--no-alt-screen',
      '-c',
      'model_context_window=1000000'
    ])
    expect(buildCliLaunchArgs('claude', '/repo/demo', [], 'default')).toEqual([])
    expect(buildCliLaunchArgs('opencode', '/repo/demo', [], 'default')).toEqual([])
  })

  it('still honours flags the user typed under 高级参数设置', () => {
    // 选了「默认权限」但自己在高级里写了旁路参数：以用户手写的为准，原样透传，
    // 不因为档位是 default 就把它吃掉。
    expect(
      buildCliLaunchArgs('claude', '/repo/demo', ['--dangerously-skip-permissions'], 'default')
    ).toEqual(['--dangerously-skip-permissions'])
    expect(buildCliLaunchArgs('opencode', '/repo/demo', ['--yolo'], 'default')).toEqual(['--yolo'])
  })

  it('keeps full access identical to the pre-permission-selector behaviour', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo', [], 'full-access')).toEqual([
      '--no-alt-screen',
      '--dangerously-bypass-approvals-and-sandbox',
      '--dangerously-bypass-hook-trust',
      '-c',
      'model_context_window=1000000'
    ])
    expect(buildCliLaunchArgs('claude', '/repo/demo', [], 'full-access')).toEqual([
      '--dangerously-skip-permissions'
    ])
    expect(buildCliLaunchArgs('opencode', '/repo/demo', [], 'full-access')).toEqual([
      '--dangerously-skip-permissions'
    ])
  })
})
