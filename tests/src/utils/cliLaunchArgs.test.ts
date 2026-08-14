import { describe, expect, it } from 'vitest'
import {
  CODEX_APPROVALS_REVIEWER_CONFIG,
  DEFAULT_AI_PERMISSION_MODE,
  buildCliLaunchArgs
} from '../../../src/utils/cliLaunchArgs.js'

describe('buildCliLaunchArgs', () => {
  it('adds Codex 1M context window config by default', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo')).toEqual([
      '--no-alt-screen',
      '--ask-for-approval',
      'on-request',
      '--sandbox',
      'danger-full-access',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
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
      '--ask-for-approval',
      'on-request',
      '--sandbox',
      'danger-full-access',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
      '-c',
      'model_context_window=272000'
    ])
  })

  it.each([
    ['-c', 'approvals_reviewer="auto_review"'],
    ['--config', 'approvals_reviewer="auto_review"'],
    ['-capprovals_reviewer="auto_review"'],
    ['-c=approvals_reviewer="auto_review"'],
    ['--config=approvals_reviewer="auto_review"']
  ])('keeps an explicit Codex approvals reviewer config: %j', (...explicitArgs) => {
    const args = buildCliLaunchArgs('codex', '/repo/demo', explicitArgs)

    expect(args).not.toContain(CODEX_APPROVALS_REVIEWER_CONFIG)
    expect(args).toEqual(expect.arrayContaining(explicitArgs))
  })

  it('does not duplicate Codex no-alt-screen when user supplies it', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', [
        '--no-alt-screen',
        '--verbose'
      ])
    ).toEqual([
      '--ask-for-approval',
      'on-request',
      '--sandbox',
      'danger-full-access',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
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
      '--ask-for-approval',
      'on-request',
      '--sandbox',
      'danger-full-access',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
      '-c',
      'model_context_window=1000000',
      '--dangerously-bypass-hook-trust',
      '--verbose'
    ])
  })

  it('honours an explicit legacy bypass without adding conflicting approval flags', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', [
        '--dangerously-bypass-approvals-and-sandbox'
      ])
    ).toEqual([
      '--no-alt-screen',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
      '-c',
      'model_context_window=1000000',
      '--dangerously-bypass-approvals-and-sandbox'
    ])
  })

  it('honours --approve-for-me without adding conflicting approval or sandbox flags', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo', ['--approve-for-me'])).toEqual([
      '--no-alt-screen',
      '--dangerously-bypass-hook-trust',
      '-c',
      'model_context_window=1000000',
      '--approve-for-me'
    ])
  })

  it('only fills the Codex permission dimension not set by advanced args', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', ['--ask-for-approval=never'])
    ).toEqual([
      '--no-alt-screen',
      '--sandbox',
      'danger-full-access',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
      '-c',
      'model_context_window=1000000',
      '--ask-for-approval=never'
    ])
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', ['--sandbox', 'workspace-write'])
    ).toEqual([
      '--no-alt-screen',
      '--ask-for-approval',
      'on-request',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
      '-c',
      'model_context_window=1000000',
      '--sandbox',
      'workspace-write'
    ])
  })

  it('honours compact Codex permission flags without adding duplicate long flags', () => {
    const approvalArgs = buildCliLaunchArgs('codex', '/repo/demo', ['-anever'])
    expect(approvalArgs).not.toContain('--ask-for-approval')
    expect(approvalArgs).toEqual(expect.arrayContaining(['-anever']))
    expect(approvalArgs).toEqual(expect.arrayContaining(['--sandbox', 'danger-full-access']))

    const sandboxArgs = buildCliLaunchArgs('codex', '/repo/demo', ['-sworkspace-write'])
    expect(sandboxArgs).not.toContain('--sandbox')
    expect(sandboxArgs).toEqual(expect.arrayContaining(['-sworkspace-write']))
    expect(sandboxArgs).toEqual(expect.arrayContaining(['--ask-for-approval', 'on-request']))
  })

  it('does not override permission dimensions configured with -c', () => {
    const approvalArgs = buildCliLaunchArgs('codex', '/repo/demo', [
      '-c',
      'approval_policy="never"'
    ])
    expect(approvalArgs).not.toContain('--ask-for-approval')
    expect(approvalArgs).toEqual(
      expect.arrayContaining(['-c', 'approval_policy="never"', '--sandbox', 'danger-full-access'])
    )

    const sandboxArgs = buildCliLaunchArgs('codex', '/repo/demo', [
      '-c',
      'sandbox_mode="read-only"'
    ])
    expect(sandboxArgs).not.toContain('--sandbox')
    expect(sandboxArgs).toEqual(
      expect.arrayContaining(['-c', 'sandbox_mode="read-only"', '--ask-for-approval', 'on-request'])
    )
  })

  it('recognizes equals-form Codex config overrides', () => {
    const args = buildCliLaunchArgs('codex', '/repo/demo', [
      '-c=approval_policy="never"',
      '-c=sandbox_mode="read-only"',
      '-c=model_context_window=272000'
    ])

    expect(args).not.toContain('--ask-for-approval')
    expect(args).not.toContain('--sandbox')
    expect(args).not.toContain('model_context_window=1000000')
    expect(args).toEqual(
      expect.arrayContaining([
        '-c=approval_policy="never"',
        '-c=sandbox_mode="read-only"',
        '-c=model_context_window=272000'
      ])
    )
  })

  it('does not treat positional text after -- as advanced Codex flags', () => {
    expect(
      buildCliLaunchArgs('codex', '/repo/demo', [
        '--',
        '--approve-for-me',
        '--sandbox=read-only'
      ])
    ).toEqual([
      '--no-alt-screen',
      '--ask-for-approval',
      'on-request',
      '--sandbox',
      'danger-full-access',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
      '-c',
      'model_context_window=1000000',
      '--',
      '--approve-for-me',
      '--sandbox=read-only'
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

  it('keeps full access unrestricted while allowing dangerous commands to request approval', () => {
    expect(buildCliLaunchArgs('codex', '/repo/demo', [], 'full-access')).toEqual([
      '--no-alt-screen',
      '--ask-for-approval',
      'on-request',
      '--sandbox',
      'danger-full-access',
      '--dangerously-bypass-hook-trust',
      '-c',
      CODEX_APPROVALS_REVIEWER_CONFIG,
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
