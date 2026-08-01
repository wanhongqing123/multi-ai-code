import type { OpenCodeProviderProfile } from '../aicli/opencodeConfig.js'

/**
 * AICLI 的权限档位。只有两档，用户不需要知道背后是哪些参数：
 * - 'default'     ：不注入任何 --dangerously-* 旁路参数，CLI 按自己的默认行为
 *                   该问就问、该沙箱就沙箱
 * - 'full-access' ：注入旁路参数，CLI 不再逐次询问（无人值守的定时任务 / IM
 *                   远程驱动依赖这一档，所以它才是本应用的默认值）
 */
export type AiPermissionMode = 'default' | 'full-access'

// 单个 AI CLI 的配置（主会话 / repo 视图共用）。
export interface AiSettings {
  ai_cli: 'claude' | 'codex' | 'opencode'
  /** 缺省视为 'full-access'——老配置里没有这个字段，行为必须保持不变。 */
  permission_mode?: AiPermissionMode
  command?: string
  args?: string[]
  env?: Record<string, string>
  opencode?: OpenCodeProviderProfile
}
