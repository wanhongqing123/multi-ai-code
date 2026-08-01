import type { AiPermissionMode } from '../../electron/settings/types'

// 老配置里没有 permission_mode 字段，缺省必须等于「完全访问权限」——本应用的
// 定时任务 / IM 远程都是无人值守，退回到逐次询问会直接把任务卡死。
// （electron/settings/types.ts 是纯类型模块，运行时常量只能放这边。）
export const DEFAULT_AI_PERMISSION_MODE: AiPermissionMode = 'full-access'

export type SupportedCli = 'claude' | 'codex' | 'opencode'
export const CODEX_CONTEXT_WINDOW_CONFIG = 'model_context_window=1000000'
const CODEX_NO_ALT_SCREEN_ARG = '--no-alt-screen'
const CODEX_BYPASS_HOOK_TRUST_ARG = '--dangerously-bypass-hook-trust'

function hasAnyArg(args: readonly string[], flags: readonly string[]): boolean {
  return args.some((arg) => flags.includes(arg))
}

function hasCodexContextWindowConfig(args: readonly string[]): boolean {
  return args.some((arg, index) => {
    if (arg === '-c' || arg === '--config') {
      return args[index + 1]?.startsWith('model_context_window=') === true
    }
    return arg.startsWith('-cmodel_context_window=') ||
      arg.startsWith('--config=model_context_window=')
  })
}

function codexDefaultArgs(
  extraArgs: readonly string[],
  permissionMode: AiPermissionMode
): string[] {
  const args: string[] = []
  if (!hasAnyArg(extraArgs, [CODEX_NO_ALT_SCREEN_ARG])) {
    args.push(CODEX_NO_ALT_SCREEN_ARG)
  }
  // 「默认权限」= 一个 --dangerously-* 都不注入。审批旁路和 hook 信任旁路都属于
  // 绕过用户确认，只在「完全访问权限」下才加。
  if (permissionMode === 'full-access') {
    if (!hasAnyArg(extraArgs, ['--dangerously-bypass-approvals-and-sandbox'])) {
      args.push('--dangerously-bypass-approvals-and-sandbox')
    }
    if (!hasAnyArg(extraArgs, [CODEX_BYPASS_HOOK_TRUST_ARG])) {
      args.push(CODEX_BYPASS_HOOK_TRUST_ARG)
    }
  }
  if (!hasCodexContextWindowConfig(extraArgs)) {
    args.push('-c', CODEX_CONTEXT_WINDOW_CONFIG)
  }
  return args
}

function opencodeDefaultArgs(
  extraArgs: readonly string[],
  permissionMode: AiPermissionMode
): string[] {
  if (permissionMode !== 'full-access') return []
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
      permissionMode === 'full-access' &&
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
