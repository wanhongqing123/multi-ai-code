import type { AiPermissionMode } from '../../electron/settings/types'

// 老配置里没有 permission_mode 字段，缺省仍等于「完全访问权限」：保持无文件
// 系统沙箱；只有 Codex 上游明确归类为高风险的命令进入本地/远程审批。
// （electron/settings/types.ts 是纯类型模块，运行时常量只能放这边。）
export const DEFAULT_AI_PERMISSION_MODE: AiPermissionMode = 'full-access'

export type SupportedCli = 'claude' | 'codex' | 'opencode'
export const CODEX_CONTEXT_WINDOW_CONFIG = 'model_context_window=1000000'
export const CODEX_APPROVALS_REVIEWER_CONFIG = 'approvals_reviewer="user"'
const CODEX_NO_ALT_SCREEN_ARG = '--no-alt-screen'
const CODEX_BYPASS_HOOK_TRUST_ARG = '--dangerously-bypass-hook-trust'
const CODEX_APPROVAL_ARGS = ['--ask-for-approval', 'on-request'] as const
const CODEX_SANDBOX_ARGS = ['--sandbox', 'danger-full-access'] as const
const CODEX_AUTO_REVIEW_FLAGS = ['--approve-for-me', '--not-so-yolo'] as const

const CODEX_EXPLICIT_APPROVAL_FLAGS = [
  '--ask-for-approval',
  '-a',
  '--approve-for-me',
  '--not-so-yolo',
  '--dangerously-bypass-approvals-and-sandbox',
  '--yolo'
] as const

const CODEX_EXPLICIT_SANDBOX_FLAGS = [
  '--sandbox',
  '-s',
  '--approve-for-me',
  '--not-so-yolo',
  '--dangerously-bypass-approvals-and-sandbox',
  '--yolo'
] as const

function hasAnyArg(args: readonly string[], flags: readonly string[]): boolean {
  return args.some((arg) => flags.includes(arg))
}

function hasAnyFlag(args: readonly string[], flags: readonly string[]): boolean {
  return args.some((arg) =>
    flags.some((flag) => {
      if (arg === flag || arg.startsWith(`${flag}=`)) return true
      // clap accepts value-taking short options in their compact form, for example
      // `-anever` and `-sworkspace-write`. Treat those as explicit user choices too;
      // otherwise adding our long-form defaults makes Codex reject the duplicate flag.
      return (
        flag.startsWith('-') &&
        !flag.startsWith('--') &&
        flag.length === 2 &&
        arg.startsWith(flag) &&
        arg.length > flag.length
      )
    })
  )
}

function hasCodexConfigKey(args: readonly string[], key: string): boolean {
  return args.some((arg, index) => {
    if (arg === '-c' || arg === '--config') {
      return args[index + 1]?.startsWith(`${key}=`) === true
    }
    return (
      arg.startsWith(`-c${key}=`) ||
      arg.startsWith(`-c=${key}=`) ||
      arg.startsWith(`--config=${key}=`)
    )
  })
}

function hasCodexContextWindowConfig(args: readonly string[]): boolean {
  return hasCodexConfigKey(args, 'model_context_window')
}

function hasCodexApprovalsReviewerConfig(args: readonly string[]): boolean {
  return hasCodexConfigKey(args, 'approvals_reviewer')
}

function codexDefaultArgs(
  extraArgs: readonly string[],
  permissionMode: AiPermissionMode
): string[] {
  const args: string[] = []
  // Anything after clap's `--` terminator is positional prompt/input text, not
  // an advanced option. Never let text that merely looks like a flag suppress
  // the permission defaults.
  const terminator = extraArgs.indexOf('--')
  const explicitArgs = terminator >= 0 ? extraArgs.slice(0, terminator) : extraArgs
  if (!hasAnyArg(explicitArgs, [CODEX_NO_ALT_SCREEN_ARG])) {
    args.push(CODEX_NO_ALT_SCREEN_ARG)
  }
  if (permissionMode === 'dangerous') {
    const hasExplicitPermissions =
      hasAnyFlag(explicitArgs, CODEX_EXPLICIT_APPROVAL_FLAGS) ||
      hasAnyFlag(explicitArgs, CODEX_EXPLICIT_SANDBOX_FLAGS) ||
      hasCodexConfigKey(explicitArgs, 'approval_policy') ||
      hasCodexConfigKey(explicitArgs, 'sandbox_mode')
    if (!hasExplicitPermissions) {
      args.push('--dangerously-bypass-approvals-and-sandbox')
    }
    if (!hasAnyArg(explicitArgs, [CODEX_BYPASS_HOOK_TRUST_ARG])) {
      args.push(CODEX_BYPASS_HOOK_TRUST_ARG)
    }
  }
  // 「完全访问权限」下，普通命令仍可在无沙箱环境中直接执行；但删除等被
  // Codex 标记为危险的命令要保留审批机会。单独配置 approval=never 时，这些
  // 命令仍会在 exec policy 层直接 Forbidden；只有上面的显式「危险模式」标志
  // 才会启用定制版 Codex 的全命令放行。
  //
  // 高级参数里手工写的权限 flag 始终优先：只补齐用户没有指定的那一维，
  // 避免与 --approve-for-me / --yolo 之类的显式选择产生 clap 冲突。
  if (permissionMode === 'full-access') {
    if (
      !hasAnyFlag(explicitArgs, CODEX_EXPLICIT_APPROVAL_FLAGS) &&
      !hasCodexConfigKey(explicitArgs, 'approval_policy')
    ) {
      args.push(...CODEX_APPROVAL_ARGS)
    }
    if (
      !hasAnyFlag(explicitArgs, CODEX_EXPLICIT_SANDBOX_FLAGS) &&
      !hasCodexConfigKey(explicitArgs, 'sandbox_mode')
    ) {
      args.push(...CODEX_SANDBOX_ARGS)
    }
    if (!hasAnyArg(explicitArgs, [CODEX_BYPASS_HOOK_TRUST_ARG])) {
      args.push(CODEX_BYPASS_HOOK_TRUST_ARG)
    }
    // 审批必须留给当前用户（宿主会通过远程 IM 转发）。否则用户的
    // config.toml 若把 reviewer 设为 auto_review，审批会被 Guardian 提前消费。
    // 显式 approvals_reviewer 配置和 --approve-for-me 仍以用户选择为准。
    if (
      !hasCodexApprovalsReviewerConfig(explicitArgs) &&
      !hasAnyFlag(explicitArgs, CODEX_AUTO_REVIEW_FLAGS)
    ) {
      args.push('-c', CODEX_APPROVALS_REVIEWER_CONFIG)
    }
  }
  if (!hasCodexContextWindowConfig(explicitArgs)) {
    args.push('-c', CODEX_CONTEXT_WINDOW_CONFIG)
  }
  return args
}

function opencodeDefaultArgs(
  extraArgs: readonly string[],
  permissionMode: AiPermissionMode
): string[] {
  if (permissionMode === 'default') return []
  if (hasAnyArg(extraArgs, ['--dangerously-skip-permissions', '--yolo', '--auto'])) {
    return []
  }
  return ['--dangerously-skip-permissions']
}

export function buildCliLaunchArgs(
  binary: SupportedCli,
  _targetRepo: string,
  extraArgs: readonly string[] = [],
  permissionMode: AiPermissionMode = 'full-access'
): string[] {
  const args: string[] = []
  if (binary === 'claude') {
    if (
      permissionMode !== 'default' &&
      !hasAnyArg(extraArgs, ['--dangerously-skip-permissions'])
    ) {
      args.push('--dangerously-skip-permissions')
    }
    return [...args, ...extraArgs]
  }

  if (binary === 'opencode') {
    args.push(...opencodeDefaultArgs(extraArgs, permissionMode))
    return [...args, ...extraArgs]
  }

  args.push(...codexDefaultArgs(extraArgs, permissionMode))
  return [...args, ...extraArgs]
}
